// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/FluidAnisotropyComputeShader.h"
#include "GPU/Managers/GPUZOrderSortManager.h"
#include "GPU/Managers/GPUBoundarySkinningManager.h"
#include "GPU/GPUBoundaryAttachment.h"  // For FGPUBoneDeltaAttachment
#include "Core/FluidParticle.h"
#include "Core/KawaiiFluidSimulationStats.h"
#include "Rendering/Shaders/FluidSpatialHashShaders.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"  // FRHIGPUBufferReadback for async GPU→CPU readback
#include "RenderingThread.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "PipelineStateCache.h"
#include "HAL/IConsoleManager.h"  // For console command execution
#include "Async/Async.h"  // For AsyncTask
#include "Async/ParallelFor.h"

DEFINE_LOG_CATEGORY(LogGPUFluidSimulator);

// =====================================================
// Debug CVars for RenderDoc Capture
// =====================================================
static int32 GFluidCaptureFirstFrame = 0;  // 0 = disabled by default
static FAutoConsoleVariableRef CVarFluidCaptureFirstFrame(
	TEXT("r.Fluid.CaptureFirstFrame"),
	GFluidCaptureFirstFrame,
	TEXT("Capture first GPU fluid simulation frame in RenderDoc.\n")
	TEXT("  0 = Disabled (default)\n")
	TEXT("  1 = Capture first frame\n")
	TEXT("Set to 1 and restart PIE to capture first frame."),
	ECVF_Default
);

static int32 GFluidCaptureFrameNumber = 0;  // 0 = use CaptureFirstFrame, >0 = capture specific frame
static FAutoConsoleVariableRef CVarFluidCaptureFrameNumber(
	TEXT("r.Fluid.CaptureFrameNumber"),
	GFluidCaptureFrameNumber,
	TEXT("Capture specific GPU fluid simulation frame number in RenderDoc.\n")
	TEXT("  0 = Use CaptureFirstFrame setting\n")
	TEXT("  N = Capture frame N (1-indexed)"),
	ECVF_Default
);

// Internal flag - reset when CVar is changed back to 1
static int32 GFluidCapturedFrame = 0;  // Tracks which frame was captured (0 = none)

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUFluidSimulator::FGPUFluidSimulator()
	: bIsInitialized(false)
	, MaxParticleCount(0)
	, CurrentParticleCount(0)
	, ExternalForce(FVector3f::ZeroVector)
	, MaxVelocity(50000.0f)   // Safety clamp: 50000 cm/s = 500 m/s
{
}

FGPUFluidSimulator::~FGPUFluidSimulator()
{
	if (bIsInitialized)
	{
		Release();
	}
}

//=============================================================================
// Initialization
//=============================================================================

void FGPUFluidSimulator::Initialize(int32 InMaxParticleCount)
{
	if (InMaxParticleCount <= 0)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("Initialize called with invalid particle count: %d"), InMaxParticleCount);
		return;
	}

	MaxParticleCount = InMaxParticleCount;

	// Initialize SpawnManager
	SpawnManager = MakeUnique<FGPUSpawnManager>();
	SpawnManager->Initialize(InMaxParticleCount);

	// Initialize CollisionManager
	CollisionManager = MakeUnique<FGPUCollisionManager>();
	CollisionManager->Initialize();

	// Initialize ZOrderSortManager
	ZOrderSortManager = MakeUnique<FGPUZOrderSortManager>();
	ZOrderSortManager->Initialize();

	// Initialize BoundarySkinningManager
	BoundarySkinningManager = MakeUnique<FGPUBoundarySkinningManager>();
	BoundarySkinningManager->Initialize();

	// Initialize AdhesionManager
	AdhesionManager = MakeUnique<FGPUAdhesionManager>();
	AdhesionManager->Initialize();

	// Initialize StaticBoundaryManager
	StaticBoundaryManager = MakeUnique<FGPUStaticBoundaryManager>();
	StaticBoundaryManager->Initialize();

	// Initialize render resource on render thread
	BeginInitResource(this);

	bIsInitialized = true;

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Fluid Simulator initialized with capacity: %d particles"), MaxParticleCount);
}

void FGPUFluidSimulator::Release()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Release render resource synchronously to avoid "deleted without being released" error
	// BeginReleaseResource is async, so we need to flush to ensure completion before destruction
	ReleaseResource();  // Synchronous release - blocks until render thread completes

	// Release SpawnManager
	if (SpawnManager.IsValid())
	{
		SpawnManager->Release();
		SpawnManager.Reset();
	}

	// Release CollisionManager
	if (CollisionManager.IsValid())
	{
		CollisionManager->Release();
		CollisionManager.Reset();
	}

	// Release ZOrderSortManager
	if (ZOrderSortManager.IsValid())
	{
		ZOrderSortManager->Release();
		ZOrderSortManager.Reset();
	}

	// Release BoundarySkinningManager
	if (BoundarySkinningManager.IsValid())
	{
		BoundarySkinningManager->Release();
		BoundarySkinningManager.Reset();
	}

	// Release AdhesionManager
	if (AdhesionManager.IsValid())
	{
		AdhesionManager->Release();
		AdhesionManager.Reset();
	}

	// Release StaticBoundaryManager
	if (StaticBoundaryManager.IsValid())
	{
		StaticBoundaryManager->Release();
		StaticBoundaryManager.Reset();
	}

	// Release Anisotropy Readback objects
	ReleaseAnisotropyReadbackObjects();

	// Clear Shadow data (extracted from StatsReadback)
	ReadyShadowPositions.Empty();
	ReadyShadowVelocities.Empty();
	ReadyShadowNeighborCounts.Empty();
	ReadyShadowPositionsFrame.store(0);

	// Release Stats Readback objects
	ReleaseStatsReadbackObjects();

	bIsInitialized = false;
	MaxParticleCount = 0;
	CurrentParticleCount = 0;
	bHasValidGPUResults.store(false);
	CachedGPUParticles.Empty();
	PersistentParticleBuffer = nullptr;

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Fluid Simulator released"));
}

//=============================================================================
// FRenderResource Interface
//=============================================================================

void FGPUFluidSimulator::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (MaxParticleCount <= 0)
	{
		return;
	}

	ResizeBuffers(RHICmdList, MaxParticleCount);
}

void FGPUFluidSimulator::ReleaseRHI()
{
	FScopeLock Lock(&BufferLock);

	ParticleBufferRHI.SafeRelease();
	ParticleSRV.SafeRelease();
	ParticleUAV.SafeRelease();

	PositionBufferRHI.SafeRelease();
	PositionSRV.SafeRelease();
	PositionUAV.SafeRelease();

	StagingBufferRHI.SafeRelease();

	CellCountsBufferRHI.SafeRelease();
	CellCountsSRV.SafeRelease();
	CellCountsUAV.SafeRelease();

	ParticleIndicesBufferRHI.SafeRelease();
	ParticleIndicesSRV.SafeRelease();
	ParticleIndicesUAV.SafeRelease();

	CachedGPUParticles.Empty();

	// Release persistent pooled buffers
	PersistentParticleBuffer.SafeRelease();
	PersistentCellCountsBuffer.SafeRelease();
	PersistentParticleIndicesBuffer.SafeRelease();

	// Release persistent Z-Order buffers (for Ray Marching)
	PersistentCellStartBuffer.SafeRelease();
	PersistentCellEndBuffer.SafeRelease();

	// Release neighbor cache buffers
	NeighborListBuffer.SafeRelease();
	NeighborCountsBuffer.SafeRelease();
	NeighborBufferParticleCapacity = 0;

	// Release particle sleeping buffers
	SleepCountersBuffer.SafeRelease();
	SleepCountersCapacity = 0;

	// Release BoneDeltaAttachment buffer (NEW simplified bone-following system)
	BoneDeltaAttachmentBuffer.SafeRelease();
	BoneDeltaAttachmentCapacity = 0;

	// Collision cleanup is handled by CollisionManager::Release()
}

void FGPUFluidSimulator::ResizeBuffers(FRHICommandListBase& RHICmdList, int32 NewCapacity)
{
	FScopeLock Lock(&BufferLock);

	// Release existing buffers
	ParticleBufferRHI.SafeRelease();
	ParticleSRV.SafeRelease();
	ParticleUAV.SafeRelease();
	PositionBufferRHI.SafeRelease();
	PositionSRV.SafeRelease();
	PositionUAV.SafeRelease();
	StagingBufferRHI.SafeRelease();
	CellCountsBufferRHI.SafeRelease();
	CellCountsSRV.SafeRelease();
	CellCountsUAV.SafeRelease();
	ParticleIndicesBufferRHI.SafeRelease();
	ParticleIndicesSRV.SafeRelease();
	ParticleIndicesUAV.SafeRelease();

	if (NewCapacity <= 0)
	{
		return;
	}

	// Create main particle buffer (UAV + SRV)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidParticleBuffer"), NewCapacity * sizeof(FGPUFluidParticle), sizeof(FGPUFluidParticle))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		ParticleBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

		ParticleSRV = RHICmdList.CreateShaderResourceView(ParticleBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(ParticleBufferRHI));
		ParticleUAV = RHICmdList.CreateUnorderedAccessView(ParticleBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(ParticleBufferRHI));
	}

	// Create position buffer for spatial hash (float3 only)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidPositionBuffer"), NewCapacity * sizeof(FVector3f), sizeof(FVector3f))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		PositionBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

		PositionSRV = RHICmdList.CreateShaderResourceView(PositionBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(PositionBufferRHI));
		PositionUAV = RHICmdList.CreateUnorderedAccessView(PositionBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(PositionBufferRHI));
	}

	// Create staging buffer for CPU readback
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidStagingBuffer"), NewCapacity * sizeof(FGPUFluidParticle), sizeof(FGPUFluidParticle))
			.AddUsage(BUF_None)
			.SetInitialState(ERHIAccess::CopyDest);
		StagingBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
	}

	// Create spatial hash buffers
	{
		// Cell counts buffer
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidCellCounts"), GPU_SPATIAL_HASH_SIZE * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		CellCountsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

		CellCountsSRV = RHICmdList.CreateShaderResourceView(CellCountsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(CellCountsBufferRHI));
		CellCountsUAV = RHICmdList.CreateUnorderedAccessView(CellCountsBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(CellCountsBufferRHI));

		// Particle indices buffer
		const uint32 TotalSlots = GPU_SPATIAL_HASH_SIZE * GPU_MAX_PARTICLES_PER_CELL;
		const FRHIBufferCreateDesc BufferDesc2 =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidParticleIndices"), TotalSlots * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		ParticleIndicesBufferRHI = RHICmdList.CreateBuffer(BufferDesc2);

		ParticleIndicesSRV = RHICmdList.CreateShaderResourceView(ParticleIndicesBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(ParticleIndicesBufferRHI));
		ParticleIndicesUAV = RHICmdList.CreateUnorderedAccessView(ParticleIndicesBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(ParticleIndicesBufferRHI));
	}

	// Reserve capacity for cached array (but don't set Num - will be filled by UploadParticles)
	CachedGPUParticles.Empty();
	CachedGPUParticles.Reserve(NewCapacity);

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Resized GPU buffers to capacity: %d"), NewCapacity);
}

//=============================================================================
// Simulation Execution
//=============================================================================

void FGPUFluidSimulator::SimulateSubstep(const FGPUFluidSimulationParams& Params)
{
	if (!bIsInitialized)
	{
		return;
	}

	// Must call BeginFrame() before SimulateSubstep()
	if (!bFrameActive)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("SimulateSubstep called without BeginFrame! Call BeginFrame first."));
		return;
	}

	// =========================================================================
	// BONE TRANSFORM REFRESH - MUST happen HERE, not in BeginRenderViewFamily!
	//
	// Simulation is called from HandlePostActorTick (OnWorldPostActorTick delegate),
	// which fires AFTER animation evaluation. Bones are current at this point.
	//
	// We refresh bones HERE because the fallback ENQUEUE_RENDER_COMMAND below
	// can execute BEFORE BeginRenderViewFamily (RT runs parallel to GT).
	// If we only refresh in BeginRenderViewFamily, fallback would read stale bones.
	//
	// BeginRenderViewFamily should NOT refresh bones (disabled separately).
	// =========================================================================
	RefreshAllBoneTransforms();

	FGPUFluidSimulator* Self = this;
	FGPUFluidSimulationParams ParamsCopy = Params;

	// Execute simulation directly in render command
	ENQUEUE_RENDER_COMMAND(GPUFluidSimulate)(
		[Self, ParamsCopy](FRHICommandListImmediate& RHICmdList)
		{
			// Limit logging to first 10 frames
			static int32 RenderFrameCounter = 0;
			const bool bLogThisFrame = (RenderFrameCounter++ < 10);

			// if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RENDER COMMAND START (Frame %d)"), RenderFrameCounter);

			// Build and execute RDG
			FRDGBuilder GraphBuilder(RHICmdList);
			Self->SimulateSubstep_RDG(GraphBuilder, ParamsCopy);

			// if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RDG EXECUTE START"));
			GraphBuilder.Execute();
			// if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RDG EXECUTE COMPLETE"));

			// Mark that we have valid GPU results
			Self->bHasValidGPUResults.store(true);
		}
	);
}

void FGPUFluidSimulator::EnqueueSimulation(const FGPUFluidSimulationParams& Params)
{
	FScopeLock Lock(&PendingSimulationLock);
	PendingSimulationParams.Add(Params);

	// Bone transforms are refreshed ONCE in BeginRenderViewFamily (FluidSceneViewExtension).
	// This is the optimal timing: after animation evaluation, before render thread starts.
	// Both ViewExtension and fallback execution paths will use these refreshed transforms.
}

bool FGPUFluidSimulator::HasPendingSimulations() const
{
	FScopeLock Lock(&PendingSimulationLock);
	return PendingSimulationParams.Num() > 0;
}

void FGPUFluidSimulator::ExecutePendingSimulations_RenderThread(FRDGBuilder& GraphBuilder)
{
	// Move pending params to local array (thread-safe)
	TArray<FGPUFluidSimulationParams> ParamsToExecute;
	{
		FScopeLock Lock(&PendingSimulationLock);
		ParamsToExecute = MoveTemp(PendingSimulationParams);
		PendingSimulationParams.Reset();
	}

	if (ParamsToExecute.Num() == 0)
	{
		return;
	}

	// Limit logging to first 10 frames
	static int32 RenderFrameCounter = 0;
	const bool bLogThisFrame = (RenderFrameCounter++ < 10);

	if (bLogThisFrame)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> ExecutePendingSimulations_RenderThread: %d substeps (Frame %d)"),
			ParamsToExecute.Num(), RenderFrameCounter);
	}

	// Execute all pending substeps in the same RDG
	// NOTE: Bone transforms are now refreshed in BeginRenderViewFamily (game thread)
	// right before render thread starts, so we always use the latest transforms
	// that match the skeletal mesh rendering.
	for (const FGPUFluidSimulationParams& Params : ParamsToExecute)
	{
		SimulateSubstep_RDG(GraphBuilder, Params);
	}

	// Mark that we have valid GPU results
	bHasValidGPUResults.store(true);
}

