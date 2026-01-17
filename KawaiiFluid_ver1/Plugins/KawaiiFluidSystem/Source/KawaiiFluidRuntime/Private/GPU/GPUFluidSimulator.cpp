// Copyright KawaiiFluid Team. All Rights Reserved.

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/FluidAnisotropyComputeShader.h"
#include "GPU/Managers/GPUZOrderSortManager.h"
#include "GPU/Managers/GPUBoundarySkinningManager.h"
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

	// Initialize StreamCompactionManager
	StreamCompactionManager = MakeUnique<FGPUStreamCompactionManager>();
	StreamCompactionManager->Initialize(InMaxParticleCount);

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

	// Release StreamCompactionManager
	if (StreamCompactionManager.IsValid())
	{
		StreamCompactionManager->Release();
		StreamCompactionManager.Reset();
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

	// Release Shadow Readback objects
	ReleaseShadowReadbackObjects();

	bIsInitialized = false;
	MaxParticleCount = 0;
	CurrentParticleCount = 0;
	bHasValidGPUResults.store(false);
	ReadbackGPUParticles.Empty();
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

	// Resize cached array
	CachedGPUParticles.SetNumUninitialized(NewCapacity);

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

	// Check if we have particles OR pending spawn requests
	// (Allow simulation to start with spawn requests even if CurrentParticleCount == 0)
	const bool bHasPendingSpawns = SpawnManager.IsValid() && SpawnManager->HasPendingSpawnRequests();
	if (CurrentParticleCount == 0 && !bHasPendingSpawns)
	{
		return;
	}

	FGPUFluidSimulator* Self = this;
	FGPUFluidSimulationParams ParamsCopy = Params;

	// =====================================================
	// PRE-SIMULATION: Check IsReady() and Lock readback buffers (2 frames ago)
	// Using FRHIGPUBufferReadback - only Lock when IsReady() == true (no flush!)
	// =====================================================
	ENQUEUE_RENDER_COMMAND(GPUFluidReadback)(
		[Self](FRHICommandListImmediate& RHICmdList)
		{
			// Process collision feedback readback (reads from readback enqueued 2 frames ago)
			// Delegated to CollisionFeedbackManager
			Self->ProcessCollisionFeedbackReadback(RHICmdList);

			// Process collider contact count readback (reads from readback enqueued 2 frames ago)
			// Delegated to CollisionFeedbackManager
			Self->ProcessColliderContactCountReadback(RHICmdList);
		}
	);

	// =====================================================
	// MAIN SIMULATION: Execute GPU simulation and EnqueueCopy to readback buffers
	// Lock is NOT here - it's in the previous render command (only when IsReady)
	// =====================================================
	ENQUEUE_RENDER_COMMAND(GPUFluidSimulate)(
		[Self, ParamsCopy](FRHICommandListImmediate& RHICmdList)
		{
			// Limit logging to first 10 frames
			static int32 RenderFrameCounter = 0;
			const bool bLogThisFrame = (RenderFrameCounter++ < 10);

			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RENDER COMMAND START (Frame %d)"), RenderFrameCounter);

			// Note: Don't reset PersistentParticleBuffer here - it breaks Phase 2 GPU→GPU rendering
			// The buffer is maintained across frames for renderer access

			// Build and execute RDG
			FRDGBuilder GraphBuilder(RHICmdList);
			Self->SimulateSubstep_RDG(GraphBuilder, ParamsCopy);

			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RDG EXECUTE START"));
			GraphBuilder.Execute();
			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RDG EXECUTE COMPLETE"));

			// =====================================================
			// Phase 2: Conditional readback for detailed stats only
			// Normal mode: GPU buffer is source of truth (no CPU readback)
			// Detailed stats mode: Perform readback for velocity/density analysis
			// =====================================================

			// Check if any readback is needed (detailed stats OR debug visualization)
			const bool bNeedReadback = GetFluidStatsCollector().IsAnyReadbackNeeded();

			if (bNeedReadback && Self->StagingBufferRHI.IsValid() && Self->PersistentParticleBuffer.IsValid())
			{
				const int32 ParticleCount = Self->CurrentParticleCount;
				const int32 CopySize = ParticleCount * sizeof(FGPUFluidParticle);
				const uint32 SourceBufferSize = Self->PersistentParticleBuffer->GetRHI()->GetSize();

				if (ParticleCount > 0 && CopySize > 0 && static_cast<uint32>(CopySize) <= SourceBufferSize)
				{
					// Transition persistent buffer for copy
					RHICmdList.Transition(FRHITransitionInfo(
						Self->PersistentParticleBuffer->GetRHI(),
						ERHIAccess::UAVCompute,
						ERHIAccess::CopySrc));

					// Copy from GPU buffer to staging buffer
					RHICmdList.CopyBufferRegion(
						Self->StagingBufferRHI,
						0,
						Self->PersistentParticleBuffer->GetRHI(),
						0,
						CopySize);

					// Read from staging buffer to CPU
					{
						FScopeLock Lock(&Self->BufferLock);
						Self->ReadbackGPUParticles.SetNumUninitialized(ParticleCount);

						FGPUFluidParticle* DataPtr = (FGPUFluidParticle*)RHICmdList.LockBuffer(
							Self->StagingBufferRHI, 0, CopySize, RLM_ReadOnly);
						FMemory::Memcpy(Self->ReadbackGPUParticles.GetData(), DataPtr, CopySize);
						RHICmdList.UnlockBuffer(Self->StagingBufferRHI);
					}

					// Transition back for next frame's compute
					RHICmdList.Transition(FRHITransitionInfo(
						Self->PersistentParticleBuffer->GetRHI(),
						ERHIAccess::CopySrc,
						ERHIAccess::UAVCompute));

					if (bLogThisFrame)
					{
						UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> READBACK: Copied %d particles for detailed stats"), ParticleCount);
					}
				}
			}

			// =====================================================
			// Phase 3: Collision Feedback Readback (Particle -> Player Interaction)
			// Delegated to FGPUCollisionFeedbackManager
			// =====================================================
			if (Self->CollisionManager.IsValid() && Self->CollisionManager->GetFeedbackManager())
			{
				Self->CollisionManager->GetFeedbackManager()->EnqueueReadbackCopy(RHICmdList);
			}

			// =====================================================
			// Phase 4: Shadow Position Readback (for HISM Shadow Instances)
			// Async GPU→CPU using FRHIGPUBufferReadback (no stall)
			// =====================================================
			if (Self->bShadowReadbackEnabled.load() && Self->PersistentParticleBuffer.IsValid())
			{
				// First, process any previously completed readback
				Self->ProcessShadowReadback();
				Self->ProcessAnisotropyReadback();

				// Then enqueue new readback for this frame
				Self->EnqueueShadowPositionReadback(RHICmdList,
					Self->PersistentParticleBuffer->GetRHI(),
					Self->CurrentParticleCount);

				// Enqueue anisotropy readback if enabled
				if (Self->bAnisotropyReadbackEnabled.load())
				{
					Self->EnqueueAnisotropyReadback(RHICmdList, Self->CurrentParticleCount);
				}
			}

			// Mark that we have valid GPU results (buffer is ready for rendering)
			Self->bHasValidGPUResults.store(true);

			// Update PersistentParticleCount AFTER SimulateSubstep_RDG
			// CurrentParticleCount is updated inside SimulateSubstep_RDG during spawn
			Self->PersistentParticleCount.store(Self->CurrentParticleCount);

			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> AFTER EXECUTE: PersistentBuffer Valid=%s, PersistentCount=%d"),
				Self->PersistentParticleBuffer.IsValid() ? TEXT("YES") : TEXT("NO"),
				Self->PersistentParticleCount.load());
		}
	);
}

