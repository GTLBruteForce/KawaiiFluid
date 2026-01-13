// Copyright KawaiiFluid Team. All Rights Reserved.

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/FluidAnisotropyComputeShader.h"
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

DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);
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

	// Clear collision primitives
	CachedSpheres.Empty();
	CachedCapsules.Empty();
	CachedBoxes.Empty();
	CachedConvexHeaders.Empty();
	CachedConvexPlanes.Empty();
	bCollisionPrimitivesValid = false;

	// Release collision feedback buffers
	ReleaseCollisionFeedbackBuffers();
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
	const bool bHasPendingSpawns = bHasPendingSpawnRequests.load();
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
			if (Self->bCollisionFeedbackEnabled && Self->FeedbackReadbacks[0] != nullptr)
			{
				Self->ProcessCollisionFeedbackReadback(RHICmdList);
			}

			// Process collider contact count readback (reads from readback enqueued 2 frames ago)
			if (Self->ContactCountReadbacks[0] != nullptr)
			{
				Self->ProcessColliderContactCountReadback(RHICmdList);
			}
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

				if (ParticleCount > 0 && CopySize > 0)
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
			// Using FRHIGPUBufferReadback for truly async readback (no flush!)
			// =====================================================

			if (Self->bCollisionFeedbackEnabled && Self->CollisionFeedbackBuffer.IsValid() && Self->CollisionCounterBuffer.IsValid())
			{
				// Ensure readback objects are allocated
				if (Self->FeedbackReadbacks[0] == nullptr)
				{
					Self->AllocateCollisionFeedbackBuffers(RHICmdList);
				}

				const int32 WriteIdx = Self->FeedbackFrameNumber % Self->NUM_FEEDBACK_BUFFERS;

				// Transition buffers for copy
				RHICmdList.Transition(FRHITransitionInfo(
					Self->CollisionFeedbackBuffer->GetRHI(),
					ERHIAccess::UAVCompute,
					ERHIAccess::CopySrc));

				RHICmdList.Transition(FRHITransitionInfo(
					Self->CollisionCounterBuffer->GetRHI(),
					ERHIAccess::UAVCompute,
					ERHIAccess::CopySrc));

				// EnqueueCopy - async copy to readback buffer (non-blocking!)
				Self->FeedbackReadbacks[WriteIdx]->EnqueueCopy(
					RHICmdList,
					Self->CollisionFeedbackBuffer->GetRHI(),
					Self->MAX_COLLISION_FEEDBACK * sizeof(FGPUCollisionFeedback)
				);

				Self->CounterReadbacks[WriteIdx]->EnqueueCopy(
					RHICmdList,
					Self->CollisionCounterBuffer->GetRHI(),
					sizeof(uint32)
				);

				// Transition back for next frame
				RHICmdList.Transition(FRHITransitionInfo(
					Self->CollisionFeedbackBuffer->GetRHI(),
					ERHIAccess::CopySrc,
					ERHIAccess::UAVCompute));

				RHICmdList.Transition(FRHITransitionInfo(
					Self->CollisionCounterBuffer->GetRHI(),
					ERHIAccess::CopySrc,
					ERHIAccess::UAVCompute));

				// Increment frame counter AFTER EnqueueCopy
				Self->FeedbackFrameNumber++;

				if (bLogThisFrame)
				{
					UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> COLLISION FEEDBACK: EnqueueCopy to readback %d"), WriteIdx);
				}
			}

			// =====================================================
			// Copy Collider Contact Counts (FRHIGPUBufferReadback)
			// EnqueueCopy → WriteIdx, IsReady+Lock → ReadIdx (2프레임 전)
			// =====================================================
			if (Self->ColliderContactCountBuffer.IsValid())
			{
				// Ensure readback objects are allocated
				if (Self->ContactCountReadbacks[0] == nullptr)
				{
					Self->AllocateCollisionFeedbackBuffers(RHICmdList);
				}

				// Triple buffer index for this frame
				const int32 WriteIdx = Self->ContactCountFrameNumber % Self->NUM_FEEDBACK_BUFFERS;

				// 디버그 로그 (60프레임마다)
				static int32 EnqueueDebugFrame = 0;
				if (EnqueueDebugFrame++ % 60 == 0)
				{
					UE_LOG(LogGPUFluidSimulator, Log, TEXT("[ContactCount EnqueueCopy] WriteIdx=%d, FrameNum=%d, BufferValid=YES"),
						WriteIdx, Self->ContactCountFrameNumber);
				}

				// Transition for copy
				RHICmdList.Transition(FRHITransitionInfo(
					Self->ColliderContactCountBuffer->GetRHI(),
					ERHIAccess::UAVCompute,
					ERHIAccess::CopySrc));

				// EnqueueCopy - async copy to readback buffer (non-blocking!)
				Self->ContactCountReadbacks[WriteIdx]->EnqueueCopy(
					RHICmdList,
					Self->ColliderContactCountBuffer->GetRHI(),
					Self->MAX_COLLIDER_COUNT * sizeof(uint32)
				);

				// Transition back for next frame
				RHICmdList.Transition(FRHITransitionInfo(
					Self->ColliderContactCountBuffer->GetRHI(),
					ERHIAccess::CopySrc,
					ERHIAccess::UAVCompute));

				// Increment frame counter AFTER EnqueueCopy
				Self->ContactCountFrameNumber++;
			}
			else
			{
				// 버퍼가 유효하지 않음!
				static int32 InvalidBufferLogFrame = 0;
				if (InvalidBufferLogFrame++ % 60 == 0)
				{
					UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[ContactCount EnqueueCopy] ColliderContactCountBuffer가 유효하지 않음!"));
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
	// Game thread writes to PendingSpawnRequests
	// Render thread consumes from ActiveSpawnRequests
	// =====================================================
	{
		FScopeLock Lock(&SpawnRequestLock);
		ActiveSpawnRequests = MoveTemp(PendingSpawnRequests);
		PendingSpawnRequests.Empty();
		bHasPendingSpawnRequests.store(false);
	}

	const bool bHasSpawnRequests = ActiveSpawnRequests.Num() > 0;
	const int32 SpawnCount = ActiveSpawnRequests.Num();

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

	// Calculate expected particle count after spawning
	const int32 ExpectedParticleCount = CurrentParticleCount + SpawnCount;

	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluidSimulation (Particles: %d, Spawning: %d)", CurrentParticleCount, SpawnCount);

	// =====================================================
	// Phase 2: Persistent GPU Buffer with Append Support
	// - First frame: full upload from CPU
	// - New particles added: copy existing + append new
	// - Same count: reuse GPU buffer (no CPU upload)
	// =====================================================

	FRDGBufferRef ParticleBuffer;

	// Determine operation mode
	const bool bHasNewParticles = (NewParticleCount > 0 && NewParticlesToAppend.Num() > 0);
	const bool bNeedFullUpload = bNeedsFullUpload || !PersistentParticleBuffer.IsValid();
	// First spawn: have spawn requests, no current particles, and no persistent buffer
	// Note: Check !PersistentParticleBuffer.IsValid() instead of !bNeedFullUpload because
	// bNeedsFullUpload may be true even when using GPU spawn system (it's not cleared by AddSpawnRequests)
	const bool bFirstSpawnOnly = bHasSpawnRequests && CurrentParticleCount == 0 && !PersistentParticleBuffer.IsValid();

	// DEBUG: Log which path is being taken (only first 10 frames to avoid spam)
	static int32 DebugFrameCounter = 0;
	const bool bShouldLog = (DebugFrameCounter++ < 10);

	// =====================================================
	// DEBUG: Trigger RenderDoc capture on configurable GPU simulation frame
	// This helps diagnose Z-Order sorting issues by capturing the first frame
	// where RadixSort runs on fresh data (not corrupted by previous frames)
	//
	// CVars:
	//   r.Fluid.CaptureFirstFrame 1  - Capture first simulation frame (default)
	//   r.Fluid.CaptureFrameNumber N - Capture specific frame N
	// =====================================================
	{
		bool bShouldCapture = false;
		int32 TargetFrame = 0;

		if (GFluidCaptureFrameNumber > 0)
		{
			// Capture specific frame number
			TargetFrame = GFluidCaptureFrameNumber;
			bShouldCapture = (DebugFrameCounter == TargetFrame && GFluidCapturedFrame != TargetFrame);
		}
		else if (GFluidCaptureFirstFrame != 0)
		{
			// Capture first frame
			TargetFrame = 1;
			bShouldCapture = (DebugFrameCounter == 1 && GFluidCapturedFrame == 0);
		}

		if (bShouldCapture)
		{
			GFluidCapturedFrame = TargetFrame;
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT(">>> TRIGGERING RENDERDOC CAPTURE ON GPU SIMULATION FRAME %d <<<"), DebugFrameCounter);

			// Trigger RenderDoc capture using console command
			// Execute on game thread since we're in render thread context
			// This works when editor is launched with -RenderDoc flag
			AsyncTask(ENamedThreads::GameThread, []()
			{
				IConsoleObject* CaptureCmd = IConsoleManager::Get().FindConsoleObject(TEXT("renderdoc.CaptureFrame"));
				if (CaptureCmd)
				{
					IConsoleCommand* Cmd = CaptureCmd->AsCommand();
					if (Cmd)
					{
						Cmd->Execute(TArray<FString>(), nullptr, *GLog);
						UE_LOG(LogGPUFluidSimulator, Warning, TEXT(">>> RenderDoc capture command executed successfully"));
					}
				}
				else
				{
					UE_LOG(LogGPUFluidSimulator, Warning, TEXT(">>> RenderDoc not available. Launch editor with -RenderDoc flag."));
				}
			});
		}
	}

	if (bShouldLog)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("=== BUFFER PATH DEBUG (Frame %d) ==="), DebugFrameCounter);
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bFirstSpawnOnly: %s"), bFirstSpawnOnly ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bNeedFullUpload: %s"), bNeedFullUpload ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bHasNewParticles: %s"), bHasNewParticles ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bHasSpawnRequests: %s"), bHasSpawnRequests ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  PersistentBuffer Valid: %s"), PersistentParticleBuffer.IsValid() ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  CurrentParticleCount: %d, SpawnCount: %d"), CurrentParticleCount, SpawnCount);
	}

	if (bFirstSpawnOnly)
	{
		// =====================================================
		// FIRST SPAWN: No existing particles, just spawn requests
		// Create buffer and use spawn pass to initialize
		// =====================================================
		const int32 BufferCapacity = FMath::Min(SpawnCount, MaxParticleCount);

		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> FIRST SPAWN PATH: BufferCapacity=%d, MaxParticleCount=%d"), BufferCapacity, MaxParticleCount);

		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		// Create counter buffer initialized to 0
		// IMPORTANT: Do NOT use NoCopy - InitialCounterData is a local variable that goes
		// out of scope before RDG executes. RDG must copy the data.
		TArray<uint32> InitialCounterData;
		InitialCounterData.Add(0);

		FRDGBufferRef CounterBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidParticleCounter"),
			sizeof(uint32),
			1,
			InitialCounterData.GetData(),
			sizeof(uint32),
			ERDGInitialDataFlags::None
		);
		FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(CounterBuffer);

		// Run spawn particles pass
		FRDGBufferUAVRef ParticleUAVForSpawn = GraphBuilder.CreateUAV(ParticleBuffer);
		AddSpawnParticlesPass(GraphBuilder, ParticleUAVForSpawn, CounterUAV, ActiveSpawnRequests);

		// Update particle count
		CurrentParticleCount = FMath::Min(SpawnCount, MaxParticleCount);
		PreviousParticleCount = CurrentParticleCount;

		// IMPORTANT: Clear the full upload flag - we've successfully created the buffer via spawn
		// Without this, Frame 2+ would incorrectly take the full upload path
		bNeedsFullUpload = false;

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: First spawn - created %d particles"), CurrentParticleCount);

		ActiveSpawnRequests.Empty();
	}
	else if (bNeedFullUpload && CachedGPUParticles.Num() > 0)
	{
		// Full upload from CPU - create new buffer with all particles
		ParticleBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidParticles"),
			sizeof(FGPUFluidParticle),
			CurrentParticleCount,
			CachedGPUParticles.GetData(),
			CurrentParticleCount * sizeof(FGPUFluidParticle),
			ERDGInitialDataFlags::NoCopy
		);

		bNeedsFullUpload = false;
		PreviousParticleCount = CurrentParticleCount;
		NewParticleCount = 0;
		NewParticlesToAppend.Empty();

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: Full upload (%d particles)"), CurrentParticleCount);
	}
	else if (bHasNewParticles)
	{
		// Append new particles while preserving existing GPU simulation results
		// 1. Create new buffer with expanded capacity
		// 2. Copy existing particles from persistent buffer using compute shader
		// 3. Upload only new particles to the end using compute shader

		const int32 ExistingCount = PreviousParticleCount;
		const int32 TotalCount = CurrentParticleCount;

		// Create new buffer for total count
		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), TotalCount);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		// Register existing persistent buffer as source
		FRDGBufferRef ExistingBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleBuffer, TEXT("GPUFluidParticlesOld"));

		// Use compute shader to copy existing particles (more reliable than AddCopyBufferPass)
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FCopyParticlesCS> CopyShader(ShaderMap);

			FCopyParticlesCS::FParameters* CopyParams = GraphBuilder.AllocParameters<FCopyParticlesCS::FParameters>();
			CopyParams->SourceParticles = GraphBuilder.CreateSRV(ExistingBuffer);
			CopyParams->DestParticles = GraphBuilder.CreateUAV(ParticleBuffer);
			CopyParams->SourceOffset = 0;
			CopyParams->DestOffset = 0;
			CopyParams->CopyCount = ExistingCount;

			const uint32 NumGroups = FMath::DivideAndRoundUp(ExistingCount, FCopyParticlesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::CopyExistingParticles(%d)", ExistingCount),
				CopyShader,
				CopyParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		// Upload and copy new particles to the end of the buffer
		if (NewParticleCount > 0)
		{
			// Create upload buffer for new particles only
			FRDGBufferRef NewParticlesUploadBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidNewParticles"),
				sizeof(FGPUFluidParticle),
				NewParticleCount,
				NewParticlesToAppend.GetData(),
				NewParticleCount * sizeof(FGPUFluidParticle),
				ERDGInitialDataFlags::NoCopy
			);

			// Use compute shader to copy new particles to the end
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FCopyParticlesCS> CopyShader(ShaderMap);

			FCopyParticlesCS::FParameters* CopyParams = GraphBuilder.AllocParameters<FCopyParticlesCS::FParameters>();
			CopyParams->SourceParticles = GraphBuilder.CreateSRV(NewParticlesUploadBuffer);
			CopyParams->DestParticles = GraphBuilder.CreateUAV(ParticleBuffer);
			CopyParams->SourceOffset = 0;
			CopyParams->DestOffset = ExistingCount;
			CopyParams->CopyCount = NewParticleCount;

			const uint32 NumGroups = FMath::DivideAndRoundUp(NewParticleCount, FCopyParticlesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::CopyNewParticles(%d)", NewParticleCount),
				CopyShader,
				CopyParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		PreviousParticleCount = CurrentParticleCount;
		NewParticleCount = 0;
		NewParticlesToAppend.Empty();

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: Appended %d new particles (existing: %d, total: %d)"),
			TotalCount - ExistingCount, ExistingCount, TotalCount);
	}
	else if (bHasSpawnRequests && PersistentParticleBuffer.IsValid())
	{
		// =====================================================
		// NEW: GPU-based spawning path (eliminates race condition)
		// Expand buffer and use spawn pass to add particles atomically
		// =====================================================
		const int32 ExistingCount = CurrentParticleCount;
		const int32 TotalCount = ExpectedParticleCount;

		// Capacity check
		if (TotalCount > MaxParticleCount)
		{
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT("GPU Spawn: Total count (%d) exceeds capacity (%d), clamping spawn requests"),
				TotalCount, MaxParticleCount);
			// Will be handled by atomic counter in shader
		}

		// Create new buffer with expanded capacity
		const int32 BufferCapacity = FMath::Min(TotalCount, MaxParticleCount);
		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		// Copy existing particles from persistent buffer
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

			const uint32 NumGroups = FMath::DivideAndRoundUp(ExistingCount, FCopyParticlesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::CopyExistingForSpawn(%d)", ExistingCount),
				CopyShader,
				CopyParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		// Create counter buffer with current particle count
		// The shader will atomically increment this counter
		// IMPORTANT: Do NOT use NoCopy - InitialCounterData is a local variable that goes
		// out of scope before RDG executes. RDG must copy the data.
		TArray<uint32> InitialCounterData;
		InitialCounterData.Add(static_cast<uint32>(ExistingCount));

		FRDGBufferRef CounterBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidParticleCounter"),
			sizeof(uint32),
			1,
			InitialCounterData.GetData(),
			sizeof(uint32),
			ERDGInitialDataFlags::None
		);
		FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(CounterBuffer);

		// Run spawn particles pass
		FRDGBufferUAVRef ParticleUAVForSpawn = GraphBuilder.CreateUAV(ParticleBuffer);
		AddSpawnParticlesPass(GraphBuilder, ParticleUAVForSpawn, CounterUAV, ActiveSpawnRequests);

		// Update particle count (after spawning, assuming all spawn requests succeed within capacity)
		CurrentParticleCount = FMath::Min(ExpectedParticleCount, MaxParticleCount);
		PreviousParticleCount = CurrentParticleCount;

		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: GPU Spawn path - spawned %d particles (existing: %d, total: %d)"),SpawnCount, ExistingCount, CurrentParticleCount);

		// Clear spawn requests
		ActiveSpawnRequests.Empty();
	}
	else
	{
		// Reuse persistent buffer from previous frame (no CPU upload!)
		// This is the key path for proper GPU simulation - gravity should work here!
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> REUSE PATH: Attempting RegisterExternalBuffer"));

		if (!PersistentParticleBuffer.IsValid())
		{
			UE_LOG(LogGPUFluidSimulator, Error, TEXT(">>> REUSE PATH FAILED: PersistentParticleBuffer is NULL/Invalid!"));
			return;
		}

		ParticleBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleBuffer, TEXT("GPUFluidParticles"));
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> REUSE PATH: RegisterExternalBuffer succeeded, ParticleCount=%d"), CurrentParticleCount);
	}

	// Clear active spawn requests if any remaining (shouldn't happen, but safety)
	ActiveSpawnRequests.Empty();

	// Create transient position buffer for spatial hash
	FRDGBufferDesc PositionBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), CurrentParticleCount);
	FRDGBufferRef PositionBuffer = GraphBuilder.CreateBuffer(PositionBufferDesc, TEXT("GPUFluidPositions"));

	FRDGBufferUAVRef ParticlesUAVLocal = GraphBuilder.CreateUAV(ParticleBuffer);
	FRDGBufferSRVRef ParticlesSRVLocal = GraphBuilder.CreateSRV(ParticleBuffer);
	FRDGBufferUAVRef PositionsUAVLocal = GraphBuilder.CreateUAV(PositionBuffer);
	FRDGBufferSRVRef PositionsSRVLocal = GraphBuilder.CreateSRV(PositionBuffer);

	// Debug: Log simulation parameters
	static int32 SimDebugCounter = 0;
	if (++SimDebugCounter % 60 == 0)
	{
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("=== SIMULATION DEBUG ==="));
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("  ParticleCount: %d"), CurrentParticleCount);
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("  Gravity: (%.2f, %.2f, %.2f)"), Params.Gravity.X, Params.Gravity.Y, Params.Gravity.Z);
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("  DeltaTime: %.4f"), Params.DeltaTime);
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("  PersistentBuffer Valid: %s"), PersistentParticleBuffer.IsValid() ? TEXT("YES") : TEXT("NO"));
	}

	// Pass 0.5: Update attached particles (move with bones) - before physics simulation
	// Only run if attachment buffer exists AND matches current particle count
	FRDGBufferRef AttachmentBufferForUpdate = nullptr;
	if (IsAdhesionEnabled() && bBoneTransformsValid && PersistentAttachmentBuffer.IsValid() && AttachmentBufferSize >= CurrentParticleCount)
	{
		AttachmentBufferForUpdate = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsUpdate"));
		FRDGBufferUAVRef AttachmentUAVForUpdate = GraphBuilder.CreateUAV(AttachmentBufferForUpdate);
		AddUpdateAttachedPositionsPassInternal(GraphBuilder, ParticlesUAVLocal, AttachmentUAVForUpdate, Params);
	}

	// Pass 1: Predict Positions
	AddPredictPositionsPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 2: Extract positions for spatial hash (use predicted positions)
	AddExtractPositionsPass(GraphBuilder, ParticlesSRVLocal, PositionsUAVLocal, CurrentParticleCount, true);

	//=========================================================================
	// Pass 3: Spatial Data Structure
	// Option A: Z-Order Sorting (cache-coherent memory access)
	// Option B: Hash Table (linked-list based)
	//=========================================================================

	// CellStart/End for Z-Order sorting (used when bUseZOrderSorting is true)
	FRDGBufferUAVRef CellStartUAVLocal = nullptr;
	FRDGBufferSRVRef CellStartSRVLocal = nullptr;
	FRDGBufferUAVRef CellEndUAVLocal = nullptr;
	FRDGBufferSRVRef CellEndSRVLocal = nullptr;

	// CellCounts/ParticleIndices for hash table (used by physics shaders)
	FRDGBufferRef CellCountsBuffer = nullptr;
	FRDGBufferRef ParticleIndicesBuffer = nullptr;
	FRDGBufferSRVRef CellCountsSRVLocal = nullptr;
	FRDGBufferSRVRef ParticleIndicesSRVLocal = nullptr;

	if (bUseZOrderSorting)
	{
		//=====================================================================
		// Z-Order Sorting Pipeline
		// Morton Code → Radix Sort → Reorder → CellStart/End
		//=====================================================================
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> Z-ORDER SORTING: Building with ParticleCount=%d"), CurrentParticleCount);

		// Execute Z-Order sorting and get sorted particle buffer
		FRDGBufferRef SortedParticleBuffer = ExecuteZOrderSortingPipeline(
			GraphBuilder,
			ParticleBuffer,
			CellStartUAVLocal,
			CellStartSRVLocal,
			CellEndUAVLocal,
			CellEndSRVLocal,
			Params);

		// Replace particle buffer with sorted version for subsequent passes
		ParticleBuffer = SortedParticleBuffer;
		ParticlesUAVLocal = GraphBuilder.CreateUAV(ParticleBuffer);
		ParticlesSRVLocal = GraphBuilder.CreateSRV(ParticleBuffer);

		// Extract positions from sorted particles for hash table build
		AddExtractPositionsPass(GraphBuilder, ParticlesSRVLocal, PositionsUAVLocal, CurrentParticleCount, true);

		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> Z-ORDER SORTING: Completed, particles reordered"));

		//=====================================================================
		// Also build hash table for compatibility with existing shaders
		// Sorted particles → better cache locality during neighbor iteration
		//=====================================================================
		if (!PersistentCellCountsBuffer.IsValid())
		{
			FRDGBufferDesc CellCountsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_SPATIAL_HASH_SIZE);
			CellCountsBuffer = GraphBuilder.CreateBuffer(CellCountsDesc, TEXT("SpatialHash.CellCounts"));

			const uint32 TotalSlots = GPU_SPATIAL_HASH_SIZE * GPU_MAX_PARTICLES_PER_CELL;
			FRDGBufferDesc ParticleIndicesDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalSlots);
			ParticleIndicesBuffer = GraphBuilder.CreateBuffer(ParticleIndicesDesc, TEXT("SpatialHash.ParticleIndices"));
		}
		else
		{
			CellCountsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentCellCountsBuffer, TEXT("SpatialHash.CellCounts"));
			ParticleIndicesBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleIndicesBuffer, TEXT("SpatialHash.ParticleIndices"));
		}

		CellCountsSRVLocal = GraphBuilder.CreateSRV(CellCountsBuffer);
		FRDGBufferUAVRef CellCountsUAVLocal = GraphBuilder.CreateUAV(CellCountsBuffer);
		ParticleIndicesSRVLocal = GraphBuilder.CreateSRV(ParticleIndicesBuffer);
		FRDGBufferUAVRef ParticleIndicesUAVLocal = GraphBuilder.CreateUAV(ParticleIndicesBuffer);

		// GPU Clear pass
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FClearCellDataCS> ClearShader(ShaderMap);

			FClearCellDataCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellDataCS::FParameters>();
			ClearParams->CellCounts = CellCountsUAVLocal;

			const uint32 NumGroups = FMath::DivideAndRoundUp<uint32>(GPU_SPATIAL_HASH_SIZE, SPATIAL_HASH_THREAD_GROUP_SIZE);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SpatialHash::GPUClear(ZOrder)"),
				ClearShader,
				ClearParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		// GPU Build pass with sorted positions
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FBuildSpatialHashSimpleCS> BuildShader(ShaderMap);

			FBuildSpatialHashSimpleCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FBuildSpatialHashSimpleCS::FParameters>();
			BuildParams->ParticlePositions = PositionsSRVLocal;
			BuildParams->ParticleCount = CurrentParticleCount;
			BuildParams->ParticleRadius = Params.ParticleRadius;
			BuildParams->CellSize = Params.CellSize;
			BuildParams->CellCounts = CellCountsUAVLocal;
			BuildParams->ParticleIndices = ParticleIndicesUAVLocal;

			const uint32 NumGroups = FMath::DivideAndRoundUp<uint32>(CurrentParticleCount, SPATIAL_HASH_THREAD_GROUP_SIZE);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SpatialHash::GPUBuild(ZOrder)"),
				BuildShader,
				BuildParams,
				FIntVector(NumGroups, 1, 1)
			);
		}
	}
	else
	{
		//=====================================================================
		// Traditional Hash Table (fallback)
		//=====================================================================
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Building with ParticleCount=%d, Radius=%.2f, CellSize=%.2f"),
			CurrentParticleCount, Params.ParticleRadius, Params.CellSize);

		if (!PersistentCellCountsBuffer.IsValid())
		{
			// First frame: create buffers
			FRDGBufferDesc CellCountsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_SPATIAL_HASH_SIZE);
			CellCountsBuffer = GraphBuilder.CreateBuffer(CellCountsDesc, TEXT("SpatialHash.CellCounts"));

			const uint32 TotalSlots = GPU_SPATIAL_HASH_SIZE * GPU_MAX_PARTICLES_PER_CELL;
			FRDGBufferDesc ParticleIndicesDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalSlots);
			ParticleIndicesBuffer = GraphBuilder.CreateBuffer(ParticleIndicesDesc, TEXT("SpatialHash.ParticleIndices"));

			if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Created new persistent buffers"));
		}
		else
		{
			// Subsequent frames: reuse persistent buffers
			CellCountsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentCellCountsBuffer, TEXT("SpatialHash.CellCounts"));
			ParticleIndicesBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleIndicesBuffer, TEXT("SpatialHash.ParticleIndices"));

			if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Reusing persistent buffers"));
		}

		CellCountsSRVLocal = GraphBuilder.CreateSRV(CellCountsBuffer);
		FRDGBufferUAVRef CellCountsUAVLocal = GraphBuilder.CreateUAV(CellCountsBuffer);
		ParticleIndicesSRVLocal = GraphBuilder.CreateSRV(ParticleIndicesBuffer);
		FRDGBufferUAVRef ParticleIndicesUAVLocal = GraphBuilder.CreateUAV(ParticleIndicesBuffer);

		// GPU Clear pass - clears CellCounts to 0 entirely on GPU
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FClearCellDataCS> ClearShader(ShaderMap);

			FClearCellDataCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellDataCS::FParameters>();
			ClearParams->CellCounts = CellCountsUAVLocal;

			const uint32 NumGroups = FMath::DivideAndRoundUp<uint32>(GPU_SPATIAL_HASH_SIZE, SPATIAL_HASH_THREAD_GROUP_SIZE);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SpatialHash::GPUClear"),
				ClearShader,
				ClearParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		// GPU Build pass - writes particle indices into hash grid
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FBuildSpatialHashSimpleCS> BuildShader(ShaderMap);

			FBuildSpatialHashSimpleCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FBuildSpatialHashSimpleCS::FParameters>();
			BuildParams->ParticlePositions = PositionsSRVLocal;
			BuildParams->ParticleCount = CurrentParticleCount;
			BuildParams->ParticleRadius = Params.ParticleRadius;
			BuildParams->CellSize = Params.CellSize;
			BuildParams->CellCounts = CellCountsUAVLocal;
			BuildParams->ParticleIndices = ParticleIndicesUAVLocal;

			const uint32 NumGroups = FMath::DivideAndRoundUp<uint32>(CurrentParticleCount, SPATIAL_HASH_THREAD_GROUP_SIZE);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SpatialHash::GPUBuild"),
				BuildShader,
				BuildParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: GPU clear+build completed"));

		// Create dummy CellStart/CellEnd buffers for shader parameter validation
		// These are not used when bUseZOrderSorting = 0, but RDG requires valid SRVs
		// Must use QueueBufferUpload to mark buffer as "produced" for RDG validation
		{
			FRDGBufferDesc DummyCellDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
			FRDGBufferRef DummyCellStartBuffer = GraphBuilder.CreateBuffer(DummyCellDesc, TEXT("DummyCellStart"));
			FRDGBufferRef DummyCellEndBuffer = GraphBuilder.CreateBuffer(DummyCellDesc, TEXT("DummyCellEnd"));
			uint32 InvalidIndex = 0xFFFFFFFF;
			GraphBuilder.QueueBufferUpload(DummyCellStartBuffer, &InvalidIndex, sizeof(uint32));
			GraphBuilder.QueueBufferUpload(DummyCellEndBuffer, &InvalidIndex, sizeof(uint32));
			CellStartSRVLocal = GraphBuilder.CreateSRV(DummyCellStartBuffer);
			CellEndSRVLocal = GraphBuilder.CreateSRV(DummyCellEndBuffer);
		}
	}

	// Pass 3.5: GPU Boundary Skinning (if using GPU skinning system)
	// Transforms bone-local boundary particles to world space on GPU
	// Must run before density/pressure solver so boundary particles are in correct positions
	if (IsGPUBoundarySkinningEnabled())
	{
		AddBoundarySkinningPass(GraphBuilder, Params);
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> BOUNDARY SKINNING: GPU skinning pass completed"));
	}

	// Pass 4-5: XPBD Density Constraint Solver (OPTIMIZED: Combined Density + Pressure + Neighbor Caching)
	// Neighbor Caching: First iteration builds neighbor list, subsequent iterations reuse
	// Each iteration:
	//   - Single neighbor traversal computes both Density/Lambda AND Position corrections
	//   - Uses previous iteration's Lambda for Jacobi-style update (parallel-safe)
	// Performance: Hash lookup only in first iteration, cached for subsequent iterations

	// Create/resize neighbor caching buffers if needed
	FRDGBufferRef NeighborListRDGBuffer = nullptr;
	FRDGBufferRef NeighborCountsRDGBuffer = nullptr;
	FRDGBufferUAVRef NeighborListUAVLocal = nullptr;
	FRDGBufferUAVRef NeighborCountsUAVLocal = nullptr;
	FRDGBufferSRVRef NeighborListSRVLocal = nullptr;
	FRDGBufferSRVRef NeighborCountsSRVLocal = nullptr;

	const int32 NeighborListSize = CurrentParticleCount * GPU_MAX_NEIGHBORS_PER_PARTICLE;

	if (NeighborBufferParticleCapacity < CurrentParticleCount || !NeighborListBuffer.IsValid())
	{
		// Create new buffers
		NeighborListRDGBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NeighborListSize),
			TEXT("GPUFluidNeighborList"));
		NeighborCountsRDGBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CurrentParticleCount),
			TEXT("GPUFluidNeighborCounts"));

		NeighborBufferParticleCapacity = CurrentParticleCount;
	}
	else
	{
		// Reuse existing buffers
		NeighborListRDGBuffer = GraphBuilder.RegisterExternalBuffer(NeighborListBuffer, TEXT("GPUFluidNeighborList"));
		NeighborCountsRDGBuffer = GraphBuilder.RegisterExternalBuffer(NeighborCountsBuffer, TEXT("GPUFluidNeighborCounts"));
	}

	NeighborListUAVLocal = GraphBuilder.CreateUAV(NeighborListRDGBuffer);
	NeighborCountsUAVLocal = GraphBuilder.CreateUAV(NeighborCountsRDGBuffer);

	for (int32 i = 0; i < Params.SolverIterations; ++i)
	{
		// Combined pass: Compute Density + Lambda + Apply Position Corrections
		// First iteration (i=0) builds neighbor cache, subsequent iterations reuse it
		// When bUseZOrderSorting is true, CellStartSRVLocal/CellEndSRVLocal are valid and shader uses sequential access
		AddSolveDensityPressurePass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal,
			CellStartSRVLocal, CellEndSRVLocal, NeighborListUAVLocal, NeighborCountsUAVLocal, i, Params);
	}

	// Create SRVs from neighbor cache buffers for use in Viscosity and Cohesion passes
	NeighborListSRVLocal = GraphBuilder.CreateSRV(NeighborListRDGBuffer);
	NeighborCountsSRVLocal = GraphBuilder.CreateSRV(NeighborCountsRDGBuffer);

	// Extract neighbor buffers for next frame (persist across substeps)
	GraphBuilder.QueueBufferExtraction(NeighborListRDGBuffer, &NeighborListBuffer, ERHIAccess::UAVCompute);
	GraphBuilder.QueueBufferExtraction(NeighborCountsRDGBuffer, &NeighborCountsBuffer, ERHIAccess::UAVCompute);

	// Pass 6: Bounds Collision
	AddBoundsCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 6.5: Distance Field Collision (if enabled)
	AddDistanceFieldCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 6.6: Primitive Collision (spheres, capsules, boxes, convexes from FluidCollider)
	AddPrimitiveCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 6.7: Adhesion - Create attachments to bone colliders (GPU-based)
	if (IsAdhesionEnabled() && bBoneTransformsValid)
	{
		// Check if we need to create or resize attachment buffer
		const bool bNeedNewBuffer = !PersistentAttachmentBuffer.IsValid() || AttachmentBufferSize < CurrentParticleCount;

		FRDGBufferRef AttachmentBuffer;
		if (bNeedNewBuffer)
		{
			// Create new buffer with initialized data (PrimitiveType = -1 means no attachment)
			TArray<FGPUParticleAttachment> InitialAttachments;
			InitialAttachments.SetNum(CurrentParticleCount);
			for (int32 i = 0; i < CurrentParticleCount; ++i)
			{
				InitialAttachments[i].PrimitiveType = -1;
				InitialAttachments[i].PrimitiveIndex = -1;
				InitialAttachments[i].BoneIndex = -1;
				InitialAttachments[i].AdhesionStrength = 0.0f;
				InitialAttachments[i].LocalOffset = FVector3f::ZeroVector;
				InitialAttachments[i].AttachmentTime = 0.0f;
			}

			AttachmentBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidAttachments"),
				sizeof(FGPUParticleAttachment),
				CurrentParticleCount,
				InitialAttachments.GetData(),
				CurrentParticleCount * sizeof(FGPUParticleAttachment),
				ERDGInitialDataFlags::None  // Copy the data, don't use NoCopy
			);

			// If we had existing data, copy it to the new buffer
			if (PersistentAttachmentBuffer.IsValid() && AttachmentBufferSize > 0)
			{
				FRDGBufferRef OldBuffer = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsOld"));

				// Copy existing attachment data (preserve attached particles)
				AddCopyBufferPass(GraphBuilder, AttachmentBuffer, 0, OldBuffer, 0, AttachmentBufferSize * sizeof(FGPUParticleAttachment));
			}

			// Update tracked size
			AttachmentBufferSize = CurrentParticleCount;
		}
		else
		{
			AttachmentBuffer = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachments"));
		}
		FRDGBufferUAVRef AttachmentUAV = GraphBuilder.CreateUAV(AttachmentBuffer);

		AddAdhesionPass(GraphBuilder, ParticlesUAVLocal, AttachmentUAV, Params);

		// Extract attachment buffer for next frame
		GraphBuilder.QueueBufferExtraction(
			AttachmentBuffer,
			&PersistentAttachmentBuffer,
			ERHIAccess::UAVCompute
		);
	}

	// Pass 7: Finalize Positions (update Position from PredictedPosition, recalculate Velocity: v = (x* - x) / dt)
	AddFinalizePositionsPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 8: Apply Viscosity (XSPH velocity smoothing) - Applied AFTER velocity is finalized per PBF paper
	// Uses cached neighbor list from DensityPressure pass for faster lookup
	AddApplyViscosityPass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal,
		NeighborListSRVLocal, NeighborCountsSRVLocal, Params);

	// Pass 8.5: Apply Cohesion (surface tension between particles)
	// Uses cached neighbor list from DensityPressure pass for faster lookup
	AddApplyCohesionPass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal,
		NeighborListSRVLocal, NeighborCountsSRVLocal, Params);

	// Pass 8.6: Apply Stack Pressure (weight transfer from stacked attached particles)
	if (Params.StackPressureScale > 0.0f && IsAdhesionEnabled() && PersistentAttachmentBuffer.IsValid())
	{
		FRDGBufferRef AttachmentBufferForStackPressure = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsStackPressure"));
		FRDGBufferSRVRef AttachmentSRVForStackPressure = GraphBuilder.CreateSRV(AttachmentBufferForStackPressure);
		AddStackPressurePass(GraphBuilder, ParticlesUAVLocal, AttachmentSRVForStackPressure, CellCountsSRVLocal, ParticleIndicesSRVLocal, Params);
	}

	// Pass 9: Clear just-detached flag at end of frame
	AddClearDetachedFlagPass(GraphBuilder, ParticlesUAVLocal);

	// Pass 9.5: Boundary Adhesion (Flex-style adhesion from mesh surface particles)
	if (IsBoundaryAdhesionEnabled())
	{
		AddBoundaryAdhesionPass(GraphBuilder, ParticlesUAVLocal, Params);
	}

	// Pass 10: Anisotropy calculation (for ellipsoid rendering)
	// Runs after simulation is complete, uses spatial hash for neighbor lookup
	if (CachedAnisotropyParams.bEnabled && CurrentParticleCount > 0)
	{
		// Update interval optimization: skip calculation on some frames
		const int32 UpdateInterval = FMath::Max(1, CachedAnisotropyParams.UpdateInterval);
		++AnisotropyFrameCounter;
		const bool bShouldUpdateAnisotropy = (AnisotropyFrameCounter >= UpdateInterval)
			|| !PersistentAnisotropyAxis1Buffer.IsValid();  // Always update if no buffer exists

		if (bShouldUpdateAnisotropy)
		{
			AnisotropyFrameCounter = 0;  // Reset counter
		}
		else
		{
			// Skip this frame - reuse existing anisotropy buffers
			// No extraction needed, buffers persist from previous frame
			goto AnisotropyPassEnd;
		}

		// Create anisotropy output buffers
		FRDGBufferRef Axis1Buffer = nullptr;
		FRDGBufferRef Axis2Buffer = nullptr;
		FRDGBufferRef Axis3Buffer = nullptr;
		FFluidAnisotropyPassBuilder::CreateAnisotropyBuffers(
			GraphBuilder, CurrentParticleCount, Axis1Buffer, Axis2Buffer, Axis3Buffer);

		if (Axis1Buffer && Axis2Buffer && Axis3Buffer && CellCountsBuffer && ParticleIndicesBuffer)
		{
			// Prepare anisotropy compute parameters
			FAnisotropyComputeParams AnisotropyParams;
			AnisotropyParams.PhysicsParticlesSRV = GraphBuilder.CreateSRV(ParticleBuffer);

			// Pass Attachment buffer for attached particle anisotropy
			// Always create a valid SRV (dummy if adhesion disabled) to satisfy shader requirements
			if (IsAdhesionEnabled() && PersistentAttachmentBuffer.IsValid())
			{
				FRDGBufferRef AttachmentBufferForAnisotropy = GraphBuilder.RegisterExternalBuffer(
					PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsAnisotropy"));
				AnisotropyParams.AttachmentsSRV = GraphBuilder.CreateSRV(AttachmentBufferForAnisotropy);
			}
			else
			{
				// Create dummy attachment buffer (shader requires valid SRV)
				// Must upload data so RDG marks buffer as "produced" (bQueuedForUpload = true)
				FRDGBufferRef DummyAttachmentBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUParticleAttachment), 1),
					TEXT("DummyAttachmentBuffer"));
				FGPUParticleAttachment ZeroData = {};
				GraphBuilder.QueueBufferUpload(DummyAttachmentBuffer, &ZeroData, sizeof(FGPUParticleAttachment));
				AnisotropyParams.AttachmentsSRV = GraphBuilder.CreateSRV(DummyAttachmentBuffer);
			}

			// Create fresh SRVs for Anisotropy pass to avoid RDG resource state conflicts
			// Previous SRVs may have been used in passes that also had UAV writes to same buffers
			AnisotropyParams.CellCountsSRV = GraphBuilder.CreateSRV(CellCountsBuffer);
			AnisotropyParams.ParticleIndicesSRV = GraphBuilder.CreateSRV(ParticleIndicesBuffer);
			AnisotropyParams.OutAxis1UAV = GraphBuilder.CreateUAV(Axis1Buffer);
			AnisotropyParams.OutAxis2UAV = GraphBuilder.CreateUAV(Axis2Buffer);
			AnisotropyParams.OutAxis3UAV = GraphBuilder.CreateUAV(Axis3Buffer);
			AnisotropyParams.ParticleCount = CurrentParticleCount;

			// Map anisotropy mode
			switch (CachedAnisotropyParams.Mode)
			{
			case EFluidAnisotropyMode::VelocityBased:
				AnisotropyParams.Mode = EGPUAnisotropyMode::VelocityBased;
				break;
			case EFluidAnisotropyMode::DensityBased:
				AnisotropyParams.Mode = EGPUAnisotropyMode::DensityBased;
				break;
			case EFluidAnisotropyMode::Hybrid:
				AnisotropyParams.Mode = EGPUAnisotropyMode::Hybrid;
				break;
			default:
				AnisotropyParams.Mode = EGPUAnisotropyMode::DensityBased;
				break;
			}

			AnisotropyParams.VelocityStretchFactor = CachedAnisotropyParams.VelocityStretchFactor;
			AnisotropyParams.AnisotropyScale = CachedAnisotropyParams.AnisotropyScale;
			AnisotropyParams.AnisotropyMin = CachedAnisotropyParams.AnisotropyMin;
			AnisotropyParams.AnisotropyMax = CachedAnisotropyParams.AnisotropyMax;
			AnisotropyParams.DensityWeight = CachedAnisotropyParams.DensityWeight;

			// Isolated particle handling params
			AnisotropyParams.MinNeighborsForAnisotropy = CachedAnisotropyParams.MinNeighborsForAnisotropy;
			AnisotropyParams.bFadeIsolatedParticles = CachedAnisotropyParams.bFadeIsolatedParticles;
			AnisotropyParams.MinIsolatedScale = CachedAnisotropyParams.MinIsolatedScale;
			AnisotropyParams.bStretchIsolatedByVelocity = CachedAnisotropyParams.bStretchIsolatedByVelocity;
			AnisotropyParams.bFadeSlowIsolated = CachedAnisotropyParams.bFadeSlowIsolated;
			AnisotropyParams.IsolationFadeSpeed = CachedAnisotropyParams.IsolationFadeSpeed;

			// Density-based anisotropy needs wider neighbor search than simulation
			// Use 2.5x smoothing radius to find enough neighbors for reliable covariance
			AnisotropyParams.SmoothingRadius = Params.SmoothingRadius * 2.5f;
			AnisotropyParams.CellSize = Params.CellSize;

			// Morton-sorted spatial lookup (cache-friendly sequential access)
			// When bUseZOrderSorting=true, ParticleBuffer is already sorted by Morton code
			AnisotropyParams.bUseZOrderSorting = bUseZOrderSorting;
			if (bUseZOrderSorting && CellStartSRVLocal && CellEndSRVLocal)
			{
				AnisotropyParams.CellStartSRV = CellStartSRVLocal;
				AnisotropyParams.CellEndSRV = CellEndSRVLocal;
				// IMPORTANT: Must use SimulationBoundsMin (same as simulation passes)
				// Using Params.BoundsMin causes cell ID mismatch and neighbor lookup failure
				AnisotropyParams.MortonBoundsMin = SimulationBoundsMin;
			}

			// Debug log for density-based anisotropy parameters
			static int32 AnisotropyDebugCounter = 0;
			if (++AnisotropyDebugCounter % 60 == 0)
			{
				UE_LOG(LogGPUFluidSimulator, Warning,
					TEXT("Anisotropy Pass: CachedMode=%d -> GPUMode=%d, SmoothingRadius=%.2f, CellSize=%.2f, CellRadius=%d"),
					static_cast<int32>(CachedAnisotropyParams.Mode),
					static_cast<int32>(AnisotropyParams.Mode),
					AnisotropyParams.SmoothingRadius,
					AnisotropyParams.CellSize,
					AnisotropyParams.CellSize > 0.01f ? (int32)FMath::CeilToInt(AnisotropyParams.SmoothingRadius / AnisotropyParams.CellSize) : 0);
			}

			// Add anisotropy compute pass
			FFluidAnisotropyPassBuilder::AddAnisotropyPass(GraphBuilder, AnisotropyParams);

			// Extract anisotropy buffers for rendering
			GraphBuilder.QueueBufferExtraction(
				Axis1Buffer,
				&PersistentAnisotropyAxis1Buffer,
				ERHIAccess::SRVCompute);
			GraphBuilder.QueueBufferExtraction(
				Axis2Buffer,
				&PersistentAnisotropyAxis2Buffer,
				ERHIAccess::SRVCompute);
			GraphBuilder.QueueBufferExtraction(
				Axis3Buffer,
				&PersistentAnisotropyAxis3Buffer,
				ERHIAccess::SRVCompute);

			if (bShouldLog)
			{
				UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> ANISOTROPY: Pass added (mode=%d, particles=%d)"),
					static_cast<int32>(AnisotropyParams.Mode), CurrentParticleCount);
			}
		}
	}
AnisotropyPassEnd:

	// Debug: log that we reached the end of simulation
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SIMULATION COMPLETE: All passes added for %d particles"), CurrentParticleCount);

	// Phase 2: Extract buffers to persistent storage for next frame reuse
	// This keeps simulation results on GPU without CPU readback
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> EXTRACTION: Queuing buffer extraction..."));

	// Extract particle buffer
	GraphBuilder.QueueBufferExtraction(
		ParticleBuffer,
		&PersistentParticleBuffer,
		ERHIAccess::UAVCompute  // Ready for next frame's compute passes
	);

	// Extract spatial hash buffers for reuse next frame
	GraphBuilder.QueueBufferExtraction(
		CellCountsBuffer,
		&PersistentCellCountsBuffer,
		ERHIAccess::UAVCompute
	);
	GraphBuilder.QueueBufferExtraction(
		ParticleIndicesBuffer,
		&PersistentParticleIndicesBuffer,
		ERHIAccess::UAVCompute
	);

	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> EXTRACTION: Buffer extraction queued successfully"));
}