void FGPUFluidSimulator::SimulateSubstep_RDG(FRDGBuilder& GraphBuilder, const FGPUFluidSimulationParams& Params)
{
	// Spawn/despawn handled in BeginFrame - this is physics only

	// Skip if no particles
	if (CurrentParticleCount == 0)
	{
		return;
	}

	// Need persistent buffer for simulation
	if (!PersistentParticleBuffer.IsValid())
	{
		return;
	}

	// Cache CellSize for Ray Marching volume building
	CachedCellSize = Params.CellSize;

	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluidSimulation (Particles: %d)", CurrentParticleCount);

	// =====================================================
	// Phase 1: Prepare Particle Buffer (CPU Upload or Reuse)
	// =====================================================
	FRDGBufferRef ParticleBuffer = PrepareParticleBuffer(GraphBuilder, Params);
	if (!ParticleBuffer)
	{
		return;
	}

	// Create transient position buffer for spatial hash
	FRDGBufferDesc PositionBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), CurrentParticleCount);
	FRDGBufferRef PositionBuffer = GraphBuilder.CreateBuffer(PositionBufferDesc, TEXT("GPUFluidPositions"));

	FRDGBufferUAVRef ParticlesUAVLocal = GraphBuilder.CreateUAV(ParticleBuffer);
	FRDGBufferSRVRef ParticlesSRVLocal = GraphBuilder.CreateSRV(ParticleBuffer);
	FRDGBufferUAVRef PositionsUAVLocal = GraphBuilder.CreateUAV(PositionBuffer);
	FRDGBufferSRVRef PositionsSRVLocal = GraphBuilder.CreateSRV(PositionBuffer);

	// =====================================================
	// Phase 1.5: Bone Delta Attachment - Apply Bone Transform (SIMULATION START)
	// Moves attached particles to follow bone positions.
	// FinalizePositions will preserve this position for NEAR_BOUNDARY particles.
	// =====================================================
	FRDGBufferRef BoneDeltaAttachmentBufferRDG = nullptr;
	FRDGBufferSRVRef BoneDeltaAttachmentSRVLocal = nullptr;
	FRDGBufferUAVRef BoneDeltaAttachmentUAVLocal = nullptr;

	// WorldBoundaryParticles buffer (output of BoundarySkinningCS, unsorted)
	FRDGBufferRef WorldBoundaryParticlesBuffer = nullptr;
	FRDGBufferSRVRef WorldBoundaryParticlesSRVLocal = nullptr;
	int32 WorldBoundaryParticleCount = 0;

	if (BoundarySkinningManager.IsValid() && BoundarySkinningManager->IsGPUBoundarySkinningEnabled())
	{
		// Step 1: Run BoundarySkinningCS first to create WorldBoundaryParticles
		FGPUBoundarySkinningManager::FBoundarySkinningOutputs SkinningOutputs;
		BoundarySkinningManager->AddBoundarySkinningPass(
			GraphBuilder, WorldBoundaryParticlesBuffer, WorldBoundaryParticleCount, Params.DeltaTime,
			&SkinningOutputs);

		if (WorldBoundaryParticlesBuffer && WorldBoundaryParticleCount > 0)
		{
			WorldBoundaryParticlesSRVLocal = GraphBuilder.CreateSRV(WorldBoundaryParticlesBuffer);

			// Ensure BoneDeltaAttachment buffer exists
			BoneDeltaAttachmentBufferRDG = EnsureBoneDeltaAttachmentBuffer(GraphBuilder, CurrentParticleCount);
			BoneDeltaAttachmentSRVLocal = GraphBuilder.CreateSRV(BoneDeltaAttachmentBufferRDG);
			BoneDeltaAttachmentUAVLocal = GraphBuilder.CreateUAV(BoneDeltaAttachmentBufferRDG);

			// Step 2: Apply bone transform (SIMULATION START)
			// Sets VELOCITY for attached particles to follow bone movement naturally
			// No position teleport - physics will move particle via velocity
			// Only run if BoundaryAttachment is enabled
			if (Params.bEnableBoundaryAttachment && SkinningOutputs.LocalBoundaryParticlesBuffer && SkinningOutputs.BoneTransformsBuffer)
			{
				FRDGBufferSRVRef LocalBoundarySRV = GraphBuilder.CreateSRV(SkinningOutputs.LocalBoundaryParticlesBuffer);
				FRDGBufferSRVRef BoneTransformsSRV = GraphBuilder.CreateSRV(SkinningOutputs.BoneTransformsBuffer);

				AddApplyBoneTransformPass(
					GraphBuilder,
					ParticlesUAVLocal,
					BoneDeltaAttachmentSRVLocal,
					LocalBoundarySRV,
					WorldBoundaryParticleCount,
					BoneTransformsSRV,
					SkinningOutputs.BoneCount,
					SkinningOutputs.ComponentTransform,
					Params.DeltaTime);
			}

			// Debug log
			static int32 BoneAttachDebugCounter = 0;
			if (++BoneAttachDebugCounter % 60 == 0)
			{
				UE_LOG(LogGPUFluidSimulator, Log, TEXT("[BoneDeltaAttachment] ApplyBoneTransform: BoundaryCount=%d, BoneCount=%d"),
					WorldBoundaryParticleCount, SkinningOutputs.BoneCount);
			}
		}
	}

	// =====================================================
	// Phase 2: Build Spatial Structures (Predict -> Extract -> Sort -> Hash)
	// Also reorders BoneDeltaAttachment buffer to stay synchronized with particles after Z-Order sorting
	// =====================================================
	FSimulationSpatialData SpatialData = BuildSpatialStructures(
		GraphBuilder,
		ParticleBuffer,
		ParticlesSRVLocal,
		ParticlesUAVLocal,
		PositionsSRVLocal,
		PositionsUAVLocal,
		Params,
		BoneDeltaAttachmentBufferRDG ? &BoneDeltaAttachmentBufferRDG : nullptr);

	// If we pre-computed WorldBoundaryParticles in Phase 1.5, store it in SpatialData
	// This ensures the data is available for density calculations and other passes
	// Note: BuildSpatialStructures may have also run BoundarySkinningPass,
	// but we prefer the Phase 1.5 result as it was used for ApplyBoneTransform
	if (WorldBoundaryParticlesBuffer && WorldBoundaryParticleCount > 0)
	{
		SpatialData.WorldBoundaryBuffer = WorldBoundaryParticlesBuffer;
		SpatialData.WorldBoundarySRV = WorldBoundaryParticlesSRVLocal;
		SpatialData.WorldBoundaryParticleCount = WorldBoundaryParticleCount;
		SpatialData.bBoundarySkinningPerformed = true;
	}

	// Update attachment SRV/UAV refs after sorting (buffer was replaced with sorted version)
	if (BoneDeltaAttachmentBufferRDG)
	{
		BoneDeltaAttachmentSRVLocal = GraphBuilder.CreateSRV(BoneDeltaAttachmentBufferRDG);
		BoneDeltaAttachmentUAVLocal = GraphBuilder.CreateUAV(BoneDeltaAttachmentBufferRDG);
	}

	// =====================================================
	// Phase 3: Constraint Solver Loop (Density/Pressure + Collision per iteration)
	// XPBD Principle: Collision is solved inside solver loop to prevent jittering
	// =====================================================
	ExecuteConstraintSolverLoop(GraphBuilder, ParticlesUAVLocal, SpatialData, Params);

	// =====================================================
	// Phase 4: Adhesion (Bone attachment - runs after constraint solving)
	// =====================================================
	ExecuteAdhesion(GraphBuilder, ParticlesUAVLocal, SpatialData, Params);

	// =====================================================
	// Phase 5: Post-Simulation (Viscosity, Finalize, Anisotropy)
	// =====================================================
	ExecutePostSimulation(GraphBuilder, ParticleBuffer, ParticlesUAVLocal, SpatialData, Params);

	// =====================================================
	// Phase 5.5: Bone Delta Attachment - Update Attachment Data (SIMULATION END)
	// After physics simulation: detach if moved too far, find new attachment.
	// Stores OriginalIndex for stable attachment across Z-Order sorting.
	// =====================================================
	if (BoneDeltaAttachmentUAVLocal != nullptr && WorldBoundaryParticleCount > 0)
	{
		// Get Z-Order sorted boundary data (prefer skinned, fallback to static)
		FRDGBufferSRVRef BoundarySRV = nullptr;
		FRDGBufferSRVRef CellStartSRV = nullptr;
		FRDGBufferSRVRef CellEndSRV = nullptr;
		int32 BoundaryCount = 0;

		if (SpatialData.bSkinnedZOrderPerformed && SpatialData.SkinnedZOrderSortedSRV)
		{
			BoundarySRV = SpatialData.SkinnedZOrderSortedSRV;
			CellStartSRV = SpatialData.SkinnedZOrderCellStartSRV;
			CellEndSRV = SpatialData.SkinnedZOrderCellEndSRV;
			BoundaryCount = SpatialData.SkinnedZOrderParticleCount;
		}
		else if (SpatialData.bStaticBoundaryAvailable && SpatialData.StaticZOrderSortedSRV)
		{
			BoundarySRV = SpatialData.StaticZOrderSortedSRV;
			CellStartSRV = SpatialData.StaticZOrderCellStartSRV;
			CellEndSRV = SpatialData.StaticZOrderCellEndSRV;
			BoundaryCount = SpatialData.StaticBoundaryParticleCount;
		}

		// Only run if BoundaryAttachment is enabled
		if (Params.bEnableBoundaryAttachment && BoundarySRV && CellStartSRV && CellEndSRV && BoundaryCount > 0 && WorldBoundaryParticlesSRVLocal)
		{
			AddUpdateBoneDeltaAttachmentPass(
				GraphBuilder,
				ParticlesUAVLocal,
				BoneDeltaAttachmentUAVLocal,
				BoundarySRV,
				CellStartSRV,
				CellEndSRV,
				BoundaryCount,
				WorldBoundaryParticlesSRVLocal,  // Unsorted, for LocalOffset calculation
				WorldBoundaryParticleCount,
				Params);

			// Debug log
			static int32 UpdateAttachDebugCounter = 0;
			if (++UpdateAttachDebugCounter % 60 == 0)
			{
				UE_LOG(LogGPUFluidSimulator, Log, TEXT("[BoneDeltaAttachment] UpdatePass: BoundaryCount=%d, WorldBoundaryCount=%d, AttachRadius=%.1f"),
					BoundaryCount, WorldBoundaryParticleCount, Params.SmoothingRadius);
			}
		}
		else
		{
			// Debug: why is UpdatePass being skipped?
			static int32 SkipDebugCounter = 0;
			if (++SkipDebugCounter % 60 == 0)
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[BoneDeltaAttachment] UpdatePass SKIPPED: BoundarySRV=%d, CellStartSRV=%d, BoundaryCount=%d"),
					BoundarySRV != nullptr, CellStartSRV != nullptr, BoundaryCount);
			}
		}

		// Store buffer reference for extraction
		SpatialData.BoneDeltaAttachmentBuffer = BoneDeltaAttachmentBufferRDG;
		SpatialData.BoneDeltaAttachmentUAV = BoneDeltaAttachmentUAVLocal;
		SpatialData.BoneDeltaAttachmentSRV = BoneDeltaAttachmentSRVLocal;
	}

	// =====================================================
	// Phase 6: Extract Persistent Buffers
	// =====================================================
	ExtractPersistentBuffers(GraphBuilder, ParticleBuffer, SpatialData);
}

//=============================================================================
// Frame Lifecycle Functions
//=============================================================================

void FGPUFluidSimulator::BeginFrame()
{
	if (!bIsInitialized)
	{
		return;
	}

	if (bFrameActive)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("BeginFrame called while frame already active! Call EndFrame first."));
		return;
	}

	bFrameActive = true;
	FGPUFluidSimulator* Self = this;

	// Capture pending flags on game thread (before render command)
	const bool bHasPendingSpawns = SpawnManager.IsValid() && SpawnManager->HasPendingSpawnRequests();
	const bool bHasPendingDespawns = SpawnManager.IsValid() && SpawnManager->HasPendingDespawnByIDRequests();

	// Single render command for all BeginFrame operations
	ENQUEUE_RENDER_COMMAND(GPUFluidBeginFrame)(
		[Self, bHasPendingSpawns, bHasPendingDespawns](FRHICommandListImmediate& RHICmdList)
		{
			SCOPED_DRAW_EVENT(RHICmdList, GPUFluid_BeginFrame);

			// =====================================================
			// Step 1: Process Readbacks (from previous frame)
			// =====================================================
			Self->ProcessCollisionFeedbackReadback(RHICmdList);
			Self->ProcessColliderContactCountReadback(RHICmdList);

			// Process stats readback - also extracts shadow data if bShadowReadbackEnabled
			const bool bNeedStatsReadback = GetFluidStatsCollector().IsAnyReadbackNeeded();
			const bool bNeedShadowReadback = Self->bShadowReadbackEnabled.load();
			if (bNeedStatsReadback || bNeedShadowReadback)
			{
				SCOPED_DRAW_EVENT(RHICmdList, GPUFluid_BeginFrame_ProcessStatsReadback);
				Self->ProcessStatsReadback(RHICmdList);
			}

			// Anisotropy readback is separate (different GPU buffer)
			if (Self->bAnisotropyReadbackEnabled.load())
			{
				SCOPED_DRAW_EVENT(RHICmdList, GPUFluid_BeginFrame_ANISO);
				Self->ProcessAnisotropyReadback();
			}

			if (Self->SpawnManager.IsValid())
			{
				SCOPED_DRAW_EVENT(RHICmdList, GPUFluid_BeginFrame_SOURCECOUNT);
				Self->SpawnManager->ProcessSourceCounterReadback();
			}

			// =====================================================
			// Step 2: Process Spawn/Despawn Operations
			// =====================================================
			if (!bHasPendingSpawns && !bHasPendingDespawns)
			{
				return;  // No spawn/despawn to process
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef ParticleBuffer = nullptr;
			int32 PostOpCount = Self->CurrentParticleCount;

			// Swap buffers first
			if (Self->SpawnManager.IsValid())
			{
				Self->SpawnManager->SwapBuffers();
			}

			// Process Despawn (ID-based removal)
			if (bHasPendingDespawns && Self->SpawnManager.IsValid() && Self->PersistentParticleBuffer.IsValid())
			{
				SCOPED_DRAW_EVENT(RHICmdList, GPUFluid_BeginFrame_Despawn);

				int32 DespawnCount = 0;
				{
					RDG_EVENT_SCOPE(GraphBuilder, "Despawn_SwapBuffers");
					DespawnCount = Self->SpawnManager->SwapDespawnByIDBuffers();
				}
				if (DespawnCount > 0)
				{
					{
						RDG_EVENT_SCOPE(GraphBuilder, "Despawn_RegisterBuffer");
						ParticleBuffer = GraphBuilder.RegisterExternalBuffer(Self->PersistentParticleBuffer, TEXT("GPUFluidParticlesForDespawn"));
					}
					{
						RDG_EVENT_SCOPE(GraphBuilder, "Despawn_AddPass");
						Self->SpawnManager->AddDespawnByIDPass(GraphBuilder, ParticleBuffer, PostOpCount);
					}
					PostOpCount = FMath::Max(0, PostOpCount - DespawnCount);
				}
			}

			// Process Spawn
			const int32 SpawnCount = Self->SpawnManager.IsValid() ? Self->SpawnManager->GetActiveRequestCount() : 0;
			if (SpawnCount > 0)
			{
				SCOPED_DRAW_EVENT(RHICmdList, GPUFluid_BeginFrame_Spawn);

				const bool bFirstSpawn = (PostOpCount == 0) && !ParticleBuffer;

				if (bFirstSpawn)
				{
					// PATH 1: First spawn - create new buffer
					const int32 BufferCapacity = FMath::Min(SpawnCount, Self->MaxParticleCount);
					FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
					ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

					TArray<uint32> InitialCounterData;
					InitialCounterData.Add(0);
					FRDGBufferRef CounterBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("GPUFluidParticleCounter"), sizeof(uint32), 1, InitialCounterData.GetData(), sizeof(uint32), ERDGInitialDataFlags::None);
					FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(CounterBuffer);
					FRDGBufferUAVRef ParticleUAVForSpawn = GraphBuilder.CreateUAV(ParticleBuffer);

					Self->SpawnManager->AddSpawnParticlesPass(GraphBuilder, ParticleUAVForSpawn, CounterUAV, Self->MaxParticleCount);
					PostOpCount = FMath::Min(SpawnCount, Self->MaxParticleCount);
					Self->SpawnManager->OnSpawnComplete(PostOpCount);
				}
				else
				{
					// PATH 2: Append spawn - copy existing + spawn new
					const int32 TotalCount = PostOpCount + SpawnCount;
					const int32 BufferCapacity = FMath::Min(TotalCount, Self->MaxParticleCount);
					FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
					FRDGBufferRef NewParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

					FRDGBufferRef ExistingBuffer = ParticleBuffer ? ParticleBuffer :
						GraphBuilder.RegisterExternalBuffer(Self->PersistentParticleBuffer, TEXT("GPUFluidParticlesOld"));

					if (PostOpCount > 0)
					{
						FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
						TShaderMapRef<FCopyParticlesCS> CopyShader(ShaderMap);
						FCopyParticlesCS::FParameters* CopyParams = GraphBuilder.AllocParameters<FCopyParticlesCS::FParameters>();
						CopyParams->SourceParticles = GraphBuilder.CreateSRV(ExistingBuffer);
						CopyParams->DestParticles = GraphBuilder.CreateUAV(NewParticleBuffer);
						CopyParams->SourceOffset = 0;
						CopyParams->DestOffset = 0;
						CopyParams->CopyCount = PostOpCount;
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPUFluid::CopyForSpawn(%d)", PostOpCount),
							CopyShader, CopyParams, FIntVector(FMath::DivideAndRoundUp(PostOpCount, FCopyParticlesCS::ThreadGroupSize), 1, 1));
					}

					TArray<uint32> InitialCounterData;
					InitialCounterData.Add(static_cast<uint32>(PostOpCount));
					FRDGBufferRef CounterBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("GPUFluidParticleCounter"), sizeof(uint32), 1, InitialCounterData.GetData(), sizeof(uint32), ERDGInitialDataFlags::None);
					FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(CounterBuffer);
					FRDGBufferUAVRef ParticleUAVForSpawn = GraphBuilder.CreateUAV(NewParticleBuffer);

					Self->SpawnManager->AddSpawnParticlesPass(GraphBuilder, ParticleUAVForSpawn, CounterUAV, Self->MaxParticleCount);
					PostOpCount = FMath::Min(TotalCount, Self->MaxParticleCount);
					Self->SpawnManager->OnSpawnComplete(SpawnCount);
					ParticleBuffer = NewParticleBuffer;
				}

			}

			// Clear active requests
			if (Self->SpawnManager.IsValid())
			{
				Self->SpawnManager->ClearActiveRequests();
			}

			// Extract to persistent buffer
			if (ParticleBuffer)
			{
				GraphBuilder.QueueBufferExtraction(ParticleBuffer, &Self->PersistentParticleBuffer, ERHIAccess::UAVCompute);
			}

			// Update counts
			Self->CurrentParticleCount = PostOpCount;
			Self->PreviousParticleCount = PostOpCount;
			Self->bNeedsFullUpload = false;

			GraphBuilder.Execute();
		}
	);
}

void FGPUFluidSimulator::EndFrame()
{
	if (!bIsInitialized)
	{
		return;
	}

	if (!bFrameActive)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("EndFrame called without BeginFrame!"));
		return;
	}

	FGPUFluidSimulator* Self = this;

	ENQUEUE_RENDER_COMMAND(GPUFluidEndFrame)(
		[Self](FRHICommandListImmediate& RHICmdList)
		{
			SCOPED_DRAW_EVENT(RHICmdList, GPUFluid_EndFrame);

			// Cleanup despawn tracking when particles become 0
			// This allows reusing the same IDs immediately after a full clear
			if (Self->CurrentParticleCount == 0 && Self->SpawnManager.IsValid())
			{
				Self->SpawnManager->CleanupCompletedRequests(TArray<int32>());
			}

			// Reset NextParticleID when particle count is 0 (prevents overflow)
			if (Self->CurrentParticleCount == 0 && Self->SpawnManager.IsValid())
			{
				Self->SpawnManager->TryResetParticleID(Self->CurrentParticleCount);
			}

			// Enqueue source counter readback
			if (Self->SpawnManager.IsValid() && Self->SpawnManager->GetSourceCounterBuffer().IsValid())
			{
				Self->SpawnManager->EnqueueSourceCounterReadback(RHICmdList);
			}

			// Only enqueue readbacks if we have particles
			if (Self->CurrentParticleCount > 0 && Self->PersistentParticleBuffer.IsValid())
			{
				// Stats Readback after ParticleID sorting (RemoveOldestParticles O(1) optimization)
				{
					FRDGBuilder GraphBuilder(RHICmdList);

					// Register PersistentParticleBuffer in RDG
					FRDGBufferRef ParticleBuffer = GraphBuilder.RegisterExternalBuffer(Self->PersistentParticleBuffer);

					// Sort by ParticleID
					FRDGBufferRef SortedBuffer = Self->ExecuteParticleIDSortPipeline(
						GraphBuilder, ParticleBuffer, Self->CurrentParticleCount);

					// Add readback pass (uses AddReadbackBufferPass - automatic buffer dependency handling)
					const int32 ParticleCount = Self->CurrentParticleCount;
					AddReadbackBufferPass(GraphBuilder,
						RDG_EVENT_NAME("GPUFluid::ParticleIDSortedReadback"),
						SortedBuffer,
						[Self, SortedBuffer, ParticleCount](FRHICommandListImmediate& InRHICmdList)
						{
							Self->EnqueueStatsReadback(InRHICmdList, SortedBuffer->GetRHI(), ParticleCount);
						});

					GraphBuilder.Execute();
				}

				// Collision feedback readback
				if (Self->CollisionManager.IsValid() && Self->CollisionManager->GetFeedbackManager())
				{
					Self->CollisionManager->GetFeedbackManager()->EnqueueReadbackCopy(RHICmdList);
				}

				// Anisotropy readback (Shadow data is now extracted in ProcessStatsReadback)
				if (Self->bShadowReadbackEnabled.load() && Self->bAnisotropyReadbackEnabled.load())
				{
					Self->EnqueueAnisotropyReadback(RHICmdList, Self->CurrentParticleCount);
				}
			}

			Self->bHasValidGPUResults.store(true);

			// =====================================================
			// Swap Neighbor Cache Buffers for Cohesion Double Buffering
			// MUST be called after RDG execution completes (buffers are extracted)
			// This moves current frame's neighbor cache to PrevNeighborCache
			// so PredictPositions (Phase 2) can use it next frame for Cohesion Force
			// =====================================================
			Self->SwapNeighborCacheBuffers();
		}
	);

	bFrameActive = false;
}

FRDGBufferRef FGPUFluidSimulator::PrepareParticleBuffer(
	FRDGBuilder& GraphBuilder,
	const FGPUFluidSimulationParams& Params)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid_PrepareParticleBuffer");

	// Spawn/Despawn are handled in BeginFrame
	// This function only handles:
	// - PATH 1: CPU Upload (PIE transfer, save/load)
	// - PATH 2: Reuse PersistentParticleBuffer

	FRDGBufferRef ParticleBuffer = nullptr;

	static int32 DebugFrameCounter = 0;
	const bool bShouldLog = (DebugFrameCounter++ < 10);

	// =====================================================
	// DEBUG: Trigger RenderDoc capture
	// =====================================================
	{
		bool bShouldCapture = false;
		int32 TargetFrame = 0;

		if (GFluidCaptureFrameNumber > 0)
		{
			TargetFrame = GFluidCaptureFrameNumber;
			bShouldCapture = (DebugFrameCounter == TargetFrame && GFluidCapturedFrame != TargetFrame);
		}
		else if (GFluidCaptureFirstFrame != 0)
		{
			TargetFrame = 1;
			bShouldCapture = (DebugFrameCounter == 1 && GFluidCapturedFrame == 0);
		}

		if (bShouldCapture)
		{
			GFluidCapturedFrame = TargetFrame;
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT(">>> TRIGGERING RENDERDOC CAPTURE ON GPU SIMULATION FRAME %d <<<"), DebugFrameCounter);

			AsyncTask(ENamedThreads::GameThread, []()
			{
				IConsoleObject* CaptureCmd = IConsoleManager::Get().FindConsoleObject(TEXT("renderdoc.CaptureFrame"));
				if (CaptureCmd)
				{
					IConsoleCommand* Cmd = CaptureCmd->AsCommand();
					if (Cmd) Cmd->Execute(TArray<FString>(), nullptr, *GLog);
				}
			});
		}
	}

	// Render thread log disabled for performance
	// if (bShouldLog)
	// {
	// 	UE_LOG(LogGPUFluidSimulator, Log, TEXT("=== PrepareParticleBuffer (Frame %d) ==="), DebugFrameCounter);
	// 	UE_LOG(LogGPUFluidSimulator, Log, TEXT("  CurrentParticleCount: %d, bNeedsFullUpload: %s"),
	// 		CurrentParticleCount, bNeedsFullUpload ? TEXT("TRUE") : TEXT("FALSE"));
	// }

	// =====================================================
	// PATH 1: CPU Upload (Upload CachedGPUParticles to GPU)
	// Used for PIE transfer and save/load - particles synced from CPU array
	// =====================================================
	if (CachedGPUParticles.Num() > 0 && bNeedsFullUpload)
	{
		const int32 UploadCount = CachedGPUParticles.Num();
		const int32 BufferCapacity = FMath::Min(UploadCount, MaxParticleCount);
		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		// Upload CPU particles to GPU buffer
		GraphBuilder.QueueBufferUpload(
			ParticleBuffer,
			CachedGPUParticles.GetData(),
			BufferCapacity * sizeof(FGPUFluidParticle));

		CurrentParticleCount = BufferCapacity;
		PreviousParticleCount = CurrentParticleCount;
		bNeedsFullUpload = false;

		// UE_LOG(LogGPUFluidSimulator, Log, TEXT("PATH 1 (CPU Upload): Uploaded %d particles from CPU to GPU"), BufferCapacity);
	}
	// =====================================================
	// PATH 2: Reuse PersistentParticleBuffer
	// Normal simulation - just register the existing buffer
	// =====================================================
	else if (PersistentParticleBuffer.IsValid())
	{
		ParticleBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleBuffer, TEXT("GPUFluidParticles"));
	}
	else
	{
		return nullptr;
	}

	return ParticleBuffer;
}