void FGPUFluidSimulator::SimulateSubstep_RDG(FRDGBuilder& GraphBuilder, const FGPUFluidSimulationParams& Params)
{
	// =====================================================
	// Phase 1: Swap spawn requests (double buffer for thread safety)
	// =====================================================
	if (SpawnManager.IsValid())
	{
		SpawnManager->SwapBuffers();
	}

	const bool bHasSpawnRequests = SpawnManager.IsValid() && SpawnManager->HasActiveRequests();
	const int32 SpawnCount = SpawnManager.IsValid() ? SpawnManager->GetActiveRequestCount() : 0;

	// Allow simulation to start even with 0 particles if we have spawn requests
	if (CurrentParticleCount == 0 && !bHasSpawnRequests)
	{
		return;
	}

	// Need either cached particles (for full upload), persistent buffer (for reuse/append), or spawn requests
	if (CachedGPUParticles.Num() == 0 && !PersistentParticleBuffer.IsValid() && !bHasSpawnRequests)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluidSimulation (Particles: %d, Spawning: %d)", CurrentParticleCount, SpawnCount);

	// =====================================================
	// Phase 1: Prepare Particle Buffer (Spawn, Upload, Reuse)
	// =====================================================
	FRDGBufferRef ParticleBuffer = PrepareParticleBuffer(GraphBuilder, Params, SpawnCount);
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
	// Phase 2: Build Spatial Structures (Predict -> Extract -> Sort -> Hash)
	// =====================================================
	FSimulationSpatialData SpatialData = BuildSpatialStructures(
		GraphBuilder, 
		ParticleBuffer, 
		ParticlesSRVLocal, 
		ParticlesUAVLocal, 
		PositionsSRVLocal, 
		PositionsUAVLocal, 
		Params);

	// =====================================================
	// Phase 3: Physics Solver (Density, Pressure)
	// =====================================================
	ExecutePhysicsSolver(GraphBuilder, ParticlesUAVLocal, SpatialData, Params);

	// =====================================================
	// Phase 4: Collision & Adhesion
	// =====================================================
	ExecuteCollisionAndAdhesion(GraphBuilder, ParticlesUAVLocal, SpatialData, Params);

	// =====================================================
	// Phase 5: Post-Simulation (Viscosity, Finalize, Anisotropy)
	// =====================================================
	ExecutePostSimulation(GraphBuilder, ParticleBuffer, ParticlesUAVLocal, SpatialData, Params);

	// =====================================================
	// Phase 6: Extract Persistent Buffers
	// =====================================================
	ExtractPersistentBuffers(GraphBuilder, ParticleBuffer, SpatialData);
}