FGPUFluidSimulator::FSimulationSpatialData FGPUFluidSimulator::BuildSpatialStructures(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef& InOutParticleBuffer,
	FRDGBufferSRVRef& OutParticlesSRV,
	FRDGBufferUAVRef& OutParticlesUAV,
	FRDGBufferSRVRef& OutPositionsSRV,
	FRDGBufferUAVRef& OutPositionsUAV,
	const FGPUFluidSimulationParams& Params,
	FRDGBufferRef* InOutAttachmentBuffer)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid_BuildSpatialStructures");

	FSimulationSpatialData SpatialData;

	// Skip when no particles (avoid unnecessary GPU work)
	if (CurrentParticleCount == 0)
	{
		return SpatialData;
	}

	// Pass 1: Predict Positions
	AddPredictPositionsPass(GraphBuilder, OutParticlesUAV, Params);

	// Pass 2: Extract positions (Initial)
	AddExtractPositionsPass(GraphBuilder, OutParticlesSRV, OutPositionsUAV, CurrentParticleCount, true);

	// Pass 3: Spatial Data Structure
	// Check both manager validity AND enabled flag for Z-Order sorting
	const bool bUseZOrderSorting = ZOrderSortManager.IsValid() && ZOrderSortManager->IsZOrderSortingEnabled();
	if (bUseZOrderSorting)
	{
		// Z-Order Sorting
		FRDGBufferUAVRef CellStartUAVLocal = nullptr;
		FRDGBufferUAVRef CellEndUAVLocal = nullptr;

		// Pass attachment buffer for reordering if provided
		FRDGBufferRef InAttachment = (InOutAttachmentBuffer && *InOutAttachmentBuffer) ? *InOutAttachmentBuffer : nullptr;
		FRDGBufferRef SortedAttachment = nullptr;

		FRDGBufferRef SortedParticleBuffer = ExecuteZOrderSortingPipeline(
			GraphBuilder, InOutParticleBuffer,
			CellStartUAVLocal, SpatialData.CellStartSRV,
			CellEndUAVLocal, SpatialData.CellEndSRV,
			SpatialData.CellStartBuffer, SpatialData.CellEndBuffer,
			Params,
			InAttachment,
			InAttachment ? &SortedAttachment : nullptr);

		// Replace attachment buffer with sorted version if provided
		if (InOutAttachmentBuffer && SortedAttachment)
		{
			*InOutAttachmentBuffer = SortedAttachment;
		}

		// Replace particle buffer with sorted version
		InOutParticleBuffer = SortedParticleBuffer;
		OutParticlesUAV = GraphBuilder.CreateUAV(InOutParticleBuffer);
		OutParticlesSRV = GraphBuilder.CreateSRV(InOutParticleBuffer);

		// Create dummy buffers for shader binding (legacy CellCounts/ParticleIndices are not used when Z-Order is enabled)
		// The shader requires valid SRVs even if bUseZOrderSorting=1
		if (!PersistentCellCountsBuffer.IsValid())
		{
			// Create minimal dummy buffers (1 element each) - not actually used
			// Must use QueueBufferUpload so RDG marks them as "produced"
			// Otherwise RDG validation fails: "has a read dependency but was never written to"
			static uint32 ZeroData = 0;
			SpatialData.CellCountsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("SpatialHash.CellCounts.Dummy"));
			GraphBuilder.QueueBufferUpload(SpatialData.CellCountsBuffer, &ZeroData, sizeof(uint32));
			SpatialData.ParticleIndicesBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("SpatialHash.ParticleIndices.Dummy"));
			GraphBuilder.QueueBufferUpload(SpatialData.ParticleIndicesBuffer, &ZeroData, sizeof(uint32));
		}
		else
		{
			SpatialData.CellCountsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentCellCountsBuffer, TEXT("SpatialHash.CellCounts"));
			SpatialData.ParticleIndicesBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleIndicesBuffer, TEXT("SpatialHash.ParticleIndices"));
		}

		SpatialData.CellCountsSRV = GraphBuilder.CreateSRV(SpatialData.CellCountsBuffer);
		SpatialData.ParticleIndicesSRV = GraphBuilder.CreateSRV(SpatialData.ParticleIndicesBuffer);
		// NOTE: GPUClear and GPUBuild passes removed - not needed when Z-Order sorting is enabled
	}
	else
	{
		// LEGACY: Traditional Hash Table (Fallback when ZOrderSortManager is not available)
		if (!PersistentCellCountsBuffer.IsValid())
		{
			SpatialData.CellCountsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_SPATIAL_HASH_SIZE), TEXT("SpatialHash.CellCounts"));
			SpatialData.ParticleIndicesBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_SPATIAL_HASH_SIZE * GPU_MAX_PARTICLES_PER_CELL), TEXT("SpatialHash.ParticleIndices"));
		}
		else
		{
			SpatialData.CellCountsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentCellCountsBuffer, TEXT("SpatialHash.CellCounts"));
			SpatialData.ParticleIndicesBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleIndicesBuffer, TEXT("SpatialHash.ParticleIndices"));
		}

		SpatialData.CellCountsSRV = GraphBuilder.CreateSRV(SpatialData.CellCountsBuffer);
		SpatialData.ParticleIndicesSRV = GraphBuilder.CreateSRV(SpatialData.ParticleIndicesBuffer);
		FRDGBufferUAVRef CellCountsUAVLocal = GraphBuilder.CreateUAV(SpatialData.CellCountsBuffer);
		FRDGBufferUAVRef ParticleIndicesUAVLocal = GraphBuilder.CreateUAV(SpatialData.ParticleIndicesBuffer);

		// GPU Clear
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FClearCellDataCS> ClearShader(ShaderMap);
			FClearCellDataCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellDataCS::FParameters>();
			ClearParams->CellCounts = CellCountsUAVLocal;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SpatialHash::GPUClear"), ClearShader, ClearParams, FIntVector(FMath::DivideAndRoundUp<uint32>(GPU_SPATIAL_HASH_SIZE, SPATIAL_HASH_THREAD_GROUP_SIZE), 1, 1));
		}

		// GPU Build
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FBuildSpatialHashSimpleCS> BuildShader(ShaderMap);
			FBuildSpatialHashSimpleCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FBuildSpatialHashSimpleCS::FParameters>();
			BuildParams->ParticlePositions = OutPositionsSRV;
			BuildParams->ParticleCount = CurrentParticleCount;
			BuildParams->ParticleRadius = Params.ParticleRadius;
			BuildParams->CellSize = Params.CellSize;
			BuildParams->CellCounts = CellCountsUAVLocal;
			BuildParams->ParticleIndices = ParticleIndicesUAVLocal;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SpatialHash::GPUBuild"), BuildShader, BuildParams, FIntVector(FMath::DivideAndRoundUp<uint32>(CurrentParticleCount, SPATIAL_HASH_THREAD_GROUP_SIZE), 1, 1));
		}

		// Dummy CellStart/End for validation
		FRDGBufferRef DummyCellStartBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("DummyCellStart"));
		FRDGBufferRef DummyCellEndBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("DummyCellEnd"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyCellStartBuffer, &InvalidIndex, sizeof(uint32));
		GraphBuilder.QueueBufferUpload(DummyCellEndBuffer, &InvalidIndex, sizeof(uint32));
		SpatialData.CellStartSRV = GraphBuilder.CreateSRV(DummyCellStartBuffer);
		SpatialData.CellEndSRV = GraphBuilder.CreateSRV(DummyCellEndBuffer);
	}

	// Pass 3.5: GPU Boundary Skinning (SkeletalMesh - same-frame)


	if (IsGPUBoundarySkinningEnabled())
	{

		AddBoundarySkinningPass(GraphBuilder, SpatialData, Params);
		
	}

	// Pass 3.6: Skinned Boundary Z-Order Sorting (after skinning)
	// CRITICAL: Set bounds to match fluid simulation bounds for correct cell ID calculation
	if (BoundarySkinningManager.IsValid() && ZOrderSortManager.IsValid())
	{
		BoundarySkinningManager->SetBoundaryZOrderConfig(
			ZOrderSortManager->GetGridResolutionPreset(),
			SimulationBoundsMin,
			SimulationBoundsMax);
	}

	if (BoundarySkinningManager.IsValid() && BoundarySkinningManager->IsBoundaryZOrderEnabled()
		&& SpatialData.bSkinnedBoundaryPerformed)
	{
		// Pass same-frame buffer, get Z-Order buffers for same-frame use
		SpatialData.bSkinnedZOrderPerformed = BoundarySkinningManager->ExecuteBoundaryZOrderSort(
			GraphBuilder, Params,
			SpatialData.SkinnedBoundaryBuffer,
			SpatialData.SkinnedBoundaryParticleCount,
			SpatialData.SkinnedZOrderSortedBuffer,
			SpatialData.SkinnedZOrderCellStartBuffer,
			SpatialData.SkinnedZOrderCellEndBuffer,
			SpatialData.SkinnedZOrderParticleCount);

		// Create SRVs for same-frame access
		if (SpatialData.bSkinnedZOrderPerformed)
		{
			SpatialData.SkinnedZOrderSortedSRV = GraphBuilder.CreateSRV(SpatialData.SkinnedZOrderSortedBuffer);
			SpatialData.SkinnedZOrderCellStartSRV = GraphBuilder.CreateSRV(SpatialData.SkinnedZOrderCellStartBuffer);
			SpatialData.SkinnedZOrderCellEndSRV = GraphBuilder.CreateSRV(SpatialData.SkinnedZOrderCellEndBuffer);
		}
	}

	// Pass 3.7: Static Boundary Z-Order Sorting (StaticMesh - persistent GPU, sorted once)
	if (BoundarySkinningManager.IsValid() && BoundarySkinningManager->IsStaticBoundaryEnabled())
	{
		// Execute Z-Order sort for static boundary (only if dirty)
		BoundarySkinningManager->ExecuteStaticBoundaryZOrderSort(GraphBuilder, Params);

		// Register persistent static buffers if available
		if (BoundarySkinningManager->HasStaticZOrderData())
		{
			FRDGBufferRef StaticBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(
				BoundarySkinningManager->GetStaticBoundaryBuffer(),
				TEXT("GPUFluid.StaticBoundaryParticles"));
			FRDGBufferRef StaticSortedBuffer = GraphBuilder.RegisterExternalBuffer(
				BoundarySkinningManager->GetStaticZOrderSortedBuffer(),
				TEXT("GPUFluid.StaticSortedBoundary"));
			FRDGBufferRef StaticCellStartBuffer = GraphBuilder.RegisterExternalBuffer(
				BoundarySkinningManager->GetStaticCellStartBuffer(),
				TEXT("GPUFluid.StaticCellStart"));
			FRDGBufferRef StaticCellEndBuffer = GraphBuilder.RegisterExternalBuffer(
				BoundarySkinningManager->GetStaticCellEndBuffer(),
				TEXT("GPUFluid.StaticCellEnd"));

			SpatialData.StaticBoundarySRV = GraphBuilder.CreateSRV(StaticBoundaryBuffer);
			SpatialData.StaticZOrderSortedSRV = GraphBuilder.CreateSRV(StaticSortedBuffer);
			SpatialData.StaticZOrderCellStartSRV = GraphBuilder.CreateSRV(StaticCellStartBuffer);
			SpatialData.StaticZOrderCellEndSRV = GraphBuilder.CreateSRV(StaticCellEndBuffer);
			SpatialData.StaticBoundaryParticleCount = BoundarySkinningManager->GetStaticBoundaryParticleCount();
			SpatialData.bStaticBoundaryAvailable = true;
		}
	}

	return SpatialData;
}

void FGPUFluidSimulator::ExecuteConstraintSolverLoop(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FSimulationSpatialData& SpatialData,
	const FGPUFluidSimulationParams& Params)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid_ConstraintSolverLoop");

	// Skip constraint solver when no particles (avoid 0-size buffer creation)
	if (CurrentParticleCount == 0)
	{
		return;
	}

	// Create/resize neighbor caching buffers
	const int32 NeighborListSize = CurrentParticleCount * GPU_MAX_NEIGHBORS_PER_PARTICLE;

	if (NeighborBufferParticleCapacity < CurrentParticleCount || !NeighborListBuffer.IsValid())
	{
		SpatialData.NeighborListBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NeighborListSize), TEXT("GPUFluidNeighborList"));
		SpatialData.NeighborCountsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CurrentParticleCount), TEXT("GPUFluidNeighborCounts"));
		NeighborBufferParticleCapacity = CurrentParticleCount;
	}
	else
	{
		SpatialData.NeighborListBuffer = GraphBuilder.RegisterExternalBuffer(NeighborListBuffer, TEXT("GPUFluidNeighborList"));
		SpatialData.NeighborCountsBuffer = GraphBuilder.RegisterExternalBuffer(NeighborCountsBuffer, TEXT("GPUFluidNeighborCounts"));
	}

	FRDGBufferUAVRef NeighborListUAVLocal = GraphBuilder.CreateUAV(SpatialData.NeighborListBuffer);
	FRDGBufferUAVRef NeighborCountsUAVLocal = GraphBuilder.CreateUAV(SpatialData.NeighborCountsBuffer);

	// =====================================================
	// XPBD Constraint Solver Loop
	// Principle 2: "Collision is the strongest constraint"
	// Density and Collision constraints are solved together per iteration
	// to ensure proper convergence and prevent jittering.
	// =====================================================
	for (int32 i = 0; i < Params.SolverIterations; ++i)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "SolverIteration_%d", i);

		// Step 1: Density/Pressure Constraint (PBF)
		// Pushes particles apart when density > rest density
		AddSolveDensityPressurePass(
			GraphBuilder, ParticlesUAV,
			SpatialData.CellCountsSRV, SpatialData.ParticleIndicesSRV,
			SpatialData.CellStartSRV, SpatialData.CellEndSRV,
			NeighborListUAVLocal, NeighborCountsUAVLocal, i, Params, SpatialData);

		// Step 2: Collision Constraints (MUST be inside solver loop!)
		// If density pushes a particle through a wall, collision pushes it back.
		// Solving both constraints together allows convergence to a valid state.
		AddBoundsCollisionPass(GraphBuilder, ParticlesUAV, Params);
		AddPrimitiveCollisionPass(GraphBuilder, ParticlesUAV, Params);
		AddHeightmapCollisionPass(GraphBuilder, ParticlesUAV, Params);
	}

	// Create SRVs for use in subsequent passes
	SpatialData.NeighborListSRV = GraphBuilder.CreateSRV(SpatialData.NeighborListBuffer);
	SpatialData.NeighborCountsSRV = GraphBuilder.CreateSRV(SpatialData.NeighborCountsBuffer);

	// Queue extraction
	GraphBuilder.QueueBufferExtraction(SpatialData.NeighborListBuffer, &NeighborListBuffer, ERHIAccess::UAVCompute);
	GraphBuilder.QueueBufferExtraction(SpatialData.NeighborCountsBuffer, &NeighborCountsBuffer, ERHIAccess::UAVCompute);
}

void FGPUFluidSimulator::ExecuteAdhesion(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FSimulationSpatialData& SpatialData,
	const FGPUFluidSimulationParams& Params)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid_Adhesion");

	// Skip when no particles
	if (CurrentParticleCount == 0)
	{
		return;
	}

	// =====================================================
	// XPBD Pipeline Change: Collision passes moved to ExecuteConstraintSolverLoop
	// This function now only handles Bone-based Adhesion (attachment tracking)
	// Collision passes are now inside the solver loop for proper constraint convergence.
	// =====================================================

	if (AdhesionManager.IsValid() && AdhesionManager->IsAdhesionEnabled() && CollisionManager.IsValid() && CollisionManager->AreBoneTransformsValid())
	{
		TRefCountPtr<FRDGPooledBuffer>& PersistentAttachmentBuffer = AdhesionManager->AccessPersistentAttachmentBuffer();
		int32 AttachmentBufferSize = AdhesionManager->GetAttachmentBufferSize();
		const bool bNeedNewBuffer = !PersistentAttachmentBuffer.IsValid() || AttachmentBufferSize < CurrentParticleCount;

		FRDGBufferRef AttachmentBuffer;
		if (bNeedNewBuffer)
		{
			TArray<FGPUParticleAttachment> InitialAttachments;
			InitialAttachments.SetNum(CurrentParticleCount);
			for (int32 i = 0; i < CurrentParticleCount; ++i)
			{
				FGPUParticleAttachment& Attachment = InitialAttachments[i];
				Attachment.PrimitiveType = -1;
				Attachment.PrimitiveIndex = -1;
				Attachment.BoneIndex = -1;
				Attachment.AdhesionStrength = 0.0f;
				Attachment.LocalOffset = FVector3f::ZeroVector;
				Attachment.AttachmentTime = 0.0f;
				Attachment.RelativeVelocity = FVector3f::ZeroVector;
				Attachment.Padding = 0.0f;
			}

			AttachmentBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("GPUFluidAttachments"), sizeof(FGPUParticleAttachment), CurrentParticleCount, InitialAttachments.GetData(), CurrentParticleCount * sizeof(FGPUParticleAttachment), ERDGInitialDataFlags::None);

			if (PersistentAttachmentBuffer.IsValid() && AttachmentBufferSize > 0)
			{
				FRDGBufferRef OldBuffer = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsOld"));
				AddCopyBufferPass(GraphBuilder, AttachmentBuffer, 0, OldBuffer, 0, AttachmentBufferSize * sizeof(FGPUParticleAttachment));
			}
			AdhesionManager->SetAttachmentBufferSize(CurrentParticleCount);
		}
		else
		{
			AttachmentBuffer = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachments"));
		}

		FRDGBufferUAVRef AttachmentUAV = GraphBuilder.CreateUAV(AttachmentBuffer);
		AdhesionManager->AddAdhesionPass(GraphBuilder, ParticlesUAV, AttachmentUAV, CollisionManager.Get(), CurrentParticleCount, Params);
		GraphBuilder.QueueBufferExtraction(AttachmentBuffer, &PersistentAttachmentBuffer, ERHIAccess::UAVCompute);
	}
}