FRDGBufferRef FGPUFluidSimulator::PrepareParticleBuffer(
	FRDGBuilder& GraphBuilder,
	const FGPUFluidSimulationParams& Params,
	int32 SpawnCount)
{
	// =====================================================
	// Phase 1: Initialize Variables
	// Calculate expected counts and determine operation mode
	// =====================================================
	const int32 ExpectedParticleCount = CurrentParticleCount + SpawnCount;
	FRDGBufferRef ParticleBuffer = nullptr;

	const bool bHasSpawnRequests = SpawnCount > 0;
	const bool bFirstSpawnOnly = bHasSpawnRequests && CurrentParticleCount == 0 && !PersistentParticleBuffer.IsValid();

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

	if (bShouldLog)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("=== BUFFER PATH DEBUG (Frame %d) ==="), DebugFrameCounter);
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bFirstSpawnOnly: %s"), bFirstSpawnOnly ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bHasSpawnRequests: %s"), bHasSpawnRequests ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  CurrentParticleCount: %d, SpawnCount: %d"), CurrentParticleCount, SpawnCount);
	}

	// =====================================================
	// Phase 2: Buffer Preparation (Select Path)
	// Choose appropriate buffer strategy based on current state
	// =====================================================

	// =====================================================
	// PATH 1: First Spawn (GPU Direct Creation)
	// No existing particles, create new buffer and spawn directly on GPU
	// =====================================================
	if (bFirstSpawnOnly)
	{
		const int32 BufferCapacity = FMath::Min(SpawnCount, MaxParticleCount);
		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		TArray<uint32> InitialCounterData;
		InitialCounterData.Add(0);
		FRDGBufferRef CounterBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("GPUFluidParticleCounter"), sizeof(uint32), 1, InitialCounterData.GetData(), sizeof(uint32), ERDGInitialDataFlags::None);
		FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(CounterBuffer);

		FRDGBufferUAVRef ParticleUAVForSpawn = GraphBuilder.CreateUAV(ParticleBuffer);
		SpawnManager->AddSpawnParticlesPass(GraphBuilder, ParticleUAVForSpawn, CounterUAV, MaxParticleCount);

		CurrentParticleCount = FMath::Min(SpawnCount, MaxParticleCount);
		PreviousParticleCount = CurrentParticleCount;
		bNeedsFullUpload = false;
		SpawnManager->ClearActiveRequests();
	}
	// =====================================================
	// PATH 2: Append Spawn (GPU Spawning with Existing Particles)
	// Copy existing particles to new buffer, then spawn additional particles on GPU
	// =====================================================
	else if (bHasSpawnRequests && PersistentParticleBuffer.IsValid())
	{
		const int32 ExistingCount = CurrentParticleCount;
		const int32 TotalCount = ExpectedParticleCount;
		const int32 BufferCapacity = FMath::Min(TotalCount, MaxParticleCount);
		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		FRDGBufferRef ExistingBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleBuffer, TEXT("GPUFluidParticlesOld"));
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FCopyParticlesCS> CopyShader(ShaderMap);
			FCopyParticlesCS::FParameters* CopyParams = GraphBuilder.AllocParameters<FCopyParticlesCS::FParameters>();
			CopyParams->SourceParticles = GraphBuilder.CreateSRV(ExistingBuffer);
			CopyParams->DestParticles = GraphBuilder.CreateUAV(ParticleBuffer);
			CopyParams->SourceOffset = 0;
			CopyParams->DestOffset = 0;
			CopyParams->CopyCount = ExistingCount;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPUFluid::CopyExistingForSpawn(%d)", ExistingCount), CopyShader, CopyParams, FIntVector(FMath::DivideAndRoundUp(ExistingCount, FCopyParticlesCS::ThreadGroupSize), 1, 1));
		}

		TArray<uint32> InitialCounterData;
		InitialCounterData.Add(static_cast<uint32>(ExistingCount));
		FRDGBufferRef CounterBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("GPUFluidParticleCounter"), sizeof(uint32), 1, InitialCounterData.GetData(), sizeof(uint32), ERDGInitialDataFlags::None);
		FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(CounterBuffer);

		FRDGBufferUAVRef ParticleUAVForSpawn = GraphBuilder.CreateUAV(ParticleBuffer);
		SpawnManager->AddSpawnParticlesPass(GraphBuilder, ParticleUAVForSpawn, CounterUAV, MaxParticleCount);

		CurrentParticleCount = FMath::Min(ExpectedParticleCount, MaxParticleCount);
		PreviousParticleCount = CurrentParticleCount;
		SpawnManager->ClearActiveRequests();
	}
	// =====================================================
	// PATH 3: Reuse Buffer (No Changes)
	// No spawn requests, reuse existing persistent buffer as-is
	// =====================================================
	else
	{
		if (!PersistentParticleBuffer.IsValid()) return nullptr;
		ParticleBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleBuffer, TEXT("GPUFluidParticles"));
	}

	// =====================================================
	// Phase 3: ID-Based Despawn Processing
	// Mark dead particles by ParticleID matching (binary search on GPU)
	// =====================================================
	if (SpawnManager.IsValid() && SpawnManager->HasPendingDespawnByIDRequests())
	{
		SpawnManager->SwapDespawnByIDBuffers();
		SpawnManager->AddDespawnByIDPass(GraphBuilder, ParticleBuffer, CurrentParticleCount);
	}

	// =====================================================
	// Phase 4: Async Readback
	// Retrieve dead particle count from GPU and update CurrentParticleCount
	// ProcessAsyncReadback() returns:
	//   > 0: actual dead particle count from GPU readback
	//   -1:  readback not ready yet (must NOT be treated as dead count!)
	// =====================================================
	if (SpawnManager.IsValid())
	{
		int32 DeadCount = SpawnManager->ProcessAsyncReadback();
		if (DeadCount > 0)
		{
			CurrentParticleCount -= DeadCount;
			PreviousParticleCount = CurrentParticleCount;
		}
	}

	if (SpawnManager.IsValid()) SpawnManager->ClearActiveRequests();
	return ParticleBuffer;
}