void FGPUFluidSimulator::ExecutePostSimulation(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef ParticleBuffer,
	FRDGBufferUAVRef ParticlesUAV,
	const FSimulationSpatialData& SpatialData,
	const FGPUFluidSimulationParams& Params)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid_PostSimulation");

	// Skip when no particles (SpatialData buffers may be invalid)
	if (CurrentParticleCount == 0)
	{
		return;
	}

	AddFinalizePositionsPass(GraphBuilder, ParticlesUAV, Params);

	// REMOVED: Viscosity pass is now integrated into Phase 2 (PredictPositions)
	// - Fluid Viscosity (XSPH + Laplacian): Calculated together with Cohesion in single PrevNeighborCache loop
	// - Boundary Viscosity: Integrated into Phase 3 (SolveDensityPressure) Boundary loop
	// This optimization reduces neighbor traversal from 2x to 1x, saving ~400us at 76k particles.
	// AddApplyViscosityPass(...) - DEPRECATED

	// Particle Sleeping Pass (NVIDIA Flex stabilization)
	if (false && Params.bEnableParticleSleeping && SpatialData.NeighborListSRV && SpatialData.NeighborCountsSRV)
	{
		// Ensure SleepCounters buffer exists and has correct size
		if (!SleepCountersBuffer.IsValid() || SleepCountersCapacity < CurrentParticleCount)
		{
			const int32 NewCapacity = FMath::Max(CurrentParticleCount, 1024);
			FRDGBufferDesc SleepCountersDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NewCapacity);
			FRDGBufferRef SleepCountersRDG = GraphBuilder.CreateBuffer(SleepCountersDesc, TEXT("GPUFluidSleepCounters"));

			// Initialize to zero (no sleep counters)
			TArray<uint32> ZeroCounters;
			ZeroCounters.SetNumZeroed(NewCapacity);
			GraphBuilder.QueueBufferUpload(SleepCountersRDG, ZeroCounters.GetData(), NewCapacity * sizeof(uint32));

			// Extract to persistent buffer
			SleepCountersBuffer = GraphBuilder.ConvertToExternalBuffer(SleepCountersRDG);
			SleepCountersCapacity = NewCapacity;
		}

		// Register persistent buffer for this frame
		FRDGBufferRef SleepCountersRDG = GraphBuilder.RegisterExternalBuffer(SleepCountersBuffer, TEXT("GPUFluidSleepCounters"));
		FRDGBufferUAVRef SleepCountersUAV = GraphBuilder.CreateUAV(SleepCountersRDG, PF_R32_UINT);

		AddParticleSleepingPass(GraphBuilder, ParticlesUAV, SleepCountersUAV, SpatialData.NeighborListSRV, SpatialData.NeighborCountsSRV, Params);
	}

	if (Params.StackPressureScale > 0.0f && AdhesionManager.IsValid() && AdhesionManager->IsAdhesionEnabled())
	{
		TRefCountPtr<FRDGPooledBuffer> PersistentAttachmentBuffer = AdhesionManager->GetPersistentAttachmentBuffer();
		if (PersistentAttachmentBuffer.IsValid())
		{
			FRDGBufferRef AttachmentBufferForStackPressure = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsStackPressure"));
			FRDGBufferSRVRef AttachmentSRVForStackPressure = GraphBuilder.CreateSRV(AttachmentBufferForStackPressure);
			AdhesionManager->AddStackPressurePass(GraphBuilder, ParticlesUAV, AttachmentSRVForStackPressure, SpatialData.CellCountsSRV, SpatialData.ParticleIndicesSRV, CollisionManager.Get(), CurrentParticleCount, Params);
		}
	}

	if (AdhesionManager.IsValid() && AdhesionManager->IsAdhesionEnabled())
	{
		AdhesionManager->AddClearDetachedFlagPass(GraphBuilder, ParticlesUAV, CurrentParticleCount);
	}

	if (IsBoundaryAdhesionEnabled())
	{
		AddBoundaryAdhesionPass(GraphBuilder, ParticlesUAV, SpatialData, Params);
	}

	// Anisotropy
    bool bIsLastSubstep = (Params.SubstepIndex == Params.TotalSubsteps - 1);
	if (bIsLastSubstep && CachedAnisotropyParams.bEnabled && CurrentParticleCount > 0)
	{
		const int32 UpdateInterval = FMath::Max(1, CachedAnisotropyParams.UpdateInterval);
		++AnisotropyFrameCounter;

		// DEBUG: Log whether anisotropy computation will execute - disabled for performance
		// static int32 AnisoDebugCounter = 0;
		const bool bWillCompute = (AnisotropyFrameCounter >= UpdateInterval || !PersistentAnisotropyAxis1Buffer.IsValid());
		// if (++AnisoDebugCounter % 30 == 1)
		// {
		// 	UE_LOG(LogGPUFluidSimulator, Warning,
		// 		TEXT("[ANISO_COMPUTE] UpdateInterval=%d, FrameCounter=%d, WillCompute=%s"),
		// 		UpdateInterval, AnisotropyFrameCounter, bWillCompute ? TEXT("YES") : TEXT("NO"));
		// }

		if (AnisotropyFrameCounter >= UpdateInterval || !PersistentAnisotropyAxis1Buffer.IsValid())
		{
			AnisotropyFrameCounter = 0;

			// Create or reuse anisotropy output buffers
			// For temporal smoothing, we need to preserve previous frame's data
			FRDGBufferRef Axis1Buffer = nullptr;
			FRDGBufferRef Axis2Buffer = nullptr;
			FRDGBufferRef Axis3Buffer = nullptr;

			// Check if persistent buffers exist and have correct size
			const bool bHasPersistentAnisotropyBuffers =
				PersistentAnisotropyAxis1Buffer.IsValid() &&
				PersistentAnisotropyAxis2Buffer.IsValid() &&
				PersistentAnisotropyAxis3Buffer.IsValid() &&
				PersistentAnisotropyAxis1Buffer->GetSize() >= static_cast<uint32>(CurrentParticleCount * sizeof(FVector4f));

			if (bHasPersistentAnisotropyBuffers)
			{
				// Reuse persistent buffers (contains previous frame's anisotropy for temporal smoothing)
				Axis1Buffer = GraphBuilder.RegisterExternalBuffer(
					PersistentAnisotropyAxis1Buffer, TEXT("FluidAnisotropyAxis1"));
				Axis2Buffer = GraphBuilder.RegisterExternalBuffer(
					PersistentAnisotropyAxis2Buffer, TEXT("FluidAnisotropyAxis2"));
				Axis3Buffer = GraphBuilder.RegisterExternalBuffer(
					PersistentAnisotropyAxis3Buffer, TEXT("FluidAnisotropyAxis3"));
			}
			else
			{
				// First frame or particle count changed - create new buffers
				FFluidAnisotropyPassBuilder::CreateAnisotropyBuffers(
					GraphBuilder, CurrentParticleCount, Axis1Buffer, Axis2Buffer, Axis3Buffer);
			}

			if (Axis1Buffer && Axis2Buffer && Axis3Buffer && SpatialData.CellCountsBuffer && SpatialData.ParticleIndicesBuffer)
			{
				FAnisotropyComputeParams AnisotropyParams;
				AnisotropyParams.PhysicsParticlesSRV = GraphBuilder.CreateSRV(ParticleBuffer);

				// Attachment buffer for anisotropy
				TRefCountPtr<FRDGPooledBuffer> AttachmentBufferForAniso = AdhesionManager.IsValid() ? AdhesionManager->GetPersistentAttachmentBuffer() : nullptr;
				if (AdhesionManager.IsValid() && AdhesionManager->IsAdhesionEnabled() && AttachmentBufferForAniso.IsValid())
				{
					FRDGBufferRef AttachmentBuffer = GraphBuilder.RegisterExternalBuffer(AttachmentBufferForAniso, TEXT("GPUFluidAttachmentsAnisotropy"));
					AnisotropyParams.AttachmentsSRV = GraphBuilder.CreateSRV(AttachmentBuffer);
				}
				else
				{
					FRDGBufferRef Dummy = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUParticleAttachment), 1), TEXT("DummyAttachmentBuffer"));
					FGPUParticleAttachment Zero = {};
					GraphBuilder.QueueBufferUpload(Dummy, &Zero, sizeof(FGPUParticleAttachment));
					AnisotropyParams.AttachmentsSRV = GraphBuilder.CreateSRV(Dummy);
				}

				AnisotropyParams.CellCountsSRV = GraphBuilder.CreateSRV(SpatialData.CellCountsBuffer);
				AnisotropyParams.ParticleIndicesSRV = GraphBuilder.CreateSRV(SpatialData.ParticleIndicesBuffer);
				AnisotropyParams.OutAxis1UAV = GraphBuilder.CreateUAV(Axis1Buffer);
				AnisotropyParams.OutAxis2UAV = GraphBuilder.CreateUAV(Axis2Buffer);
				AnisotropyParams.OutAxis3UAV = GraphBuilder.CreateUAV(Axis3Buffer);
				AnisotropyParams.ParticleCount = CurrentParticleCount;

				// Params Mapping
				AnisotropyParams.Mode = (EGPUAnisotropyMode)CachedAnisotropyParams.Mode;
				AnisotropyParams.VelocityStretchFactor = CachedAnisotropyParams.VelocityStretchFactor;
				AnisotropyParams.Strength = CachedAnisotropyParams.Strength;
				AnisotropyParams.MinStretch = CachedAnisotropyParams.MinStretch;
				AnisotropyParams.MaxStretch = CachedAnisotropyParams.MaxStretch;
				AnisotropyParams.DensityWeight = CachedAnisotropyParams.DensityWeight;

				// Use same radius as simulation (per Yu & Turk 2013, NVIDIA FleX)
				AnisotropyParams.SmoothingRadius = Params.SmoothingRadius * 1.0f;
				AnisotropyParams.CellSize = Params.CellSize;

				// Morton-sorted spatial lookup (cache-friendly sequential access)
				const bool bAnisotropyUseZOrder = ZOrderSortManager.IsValid() && ZOrderSortManager->IsZOrderSortingEnabled();
				AnisotropyParams.bUseZOrderSorting = bAnisotropyUseZOrder;
				if (bAnisotropyUseZOrder)
				{
					AnisotropyParams.CellStartSRV = SpatialData.CellStartSRV;
					AnisotropyParams.CellEndSRV = SpatialData.CellEndSRV;
					AnisotropyParams.MortonBoundsMin = SimulationBoundsMin;
					// CRITICAL: Pass GridResolutionPreset for correct Morton code calculation
					// Without this, shader uses default Medium (7-bit) even when Large (8-bit) is configured
					AnisotropyParams.GridResolutionPreset = ZOrderSortManager->GetGridResolutionPreset();
				}

				// =========================================================================
				// Boundary Particles for surface-aware anisotropy (Akinci 2012 pattern)
				// Boundary particles contribute to covariance matrix, influencing ellipsoid
				// orientation near surfaces for more accurate fluid-surface interactions
				// =========================================================================
				const bool bHasSkinnedBoundary = SpatialData.bSkinnedBoundaryPerformed && SpatialData.SkinnedBoundarySRV != nullptr;
				const bool bHasStaticBoundary = SpatialData.bStaticBoundaryAvailable && SpatialData.StaticBoundarySRV != nullptr;

				if (bHasSkinnedBoundary)
				{
					// Use Skinned boundary (SkeletalMesh - same-frame skinning)
					AnisotropyParams.BoundaryParticlesSRV = SpatialData.SkinnedBoundarySRV;
					AnisotropyParams.BoundaryParticleCount = SpatialData.SkinnedBoundaryParticleCount;
					AnisotropyParams.bUseBoundaryAnisotropy = true;

					// Skinned Boundary Z-Order (if available)
					if (SpatialData.bSkinnedZOrderPerformed && SpatialData.SkinnedZOrderSortedSRV != nullptr)
					{
						AnisotropyParams.SortedBoundaryParticlesSRV = SpatialData.SkinnedZOrderSortedSRV;
						AnisotropyParams.BoundaryCellStartSRV = SpatialData.SkinnedZOrderCellStartSRV;
						AnisotropyParams.BoundaryCellEndSRV = SpatialData.SkinnedZOrderCellEndSRV;
						AnisotropyParams.bUseBoundaryZOrder = true;
					}
				}
				else if (bHasStaticBoundary)
				{
					// Use Static boundary (StaticMesh - persistent GPU buffer)
					AnisotropyParams.BoundaryParticlesSRV = SpatialData.StaticBoundarySRV;
					AnisotropyParams.BoundaryParticleCount = SpatialData.StaticBoundaryParticleCount;
					AnisotropyParams.bUseBoundaryAnisotropy = true;

					// Static Boundary Z-Order (if available)
					if (SpatialData.StaticZOrderSortedSRV != nullptr)
					{
						AnisotropyParams.SortedBoundaryParticlesSRV = SpatialData.StaticZOrderSortedSRV;
						AnisotropyParams.BoundaryCellStartSRV = SpatialData.StaticZOrderCellStartSRV;
						AnisotropyParams.BoundaryCellEndSRV = SpatialData.StaticZOrderCellEndSRV;
						AnisotropyParams.bUseBoundaryZOrder = true;
					}
				}

				// BoundaryWeight: How much boundary particles influence anisotropy (0-1)
				// 1.0 = full influence (same as fluid particles)
				AnisotropyParams.BoundaryWeight = 1.0f;

				FFluidAnisotropyPassBuilder::AddAnisotropyPass(GraphBuilder, AnisotropyParams);

				GraphBuilder.QueueBufferExtraction(Axis1Buffer, &PersistentAnisotropyAxis1Buffer, ERHIAccess::SRVCompute);
				GraphBuilder.QueueBufferExtraction(Axis2Buffer, &PersistentAnisotropyAxis2Buffer, ERHIAccess::SRVCompute);
				GraphBuilder.QueueBufferExtraction(Axis3Buffer, &PersistentAnisotropyAxis3Buffer, ERHIAccess::SRVCompute);
			}
		}
	}
}

//=============================================================================
// ParticleID Sort Pipeline - Sorts particles by ParticleID for O(1) oldest removal
//=============================================================================

FRDGBufferRef FGPUFluidSimulator::ExecuteParticleIDSortPipeline(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef InParticleBuffer,
	int32 ParticleCount)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid::ParticleIDSort");

	if (ParticleCount <= 0)
	{
		return InParticleBuffer;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// ParticleID is 32-bit, 8 bits per pass = 4 passes total
	const int32 RadixSortPasses = 4;
	const int32 NumBlocks = FMath::DivideAndRoundUp(ParticleCount, GPU_RADIX_ELEMENTS_PER_GROUP);
	const int32 RequiredHistogramSize = GPU_RADIX_SIZE * NumBlocks;

	// Create transient buffers for sorting
	FRDGBufferDesc KeysDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
	FRDGBufferRef Keys[2] = {
		GraphBuilder.CreateBuffer(KeysDesc, TEXT("ParticleIDSort.Keys0")),
		GraphBuilder.CreateBuffer(KeysDesc, TEXT("ParticleIDSort.Keys1"))
	};

	FRDGBufferDesc ValuesDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
	FRDGBufferRef Values[2] = {
		GraphBuilder.CreateBuffer(ValuesDesc, TEXT("ParticleIDSort.Values0")),
		GraphBuilder.CreateBuffer(ValuesDesc, TEXT("ParticleIDSort.Values1"))
	};

	FRDGBufferDesc HistogramDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RequiredHistogramSize);
	FRDGBufferRef Histogram = GraphBuilder.CreateBuffer(HistogramDesc, TEXT("ParticleIDSort.Histogram"));

	FRDGBufferDesc BucketOffsetsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE);
	FRDGBufferRef BucketOffsets = GraphBuilder.CreateBuffer(BucketOffsetsDesc, TEXT("ParticleIDSort.BucketOffsets"));

	int32 BufferIndex = 0;

	for (int32 Pass = 0; Pass < RadixSortPasses; ++Pass)
	{
		const int32 BitOffset = Pass * GPU_RADIX_BITS;
		const int32 SrcIndex = BufferIndex;
		const int32 DstIndex = BufferIndex ^ 1;

		RDG_EVENT_SCOPE(GraphBuilder, "ParticleIDSort Pass %d (bits %d-%d)", Pass, BitOffset, BitOffset + 7);

		if (Pass == 0)
		{
			// First pass: Read directly from Particles[].ParticleID
			// Histogram pass
			{
				TShaderMapRef<FRadixSortHistogramParticleIDCS> HistogramShader(ShaderMap);
				FRadixSortHistogramParticleIDCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortHistogramParticleIDCS::FParameters>();
				Params->Particles = GraphBuilder.CreateSRV(InParticleBuffer);
				Params->Histogram = GraphBuilder.CreateUAV(Histogram);
				Params->ElementCount = ParticleCount;
				Params->BitOffset = BitOffset;
				Params->NumGroups = NumBlocks;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ParticleID_Histogram"), HistogramShader, Params, FIntVector(NumBlocks, 1, 1));
			}

			// Global Prefix Sum
			{
				TShaderMapRef<FRadixSortGlobalPrefixSumCS> PrefixSumShader(ShaderMap);
				FRadixSortGlobalPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortGlobalPrefixSumCS::FParameters>();
				Params->Histogram = GraphBuilder.CreateUAV(Histogram);
				Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);
				Params->NumGroups = NumBlocks;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GlobalPrefixSum"), PrefixSumShader, Params, FIntVector(1, 1, 1));
			}

			// Bucket Prefix Sum
			{
				TShaderMapRef<FRadixSortBucketPrefixSumCS> BucketSumShader(ShaderMap);
				FRadixSortBucketPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortBucketPrefixSumCS::FParameters>();
				Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BucketPrefixSum"), BucketSumShader, Params, FIntVector(1, 1, 1));
			}

			// Scatter pass - reads from Particles, writes to Keys/Values
			{
				TShaderMapRef<FRadixSortScatterParticleIDCS> ScatterShader(ShaderMap);
				FRadixSortScatterParticleIDCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortScatterParticleIDCS::FParameters>();
				Params->Particles = GraphBuilder.CreateSRV(InParticleBuffer);
				Params->KeysOut = GraphBuilder.CreateUAV(Keys[DstIndex]);
				Params->ValuesOut = GraphBuilder.CreateUAV(Values[DstIndex]);
				Params->HistogramSRV = GraphBuilder.CreateSRV(Histogram);
				Params->GlobalOffsetsSRV = GraphBuilder.CreateSRV(BucketOffsets);
				Params->ElementCount = ParticleCount;
				Params->BitOffset = BitOffset;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ParticleID_Scatter"), ScatterShader, Params, FIntVector(NumBlocks, 1, 1));
			}
		}
		else
		{
			// Subsequent passes: Read from Keys/Values buffers (standard radix sort)
			// Histogram pass
			{
				TShaderMapRef<FRadixSortHistogramCS> HistogramShader(ShaderMap);
				FRadixSortHistogramCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortHistogramCS::FParameters>();
				Params->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
				Params->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
				Params->Histogram = GraphBuilder.CreateUAV(Histogram);
				Params->ElementCount = ParticleCount;
				Params->BitOffset = BitOffset;
				Params->NumGroups = NumBlocks;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Histogram"), HistogramShader, Params, FIntVector(NumBlocks, 1, 1));
			}

			// Global Prefix Sum
			{
				TShaderMapRef<FRadixSortGlobalPrefixSumCS> PrefixSumShader(ShaderMap);
				FRadixSortGlobalPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortGlobalPrefixSumCS::FParameters>();
				Params->Histogram = GraphBuilder.CreateUAV(Histogram);
				Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);
				Params->NumGroups = NumBlocks;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GlobalPrefixSum"), PrefixSumShader, Params, FIntVector(1, 1, 1));
			}

			// Bucket Prefix Sum
			{
				TShaderMapRef<FRadixSortBucketPrefixSumCS> BucketSumShader(ShaderMap);
				FRadixSortBucketPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortBucketPrefixSumCS::FParameters>();
				Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BucketPrefixSum"), BucketSumShader, Params, FIntVector(1, 1, 1));
			}

			// Scatter pass
			{
				TShaderMapRef<FRadixSortScatterCS> ScatterShader(ShaderMap);
				FRadixSortScatterCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortScatterCS::FParameters>();
				Params->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
				Params->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
				Params->KeysOut = GraphBuilder.CreateUAV(Keys[DstIndex]);
				Params->ValuesOut = GraphBuilder.CreateUAV(Values[DstIndex]);
				Params->HistogramSRV = GraphBuilder.CreateSRV(Histogram);
				Params->GlobalOffsetsSRV = GraphBuilder.CreateSRV(BucketOffsets);
				Params->ElementCount = ParticleCount;
				Params->BitOffset = BitOffset;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Scatter"), ScatterShader, Params, FIntVector(NumBlocks, 1, 1));
			}
		}

		BufferIndex ^= 1;
	}

	// Final sorted indices are in Values[BufferIndex]
	FRDGBufferRef SortedIndices = Values[BufferIndex];

	// Reorder particle data based on sorted indices
	FRDGBufferDesc SortedParticlesDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), ParticleCount);
	FRDGBufferRef SortedParticleBuffer = GraphBuilder.CreateBuffer(SortedParticlesDesc, TEXT("ParticleIDSort.SortedParticles"));

	// ParticleID 정렬에서는 Attachment 버퍼 재정렬 불필요 - 더미 버퍼 생성
	FRDGBufferDesc DummyAttachmentDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoneDeltaAttachment), 1);
	FRDGBufferRef DummyAttachmentBuffer = GraphBuilder.CreateBuffer(DummyAttachmentDesc, TEXT("ParticleIDSort.DummyAttachment"));

	// Clear dummy buffer to satisfy RDG validation (buffer must be written before read)
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyAttachmentBuffer), 0);

	{
		TShaderMapRef<FReorderParticlesCS> ComputeShader(ShaderMap);
		FReorderParticlesCS::FParameters* Params = GraphBuilder.AllocParameters<FReorderParticlesCS::FParameters>();
		Params->OldParticles = GraphBuilder.CreateSRV(InParticleBuffer);
		Params->SortedIndices = GraphBuilder.CreateSRV(SortedIndices);
		Params->SortedParticles = GraphBuilder.CreateUAV(SortedParticleBuffer);
		// 더미 버퍼 바인딩 (RDG는 nullptr 허용 안함)
		Params->OldBoneDeltaAttachments = GraphBuilder.CreateSRV(DummyAttachmentBuffer);
		Params->SortedBoneDeltaAttachments = GraphBuilder.CreateUAV(DummyAttachmentBuffer);
		Params->bReorderAttachments = 0;  // 실제로 재정렬하지 않음
		Params->ParticleCount = ParticleCount;

		const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FReorderParticlesCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ParticleIDSort::ReorderParticles(%d)", ParticleCount),
			ComputeShader,
			Params,
			FIntVector(NumGroups, 1, 1)
		);
	}

	return SortedParticleBuffer;
}