FGPUFluidSimulator::FSimulationSpatialData FGPUFluidSimulator::BuildSpatialStructures(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef& InOutParticleBuffer,
	FRDGBufferSRVRef& OutParticlesSRV,
	FRDGBufferUAVRef& OutParticlesUAV,
	FRDGBufferSRVRef& OutPositionsSRV,
	FRDGBufferUAVRef& OutPositionsUAV,
	const FGPUFluidSimulationParams& Params)
{
	FSimulationSpatialData SpatialData;

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

		FRDGBufferRef SortedParticleBuffer = ExecuteZOrderSortingPipeline(
			GraphBuilder, InOutParticleBuffer,
			CellStartUAVLocal, SpatialData.CellStartSRV,
			CellEndUAVLocal, SpatialData.CellEndSRV,
			Params);

		// Replace particle buffer with sorted version
		InOutParticleBuffer = SortedParticleBuffer;
		OutParticlesUAV = GraphBuilder.CreateUAV(InOutParticleBuffer);
		OutParticlesSRV = GraphBuilder.CreateSRV(InOutParticleBuffer);

		// Re-extract positions from sorted particles
		AddExtractPositionsPass(GraphBuilder, OutParticlesSRV, OutPositionsUAV, CurrentParticleCount, true);

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

void FGPUFluidSimulator::ExecutePhysicsSolver(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FSimulationSpatialData& SpatialData,
	const FGPUFluidSimulationParams& Params)
{
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

	for (int32 i = 0; i < Params.SolverIterations; ++i)
	{
		AddSolveDensityPressurePass(
			GraphBuilder, ParticlesUAV,
			SpatialData.CellCountsSRV, SpatialData.ParticleIndicesSRV,
			SpatialData.CellStartSRV, SpatialData.CellEndSRV,
			NeighborListUAVLocal, NeighborCountsUAVLocal, i, Params, SpatialData);
	}

	// Create SRVs for use in subsequent passes
	SpatialData.NeighborListSRV = GraphBuilder.CreateSRV(SpatialData.NeighborListBuffer);
	SpatialData.NeighborCountsSRV = GraphBuilder.CreateSRV(SpatialData.NeighborCountsBuffer);

	// Queue extraction
	GraphBuilder.QueueBufferExtraction(SpatialData.NeighborListBuffer, &NeighborListBuffer, ERHIAccess::UAVCompute);
	GraphBuilder.QueueBufferExtraction(SpatialData.NeighborCountsBuffer, &NeighborCountsBuffer, ERHIAccess::UAVCompute);
}

void FGPUFluidSimulator::ExecuteCollisionAndAdhesion(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FSimulationSpatialData& SpatialData,
	const FGPUFluidSimulationParams& Params)
{
	AddBoundsCollisionPass(GraphBuilder, ParticlesUAV, Params);
	AddDistanceFieldCollisionPass(GraphBuilder, ParticlesUAV, Params);
	AddPrimitiveCollisionPass(GraphBuilder, ParticlesUAV, Params);

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
	AddFinalizePositionsPass(GraphBuilder, ParticlesUAV, Params);

	AddApplyViscosityPass(GraphBuilder, ParticlesUAV, SpatialData.CellCountsSRV, SpatialData.ParticleIndicesSRV, SpatialData.NeighborListSRV, SpatialData.NeighborCountsSRV, Params, SpatialData);
	AddApplyCohesionPass(GraphBuilder, ParticlesUAV, SpatialData.CellCountsSRV, SpatialData.ParticleIndicesSRV, SpatialData.NeighborListSRV, SpatialData.NeighborCountsSRV, Params);

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

		// DEBUG: Anisotropy 계산 실행 여부 로깅
		static int32 AnisoDebugCounter = 0;
		const bool bWillCompute = (AnisotropyFrameCounter >= UpdateInterval || !PersistentAnisotropyAxis1Buffer.IsValid());
		if (++AnisoDebugCounter % 30 == 1)
		{
			UE_LOG(LogGPUFluidSimulator, Warning,
				TEXT("[ANISO_COMPUTE] UpdateInterval=%d, FrameCounter=%d, WillCompute=%s"),
				UpdateInterval, AnisotropyFrameCounter, bWillCompute ? TEXT("YES") : TEXT("NO"));
		}

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
				AnisotropyParams.AnisotropyScale = CachedAnisotropyParams.AnisotropyScale;
				AnisotropyParams.AnisotropyMin = CachedAnisotropyParams.AnisotropyMin;
				AnisotropyParams.AnisotropyMax = CachedAnisotropyParams.AnisotropyMax;
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

				FFluidAnisotropyPassBuilder::AddAnisotropyPass(GraphBuilder, AnisotropyParams);

				GraphBuilder.QueueBufferExtraction(Axis1Buffer, &PersistentAnisotropyAxis1Buffer, ERHIAccess::SRVCompute);
				GraphBuilder.QueueBufferExtraction(Axis2Buffer, &PersistentAnisotropyAxis2Buffer, ERHIAccess::SRVCompute);
				GraphBuilder.QueueBufferExtraction(Axis3Buffer, &PersistentAnisotropyAxis3Buffer, ERHIAccess::SRVCompute);
			}
		}
	}
}

void FGPUFluidSimulator::ExtractPersistentBuffers(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef ParticleBuffer,
	const FSimulationSpatialData& SpatialData)
{
	GraphBuilder.QueueBufferExtraction(ParticleBuffer, &PersistentParticleBuffer, ERHIAccess::UAVCompute);

	// Only extract legacy hash table buffers when Z-Order sorting is NOT enabled
	// When Z-Order is enabled, CellCountsBuffer/ParticleIndicesBuffer are dummy buffers that weren't produced
	const bool bUseZOrderSorting = ZOrderSortManager.IsValid() && ZOrderSortManager->IsZOrderSortingEnabled();
	if (!bUseZOrderSorting)
	{
		if (SpatialData.CellCountsBuffer) GraphBuilder.QueueBufferExtraction(SpatialData.CellCountsBuffer, &PersistentCellCountsBuffer, ERHIAccess::UAVCompute);
		if (SpatialData.ParticleIndicesBuffer) GraphBuilder.QueueBufferExtraction(SpatialData.ParticleIndicesBuffer, &PersistentParticleIndicesBuffer, ERHIAccess::UAVCompute);
	}
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
	}
}

bool FGPUFluidSimulator::GetReadbackGPUParticles(TArray<FGPUFluidParticle>& OutParticles)
{
	if (!bHasValidGPUResults.load())
	{
		return false;
	}

	FScopeLock Lock(&BufferLock);

	if (ReadbackGPUParticles.Num() == 0)
	{
		return false;
	}

	OutParticles = ReadbackGPUParticles;
	return true;
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

	for (int32 i = 0; i < NumSubsteps; ++i)
	{
		SubstepParams.SubstepIndex = i;
		Simulator->SimulateSubstep(SubstepParams);
	}
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
// Z-Order Sorting (Delegated to FGPUZOrderSortManager)
//=============================================================================

FRDGBufferRef FGPUFluidSimulator::ExecuteZOrderSortingPipeline(
	FRDGBuilder& GraphBuilder, FRDGBufferRef InParticleBuffer,
	FRDGBufferUAVRef& OutCellStartUAV, FRDGBufferSRVRef& OutCellStartSRV,
	FRDGBufferUAVRef& OutCellEndUAV, FRDGBufferSRVRef& OutCellEndSRV,
	const FGPUFluidSimulationParams& Params)
{
	// Check both manager validity AND enabled flag
	if (!ZOrderSortManager.IsValid() || !ZOrderSortManager->IsZOrderSortingEnabled())
	{
		return InParticleBuffer;
	}
	return ZOrderSortManager->ExecuteZOrderSortingPipeline(GraphBuilder, InParticleBuffer,
		OutCellStartUAV, OutCellStartSRV, OutCellEndUAV, OutCellEndSRV, CurrentParticleCount, Params);
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
	OutCPUParticle.bJustDetached = (GPUParticle.Flags & EGPUParticleFlags::JustDetached) != 0;
	OutCPUParticle.bNearGround = (GPUParticle.Flags & EGPUParticleFlags::NearGround) != 0;

	// Note: bIsAttached is not updated from GPU - CPU handles attachment state
}

void FGPUFluidSimulator::UploadParticles(const TArray<FFluidParticle>& CPUParticles)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("UploadParticles: Simulator not initialized"));
		return;
	}

	const int32 NewCount = CPUParticles.Num();
	if (NewCount == 0)
	{
		CurrentParticleCount = 0;
		CachedGPUParticles.Empty();
		return;
	}

	if (NewCount > MaxParticleCount)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("UploadParticles: Particle count (%d) exceeds capacity (%d)"),
			NewCount, MaxParticleCount);
		return;
	}

	FScopeLock Lock(&BufferLock);

	// Store old count for comparison BEFORE updating
	const int32 OldCount = CurrentParticleCount;

	// Determine upload strategy based on persistent buffer state and particle count changes
	const bool bHasPersistentBuffer = PersistentParticleBuffer.IsValid() && OldCount > 0;
	const bool bSameCount = bHasPersistentBuffer && (NewCount == OldCount);
	const bool bCanAppend = bHasPersistentBuffer && (NewCount > OldCount);

	if (bSameCount)
	{
		// Same particle count - NO UPLOAD needed, reuse GPU buffer entirely
		// GPU simulation results are preserved in PersistentParticleBuffer
		NewParticleCount = 0;
		NewParticlesToAppend.Empty();
		// Note: Don't set bNeedsFullUpload = false here, it should already be false

		static int32 ReuseLogCounter = 0;
		if (++ReuseLogCounter % 60 == 0)  // Log every 60 frames
		{
			UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadParticles: Reusing GPU buffer (no upload, %d particles)"), OldCount);
		}
		return;  // Skip upload entirely!
	}
	else if (bCanAppend)
	{
		// Only cache the NEW particles (indices OldCount to NewCount-1)
		const int32 NumNewParticles = NewCount - OldCount;
		NewParticlesToAppend.SetNumUninitialized(NumNewParticles);

		for (int32 i = 0; i < NumNewParticles; ++i)
		{
			NewParticlesToAppend[i] = ConvertToGPU(CPUParticles[OldCount + i]);
		}

		NewParticleCount = NumNewParticles;
		CurrentParticleCount = NewCount;

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadParticles: Appending %d new particles (total: %d)"),
			NumNewParticles, NewCount);
	}
	else
	{
		// Full upload needed: first frame, buffer invalid, or particles reduced
		CachedGPUParticles.SetNumUninitialized(NewCount);

		// Convert particles to GPU format
		for (int32 i = 0; i < NewCount; ++i)
		{
			CachedGPUParticles[i] = ConvertToGPU(CPUParticles[i]);
		}

		// Simulation bounds for Morton code (Z-Order sorting) are set via SetSimulationBounds()
		// from SimulateGPU before this call (preset bounds + component location offset)
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadParticles: Using bounds: Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f)"),
			SimulationBoundsMin.X, SimulationBoundsMin.Y, SimulationBoundsMin.Z,
			SimulationBoundsMax.X, SimulationBoundsMax.Y, SimulationBoundsMax.Z);

		NewParticleCount = 0;
		NewParticlesToAppend.Empty();
		CurrentParticleCount = NewCount;
		bNeedsFullUpload = true;
	}
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

	FScopeLock Lock(&BufferLock);

	// Read from separate readback buffer (not CachedGPUParticles)
	const int32 Count = ReadbackGPUParticles.Num();
	if (Count == 0)
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

	// Debug: Log first particle before conversion
	static int32 DebugFrameCounter = 0;
	if (DebugFrameCounter++ % 60 == 0)
	{
		const FGPUFluidParticle& P = ReadbackGPUParticles[0];
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("DownloadParticles: GPUCount=%d, CPUCount=%d, Readback[0] Pos=(%.2f, %.2f, %.2f)"),
			Count, OutCPUParticles.Num(), P.Position.X, P.Position.Y, P.Position.Z);
	}

	// Update existing particles by matching ParticleID (don't overwrite newly spawned ones)
	// Also track bounds to detect Black Hole Cell potential
	int32 UpdatedCount = 0;
	int32 OutOfBoundsCount = 0;
	const float BoundsMargin = 100.0f;  // Warn if particles within 100 units of bounds edge

	for (int32 i = 0; i < Count; ++i)
	{
		const FGPUFluidParticle& GPUParticle = ReadbackGPUParticles[i];
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

	FScopeLock Lock(&BufferLock);

	// Read from readback buffer
	const int32 Count = ReadbackGPUParticles.Num();
	if (Count == 0)
	{
		return false;
	}

	// Create new particles from GPU data (no ParticleID matching required)
	OutParticles.SetNum(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		const FGPUFluidParticle& GPUParticle = ReadbackGPUParticles[i];
		FFluidParticle& OutParticle = OutParticles[i];

		// Initialize with default values
		OutParticle = FFluidParticle();

		// Convert GPU data to CPU particle
		FVector NewPosition = FVector(GPUParticle.Position);
		FVector NewVelocity = FVector(GPUParticle.Velocity);

		// Validate data
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

		// Unpack flags
		OutParticle.bIsAttached = (GPUParticle.Flags & EGPUParticleFlags::IsAttached) != 0;
		OutParticle.bIsSurfaceParticle = (GPUParticle.Flags & EGPUParticleFlags::IsSurface) != 0;
		OutParticle.bIsCoreParticle = (GPUParticle.Flags & EGPUParticleFlags::IsCore) != 0;
		OutParticle.bJustDetached = (GPUParticle.Flags & EGPUParticleFlags::JustDetached) != 0;
		OutParticle.bNearGround = (GPUParticle.Flags & EGPUParticleFlags::NearGround) != 0;

		// Set neighbor count (resize array so NeighborIndices.Num() returns the count)
		// GPU stores count only, not actual indices (computed on-the-fly during spatial hash queries)
		if (GPUParticle.NeighborCount > 0)
		{
			OutParticle.NeighborIndices.SetNum(GPUParticle.NeighborCount);
		}
	}

	static int32 DebugFrameCounter = 0;
	if (++DebugFrameCounter % 60 == 0)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GetAllGPUParticles: Retrieved %d particles"), Count);
	}

	return true;
}