void FGPUFluidSimulator::ExtractPersistentBuffers(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef ParticleBuffer,
	const FSimulationSpatialData& SpatialData)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid_ExtractPersistentBuffers");

	// Extract Z-Order buffers (for debug/rendering)
	GraphBuilder.QueueBufferExtraction(ParticleBuffer, &PersistentParticleBuffer, ERHIAccess::UAVCompute);

	// Only extract legacy hash table buffers when Z-Order sorting is NOT enabled
	// When Z-Order is enabled, CellCountsBuffer/ParticleIndicesBuffer are dummy buffers that weren't produced
	const bool bUseZOrderSorting = ZOrderSortManager.IsValid() && ZOrderSortManager->IsZOrderSortingEnabled();

	// DEBUG LOG - disabled for performance
	// static int32 ExtractLogCounter = 0;
	// if (++ExtractLogCounter % 60 == 0)
	// {
	// 	UE_LOG(LogTemp, Log, TEXT("[ExtractBuffers] bUseZOrderSorting=%d, CellStartBuffer=%s, CellEndBuffer=%s"),
	// 		bUseZOrderSorting ? 1 : 0,
	// 		SpatialData.CellStartBuffer ? TEXT("Valid") : TEXT("NULL"),
	// 		SpatialData.CellEndBuffer ? TEXT("Valid") : TEXT("NULL"));
	// }

	if (!bUseZOrderSorting)
	{
		if (SpatialData.CellCountsBuffer) GraphBuilder.QueueBufferExtraction(SpatialData.CellCountsBuffer, &PersistentCellCountsBuffer, ERHIAccess::UAVCompute);
		if (SpatialData.ParticleIndicesBuffer) GraphBuilder.QueueBufferExtraction(SpatialData.ParticleIndicesBuffer, &PersistentParticleIndicesBuffer, ERHIAccess::UAVCompute);
	}
	else
	{
		// Extract Z-Order CellStart/CellEnd buffers ONLY when Ray Marching pipeline needs them
		// This avoids unnecessary GPU memory copies for SSFR pipeline
		if (bExtractZOrderBuffersForRayMarching)
		{
			if (SpatialData.CellStartBuffer)
			{
				GraphBuilder.QueueBufferExtraction(SpatialData.CellStartBuffer, &PersistentCellStartBuffer, ERHIAccess::SRVCompute);
			}
			if (SpatialData.CellEndBuffer)
			{
				GraphBuilder.QueueBufferExtraction(SpatialData.CellEndBuffer, &PersistentCellEndBuffer, ERHIAccess::SRVCompute);
			}
		}
	}

	// =====================================================
	// Extract Neighbor Cache for Cohesion Force Double Buffering
	// These buffers will be swapped in EndFrame() after RDG execution completes
	// PredictPositions (Phase 2) uses PrevNeighborCache to apply Cohesion as a Force
	// =====================================================
	// Note: NeighborListBuffer/NeighborCountsBuffer are already extracted in ExecuteConstraintSolverLoop
	// We just need to track the particle count for validation in SwapNeighborCacheBuffers

	// =====================================================
	// Extract Bone Delta Attachment buffer for next frame
	// Persists BoneIndex, LocalOffset, PreviousPosition per particle
	// =====================================================
	if (SpatialData.BoneDeltaAttachmentBuffer)
	{
		GraphBuilder.QueueBufferExtraction(SpatialData.BoneDeltaAttachmentBuffer, &BoneDeltaAttachmentBuffer, ERHIAccess::UAVCompute);
	}
}

void FGPUFluidSimulator::SwapNeighborCacheBuffers()
{
	// =====================================================
	// Double Buffering for Cohesion Force in PredictPositions
	// 
	// Problem: PredictPositions runs BEFORE neighbor search, but Cohesion needs neighbors
	// Solution: Use previous frame's neighbor cache (Double Buffering)
	// 
	// Timeline:
	//   Frame N:   BuildNeighbors -> Solve -> Extract NeighborBuffer
	//   EndFrame:  NeighborBuffer -> PrevNeighborBuffer (this function)
	//   Frame N+1: PredictPositions uses PrevNeighborBuffer for Cohesion
	// =====================================================
	
	// Only swap if current frame's neighbor cache is valid
	if (!NeighborListBuffer.IsValid() || !NeighborCountsBuffer.IsValid())
	{
		// No neighbor cache available (maybe no particles or first frame)
		bPrevNeighborCacheValid = false;
		return;
	}
	
	// Move current neighbor cache to previous
	// Note: We don't need to explicitly release old PrevNeighbor buffers
	// TRefCountPtr handles reference counting automatically
	PrevNeighborListBuffer = NeighborListBuffer;
	PrevNeighborCountsBuffer = NeighborCountsBuffer;
	PrevNeighborBufferParticleCount = CurrentParticleCount;
	bPrevNeighborCacheValid = true;
}

//=============================================================================
// GPU Particle Spawning API (Delegated to FGPUSpawnManager)
//=============================================================================

void FGPUFluidSimulator::AddSpawnRequest(const FVector3f& Position, const FVector3f& Velocity, float Mass)
{
	if (SpawnManager.IsValid()) { SpawnManager->AddSpawnRequest(Position, Velocity, Mass); }
}

void FGPUFluidSimulator::AddSpawnRequests(const TArray<FGPUSpawnRequest>& Requests)
{
	if (SpawnManager.IsValid()) { SpawnManager->AddSpawnRequests(Requests); }
}

void FGPUFluidSimulator::AddDespawnByIDRequests(const TArray<int32>& ParticleIDs)
{
	if (SpawnManager.IsValid() && ParticleIDs.Num() > 0)
	{
		SpawnManager->AddDespawnByIDRequests(ParticleIDs);
		// Count update happens in PrepareParticleBuffer after AddDespawnByIDPass execution
	}
}

int32 FGPUFluidSimulator::AddDespawnByIDRequestsFiltered(const TArray<int32>& CandidateIDs, int32 MaxCount)
{
	if (SpawnManager.IsValid() && CandidateIDs.Num() > 0 && MaxCount > 0)
	{
		return SpawnManager->AddDespawnByIDRequestsFiltered(CandidateIDs, MaxCount);
	}
	return 0;
}

bool FGPUFluidSimulator::GetParticlePositionsAndIDs(TArray<FVector3f>& OutPositions, TArray<int32>& OutParticleIDs, TArray<int32>& OutSourceIDs)
{
	if (!bHasValidGPUResults.load())
	{
		return false;
	}

	FScopeLock Lock(&BufferLock);

	if (CachedParticlePositions.Num() == 0 || CachedAllParticleIDs.Num() == 0)
	{
		return false;
	}

	OutPositions = CachedParticlePositions;
	OutParticleIDs = CachedAllParticleIDs;
	OutSourceIDs = CachedParticleSourceIDs;
	return true;
}

bool FGPUFluidSimulator::GetParticlePositionsAndVelocities(TArray<FVector3f>& OutPositions, TArray<FVector3f>& OutVelocities)
{
	if (!bHasValidGPUResults.load())
	{
		return false;
	}

	FScopeLock Lock(&BufferLock);

	if (CachedParticlePositions.Num() == 0)
	{
		return false;
	}

	OutPositions = CachedParticlePositions;
	OutVelocities = CachedParticleVelocities;  // May be empty if ISM not enabled
	return true;
}

const TArray<int32>* FGPUFluidSimulator::GetParticleIDsBySourceID(int32 SourceID) const
{
	if (!bHasValidGPUResults.load())
	{
		return nullptr;
	}

	if (SourceID < 0 || SourceID >= CachedSourceIDToParticleIDs.Num())
	{
		return nullptr;
	}

	FScopeLock Lock(&const_cast<FCriticalSection&>(BufferLock));
	const TArray<int32>& Result = CachedSourceIDToParticleIDs[SourceID];
	return Result.Num() > 0 ? &Result : nullptr;
}

const TArray<int32>* FGPUFluidSimulator::GetAllParticleIDs() const
{
	if (!bHasValidGPUResults.load())
	{
		return nullptr;
	}

	FScopeLock Lock(&const_cast<FCriticalSection&>(BufferLock));
	if (CachedAllParticleIDs.Num() == 0)
	{
		return nullptr;
	}
	return &CachedAllParticleIDs;
}

void FGPUFluidSimulator::ClearSpawnRequests()
{
	if (SpawnManager.IsValid()) { SpawnManager->ClearSpawnRequests(); }
}

int32 FGPUFluidSimulator::GetPendingSpawnCount() const
{
	return SpawnManager.IsValid() ? SpawnManager->GetPendingSpawnCount() : 0;
}

//=============================================================================
// FGPUFluidSimulationTask Implementation
//=============================================================================

void FGPUFluidSimulationTask::Execute(
	FGPUFluidSimulator* Simulator,
	const FGPUFluidSimulationParams& Params,
	int32 NumSubsteps)
{
	if (!Simulator || !Simulator->IsReady()) { return; }

	FGPUFluidSimulationParams SubstepParams = Params;
	SubstepParams.DeltaTime = Params.DeltaTime / FMath::Max(1, NumSubsteps);
	SubstepParams.DeltaTimeSq = SubstepParams.DeltaTime * SubstepParams.DeltaTime;
	SubstepParams.TotalSubsteps = NumSubsteps;

	// Frame lifecycle: BeginFrame
	Simulator->BeginFrame();

	for (int32 i = 0; i < NumSubsteps; ++i)
	{
		SubstepParams.SubstepIndex = i;
		Simulator->SimulateSubstep(SubstepParams);
	}

	// Frame lifecycle: EndFrame
	Simulator->EndFrame();
}

//=============================================================================
// Boundary Particles & Skinning (Delegated to FGPUBoundarySkinningManager)
//=============================================================================

static FGPUBoundaryAdhesionParams GDefaultBoundaryAdhesionParams;

const FGPUBoundaryAdhesionParams& FGPUFluidSimulator::GetBoundaryAdhesionParams() const
{
	return BoundarySkinningManager.IsValid() ? BoundarySkinningManager->GetBoundaryAdhesionParams() : GDefaultBoundaryAdhesionParams;
}

void FGPUFluidSimulator::UploadLocalBoundaryParticles(int32 OwnerID, const TArray<FGPUBoundaryParticleLocal>& LocalParticles)
{
	if (bIsInitialized && BoundarySkinningManager.IsValid()) { BoundarySkinningManager->UploadLocalBoundaryParticles(OwnerID, LocalParticles); }
}

void FGPUFluidSimulator::UploadBoneTransformsForBoundary(int32 OwnerID, const TArray<FMatrix44f>& BoneTransforms, const FMatrix44f& ComponentTransform)
{
	if (bIsInitialized && BoundarySkinningManager.IsValid()) { BoundarySkinningManager->UploadBoneTransformsForBoundary(OwnerID, BoneTransforms, ComponentTransform); }
}

void FGPUFluidSimulator::RegisterSkeletalMeshForBoundary(int32 OwnerID, USkeletalMeshComponent* SkelMesh)
{
	if (bIsInitialized && BoundarySkinningManager.IsValid()) { BoundarySkinningManager->RegisterSkeletalMeshReference(OwnerID, SkelMesh); }
}

void FGPUFluidSimulator::RefreshAllBoneTransforms()
{
	if (bIsInitialized && BoundarySkinningManager.IsValid()) { BoundarySkinningManager->RefreshAllBoneTransforms(); }
}

void FGPUFluidSimulator::UpdateBoundaryOwnerAABB(int32 OwnerID, const FGPUBoundaryOwnerAABB& AABB)
{
	if (bIsInitialized && BoundarySkinningManager.IsValid()) { BoundarySkinningManager->UpdateBoundaryOwnerAABB(OwnerID, AABB); }
}

void FGPUFluidSimulator::RemoveBoundarySkinningData(int32 OwnerID)
{
	if (BoundarySkinningManager.IsValid()) { BoundarySkinningManager->RemoveBoundarySkinningData(OwnerID); }
}

void FGPUFluidSimulator::ClearAllBoundarySkinningData()
{
	if (BoundarySkinningManager.IsValid()) { BoundarySkinningManager->ClearAllBoundarySkinningData(); }
}

//=============================================================================
// Static Boundary Particles
//=============================================================================

void FGPUFluidSimulator::GenerateStaticBoundaryParticles(float SmoothingRadius, float RestDensity)
{
	if (!bIsInitialized || !StaticBoundaryManager.IsValid() || !CollisionManager.IsValid())
	{
		return;
	}

	// Generate boundary particles from cached collision primitives
	StaticBoundaryManager->GenerateBoundaryParticles(
		CollisionManager->GetCachedSpheres(),
		CollisionManager->GetCachedCapsules(),
		CollisionManager->GetCachedBoxes(),
		CollisionManager->GetCachedConvexHeaders(),
		CollisionManager->GetCachedConvexPlanes(),
		SmoothingRadius,
		RestDensity);

	// Upload static boundary particles to BoundarySkinningManager (Persistent GPU buffer)
	// Static boundary particles are uploaded once and cached on GPU
	if (BoundarySkinningManager.IsValid() && StaticBoundaryManager->HasBoundaryParticles())
	{
		const TArray<FGPUBoundaryParticle>& StaticParticles = StaticBoundaryManager->GetBoundaryParticles();

		// Upload to persistent GPU buffer (not CPU cache)
		BoundarySkinningManager->UploadStaticBoundaryParticles(StaticParticles);
		BoundarySkinningManager->SetStaticBoundaryEnabled(true);

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("Static boundary particles queued for GPU upload: %d particles"), StaticParticles.Num());
	}
	else if (BoundarySkinningManager.IsValid())
	{
		// No static boundary particles - disable static boundary processing
		BoundarySkinningManager->SetStaticBoundaryEnabled(false);
	}
}

void FGPUFluidSimulator::ClearStaticBoundaryParticles()
{
	// Clear StaticBoundaryManager
	if (StaticBoundaryManager.IsValid())
	{
		StaticBoundaryManager->ClearBoundaryParticles();
	}

	// Clear BoundarySkinningManager's persistent static buffers
	if (BoundarySkinningManager.IsValid())
	{
		BoundarySkinningManager->ClearStaticBoundaryParticles();
		BoundarySkinningManager->SetStaticBoundaryEnabled(false);
	}

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("Static boundary particles cleared"));
}

void FGPUFluidSimulator::AddBoundaryAdhesionPass(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef ParticlesUAV, const FSimulationSpatialData& SpatialData, const FGPUFluidSimulationParams& Params)
{
	if (BoundarySkinningManager.IsValid())
	{
		// Determine which boundary to use (Skinned or Static)
		const bool bHasSkinnedBoundary = SpatialData.bSkinnedBoundaryPerformed && SpatialData.SkinnedBoundaryBuffer != nullptr;
		const bool bHasStaticBoundary = SpatialData.bStaticBoundaryAvailable && SpatialData.StaticBoundarySRV != nullptr;

		FRDGBufferRef BoundaryBuffer = nullptr;
		int32 BoundaryCount = 0;
		FRDGBufferSRVRef ZOrderSortedSRV = nullptr;
		FRDGBufferSRVRef ZOrderCellStartSRV = nullptr;
		FRDGBufferSRVRef ZOrderCellEndSRV = nullptr;

		if (bHasSkinnedBoundary)
		{
			// Use Skinned boundary
			BoundaryBuffer = SpatialData.SkinnedBoundaryBuffer;
			BoundaryCount = SpatialData.SkinnedBoundaryParticleCount;

			if (SpatialData.bSkinnedZOrderPerformed)
			{
				ZOrderSortedSRV = SpatialData.SkinnedZOrderSortedSRV;
				ZOrderCellStartSRV = SpatialData.SkinnedZOrderCellStartSRV;
				ZOrderCellEndSRV = SpatialData.SkinnedZOrderCellEndSRV;
			}
		}
		else if (bHasStaticBoundary)
		{
			// Use Static boundary - need to get buffer from manager
			BoundaryBuffer = GraphBuilder.RegisterExternalBuffer(
				BoundarySkinningManager->GetStaticBoundaryBuffer(),
				TEXT("GPUFluid.StaticBoundaryForAdhesion"));
			BoundaryCount = SpatialData.StaticBoundaryParticleCount;

			ZOrderSortedSRV = SpatialData.StaticZOrderSortedSRV;
			ZOrderCellStartSRV = SpatialData.StaticZOrderCellStartSRV;
			ZOrderCellEndSRV = SpatialData.StaticZOrderCellEndSRV;
		}

		if (BoundaryBuffer != nullptr && BoundaryCount > 0)
		{
			BoundarySkinningManager->AddBoundaryAdhesionPass(
				GraphBuilder, ParticlesUAV, CurrentParticleCount, Params,
				BoundaryBuffer, BoundaryCount,
				ZOrderSortedSRV, ZOrderCellStartSRV, ZOrderCellEndSRV);
		}
	}
}

void FGPUFluidSimulator::AddBoundarySkinningPass(FRDGBuilder& GraphBuilder, FSimulationSpatialData& SpatialData, const FGPUFluidSimulationParams& Params)
{
	if (BoundarySkinningManager.IsValid())
	{
		FRDGBufferRef OutBuffer = nullptr;
		int32 OutCount = 0;
		BoundarySkinningManager->AddBoundarySkinningPass(GraphBuilder, OutBuffer, OutCount, Params.DeltaTime);

		if (OutBuffer && OutCount > 0)
		{
			SpatialData.WorldBoundaryBuffer = OutBuffer;
			SpatialData.WorldBoundarySRV = GraphBuilder.CreateSRV(OutBuffer);
			SpatialData.WorldBoundaryParticleCount = OutCount;
			SpatialData.bBoundarySkinningPerformed = true;
		}
	}
}

//=============================================================================
// Bone Delta Attachment Buffer Management (NEW simplified bone-following)
//=============================================================================

FRDGBufferRef FGPUFluidSimulator::EnsureBoneDeltaAttachmentBuffer(
	FRDGBuilder& GraphBuilder,
	int32 RequiredCapacity)
{
	// Check if we need to create a new buffer (capacity insufficient or buffer invalid)
	if (BoneDeltaAttachmentCapacity < RequiredCapacity || !BoneDeltaAttachmentBuffer.IsValid())
	{
		// Initialize attachment data with BoundaryParticleIndex = -1 (not attached)
		// IMPORTANT: GPU memory might be zero-initialized, which would make BoundaryParticleIndex = 0 (attached to boundary 0)
		// This would cause newly spawned particles to teleport to boundary particle 0's position!
		TArray<FGPUBoneDeltaAttachment> InitialData;
		InitialData.SetNum(RequiredCapacity);
		for (int32 i = 0; i < RequiredCapacity; ++i)
		{
			InitialData[i].BoundaryParticleIndex = -1;  // -1 = not attached
			InitialData[i].Reserved = FVector3f::ZeroVector;
			InitialData[i].PreviousPosition = FVector3f::ZeroVector;
		}

		// Create buffer with initialized data
		// IMPORTANT: Use ERDGInitialDataFlags::None to copy data, NOT NoCopy
		// NoCopy would cause crash because InitialData goes out of scope before RDG executes
		FRDGBufferRef NewBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoneDeltaAttachment"),
			sizeof(FGPUBoneDeltaAttachment),
			RequiredCapacity,
			InitialData.GetData(),
			RequiredCapacity * sizeof(FGPUBoneDeltaAttachment),
			ERDGInitialDataFlags::None);

		// Update capacity tracking
		BoneDeltaAttachmentCapacity = RequiredCapacity;

		UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Created new BoneDeltaAttachment buffer with capacity: %d (initialized with BoneIndex=-1)"), RequiredCapacity);

		return NewBuffer;
	}
	else
	{
		// Re-use existing buffer
		return GraphBuilder.RegisterExternalBuffer(BoneDeltaAttachmentBuffer, TEXT("GPUFluidBoneDeltaAttachment"));
	}
}

//=============================================================================
// Z-Order Sorting (Delegated to FGPUZOrderSortManager)
//=============================================================================