//=============================================================================
// Stream Compaction API (Delegated to FGPUStreamCompactionManager)
//=============================================================================

void FGPUFluidSimulator::ExecuteAABBFiltering(const TArray<FGPUFilterAABB>& FilterAABBs)
{
	if (!StreamCompactionManager.IsValid() || !bIsInitialized || CurrentParticleCount == 0)
	{
		return;
	}

	// Pass PersistentParticleBuffer and fallback SRV to manager
	// Manager will create proper SRV on render thread from PersistentParticleBuffer if valid
	StreamCompactionManager->ExecuteAABBFiltering(FilterAABBs, CurrentParticleCount, PersistentParticleBuffer, ParticleSRV);
}

bool FGPUFluidSimulator::GetFilteredCandidates(TArray<FGPUCandidateParticle>& OutCandidates)
{
	if (!StreamCompactionManager.IsValid())
	{
		OutCandidates.Empty();
		return false;
	}
	return StreamCompactionManager->GetFilteredCandidates(OutCandidates);
}

void FGPUFluidSimulator::ApplyCorrections(const TArray<FParticleCorrection>& Corrections)
{
	if (!StreamCompactionManager.IsValid() || !bIsInitialized)
	{
		return;
	}
	StreamCompactionManager->ApplyCorrections(Corrections, PersistentParticleBuffer);
}

void FGPUFluidSimulator::ApplyAttachmentUpdates(const TArray<FAttachedParticleUpdate>& Updates)
{
	if (!StreamCompactionManager.IsValid() || !bIsInitialized)
	{
		return;
	}
	StreamCompactionManager->ApplyAttachmentUpdates(Updates, PersistentParticleBuffer);
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

void FGPUFluidSimulator::AddDistanceFieldCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (CollisionManager.IsValid())
	{
		CollisionManager->AddDistanceFieldCollisionPass(GraphBuilder, ParticlesUAV, CurrentParticleCount, Params);
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
// Shadow Position Readback (Async GPU→CPU for HISM Shadow Instances)
//=============================================================================

/**
 * @brief Allocate shadow readback objects for async GPU→CPU transfer.
 * @param RHICmdList RHI command list.
 */
void FGPUFluidSimulator::AllocateShadowReadbackObjects(FRHICommandListImmediate& RHICmdList)
{
	for (int32 i = 0; i < NUM_SHADOW_READBACK_BUFFERS; ++i)
	{
		if (ShadowPositionReadbacks[i] == nullptr)
		{
			ShadowPositionReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("ShadowPositionReadback_%d"), i));
		}
		ShadowReadbackFrameNumbers[i] = 0;
		ShadowReadbackParticleCounts[i] = 0;

		// Allocate anisotropy readback objects (3 buffers per frame: Axis1, Axis2, Axis3)
		for (int32 j = 0; j < 3; ++j)
		{
			if (ShadowAnisotropyReadbacks[i][j] == nullptr)
			{
				ShadowAnisotropyReadbacks[i][j] = new FRHIGPUBufferReadback(
					*FString::Printf(TEXT("ShadowAnisotropyReadback_%d_Axis%d"), i, j + 1));
			}
		}
	}

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("Shadow readback objects allocated (NumBuffers=%d, with Anisotropy)"), NUM_SHADOW_READBACK_BUFFERS);
}

/**
 * @brief Release shadow readback objects.
 */