FRDGBufferRef FGPUFluidSimulator::ExecuteZOrderSortingPipeline(
	FRDGBuilder& GraphBuilder, FRDGBufferRef InParticleBuffer,
	FRDGBufferUAVRef& OutCellStartUAV, FRDGBufferSRVRef& OutCellStartSRV,
	FRDGBufferUAVRef& OutCellEndUAV, FRDGBufferSRVRef& OutCellEndSRV,
	FRDGBufferRef& OutCellStartBuffer, FRDGBufferRef& OutCellEndBuffer,
	const FGPUFluidSimulationParams& Params,
	FRDGBufferRef InAttachmentBuffer,
	FRDGBufferRef* OutSortedAttachmentBuffer)
{
	// Check both manager validity AND enabled flag
	if (!ZOrderSortManager.IsValid() || !ZOrderSortManager->IsZOrderSortingEnabled())
	{
		return InParticleBuffer;
	}
	return ZOrderSortManager->ExecuteZOrderSortingPipeline(GraphBuilder, InParticleBuffer,
		OutCellStartUAV, OutCellStartSRV, OutCellEndUAV, OutCellEndSRV,
		OutCellStartBuffer, OutCellEndBuffer,
		CurrentParticleCount, Params,
		InAttachmentBuffer, OutSortedAttachmentBuffer);
}

//=============================================================================
// Data Transfer (CPU <-> GPU)
//=============================================================================

FGPUFluidParticle FGPUFluidSimulator::ConvertToGPU(const FFluidParticle& CPUParticle)
{
	FGPUFluidParticle GPUParticle;

	GPUParticle.Position = FVector3f(CPUParticle.Position);
	GPUParticle.Mass = CPUParticle.Mass;
	GPUParticle.PredictedPosition = FVector3f(CPUParticle.PredictedPosition);
	GPUParticle.Density = CPUParticle.Density;
	GPUParticle.Velocity = FVector3f(CPUParticle.Velocity);
	GPUParticle.Lambda = CPUParticle.Lambda;
	GPUParticle.ParticleID = CPUParticle.ParticleID;
	GPUParticle.SourceID = CPUParticle.SourceID;

	// Pack flags
	uint32 Flags = 0;
	if (CPUParticle.bIsAttached) Flags |= EGPUParticleFlags::IsAttached;
	if (CPUParticle.bIsSurfaceParticle) Flags |= EGPUParticleFlags::IsSurface;
	if (CPUParticle.bIsCoreParticle) Flags |= EGPUParticleFlags::IsCore;
	if (CPUParticle.bJustDetached) Flags |= EGPUParticleFlags::JustDetached;
	if (CPUParticle.bNearGround) Flags |= EGPUParticleFlags::NearGround;
	GPUParticle.Flags = Flags;

	// NeighborCount is calculated on GPU during density solve
	GPUParticle.NeighborCount = 0;

	return GPUParticle;
}

void FGPUFluidSimulator::ConvertFromGPU(FFluidParticle& OutCPUParticle, const FGPUFluidParticle& GPUParticle)
{
	// Safety check: validate GPU data before converting
	// If data is NaN or invalid, keep the original CPU values
	FVector NewPosition = FVector(GPUParticle.Position);
	FVector NewVelocity = FVector(GPUParticle.Velocity);

	// Check for NaN or extremely large values (indicates invalid data)
	const float MaxValidValue = 1000000.0f;
	bool bValidPosition = !NewPosition.ContainsNaN() && NewPosition.GetAbsMax() < MaxValidValue;
	bool bValidVelocity = !NewVelocity.ContainsNaN() && NewVelocity.GetAbsMax() < MaxValidValue;

	if (!bValidPosition || !bValidVelocity)
	{
		// Invalid GPU data - don't update the particle
		// This can happen if readback hasn't completed yet
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ConvertFromGPU: Invalid data detected (NaN or extreme values) - skipping update"));
			bLoggedOnce = true;
		}
		return;
	}

	OutCPUParticle.Position = NewPosition;
	OutCPUParticle.PredictedPosition = FVector(GPUParticle.PredictedPosition);
	OutCPUParticle.Velocity = NewVelocity;
	OutCPUParticle.Mass = FMath::IsFinite(GPUParticle.Mass) ? GPUParticle.Mass : OutCPUParticle.Mass;
	OutCPUParticle.Density = FMath::IsFinite(GPUParticle.Density) ? GPUParticle.Density : OutCPUParticle.Density;
	OutCPUParticle.Lambda = FMath::IsFinite(GPUParticle.Lambda) ? GPUParticle.Lambda : OutCPUParticle.Lambda;

	// Unpack flags
	OutCPUParticle.bIsAttached = (GPUParticle.Flags & EGPUParticleFlags::IsAttached) != 0;
	OutCPUParticle.bJustDetached = (GPUParticle.Flags & EGPUParticleFlags::JustDetached) != 0;
	OutCPUParticle.bNearGround = (GPUParticle.Flags & EGPUParticleFlags::NearGround) != 0;
	OutCPUParticle.bNearBoundary = (GPUParticle.Flags & EGPUParticleFlags::NearBoundary) != 0;
}

void FGPUFluidSimulator::UploadParticles(const TArray<FFluidParticle>& CPUParticles, bool bAppend)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("UploadParticles: Simulator not initialized"));
		return;
	}

	const int32 NewCount = CPUParticles.Num();
	if (NewCount == 0)
	{
		// When bAppend=true, do nothing if array is empty
		if (bAppend)
		{
			return;
		}
		CurrentParticleCount = 0;
		CachedGPUParticles.Empty();
		return;
	}

	FScopeLock Lock(&BufferLock);

	//=========================================================================
	// Append Mode (multiple components upload sequentially in batching environment)
	//=========================================================================
	if (bAppend)
	{
		const int32 AppendOffset = CachedGPUParticles.Num();
		const int32 TotalAfterAppend = AppendOffset + NewCount;

		if (TotalAfterAppend > MaxParticleCount)
		{
			UE_LOG(LogGPUFluidSimulator, Warning,
				TEXT("UploadParticles (Append): Total count (%d + %d = %d) exceeds capacity (%d)"),
				AppendOffset, NewCount, TotalAfterAppend, MaxParticleCount);
			return;
		}

		// Add to CachedGPUParticles
		const int32 OldNum = CachedGPUParticles.Num();
		CachedGPUParticles.SetNumUninitialized(TotalAfterAppend);

		// ParallelFor optimization: parallelize for large particle counts (>= 2048)
		ParallelFor(NewCount, [&](int32 i)
		{
			CachedGPUParticles[OldNum + i] = ConvertToGPU(CPUParticles[i]);
		}, NewCount < 2048);

		UE_LOG(LogGPUFluidSimulator, Log,
			TEXT("UploadParticles (Append): Added %d particles at offset %d (total: %d)"),
			NewCount, AppendOffset, TotalAfterAppend);

		// Don't call CreateImmediatePersistentBuffer() yet
		// Should be called once after all components finish uploading
		return;
	}

	//=========================================================================
	// Replace Mode (existing behavior - full replacement)
	//=========================================================================
	if (NewCount > MaxParticleCount)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("UploadParticles: Particle count (%d) exceeds capacity (%d)"),
			NewCount, MaxParticleCount);
		return;
	}

	// Store old count for comparison BEFORE updating
	const int32 OldCount = CurrentParticleCount;

	// Determine upload strategy based on persistent buffer state and particle count changes
	const bool bHasPersistentBuffer = PersistentParticleBuffer.IsValid() && OldCount > 0;
	const bool bCanAppendRuntime = bHasPersistentBuffer && (NewCount > OldCount);

	if (bCanAppendRuntime)
	{
		// Only cache the NEW particles (indices OldCount to NewCount-1)
		const int32 NumNewParticles = NewCount - OldCount;
		NewParticlesToAppend.SetNumUninitialized(NumNewParticles);

		for (int32 i = 0; i < NumNewParticles; ++i)
		{
			NewParticlesToAppend[i] = ConvertToGPU(CPUParticles[OldCount + i]);
		}

		NewParticleCount = NumNewParticles;

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadParticles: Appending %d new particles (total: %d)"),
			NumNewParticles, NewCount);
	}
	else
	{
		// Full upload needed: first frame, buffer invalid, or particles reduced
		CachedGPUParticles.SetNumUninitialized(NewCount);

		// Convert particles to GPU format
		// ParallelFor optimization: parallelize for large particle counts (>= 2048)
		ParallelFor(NewCount, [&](int32 i)
		{
			CachedGPUParticles[i] = ConvertToGPU(CPUParticles[i]);
		}, NewCount < 2048);

		// Simulation bounds for Morton code (Z-Order sorting) are set via SetSimulationBounds()
		// from SimulateGPU before this call (preset bounds + component location offset)
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadParticles: Using bounds: Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f)"),
			SimulationBoundsMin.X, SimulationBoundsMin.Y, SimulationBoundsMin.Z,
			SimulationBoundsMax.X, SimulationBoundsMax.Y, SimulationBoundsMax.Z);

		NewParticleCount = 0;
		NewParticlesToAppend.Empty();

		// Create PersistentParticleBuffer immediately (enable rendering without simulation)
		CreateImmediatePersistentBuffer();

		// Skip PATH 3 in Simulate() since buffer is already created
		bNeedsFullUpload = false;
	}
}

void FGPUFluidSimulator::FinalizeUpload()
{
	TArray<FGPUFluidParticle> ParticlesCopy;
	int32 ParticleCount = 0;

	// ═══════════════════════════════════════════════════
	// Lock Scope 1: Read/copy shared data (minimal scope)
	// Deadlock prevention: Release lock before FlushRenderingCommands()
	// ═══════════════════════════════════════════════════
	{
		FScopeLock Lock(&BufferLock);

		if (CachedGPUParticles.Num() == 0)
		{
			UE_LOG(LogGPUFluidSimulator, Log, TEXT("FinalizeUpload: No particles to upload"));
			return;
		}

		// Deep copy (within lock scope - prevents race condition with RenderThread's ResizeBuffers)
		ParticlesCopy = CachedGPUParticles;
		ParticleCount = CachedGPUParticles.Num();
		CurrentParticleCount = ParticleCount;
		bNeedsFullUpload = false;

		// Build readback cache at upload time (immediately usable in ClearAllParticles etc.)
		// Build cache immediately from CPU data without waiting for GPU readback
		constexpr int32 MaxSources = EGPUParticleSource::MaxSourceCount;
		CachedSourceIDToParticleIDs.SetNum(MaxSources);
		for (int32 i = 0; i < MaxSources; ++i)
		{
			CachedSourceIDToParticleIDs[i].Reset();
		}
		CachedAllParticleIDs.Empty();
		CachedAllParticleIDs.Reserve(ParticleCount);

		for (const FGPUFluidParticle& P : CachedGPUParticles)
		{
			if (P.SourceID >= 0 && P.SourceID < MaxSources)
			{
				CachedSourceIDToParticleIDs[P.SourceID].Add(P.ParticleID);
			}
			CachedAllParticleIDs.Add(P.ParticleID);
		}

		bHasValidGPUResults.store(true);
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("FinalizeUpload: Built readback cache for %d particles"), ParticleCount);
	}
	// ═══════════════════════════════════════════════════
	// Lock released - FlushRenderingCommands is now safe
	// ═══════════════════════════════════════════════════

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("FinalizeUpload: Creating persistent buffer for %d particles"), ParticleCount);

	// Create GPU buffer (using copy, no lock needed)
	CreateImmediatePersistentBufferFromCopy(ParticlesCopy, ParticleCount);

	// Clear only despawn tracking state (uploaded particles have new IDs, old tracking is invalid)
	// NextParticleID is managed atomically in AllocateParticleIDs, so don't touch it
	if (SpawnManager.IsValid())
	{
		SpawnManager->ClearDespawnTracking();
	}
}

void FGPUFluidSimulator::ClearCachedParticles()
{
	FScopeLock Lock(&BufferLock);

	CachedGPUParticles.Empty();
	CurrentParticleCount = 0;

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("ClearCachedParticles: Cleared cached particles"));
}

void FGPUFluidSimulator::CreateImmediatePersistentBuffer()
{
	if (CachedGPUParticles.Num() == 0)
	{
		return;
	}

	CurrentParticleCount = CachedGPUParticles.Num();

	// Prepare data to copy to render thread
	TArray<FGPUFluidParticle> ParticlesCopy = CachedGPUParticles;
	FGPUFluidSimulator* Self = this;

	ENQUEUE_RENDER_COMMAND(CreateImmediatePersistentBuffer)(
		[Self, ParticlesCopy = MoveTemp(ParticlesCopy)](FRHICommandListImmediate& RHICmdList)
		{
			// Create buffer via RDG and extract immediately
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("ImmediateBufferCreate"));

			// Create buffer
			FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), Self->CurrentParticleCount);
			FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, TEXT("ImmediatePersistentParticles"));

			// CPU → GPU upload
			GraphBuilder.QueueBufferUpload(
				Buffer,
				ParticlesCopy.GetData(),
				Self->CurrentParticleCount * sizeof(FGPUFluidParticle));

			// Extract to PersistentParticleBuffer
			GraphBuilder.QueueBufferExtraction(
				Buffer,
				&Self->PersistentParticleBuffer,
				ERHIAccess::SRVMask);

			// Execute immediately
			GraphBuilder.Execute();
			
			UE_LOG(LogGPUFluidSimulator, Log,
				TEXT("CreateImmediatePersistentBuffer: Created buffer with %d particles"), Self->CurrentParticleCount);
		}
	);

	// Mark as valid (sync readback functions will read from GPU directly when needed)
	bHasValidGPUResults.store(true);


	// Initialize SourceCounter (level load doesn't go through spawn shader, so initialize directly)
	if (SpawnManager.IsValid())
	{
		SpawnManager->InitializeSourceCountersFromParticles(CachedGPUParticles);
	}

	// Wait for render thread completion (called once during level load, minimal performance impact)
	FlushRenderingCommands();
}

void FGPUFluidSimulator::CreateImmediatePersistentBufferFromCopy(const TArray<FGPUFluidParticle>& InParticles, int32 InParticleCount)
{
	if (InParticleCount == 0)
	{
		return;
	}

	FGPUFluidSimulator* Self = this;
	const int32 ParticleCount = InParticleCount;

	// Copy for use in lambda (copy from const ref)
	ENQUEUE_RENDER_COMMAND(CreateImmediatePersistentBufferFromCopy)(
		[Self, ParticlesCopy = InParticles, ParticleCount](FRHICommandListImmediate& RHICmdList)
		{
			// Create buffer via RDG and extract immediately
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("ImmediateBufferCreate"));

			// Create buffer
			FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), ParticleCount);
			FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, TEXT("ImmediatePersistentParticles"));

			// CPU → GPU upload
			GraphBuilder.QueueBufferUpload(
				Buffer,
				ParticlesCopy.GetData(),
				ParticleCount * sizeof(FGPUFluidParticle));

			// Extract to PersistentParticleBuffer
			GraphBuilder.QueueBufferExtraction(
				Buffer,
				&Self->PersistentParticleBuffer,
				ERHIAccess::SRVMask);

			// Execute immediately
			GraphBuilder.Execute();
			
			UE_LOG(LogGPUFluidSimulator, Log,
				TEXT("CreateImmediatePersistentBufferFromCopy: Created buffer with %d particles"), ParticleCount);
		}
	);

	// Mark as valid (sync readback functions will read from GPU directly when needed)
	bHasValidGPUResults.store(true);

	// Initialize SourceCounter (level load doesn't go through spawn shader, so initialize directly)
	// InParticles is still valid (const ref)
	if (SpawnManager.IsValid())
	{
		SpawnManager->InitializeSourceCountersFromParticles(InParticles);
	}

	// Wait for render thread completion (called once during level load, minimal performance impact)
	// Deadlock prevention: BufferLock is not held at this point
	FlushRenderingCommands();
}

void FGPUFluidSimulator::DownloadParticles(TArray<FFluidParticle>& OutCPUParticles)
{
	if (!bIsInitialized || CurrentParticleCount == 0)
	{
		return;
	}

	// Only download if we have valid GPU results from a previous simulation
	if (!bHasValidGPUResults.load())
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogGPUFluidSimulator, Log, TEXT("DownloadParticles: No valid GPU results yet, skipping"));
			bLoggedOnce = true;
		}
		return;
	}

	// Perform synchronous readback for Save/Load
	TArray<FGPUFluidParticle> ParticleBuffer;
	int32 Count = 0;

	if (PersistentParticleBuffer.IsValid())
	{

		Count = CurrentParticleCount;
		const int32 DataSize = Count * sizeof(FGPUFluidParticle);
		ParticleBuffer.SetNum(Count);
		FGPUFluidParticle* DestPtr = ParticleBuffer.GetData();
		bool bSuccess = false;

		// Use FRHIGPUBufferReadback for proper GPU->CPU transfer
		FRHIGPUBufferReadback* SyncReadback = new FRHIGPUBufferReadback(TEXT("DownloadParticles_SyncReadback"));

		ENQUEUE_RENDER_COMMAND(SyncReadbackEnqueue)(
			[this, SyncReadback, DestPtr, DataSize, &bSuccess](FRHICommandListImmediate& RHICmdList)
			{
				if (!PersistentParticleBuffer.IsValid())
				{
					return;
				}

				FRHIBuffer* Buffer = PersistentParticleBuffer->GetRHI();
				if (!Buffer)
				{
					return;
				}

				// Enqueue copy and immediately submit + flush GPU
				SyncReadback->EnqueueCopy(RHICmdList, Buffer, DataSize);
				RHICmdList.SubmitCommandsAndFlushGPU();
				RHICmdList.BlockUntilGPUIdle();

				// Now readback should be ready
				if (SyncReadback->IsReady())
				{
					const void* ReadbackData = SyncReadback->Lock(DataSize);
					if (ReadbackData)
					{
						FMemory::Memcpy(DestPtr, ReadbackData, DataSize);
						bSuccess = true;
					}
					SyncReadback->Unlock();
				}
			}
		);
		FlushRenderingCommands();

		if (!bSuccess)
		{
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT("DownloadParticles: Sync readback failed"));
			ParticleBuffer.Empty();
			Count = 0;
		}

		delete SyncReadback;
	}

	if (Count == 0 || ParticleBuffer.Num() == 0)
	{
		return;
	}

	// Build ParticleID -> CPU index map for matching
	TMap<int32, int32> ParticleIDToIndex;
	ParticleIDToIndex.Reserve(OutCPUParticles.Num());
	for (int32 i = 0; i < OutCPUParticles.Num(); ++i)
	{
		ParticleIDToIndex.Add(OutCPUParticles[i].ParticleID, i);
	}

	// Debug: Log first particle before conversion - disabled for performance
	// static int32 DebugFrameCounter = 0;
	// if (DebugFrameCounter++ % 60 == 0)
	// {
	// 	const FGPUFluidParticle& P = ParticleBuffer[0];
	// 	UE_LOG(LogGPUFluidSimulator, Log, TEXT("DownloadParticles: GPUCount=%d, CPUCount=%d, Readback[0] Pos=(%.2f, %.2f, %.2f)"),
	// 		Count, OutCPUParticles.Num(), P.Position.X, P.Position.Y, P.Position.Z);
	// }

	// Update existing particles by matching ParticleID (don't overwrite newly spawned ones)
	// Also track bounds to detect Black Hole Cell potential
	int32 UpdatedCount = 0;
	int32 OutOfBoundsCount = 0;
	const float BoundsMargin = 100.0f;  // Warn if particles within 100 units of bounds edge

	for (int32 i = 0; i < Count; ++i)
	{
		const FGPUFluidParticle& GPUParticle = ParticleBuffer[i];
		if (int32* CPUIndex = ParticleIDToIndex.Find(GPUParticle.ParticleID))
		{
			ConvertFromGPU(OutCPUParticles[*CPUIndex], GPUParticle);
			++UpdatedCount;

			// Check if particle is near or outside bounds
			const FVector3f& Pos = GPUParticle.PredictedPosition;
			if (Pos.X < SimulationBoundsMin.X + BoundsMargin ||
				Pos.Y < SimulationBoundsMin.Y + BoundsMargin ||
				Pos.Z < SimulationBoundsMin.Z + BoundsMargin ||
				Pos.X > SimulationBoundsMax.X - BoundsMargin ||
				Pos.Y > SimulationBoundsMax.Y - BoundsMargin ||
				Pos.Z > SimulationBoundsMax.Z - BoundsMargin)
			{
				OutOfBoundsCount++;
			}
		}
	}

	// Warn if many particles are near bounds edge (potential Black Hole Cell issue)
	static int32 LastBoundsWarningFrame = -1000;
	if (OutOfBoundsCount > Count / 10 && (GFrameCounter - LastBoundsWarningFrame) > 300)  // >10% near edge, warn every 5 sec
	{
		LastBoundsWarningFrame = GFrameCounter;
		UE_LOG(LogGPUFluidSimulator, Warning,
			TEXT("Z-Order WARNING: %d/%d particles (%.1f%%) are near simulation bounds edge! "
			     "This may cause Black Hole Cell problem with Z-Order sorting. "
			     "Bounds: Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f)"),
			OutOfBoundsCount, Count, 100.0f * OutOfBoundsCount / Count,
			SimulationBoundsMin.X, SimulationBoundsMin.Y, SimulationBoundsMin.Z,
			SimulationBoundsMax.X, SimulationBoundsMax.Y, SimulationBoundsMax.Z);
	}

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("DownloadParticles: Updated %d/%d particles"), UpdatedCount, Count);
}