void FGPUFluidSimulator::ReleaseShadowReadbackObjects()
{
	for (int32 i = 0; i < NUM_SHADOW_READBACK_BUFFERS; ++i)
	{
		if (ShadowPositionReadbacks[i] != nullptr)
		{
			delete ShadowPositionReadbacks[i];
			ShadowPositionReadbacks[i] = nullptr;
		}
		ShadowReadbackFrameNumbers[i] = 0;
		ShadowReadbackParticleCounts[i] = 0;

		// Release anisotropy readback objects
		for (int32 j = 0; j < 3; ++j)
		{
			if (ShadowAnisotropyReadbacks[i][j] != nullptr)
			{
				delete ShadowAnisotropyReadbacks[i][j];
				ShadowAnisotropyReadbacks[i][j] = nullptr;
			}
		}
	}
	ShadowReadbackWriteIndex = 0;
	ReadyShadowPositions.Empty();
	ReadyShadowVelocities.Empty();
	ReadyShadowNeighborCounts.Empty();
	ReadyShadowAnisotropyAxis1.Empty();
	ReadyShadowAnisotropyAxis2.Empty();
	ReadyShadowAnisotropyAxis3.Empty();
	ReadyShadowPositionsFrame.store(0);
}

/**
 * @brief Enqueue shadow position copy to readback buffer (non-blocking).
 * @param RHICmdList RHI command list.
 * @param SourceBuffer Source GPU buffer containing particle data.
 * @param ParticleCount Number of particles to read back.
 */
void FGPUFluidSimulator::EnqueueShadowPositionReadback(FRHICommandListImmediate& RHICmdList, FRHIBuffer* SourceBuffer, int32 ParticleCount)
{
	if (!bShadowReadbackEnabled.load() || ParticleCount <= 0 || SourceBuffer == nullptr)
	{
		return;
	}

	// Validate source buffer size before copying
	const uint32 SourceBufferSize = SourceBuffer->GetSize();
	const uint32 RequiredSize = ParticleCount * sizeof(FGPUFluidParticle);
	if (RequiredSize > SourceBufferSize)
	{
		UE_LOG(LogGPUFluidSimulator, Warning,
			TEXT("EnqueueShadowPositionReadback: CopySize (%u) exceeds SourceBuffer size (%u). ParticleCount=%d, Skipping."),
			RequiredSize, SourceBufferSize, ParticleCount);
		return;
	}

	// Allocate readback objects if needed
	if (ShadowPositionReadbacks[0] == nullptr)
	{
		AllocateShadowReadbackObjects(RHICmdList);
	}

	// Get current write index and advance for next frame
	const int32 WriteIdx = ShadowReadbackWriteIndex;
	ShadowReadbackWriteIndex = (ShadowReadbackWriteIndex + 1) % NUM_SHADOW_READBACK_BUFFERS;

	// Calculate copy size (only positions: FVector3f per particle)
	// Note: We're reading from FGPUFluidParticle buffer, position is at offset 0
	const int32 CopySize = ParticleCount * sizeof(FGPUFluidParticle);

	// Enqueue async copy
	ShadowPositionReadbacks[WriteIdx]->EnqueueCopy(RHICmdList, SourceBuffer, CopySize);
	ShadowReadbackFrameNumbers[WriteIdx] = GFrameCounterRenderThread;
	ShadowReadbackParticleCounts[WriteIdx] = ParticleCount;
}

/**
 * @brief Process shadow readback - check for completion and copy to ready buffer.
 */
void FGPUFluidSimulator::ProcessShadowReadback()
{
	if (!bShadowReadbackEnabled.load() || ShadowPositionReadbacks[0] == nullptr)
	{
		return;
	}

	// Search for oldest ready buffer
	int32 ReadIdx = -1;
	uint64 OldestFrame = UINT64_MAX;

	for (int32 i = 0; i < NUM_SHADOW_READBACK_BUFFERS; ++i)
	{
		if (ShadowPositionReadbacks[i] != nullptr &&
			ShadowReadbackFrameNumbers[i] > 0 &&
			ShadowPositionReadbacks[i]->IsReady())
		{
			if (ShadowReadbackFrameNumbers[i] < OldestFrame)
			{
				OldestFrame = ShadowReadbackFrameNumbers[i];
				ReadIdx = i;
			}
		}
	}

	if (ReadIdx < 0)
	{
		return;  // No ready buffers
	}

	const int32 ParticleCount = ShadowReadbackParticleCounts[ReadIdx];
	if (ParticleCount <= 0)
	{
		return;
	}

	// Lock and copy position data
	const int32 BufferSize = ParticleCount * sizeof(FGPUFluidParticle);
	const FGPUFluidParticle* ParticleData = (const FGPUFluidParticle*)ShadowPositionReadbacks[ReadIdx]->Lock(BufferSize);

	if (ParticleData)
	{
		FScopeLock Lock(&BufferLock);
		ReadyShadowPositions.SetNumUninitialized(ParticleCount);
		ReadyShadowVelocities.SetNumUninitialized(ParticleCount);
		ReadyShadowNeighborCounts.SetNumUninitialized(ParticleCount);

		for (int32 i = 0; i < ParticleCount; ++i)
		{
			ReadyShadowPositions[i] = ParticleData[i].Position;
			ReadyShadowVelocities[i] = ParticleData[i].Velocity;
			ReadyShadowNeighborCounts[i] = ParticleData[i].NeighborCount;
		}

		ReadyShadowPositionsFrame.store(ShadowReadbackFrameNumbers[ReadIdx]);
	}

	ShadowPositionReadbacks[ReadIdx]->Unlock();

	// Store the buffer index so ProcessAnisotropyReadback() can read from the same buffer.
	// This ensures position and anisotropy data are synchronized (same frame).
	LastProcessedShadowReadbackIndex = ReadIdx;

	// Mark buffer as available for next write cycle
	ShadowReadbackFrameNumbers[ReadIdx] = 0;
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

	// Use the same write index as position readback (they are synchronized)
	// Note: ShadowReadbackWriteIndex was already advanced in EnqueueShadowPositionReadback
	// So we use the previous index
	const int32 WriteIdx = (ShadowReadbackWriteIndex + NUM_SHADOW_READBACK_BUFFERS - 1) % NUM_SHADOW_READBACK_BUFFERS;

	// Calculate copy size (float4 per particle per axis)
	const uint32 RequiredSize = ParticleCount * sizeof(FVector4f);

	// Get RHI buffers
	FRHIBuffer* Axis1RHI = PersistentAnisotropyAxis1Buffer->GetRHI();
	FRHIBuffer* Axis2RHI = PersistentAnisotropyAxis2Buffer->GetRHI();
	FRHIBuffer* Axis3RHI = PersistentAnisotropyAxis3Buffer->GetRHI();

	// Validate all buffers and readbacks exist
	if (!Axis1RHI || !Axis2RHI || !Axis3RHI ||
		!ShadowAnisotropyReadbacks[WriteIdx][0] ||
		!ShadowAnisotropyReadbacks[WriteIdx][1] ||
		!ShadowAnisotropyReadbacks[WriteIdx][2])
	{
		return;
	}

	// Check if ALL buffers have sufficient size BEFORE enqueueing any copies
	// This prevents partial copies and ensures ParticleCount matches actual copied data
	const uint32 Axis1Size = Axis1RHI->GetSize();
	const uint32 Axis2Size = Axis2RHI->GetSize();
	const uint32 Axis3Size = Axis3RHI->GetSize();

	if (RequiredSize > Axis1Size || RequiredSize > Axis2Size || RequiredSize > Axis3Size)
	{
		// Buffer too small - don't enqueue and don't update particle count
		// This prevents buffer overrun in ProcessAnisotropyReadback
		return;
	}

	// All buffers are valid and large enough - now safe to store particle count and enqueue
	ShadowAnisotropyReadbackParticleCounts[WriteIdx] = ParticleCount;

	ShadowAnisotropyReadbacks[WriteIdx][0]->EnqueueCopy(RHICmdList, Axis1RHI, RequiredSize);
	ShadowAnisotropyReadbacks[WriteIdx][1]->EnqueueCopy(RHICmdList, Axis2RHI, RequiredSize);
	ShadowAnisotropyReadbacks[WriteIdx][2]->EnqueueCopy(RHICmdList, Axis3RHI, RequiredSize);
}

/**
 * @brief Process anisotropy readback using the same buffer index as position readback.
 *
 * This function reads anisotropy data from the buffer that was just processed
 * by ProcessShadowReadback(), ensuring position and anisotropy data are from
 * the same simulation frame.
 */
void FGPUFluidSimulator::ProcessAnisotropyReadback()
{
	if (!bAnisotropyReadbackEnabled.load())
	{
		return;
	}

	// Read from the same buffer index that ProcessShadowReadback() just processed.
	const int32 ReadIdx = LastProcessedShadowReadbackIndex;
	if (ReadIdx < 0 || ReadIdx >= NUM_SHADOW_READBACK_BUFFERS)
	{
		return;
	}

	// Use the particle count that was stored when the readback was enqueued
	// This prevents buffer overrun when particle count changes between enqueue and process
	const int32 EnqueuedParticleCount = ShadowAnisotropyReadbackParticleCounts[ReadIdx];
	if (EnqueuedParticleCount <= 0)
	{
		return;
	}

	// All 3 axis buffers must be ready
	for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
	{
		if (ShadowAnisotropyReadbacks[ReadIdx][AxisIndex] == nullptr ||
			!ShadowAnisotropyReadbacks[ReadIdx][AxisIndex]->IsReady())
		{
			return;
		}
	}

	const int32 BufferSize = EnqueuedParticleCount * sizeof(FVector4f);
	FScopeLock Lock(&BufferLock);

	// Copy all 3 axes using the enqueued particle count (safe, matches actual buffer size)
	const FVector4f* Axis1Data = (const FVector4f*)ShadowAnisotropyReadbacks[ReadIdx][0]->Lock(BufferSize);
	if (Axis1Data)
	{
		ReadyShadowAnisotropyAxis1.SetNumUninitialized(EnqueuedParticleCount);
		FMemory::Memcpy(ReadyShadowAnisotropyAxis1.GetData(), Axis1Data, BufferSize);
		ShadowAnisotropyReadbacks[ReadIdx][0]->Unlock();
	}

	const FVector4f* Axis2Data = (const FVector4f*)ShadowAnisotropyReadbacks[ReadIdx][1]->Lock(BufferSize);
	if (Axis2Data)
	{
		ReadyShadowAnisotropyAxis2.SetNumUninitialized(EnqueuedParticleCount);
		FMemory::Memcpy(ReadyShadowAnisotropyAxis2.GetData(), Axis2Data, BufferSize);
		ShadowAnisotropyReadbacks[ReadIdx][1]->Unlock();
	}

	const FVector4f* Axis3Data = (const FVector4f*)ShadowAnisotropyReadbacks[ReadIdx][2]->Lock(BufferSize);
	if (Axis3Data)
	{
		ReadyShadowAnisotropyAxis3.SetNumUninitialized(EnqueuedParticleCount);
		FMemory::Memcpy(ReadyShadowAnisotropyAxis3.GetData(), Axis3Data, BufferSize);
		ShadowAnisotropyReadbacks[ReadIdx][2]->Unlock();
	}

	// Invalidate the index to prevent reading the same data again
	LastProcessedShadowReadbackIndex = -1;
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
		// DEBUG: 개수 불일치 로깅
		static int32 MismatchLogCounter = 0;
		if (++MismatchLogCounter % 10 == 1)
		{
			UE_LOG(LogGPUFluidSimulator, Warning,
				TEXT("[ANISO_MISMATCH] Position=%d, Aniso1=%d, Aniso2=%d, Aniso3=%d → Using default W=1.0"),
				Count, ReadyShadowAnisotropyAxis1.Num(), ReadyShadowAnisotropyAxis2.Num(), ReadyShadowAnisotropyAxis3.Num());
		}

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