bool FGPUFluidSimulator::GetAllGPUParticles(TArray<FFluidParticle>& OutParticles)
{
	if (!bIsInitialized || CurrentParticleCount == 0)
	{
		return false;
	}

	// Only download if we have valid GPU results from a previous simulation
	if (!bHasValidGPUResults.load())
	{
		return false;
	}

	// Use sync readback (acceptable for debug/stats features that don't run every frame)
	return GetAllGPUParticlesSync(OutParticles);
}

bool FGPUFluidSimulator::GetAllGPUParticlesSync(TArray<FFluidParticle>& OutParticles)
{
	if (!bIsInitialized || CurrentParticleCount == 0)
	{
		return false;
	}

	if (!PersistentParticleBuffer.IsValid())
	{
		return false;
	}

	const int32 Count = CurrentParticleCount;

	// Wait for GPU work completion
	FlushRenderingCommands();

	// Temporary array for synchronous readback
	TArray<FGPUFluidParticle> SyncReadbackBuffer;
	SyncReadbackBuffer.SetNum(Count);

	// Perform synchronous readback on render thread
	FGPUFluidSimulator* Self = this;
	FGPUFluidParticle* DestPtr = SyncReadbackBuffer.GetData();
	const int32 DataSize = Count * sizeof(FGPUFluidParticle);

	ENQUEUE_RENDER_COMMAND(SyncReadbackGPUParticles)(
		[Self, DestPtr, Count, DataSize](FRHICommandListImmediate& RHICmdList)
		{
			if (!Self->PersistentParticleBuffer.IsValid())
			{
				return;
			}

			FRHIBuffer* Buffer = Self->PersistentParticleBuffer->GetRHI();
			if (!Buffer)
			{
				return;
			}

			// Read directly from GPU buffer
			void* MappedData = RHICmdList.LockBuffer(Buffer, 0, DataSize, RLM_ReadOnly);
			if (MappedData)
			{
				FMemory::Memcpy(DestPtr, MappedData, DataSize);
				RHICmdList.UnlockBuffer(Buffer);
			}
		}
	);

	// Wait for render command completion
	FlushRenderingCommands();

	// Convert GPU particles to CPU particles
	OutParticles.SetNum(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		const FGPUFluidParticle& GPUParticle = SyncReadbackBuffer[i];
		FFluidParticle& OutParticle = OutParticles[i];

		OutParticle = FFluidParticle();

		FVector NewPosition = FVector(GPUParticle.Position);
		FVector NewVelocity = FVector(GPUParticle.Velocity);

		const float MaxValidValue = 1000000.0f;
		bool bValidPosition = !NewPosition.ContainsNaN() && NewPosition.GetAbsMax() < MaxValidValue;
		bool bValidVelocity = !NewVelocity.ContainsNaN() && NewVelocity.GetAbsMax() < MaxValidValue;

		if (bValidPosition)
		{
			OutParticle.Position = NewPosition;
			OutParticle.PredictedPosition = FVector(GPUParticle.PredictedPosition);
		}

		if (bValidVelocity)
		{
			OutParticle.Velocity = NewVelocity;
		}

		OutParticle.Mass = FMath::IsFinite(GPUParticle.Mass) ? GPUParticle.Mass : 1.0f;
		OutParticle.Density = FMath::IsFinite(GPUParticle.Density) ? GPUParticle.Density : 0.0f;
		OutParticle.Lambda = FMath::IsFinite(GPUParticle.Lambda) ? GPUParticle.Lambda : 0.0f;
		OutParticle.ParticleID = GPUParticle.ParticleID;
		OutParticle.SourceID = GPUParticle.SourceID;

		OutParticle.bIsAttached = (GPUParticle.Flags & EGPUParticleFlags::IsAttached) != 0;
		OutParticle.bIsSurfaceParticle = (GPUParticle.Flags & EGPUParticleFlags::IsSurface) != 0;
		OutParticle.bIsCoreParticle = (GPUParticle.Flags & EGPUParticleFlags::IsCore) != 0;
		OutParticle.bJustDetached = (GPUParticle.Flags & EGPUParticleFlags::JustDetached) != 0;
		OutParticle.bNearGround = (GPUParticle.Flags & EGPUParticleFlags::NearGround) != 0;
		OutParticle.bNearBoundary = (GPUParticle.Flags & EGPUParticleFlags::NearBoundary) != 0;

		if (GPUParticle.NeighborCount > 0)
		{
			OutParticle.NeighborIndices.SetNum(GPUParticle.NeighborCount);
		}
	}

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("GetAllGPUParticlesSync: Retrieved %d particles (sync readback)"), Count);

	return true;
}

bool FGPUFluidSimulator::GetParticlesBySourceID(int32 SourceID, TArray<FFluidParticle>& OutParticles)
{
	OutParticles.Reset();

	if (!bHasValidGPUResults.load())
	{
		return false;
	}

	// Perform synchronous readback for Save/Load
	TArray<FGPUFluidParticle> ParticleBuffer;
	int32 Count = 0;

	if (PersistentParticleBuffer.IsValid())
	{

		Count = CurrentParticleCount;
		const int32 DataSize = Count * sizeof(FGPUFluidParticle);
		ParticleBuffer.SetNum(Count);
		FGPUFluidParticle* DestPtr = ParticleBuffer.GetData();
		bool bSuccess = false;

		FRHIGPUBufferReadback* SyncReadback = new FRHIGPUBufferReadback(TEXT("GetParticlesBySourceID_SyncReadback"));

		ENQUEUE_RENDER_COMMAND(SyncReadbackEnqueue)(
			[this, SyncReadback, DestPtr, DataSize, &bSuccess](FRHICommandListImmediate& RHICmdList)
			{
				if (!PersistentParticleBuffer.IsValid())
				{
					return;
				}

				FRHIBuffer* Buffer = PersistentParticleBuffer->GetRHI();
				if (!Buffer)
				{
					return;
				}

				SyncReadback->EnqueueCopy(RHICmdList, Buffer, DataSize);
				RHICmdList.SubmitCommandsAndFlushGPU();
				RHICmdList.BlockUntilGPUIdle();

				if (SyncReadback->IsReady())
				{
					const void* ReadbackData = SyncReadback->Lock(DataSize);
					if (ReadbackData)
					{
						FMemory::Memcpy(DestPtr, ReadbackData, DataSize);
						bSuccess = true;
					}
					SyncReadback->Unlock();
				}
			}
		);
		FlushRenderingCommands();

		if (!bSuccess)
		{
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT("GetParticlesBySourceID: Sync readback failed"));
			ParticleBuffer.Empty();
			Count = 0;
		}

		delete SyncReadback;
	}

	if (Count == 0)
	{
		return false;
	}

	// Filter by SourceID
	for (const FGPUFluidParticle& GPUParticle : ParticleBuffer)
	{
		if (GPUParticle.SourceID != SourceID)
		{
			continue;
		}

		FFluidParticle OutParticle;

		FVector NewPosition = FVector(GPUParticle.Position);
		FVector NewVelocity = FVector(GPUParticle.Velocity);

		const float MaxValidValue = 1000000.0f;
		bool bValidPosition = !NewPosition.ContainsNaN() && NewPosition.GetAbsMax() < MaxValidValue;
		bool bValidVelocity = !NewVelocity.ContainsNaN() && NewVelocity.GetAbsMax() < MaxValidValue;

		if (bValidPosition)
		{
			OutParticle.Position = NewPosition;
			OutParticle.PredictedPosition = FVector(GPUParticle.PredictedPosition);
		}

		if (bValidVelocity)
		{
			OutParticle.Velocity = NewVelocity;
		}

		OutParticle.Mass = FMath::IsFinite(GPUParticle.Mass) ? GPUParticle.Mass : 1.0f;
		OutParticle.Density = FMath::IsFinite(GPUParticle.Density) ? GPUParticle.Density : 0.0f;
		OutParticle.Lambda = FMath::IsFinite(GPUParticle.Lambda) ? GPUParticle.Lambda : 0.0f;
		OutParticle.ParticleID = GPUParticle.ParticleID;
		OutParticle.SourceID = GPUParticle.SourceID;

		OutParticle.bIsAttached = (GPUParticle.Flags & EGPUParticleFlags::IsAttached) != 0;
		OutParticle.bIsSurfaceParticle = (GPUParticle.Flags & EGPUParticleFlags::IsSurface) != 0;
		OutParticle.bIsCoreParticle = (GPUParticle.Flags & EGPUParticleFlags::IsCore) != 0;
		OutParticle.bJustDetached = (GPUParticle.Flags & EGPUParticleFlags::JustDetached) != 0;
		OutParticle.bNearGround = (GPUParticle.Flags & EGPUParticleFlags::NearGround) != 0;
		OutParticle.bNearBoundary = (GPUParticle.Flags & EGPUParticleFlags::NearBoundary) != 0;

		if (GPUParticle.NeighborCount > 0)
		{
			OutParticle.NeighborIndices.SetNum(GPUParticle.NeighborCount);
		}

		OutParticles.Add(MoveTemp(OutParticle));
	}

	return OutParticles.Num() > 0;
}

//=============================================================================
// Collision System (Delegated to FGPUCollisionManager)
//=============================================================================

void FGPUFluidSimulator::AddBoundsCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (CollisionManager.IsValid())
	{
		CollisionManager->AddBoundsCollisionPass(GraphBuilder, ParticlesUAV, CurrentParticleCount, Params);
	}
}

void FGPUFluidSimulator::AddPrimitiveCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (CollisionManager.IsValid())
	{
		CollisionManager->AddPrimitiveCollisionPass(GraphBuilder, ParticlesUAV, CurrentParticleCount, Params);
	}
}

void FGPUFluidSimulator::AddHeightmapCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (CollisionManager.IsValid())
	{
		CollisionManager->AddHeightmapCollisionPass(GraphBuilder, ParticlesUAV, CurrentParticleCount, Params);
	}
}

void FGPUFluidSimulator::AllocateCollisionFeedbackBuffers(FRHICommandListImmediate& RHICmdList)
{
	if (CollisionManager.IsValid())
	{
		CollisionManager->AllocateCollisionFeedbackBuffers(RHICmdList);
	}
}

void FGPUFluidSimulator::ReleaseCollisionFeedbackBuffers()
{
	// Handled by CollisionManager::Release()
}

void FGPUFluidSimulator::ProcessCollisionFeedbackReadback(FRHICommandListImmediate& RHICmdList)
{
	if (CollisionManager.IsValid())
	{
		CollisionManager->ProcessCollisionFeedbackReadback(RHICmdList);
	}
}

void FGPUFluidSimulator::ProcessColliderContactCountReadback(FRHICommandListImmediate& RHICmdList)
{
	if (CollisionManager.IsValid())
	{
		CollisionManager->ProcessColliderContactCountReadback(RHICmdList);
	}
}

//=============================================================================
// Anisotropy Readback (Async GPU→CPU for Ellipsoid HISM Shadows)
//=============================================================================

/**
 * @brief Allocate anisotropy readback objects for async GPU→CPU transfer.
 * @param RHICmdList RHI command list.
 */
void FGPUFluidSimulator::AllocateAnisotropyReadbackObjects(FRHICommandListImmediate& RHICmdList)
{
	for (int32 i = 0; i < NUM_ANISOTROPY_READBACK_BUFFERS; ++i)
	{
		AnisotropyReadbackFrameNumbers[i] = 0;
		AnisotropyReadbackParticleCounts[i] = 0;

		// Allocate anisotropy readback objects (3 buffers per frame: Axis1, Axis2, Axis3)
		for (int32 j = 0; j < 3; ++j)
		{
			if (AnisotropyReadbacks[i][j] == nullptr)
			{
				AnisotropyReadbacks[i][j] = new FRHIGPUBufferReadback(
					*FString::Printf(TEXT("AnisotropyReadback_%d_Axis%d"), i, j + 1));
			}
		}
	}

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("Anisotropy readback objects allocated (NumBuffers=%d)"), NUM_ANISOTROPY_READBACK_BUFFERS);
}

/**
 * @brief Release anisotropy readback objects.
 */
void FGPUFluidSimulator::ReleaseAnisotropyReadbackObjects()
{
	for (int32 i = 0; i < NUM_ANISOTROPY_READBACK_BUFFERS; ++i)
	{
		AnisotropyReadbackFrameNumbers[i] = 0;
		AnisotropyReadbackParticleCounts[i] = 0;

		// Release anisotropy readback objects
		for (int32 j = 0; j < 3; ++j)
		{
			if (AnisotropyReadbacks[i][j] != nullptr)
			{
				delete AnisotropyReadbacks[i][j];
				AnisotropyReadbacks[i][j] = nullptr;
			}
		}
	}
	AnisotropyReadbackWriteIndex = 0;
	ReadyShadowAnisotropyAxis1.Empty();
	ReadyShadowAnisotropyAxis2.Empty();
	ReadyShadowAnisotropyAxis3.Empty();
	ReadyShadowAnisotropyFrame.store(0);
}

/**
 * @brief Get shadow positions for HISM shadow instances (non-blocking).
 * @param OutPositions Output array of particle positions (FVector).
 * @return true if valid positions were retrieved.
 */
bool FGPUFluidSimulator::GetShadowPositions(TArray<FVector>& OutPositions) const
{
	if (ReadyShadowPositionsFrame.load() == 0 || ReadyShadowPositions.Num() == 0)
	{
		OutPositions.Empty();
		return false;
	}

	FScopeLock Lock(&const_cast<FCriticalSection&>(BufferLock));

	const int32 Count = ReadyShadowPositions.Num();
	OutPositions.SetNumUninitialized(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		OutPositions[i] = FVector(ReadyShadowPositions[i]);
	}

	return true;
}

/**
 * @brief Get shadow positions and velocities for prediction.
 * @param OutPositions Output array of particle positions.
 * @param OutVelocities Output array of particle velocities.
 * @return true if valid data was retrieved.
 */
bool FGPUFluidSimulator::GetShadowPositionsAndVelocities(TArray<FVector>& OutPositions, TArray<FVector>& OutVelocities) const
{
	if (ReadyShadowPositionsFrame.load() == 0 || ReadyShadowPositions.Num() == 0)
	{
		OutPositions.Empty();
		OutVelocities.Empty();
		return false;
	}

	FScopeLock Lock(&const_cast<FCriticalSection&>(BufferLock));

	// Defensive check: ensure velocity array matches position array size
	if (ReadyShadowVelocities.Num() != ReadyShadowPositions.Num())
	{
		OutPositions.Empty();
		OutVelocities.Empty();
		return false;
	}

	const int32 Count = ReadyShadowPositions.Num();
	OutPositions.SetNumUninitialized(Count);
	OutVelocities.SetNumUninitialized(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		OutPositions[i] = FVector(ReadyShadowPositions[i]);
		OutVelocities[i] = FVector(ReadyShadowVelocities[i]);
	}

	return true;
}

/**
 * @brief Get neighbor counts for isolation detection (non-blocking).
 * @param OutNeighborCounts Output array of neighbor counts per particle.
 * @return true if valid data was retrieved.
 */
bool FGPUFluidSimulator::GetShadowNeighborCounts(TArray<int32>& OutNeighborCounts) const
{
	if (ReadyShadowPositionsFrame.load() == 0 || ReadyShadowNeighborCounts.Num() == 0)
	{
		OutNeighborCounts.Empty();
		return false;
	}

	FScopeLock Lock(&const_cast<FCriticalSection&>(BufferLock));

	const int32 Count = ReadyShadowNeighborCounts.Num();
	OutNeighborCounts.SetNumUninitialized(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		OutNeighborCounts[i] = static_cast<int32>(ReadyShadowNeighborCounts[i]);
	}

	return true;
}

/**
 * @brief Enqueue anisotropy data copy to readback buffer (non-blocking).
 * @param RHICmdList RHI command list.
 * @param ParticleCount Number of particles to read back.
 */
void FGPUFluidSimulator::EnqueueAnisotropyReadback(FRHICommandListImmediate& RHICmdList, int32 ParticleCount)
{
	if (!bAnisotropyReadbackEnabled.load() || ParticleCount <= 0)
	{
		return;
	}

	// Check if anisotropy buffers are valid
	if (!PersistentAnisotropyAxis1Buffer.IsValid() ||
		!PersistentAnisotropyAxis2Buffer.IsValid() ||
		!PersistentAnisotropyAxis3Buffer.IsValid())
	{
		return;
	}

	// Allocate readback objects if needed
	if (AnisotropyReadbacks[0][0] == nullptr)
	{
		AllocateAnisotropyReadbackObjects(RHICmdList);
	}

	// Get current write index and advance for next frame
	const int32 WriteIdx = AnisotropyReadbackWriteIndex;
	AnisotropyReadbackWriteIndex = (AnisotropyReadbackWriteIndex + 1) % NUM_ANISOTROPY_READBACK_BUFFERS;

	// Calculate copy size (float4 per particle per axis)
	const uint32 RequiredSize = ParticleCount * sizeof(FVector4f);

	// Get RHI buffers
	FRHIBuffer* Axis1RHI = PersistentAnisotropyAxis1Buffer->GetRHI();
	FRHIBuffer* Axis2RHI = PersistentAnisotropyAxis2Buffer->GetRHI();
	FRHIBuffer* Axis3RHI = PersistentAnisotropyAxis3Buffer->GetRHI();

	// Validate all buffers and readbacks exist
	if (!Axis1RHI || !Axis2RHI || !Axis3RHI ||
		!AnisotropyReadbacks[WriteIdx][0] ||
		!AnisotropyReadbacks[WriteIdx][1] ||
		!AnisotropyReadbacks[WriteIdx][2])
	{
		return;
	}

	// Check if ALL buffers have sufficient size BEFORE enqueueing any copies
	const uint32 Axis1Size = Axis1RHI->GetSize();
	const uint32 Axis2Size = Axis2RHI->GetSize();
	const uint32 Axis3Size = Axis3RHI->GetSize();

	if (RequiredSize > Axis1Size || RequiredSize > Axis2Size || RequiredSize > Axis3Size)
	{
		return;
	}

	// All buffers are valid and large enough - now safe to store particle count and enqueue
	AnisotropyReadbackParticleCounts[WriteIdx] = ParticleCount;
	AnisotropyReadbackFrameNumbers[WriteIdx] = GFrameCounterRenderThread;

	RHICmdList.Transition(FRHITransitionInfo(Axis1RHI, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(Axis2RHI, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
	RHICmdList.Transition(FRHITransitionInfo(Axis3RHI, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
	AnisotropyReadbacks[WriteIdx][0]->EnqueueCopy(RHICmdList, Axis1RHI, RequiredSize);
	AnisotropyReadbacks[WriteIdx][1]->EnqueueCopy(RHICmdList, Axis2RHI, RequiredSize);
	AnisotropyReadbacks[WriteIdx][2]->EnqueueCopy(RHICmdList, Axis3RHI, RequiredSize);
	RHICmdList.Transition(FRHITransitionInfo(Axis1RHI, ERHIAccess::CopySrc, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(Axis2RHI, ERHIAccess::CopySrc, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(Axis3RHI, ERHIAccess::CopySrc, ERHIAccess::UAVCompute));
}

/**
 * @brief Process anisotropy readback - check for completion and copy to ready buffer.
 */
void FGPUFluidSimulator::ProcessAnisotropyReadback()
{
	if (!bAnisotropyReadbackEnabled.load())
	{
		return;
	}

	// Search for oldest ready buffer
	int32 ReadIdx = -1;
	uint64 OldestFrame = UINT64_MAX;

	for (int32 i = 0; i < NUM_ANISOTROPY_READBACK_BUFFERS; ++i)
	{
		if (AnisotropyReadbackFrameNumbers[i] > 0 &&
			AnisotropyReadbacks[i][0] != nullptr &&
			AnisotropyReadbacks[i][0]->IsReady() &&
			AnisotropyReadbacks[i][1]->IsReady() &&
			AnisotropyReadbacks[i][2]->IsReady())
		{
			if (AnisotropyReadbackFrameNumbers[i] < OldestFrame)
			{
				OldestFrame = AnisotropyReadbackFrameNumbers[i];
				ReadIdx = i;
			}
		}
	}

	if (ReadIdx < 0)
	{
		return;  // No ready buffers
	}

	const int32 EnqueuedParticleCount = AnisotropyReadbackParticleCounts[ReadIdx];
	if (EnqueuedParticleCount <= 0)
	{
		return;
	}

	const int32 BufferSize = EnqueuedParticleCount * sizeof(FVector4f);
	FScopeLock Lock(&BufferLock);

	// Copy all 3 axes
	const FVector4f* Axis1Data = (const FVector4f*)AnisotropyReadbacks[ReadIdx][0]->Lock(BufferSize);
	if (Axis1Data)
	{
		ReadyShadowAnisotropyAxis1.SetNumUninitialized(EnqueuedParticleCount);
		FMemory::Memcpy(ReadyShadowAnisotropyAxis1.GetData(), Axis1Data, BufferSize);
		AnisotropyReadbacks[ReadIdx][0]->Unlock();
	}

	const FVector4f* Axis2Data = (const FVector4f*)AnisotropyReadbacks[ReadIdx][1]->Lock(BufferSize);
	if (Axis2Data)
	{
		ReadyShadowAnisotropyAxis2.SetNumUninitialized(EnqueuedParticleCount);
		FMemory::Memcpy(ReadyShadowAnisotropyAxis2.GetData(), Axis2Data, BufferSize);
		AnisotropyReadbacks[ReadIdx][1]->Unlock();
	}

	const FVector4f* Axis3Data = (const FVector4f*)AnisotropyReadbacks[ReadIdx][2]->Lock(BufferSize);
	if (Axis3Data)
	{
		ReadyShadowAnisotropyAxis3.SetNumUninitialized(EnqueuedParticleCount);
		FMemory::Memcpy(ReadyShadowAnisotropyAxis3.GetData(), Axis3Data, BufferSize);
		AnisotropyReadbacks[ReadIdx][2]->Unlock();
	}

	ReadyShadowAnisotropyFrame.store(AnisotropyReadbackFrameNumbers[ReadIdx]);

	// Mark buffer as available for next write cycle
	AnisotropyReadbackFrameNumbers[ReadIdx] = 0;
}

//=============================================================================
// Stats/Recycle Readback Implementation (Async GPU→CPU for ParticleID-based operations)
//=============================================================================

void FGPUFluidSimulator::AllocateStatsReadbackObjects(FRHICommandListImmediate& RHICmdList)
{
	for (int32 i = 0; i < NUM_STATS_READBACK_BUFFERS; ++i)
	{
		if (StatsReadbacks[i] == nullptr)
		{
			StatsReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("StatsReadback_%d"), i));
		}
		StatsReadbackFrameNumbers[i] = 0;
		StatsReadbackParticleCounts[i] = 0;
	}

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("Stats readback objects allocated (NumBuffers=%d)"), NUM_STATS_READBACK_BUFFERS);
}

void FGPUFluidSimulator::ReleaseStatsReadbackObjects()
{
	for (int32 i = 0; i < NUM_STATS_READBACK_BUFFERS; ++i)
	{
		if (StatsReadbacks[i] != nullptr)
		{
			delete StatsReadbacks[i];
			StatsReadbacks[i] = nullptr;
		}
		StatsReadbackFrameNumbers[i] = 0;
		StatsReadbackParticleCounts[i] = 0;
	}
	StatsReadbackWriteIndex = 0;
}

void FGPUFluidSimulator::EnqueueStatsReadback(FRHICommandListImmediate& RHICmdList, FRHIBuffer* SourceBuffer, int32 ParticleCount)
{
	if (ParticleCount <= 0 || SourceBuffer == nullptr)
	{
		return;
	}

	// Validate source buffer size
	const uint32 SourceBufferSize = SourceBuffer->GetSize();
	const uint32 RequiredSize = ParticleCount * sizeof(FGPUFluidParticle);
	if (RequiredSize > SourceBufferSize)
	{
		UE_LOG(LogGPUFluidSimulator, Warning,
			TEXT("EnqueueStatsReadback: CopySize (%u) exceeds SourceBuffer size (%u). ParticleCount=%d, Skipping."),
			RequiredSize, SourceBufferSize, ParticleCount);
		return;
	}

	// Allocate readback objects if needed
	if (StatsReadbacks[0] == nullptr)
	{
		AllocateStatsReadbackObjects(RHICmdList);
	}

	// Get current write index and advance for next frame
	const int32 WriteIdx = StatsReadbackWriteIndex;
	StatsReadbackWriteIndex = (StatsReadbackWriteIndex + 1) % NUM_STATS_READBACK_BUFFERS;

	// Enqueue async copy (full particle data)
	const uint32 CopySize = ParticleCount * sizeof(FGPUFluidParticle);
	RHICmdList.Transition(FRHITransitionInfo(SourceBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
	StatsReadbacks[WriteIdx]->EnqueueCopy(RHICmdList, SourceBuffer, CopySize);
	RHICmdList.Transition(FRHITransitionInfo(SourceBuffer, ERHIAccess::CopySrc, ERHIAccess::UAVCompute));
	StatsReadbackFrameNumbers[WriteIdx] = GFrameCounterRenderThread;
	StatsReadbackParticleCounts[WriteIdx] = ParticleCount;
}

void FGPUFluidSimulator::ProcessStatsReadback(FRHICommandListImmediate& RHICmdList)
{
	if (StatsReadbacks[0] == nullptr)
	{
		return;
	}

	// Search for oldest ready buffer
	int32 ReadIdx = -1;
	uint64 OldestFrame = UINT64_MAX;

	for (int32 i = 0; i < NUM_STATS_READBACK_BUFFERS; ++i)
	{
		if (StatsReadbacks[i] != nullptr &&
			StatsReadbackFrameNumbers[i] > 0 &&
			StatsReadbacks[i]->IsReady())
		{
			if (StatsReadbackFrameNumbers[i] < OldestFrame)
			{
				OldestFrame = StatsReadbackFrameNumbers[i];
				ReadIdx = i;
			}
		}
	}

	if (ReadIdx < 0)
	{
		return;  // No ready buffers
	}

	const int32 ParticleCount = StatsReadbackParticleCounts[ReadIdx];
	if (ParticleCount <= 0)
	{
		return;
	}

	// Lock and copy full particle data
	const int32 BufferSize = ParticleCount * sizeof(FGPUFluidParticle);
	const FGPUFluidParticle* ParticleData = (const FGPUFluidParticle*)StatsReadbacks[ReadIdx]->Lock(BufferSize);

	if (ParticleData && ParticleCount > 0)
	{
		// Parallel cache build: local map per chunk → merge
		// Also extract shadow data (Position, Velocity, NeighborCount) in the same pass
		const int32 NumChunks = FMath::Clamp(FPlatformMisc::NumberOfCoresIncludingHyperthreads(), 1, ParticleCount);
		const int32 ChunkSize = (ParticleCount + NumChunks - 1) / NumChunks;

		// Use fixed-size arrays instead of TMap (SourceID is 0~63 range)
		constexpr int32 MaxSources = EGPUParticleSource::MaxSourceCount;  // 64
		TArray<TArray<TArray<int32>>> ChunkSourceArrays;  // [NumChunks][MaxSources][]
		TArray<TArray<int32>> ChunkAllIDs;
		ChunkSourceArrays.SetNum(NumChunks);
		ChunkAllIDs.SetNum(NumChunks);
		for (int32 c = 0; c < NumChunks; ++c)
		{
			ChunkSourceArrays[c].SetNum(MaxSources);
		}

		// Pre-allocate data arrays (will be filled in parallel)
		// Position/SourceID are ALWAYS copied (needed for lightweight despawn API)
		// Velocity only when ISM rendering enabled
		// NeighborCount only when shadow readback enabled
		// Density/VelocityMagnitude/Mass/Flags only when detailed GPU stats enabled
		const bool bNeedShadowData = bShadowReadbackEnabled.load();
		const bool bNeedVelocity = bFullReadbackEnabled.load() || bNeedShadowData;  // ISM or Shadow needs velocity
		const bool bNeedDetailedStats = GetFluidStatsCollector().IsDetailedGPUEnabled();
		TArray<FVector3f> NewPositions;
		TArray<int32> NewSourceIDs;
		TArray<FVector3f> NewVelocities;
		TArray<uint32> NewNeighborCounts;
		TArray<float> NewDensities;
		TArray<float> NewVelocityMagnitudes;
		TArray<float> NewMasses;
		TArray<uint32> NewFlags;
		NewPositions.SetNumUninitialized(ParticleCount);   // Always needed for despawn
		NewSourceIDs.SetNumUninitialized(ParticleCount);   // Always needed for despawn
		if (bNeedVelocity)
		{
			NewVelocities.SetNumUninitialized(ParticleCount);  // ISM rendering
		}
		if (bNeedShadowData)
		{
			NewNeighborCounts.SetNumUninitialized(ParticleCount);
		}
		if (bNeedDetailedStats)
		{
			NewDensities.SetNumUninitialized(ParticleCount);
			NewVelocityMagnitudes.SetNumUninitialized(ParticleCount);
			NewMasses.SetNumUninitialized(ParticleCount);
			NewNeighborCounts.SetNumUninitialized(ParticleCount);
			NewFlags.SetNumUninitialized(ParticleCount);
		}

		ParallelFor(NumChunks, [&](int32 ChunkIndex)
		{
			const int32 StartIdx = ChunkIndex * ChunkSize;
			const int32 EndIdx = FMath::Min(StartIdx + ChunkSize, ParticleCount);
			if (StartIdx >= EndIdx) return;  // Safety check

			auto& LocalSourceArrays = ChunkSourceArrays[ChunkIndex];
			auto& LocalAllIDs = ChunkAllIDs[ChunkIndex];
			LocalAllIDs.Reserve(EndIdx - StartIdx);

			for (int32 i = StartIdx; i < EndIdx; ++i)
			{
				const FGPUFluidParticle& P = ParticleData[i];
				LocalAllIDs.Add(P.ParticleID);

				// Direct array indexing instead of TMap hash lookup
				if (P.SourceID >= 0 && P.SourceID < MaxSources)
				{
					LocalSourceArrays[P.SourceID].Add(P.ParticleID);
				}

				// Position and SourceID always copied (needed for lightweight despawn API)
				NewPositions[i] = P.Position;
				NewSourceIDs[i] = P.SourceID;

				// Velocity only when ISM rendering enabled
				if (bNeedVelocity)
				{
					NewVelocities[i] = P.Velocity;
				}

				// NeighborCount when shadow readback enabled
				if (bNeedShadowData)
				{
					NewNeighborCounts[i] = P.NeighborCount;
				}

				// Detailed GPU stats: all particle data for stats calculation
				if (bNeedDetailedStats)
				{
					NewDensities[i] = P.Density;
					NewVelocityMagnitudes[i] = P.Velocity.Length();
					NewMasses[i] = P.Mass;
					NewNeighborCounts[i] = P.NeighborCount;
					NewFlags[i] = P.Flags;
				}
			}
		}, EParallelForFlags::Unbalanced);

		// Merge (single thread) - array-based, no hash operations, MoveTemp for zero-copy
		TArray<TArray<int32>> NewSourceIDArrays;
		TArray<int32> NewAllParticleIDs;
		{
			SCOPED_DRAW_EVENT(RHICmdList, Merge);
			NewSourceIDArrays.SetNum(MaxSources);
			NewAllParticleIDs.Reserve(ParticleCount);

			for (int32 c = 0; c < NumChunks; ++c)
			{
				NewAllParticleIDs.Append(MoveTemp(ChunkAllIDs[c]));
			}

			// Merge SourceID arrays - chunk-first loop order for cache efficiency
			for (int32 c = 0; c < NumChunks; ++c)
			{
				for (int32 SourceID = 0; SourceID < MaxSources; ++SourceID)
				{
					if (ChunkSourceArrays[c][SourceID].Num() > 0)
					{
						if (NewSourceIDArrays[SourceID].Num() == 0)
						{
							NewSourceIDArrays[SourceID] = MoveTemp(ChunkSourceArrays[c][SourceID]);
						}
						else
						{
							NewSourceIDArrays[SourceID].Append(MoveTemp(ChunkSourceArrays[c][SourceID]));
						}
					}
				}
			}
		}

		// Hold lock briefly and swap
		{
			SCOPED_DRAW_EVENT(RHICmdList, Lock);
			FScopeLock Lock(&BufferLock);
			CachedSourceIDToParticleIDs = MoveTemp(NewSourceIDArrays);
			CachedAllParticleIDs = MoveTemp(NewAllParticleIDs);
			CachedParticlePositions = MoveTemp(NewPositions);    // Always available for despawn API
			CachedParticleSourceIDs = MoveTemp(NewSourceIDs);    // Always available for despawn API

			// Velocity for ISM rendering (lightweight API)
			if (bNeedVelocity)
			{
				CachedParticleVelocities = MoveTemp(NewVelocities);
			}

			bHasValidGPUResults.store(true);

			// NeighborCount only when shadow readback enabled
			if (bNeedShadowData)
			{
				ReadyShadowPositions = CachedParticlePositions;  // Share with despawn data
				ReadyShadowVelocities = CachedParticleVelocities;  // Share with ISM data
				ReadyShadowNeighborCounts = MoveTemp(NewNeighborCounts);
				ReadyShadowPositionsFrame.store(StatsReadbackFrameNumbers[ReadIdx]);
			}
		}

		// Grab DespawnByIDLock outside BufferLock (prevent lock nesting)
		// Cannot reuse built array in CleanupCompletedRequests (MoveTemped)
		// Can just reference CachedAllParticleIDs - safe here since only render thread modifies
		if (SpawnManager.IsValid())
		{
			SpawnManager->CleanupCompletedRequests(CachedAllParticleIDs);
		}

		// Calculate all stats from GPU readback data (only when detailed stats enabled)
		if (bNeedDetailedStats && ParticleCount > 0)
		{
			// Count attached particles
			int32 AttachedCount = 0;
			for (int32 i = 0; i < ParticleCount; ++i)
			{
				if (NewFlags[i] & EGPUParticleFlags::IsAttached)
				{
					++AttachedCount;
				}
			}

			// Calculate velocity stats
			float MinVel = TNumericLimits<float>::Max();
			float MaxVel = TNumericLimits<float>::Lowest();
			double VelSum = 0.0;
			for (int32 i = 0; i < ParticleCount; ++i)
			{
				const float Vel = NewVelocityMagnitudes[i];
				VelSum += Vel;
				MinVel = FMath::Min(MinVel, Vel);
				MaxVel = FMath::Max(MaxVel, Vel);
			}

			// Calculate density stats
			float MinDen = TNumericLimits<float>::Max();
			float MaxDen = TNumericLimits<float>::Lowest();
			double DenSum = 0.0;
			for (int32 i = 0; i < ParticleCount; ++i)
			{
				const float Den = NewDensities[i];
				DenSum += Den;
				MinDen = FMath::Min(MinDen, Den);
				MaxDen = FMath::Max(MaxDen, Den);
			}

			// Calculate neighbor stats
			int32 MinNeighbor = TNumericLimits<int32>::Max();
			int32 MaxNeighbor = TNumericLimits<int32>::Lowest();
			double NeighborSum = 0.0;
			for (int32 i = 0; i < ParticleCount; ++i)
			{
				const int32 Neighbor = static_cast<int32>(NewNeighborCounts[i]);
				NeighborSum += Neighbor;
				MinNeighbor = FMath::Min(MinNeighbor, Neighbor);
				MaxNeighbor = FMath::Max(MaxNeighbor, Neighbor);
			}

			// Update stats collector - basic stats
			auto& Collector = GetFluidStatsCollector();
			auto& Stats = const_cast<FKawaiiFluidSimulationStats&>(Collector.GetStats());

			Stats.ParticleCount = ParticleCount;
			Stats.ActiveParticleCount = ParticleCount - AttachedCount;
			Stats.AttachedParticleCount = AttachedCount;

			Stats.AvgVelocity = static_cast<float>(VelSum / ParticleCount);
			Stats.MinVelocity = MinVel;
			Stats.MaxVelocity = MaxVel;

			Stats.AvgDensity = static_cast<float>(DenSum / ParticleCount);
			Stats.MinDensity = MinDen;
			Stats.MaxDensity = MaxDen;

			// Density error calculation
			const float RestDensity = Stats.RestDensity;
			if (RestDensity > 0.001f)
			{
				Stats.DensityError = FMath::Abs(Stats.AvgDensity - RestDensity) / RestDensity * 100.0f;
			}

			Stats.AvgNeighborCount = static_cast<float>(NeighborSum / ParticleCount);
			Stats.MinNeighborCount = MinNeighbor;
			Stats.MaxNeighborCount = MaxNeighbor;

			Stats.bIsGPUSimulation = true;

			// Calculate stability metrics
			Collector.CalculateStabilityMetrics(
				NewDensities.GetData(),
				NewVelocityMagnitudes.GetData(),
				NewMasses.GetData(),
				ParticleCount,
				RestDensity);
		}
	}

	StatsReadbacks[ReadIdx]->Unlock();

	// Mark buffer as available for next write cycle
	StatsReadbackFrameNumbers[ReadIdx] = 0;
}

/**
 * @brief Get shadow data with anisotropy for ellipsoid HISM shadows.
 * @param OutPositions Output array of particle positions.
 * @param OutVelocities Output array of particle velocities.
 * @param OutAnisotropyAxis1 Output array of first ellipsoid axis (xyz=dir, w=scale).
 * @param OutAnisotropyAxis2 Output array of second ellipsoid axis.
 * @param OutAnisotropyAxis3 Output array of third ellipsoid axis.
 * @return true if valid data was retrieved.
 */
bool FGPUFluidSimulator::GetShadowDataWithAnisotropy(
	TArray<FVector>& OutPositions,
	TArray<FVector>& OutVelocities,
	TArray<FVector4>& OutAnisotropyAxis1,
	TArray<FVector4>& OutAnisotropyAxis2,
	TArray<FVector4>& OutAnisotropyAxis3) const
{
	// First get positions and velocities
	if (!GetShadowPositionsAndVelocities(OutPositions, OutVelocities))
	{
		OutAnisotropyAxis1.Empty();
		OutAnisotropyAxis2.Empty();
		OutAnisotropyAxis3.Empty();
		return false;
	}

	FScopeLock Lock(&const_cast<FCriticalSection&>(BufferLock));

	const int32 Count = ReadyShadowPositions.Num();

	// Check if anisotropy data is available and matches position count
	if (ReadyShadowAnisotropyAxis1.Num() != Count ||
		ReadyShadowAnisotropyAxis2.Num() != Count ||
		ReadyShadowAnisotropyAxis3.Num() != Count)
	{
		// DEBUG: Log count mismatch - disabled for performance
		// static int32 MismatchLogCounter = 0;
		// if (++MismatchLogCounter % 10 == 1)
		// {
		// 	UE_LOG(LogGPUFluidSimulator, Warning,
		// 		TEXT("[ANISO_MISMATCH] Position=%d, Aniso1=%d, Aniso2=%d, Aniso3=%d → Using default W=1.0"),
		// 		Count, ReadyShadowAnisotropyAxis1.Num(), ReadyShadowAnisotropyAxis2.Num(), ReadyShadowAnisotropyAxis3.Num());
		// }

		// Anisotropy not available - return default (uniform sphere)
		OutAnisotropyAxis1.SetNumUninitialized(Count);
		OutAnisotropyAxis2.SetNumUninitialized(Count);
		OutAnisotropyAxis3.SetNumUninitialized(Count);

		for (int32 i = 0; i < Count; ++i)
		{
			OutAnisotropyAxis1[i] = FVector4(1.0, 0.0, 0.0, 1.0);
			OutAnisotropyAxis2[i] = FVector4(0.0, 1.0, 0.0, 1.0);
			OutAnisotropyAxis3[i] = FVector4(0.0, 0.0, 1.0, 1.0);
		}
		return true;
	}

	// Copy anisotropy data
	OutAnisotropyAxis1.SetNumUninitialized(Count);
	OutAnisotropyAxis2.SetNumUninitialized(Count);
	OutAnisotropyAxis3.SetNumUninitialized(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		OutAnisotropyAxis1[i] = FVector4(ReadyShadowAnisotropyAxis1[i]);
		OutAnisotropyAxis2[i] = FVector4(ReadyShadowAnisotropyAxis2[i]);
		OutAnisotropyAxis3[i] = FVector4(ReadyShadowAnisotropyAxis3[i]);
	}

	return true;
}
