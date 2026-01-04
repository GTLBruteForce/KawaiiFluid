// Copyright KawaiiFluid Team. All Rights Reserved.

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "Core/FluidParticle.h"
#include "Rendering/Shaders/FluidSpatialHashShaders.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);
DEFINE_LOG_CATEGORY(LogGPUFluidSimulator);

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUFluidSimulator::FGPUFluidSimulator()
	: bIsInitialized(false)
	, MaxParticleCount(0)
	, CurrentParticleCount(0)
	, ExternalForce(FVector3f::ZeroVector)
	, VelocityDamping(0.99f)
	, MaxVelocity(1000.0f)
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

	// Clear collision primitives
	CachedSpheres.Empty();
	CachedCapsules.Empty();
	CachedBoxes.Empty();
	CachedConvexHeaders.Empty();
	CachedConvexPlanes.Empty();
	bCollisionPrimitivesValid = false;
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
		FRHIResourceCreateInfo CreateInfo(TEXT("GPUFluidParticleBuffer"));
		ParticleBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(FGPUFluidParticle),
			NewCapacity * sizeof(FGPUFluidParticle),
			BUF_UnorderedAccess | BUF_ShaderResource,
			CreateInfo
		);

		ParticleSRV = RHICmdList.CreateShaderResourceView(ParticleBufferRHI);
		ParticleUAV = RHICmdList.CreateUnorderedAccessView(ParticleBufferRHI, false, false);
	}

	// Create position buffer for spatial hash (float3 only)
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("GPUFluidPositionBuffer"));
		PositionBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(FVector3f),
			NewCapacity * sizeof(FVector3f),
			BUF_UnorderedAccess | BUF_ShaderResource,
			CreateInfo
		);

		PositionSRV = RHICmdList.CreateShaderResourceView(PositionBufferRHI);
		PositionUAV = RHICmdList.CreateUnorderedAccessView(PositionBufferRHI, false, false);
	}

	// Create staging buffer for CPU readback
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("GPUFluidStagingBuffer"));
		StagingBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(FGPUFluidParticle),
			NewCapacity * sizeof(FGPUFluidParticle),
			BUF_None,
			ERHIAccess::CopyDest,
			CreateInfo
		);
	}

	// Create spatial hash buffers
	{
		// Cell counts buffer
		FRHIResourceCreateInfo CreateInfo(TEXT("GPUFluidCellCounts"));
		CellCountsBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(uint32),
			GPU_SPATIAL_HASH_SIZE * sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource,
			CreateInfo
		);

		CellCountsSRV = RHICmdList.CreateShaderResourceView(CellCountsBufferRHI);
		CellCountsUAV = RHICmdList.CreateUnorderedAccessView(CellCountsBufferRHI, false, false);

		// Particle indices buffer
		const uint32 TotalSlots = GPU_SPATIAL_HASH_SIZE * GPU_MAX_PARTICLES_PER_CELL;
		FRHIResourceCreateInfo CreateInfo2(TEXT("GPUFluidParticleIndices"));
		ParticleIndicesBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(uint32),
			TotalSlots * sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource,
			CreateInfo2
		);

		ParticleIndicesSRV = RHICmdList.CreateShaderResourceView(ParticleIndicesBufferRHI);
		ParticleIndicesUAV = RHICmdList.CreateUnorderedAccessView(ParticleIndicesBufferRHI, false, false);
	}

	// Resize cached array
	CachedGPUParticles.SetNumUninitialized(NewCapacity);

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Resized GPU buffers to capacity: %d"), NewCapacity);
}

//=============================================================================
// Data Transfer
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
	GPUParticle.ClusterID = CPUParticle.ClusterID;

	// Pack flags
	uint32 Flags = 0;
	if (CPUParticle.bIsAttached) Flags |= EGPUParticleFlags::IsAttached;
	if (CPUParticle.bIsSurfaceParticle) Flags |= EGPUParticleFlags::IsSurface;
	if (CPUParticle.bIsCoreParticle) Flags |= EGPUParticleFlags::IsCore;
	if (CPUParticle.bJustDetached) Flags |= EGPUParticleFlags::JustDetached;
	if (CPUParticle.bNearGround) Flags |= EGPUParticleFlags::NearGround;
	GPUParticle.Flags = Flags;

	GPUParticle.Padding = 0.0f;

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

		for (int32 i = 0; i < NewCount; ++i)
		{
			CachedGPUParticles[i] = ConvertToGPU(CPUParticles[i]);
		}

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

	// Build ParticleID → CPU index map for matching
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
	int32 UpdatedCount = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		const FGPUFluidParticle& GPUParticle = ReadbackGPUParticles[i];
		if (int32* CPUIndex = ParticleIDToIndex.Find(GPUParticle.ParticleID))
		{
			ConvertFromGPU(OutCPUParticles[*CPUIndex], GPUParticle);
			++UpdatedCount;
		}
	}

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("DownloadParticles: Updated %d/%d particles"), UpdatedCount, Count);
}

//=============================================================================
// Collision Primitives Upload
//=============================================================================

void FGPUFluidSimulator::UploadCollisionPrimitives(const FGPUCollisionPrimitives& Primitives)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&BufferLock);

	// Cache the primitive data (will be uploaded to GPU during simulation)
	CachedSpheres = Primitives.Spheres;
	CachedCapsules = Primitives.Capsules;
	CachedBoxes = Primitives.Boxes;
	CachedConvexHeaders = Primitives.Convexes;
	CachedConvexPlanes = Primitives.ConvexPlanes;

	// Check if we have any primitives
	if (Primitives.IsEmpty())
	{
		bCollisionPrimitivesValid = false;
		return;
	}

	bCollisionPrimitivesValid = true;

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Cached collision primitives: Spheres=%d, Capsules=%d, Boxes=%d, Convexes=%d, Planes=%d"),
		CachedSpheres.Num(), CachedCapsules.Num(), CachedBoxes.Num(), CachedConvexHeaders.Num(), CachedConvexPlanes.Num());
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
			// Phase 2: NO READBACK - GPU buffer is source of truth
			// Renderer should read directly from GPU buffer
			// This eliminates CPU-GPU sync stall (~10-15ms savings)
			// =====================================================

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

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: GPU Spawn path - spawned %d particles (existing: %d, total: %d)"),
			SpawnCount, ExistingCount, CurrentParticleCount);

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
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("=== SIMULATION DEBUG ==="));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  ParticleCount: %d"), CurrentParticleCount);
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  Gravity: (%.2f, %.2f, %.2f)"), Params.Gravity.X, Params.Gravity.Y, Params.Gravity.Z);
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  DeltaTime: %.4f"), Params.DeltaTime);
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  PersistentBuffer Valid: %s"), PersistentParticleBuffer.IsValid() ? TEXT("YES") : TEXT("NO"));
	}

	// Pass 1: Predict Positions
	AddPredictPositionsPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 2: Extract positions for spatial hash (use predicted positions)
	AddExtractPositionsPass(GraphBuilder, ParticlesSRVLocal, PositionsUAVLocal, CurrentParticleCount, true);

	// Pass 3: Build Spatial Hash using CreateAndBuildHash (atomic operation)
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Building with ParticleCount=%d, Radius=%.2f, CellSize=%.2f"),
		CurrentParticleCount, Params.ParticleRadius, Params.CellSize);

	FSpatialHashGPUResources HashResources;
	bool bHashBuilt = FSpatialHashBuilder::CreateAndBuildHash(
		GraphBuilder,
		PositionsSRVLocal,
		CurrentParticleCount,
		Params.ParticleRadius,
		Params.CellSize,
		HashResources
	);

	if (!bHashBuilt)
	{
		UE_LOG(LogGPUFluidSimulator, Error, TEXT(">>> SPATIAL HASH FAILED! SIMULATION ABORTED! ParticleCount=%d"), CurrentParticleCount);
		return;
	}
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Build succeeded"));

	// Pass 4: Compute Density and Lambda
	AddComputeDensityPass(GraphBuilder, ParticlesUAVLocal, HashResources.CellCountsSRV, HashResources.ParticleIndicesSRV, Params);

	// Pass 5: Solve Pressure (multiple iterations if needed)
	for (int32 i = 0; i < Params.PressureIterations; ++i)
	{
		AddSolvePressurePass(GraphBuilder, ParticlesUAVLocal, HashResources.CellCountsSRV, HashResources.ParticleIndicesSRV, Params);
	}

	// Pass 6: Apply Viscosity
	AddApplyViscosityPass(GraphBuilder, ParticlesUAVLocal, HashResources.CellCountsSRV, HashResources.ParticleIndicesSRV, Params);

	// Pass 7: Bounds Collision
	AddBoundsCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 7.5: Distance Field Collision (if enabled)
	AddDistanceFieldCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 7.6: Primitive Collision (spheres, capsules, boxes, convexes from FluidCollider)
	AddPrimitiveCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 8: Finalize Positions
	AddFinalizePositionsPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Debug: log that we reached the end of simulation
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SIMULATION COMPLETE: All passes added for %d particles"), CurrentParticleCount);

	// Phase 2: Extract buffer to persistent storage for next frame reuse
	// This keeps simulation results on GPU without CPU readback
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> EXTRACTION: Queuing buffer extraction..."));
	GraphBuilder.QueueBufferExtraction(
		ParticleBuffer,
		&PersistentParticleBuffer,
		ERHIAccess::UAVCompute  // Ready for next frame's compute passes
	);
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> EXTRACTION: Buffer extraction queued successfully"));
}

//=============================================================================
// RDG Pass Implementations
//=============================================================================

void FGPUFluidSimulator::AddPredictPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FPredictPositionsCS> ComputeShader(ShaderMap);

	FPredictPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPredictPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->Gravity = Params.Gravity;
	PassParameters->ExternalForce = ExternalForce;

	// Debug: log gravity and delta time
	static int32 DebugCounter = 0;
	if (++DebugCounter % 60 == 0)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("PredictPositions: Gravity=(%.2f, %.2f, %.2f), DeltaTime=%.4f, Particles=%d"),
			Params.Gravity.X, Params.Gravity.Y, Params.Gravity.Z, Params.DeltaTime, CurrentParticleCount);
	}

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FPredictPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PredictPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddExtractPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticlesSRV,
	FRDGBufferUAVRef PositionsUAV,
	int32 ParticleCount,
	bool bUsePredictedPosition)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FExtractPositionsCS> ComputeShader(ShaderMap);

	FExtractPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FExtractPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesSRV;
	PassParameters->Positions = PositionsUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->bUsePredictedPosition = bUsePredictedPosition ? 1 : 0;

	const uint32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FExtractPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ExtractPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddComputeDensityPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FComputeDensityCS> ComputeShader(ShaderMap);

	FComputeDensityCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeDensityCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Compliance = Params.Compliance;
	PassParameters->DeltaTimeSq = Params.DeltaTimeSq;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FComputeDensityCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ComputeDensity"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddSolvePressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSolvePressureCS> ComputeShader(ShaderMap);

	FSolvePressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSolvePressureCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FSolvePressureCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SolvePressure"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddApplyViscosityPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyViscosityCS> ComputeShader(ShaderMap);

	FApplyViscosityCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyViscosityCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->ViscosityCoefficient = Params.ViscosityCoefficient;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->CellSize = Params.CellSize;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FApplyViscosityCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyViscosity"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddBoundsCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FBoundsCollisionCS> ComputeShader(ShaderMap);

	FBoundsCollisionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundsCollisionCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = Params.ParticleRadius;
	PassParameters->BoundsMin = Params.BoundsMin;
	PassParameters->BoundsMax = Params.BoundsMax;
	PassParameters->Restitution = Params.BoundsRestitution;
	PassParameters->Friction = Params.BoundsFriction;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FBoundsCollisionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::BoundsCollision"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddDistanceFieldCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if Distance Field collision is not enabled
	if (!DFCollisionParams.bEnabled || !CachedGDFTextureSRV)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FDistanceFieldCollisionCS> ComputeShader(ShaderMap);

	FDistanceFieldCollisionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDistanceFieldCollisionCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = DFCollisionParams.ParticleRadius;

	// Distance Field Volume Parameters
	PassParameters->GDFVolumeCenter = DFCollisionParams.VolumeCenter;
	PassParameters->GDFVolumeExtent = DFCollisionParams.VolumeExtent;
	PassParameters->GDFVoxelSize = FVector3f(DFCollisionParams.VoxelSize);
	PassParameters->GDFMaxDistance = DFCollisionParams.MaxDistance;

	// Collision Response Parameters
	PassParameters->DFCollisionRestitution = DFCollisionParams.Restitution;
	PassParameters->DFCollisionFriction = DFCollisionParams.Friction;
	PassParameters->DFCollisionThreshold = DFCollisionParams.CollisionThreshold;

	// Global Distance Field Texture
	PassParameters->GlobalDistanceFieldTexture = CachedGDFTextureSRV;
	PassParameters->GlobalDistanceFieldSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FDistanceFieldCollisionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::DistanceFieldCollision"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddPrimitiveCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if no collision primitives
	if (!bCollisionPrimitivesValid || GetCollisionPrimitiveCount() == 0)
	{
		return;
	}

	FRDGBufferSRVRef SpheresSRV = nullptr;
	FRDGBufferSRVRef CapsulesSRV = nullptr;
	FRDGBufferSRVRef BoxesSRV = nullptr;
	FRDGBufferSRVRef ConvexesSRV = nullptr;
	FRDGBufferSRVRef ConvexPlanesSRV = nullptr;

	// Dummy data for empty buffers (shader requires all SRVs to be valid)
	static FGPUCollisionSphere DummySphere;
	static FGPUCollisionCapsule DummyCapsule;
	static FGPUCollisionBox DummyBox;
	static FGPUCollisionConvex DummyConvex;
	static FGPUConvexPlane DummyPlane;

	// Create RDG buffers from cached data (or dummy for empty arrays)
	{
		const bool bHasData = CachedSpheres.Num() > 0;
		FRDGBufferRef SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionSpheres"),
			sizeof(FGPUCollisionSphere),
			bHasData ? CachedSpheres.Num() : 1,
			bHasData ? CachedSpheres.GetData() : &DummySphere,
			bHasData ? CachedSpheres.Num() * sizeof(FGPUCollisionSphere) : sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
		SpheresSRV = GraphBuilder.CreateSRV(SpheresBuffer);
	}

	{
		const bool bHasData = CachedCapsules.Num() > 0;
		FRDGBufferRef CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionCapsules"),
			sizeof(FGPUCollisionCapsule),
			bHasData ? CachedCapsules.Num() : 1,
			bHasData ? CachedCapsules.GetData() : &DummyCapsule,
			bHasData ? CachedCapsules.Num() * sizeof(FGPUCollisionCapsule) : sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
		CapsulesSRV = GraphBuilder.CreateSRV(CapsulesBuffer);
	}

	{
		const bool bHasData = CachedBoxes.Num() > 0;
		FRDGBufferRef BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionBoxes"),
			sizeof(FGPUCollisionBox),
			bHasData ? CachedBoxes.Num() : 1,
			bHasData ? CachedBoxes.GetData() : &DummyBox,
			bHasData ? CachedBoxes.Num() * sizeof(FGPUCollisionBox) : sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
		BoxesSRV = GraphBuilder.CreateSRV(BoxesBuffer);
	}

	{
		const bool bHasData = CachedConvexHeaders.Num() > 0;
		FRDGBufferRef ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionConvexes"),
			sizeof(FGPUCollisionConvex),
			bHasData ? CachedConvexHeaders.Num() : 1,
			bHasData ? CachedConvexHeaders.GetData() : &DummyConvex,
			bHasData ? CachedConvexHeaders.Num() * sizeof(FGPUCollisionConvex) : sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
		ConvexesSRV = GraphBuilder.CreateSRV(ConvexesBuffer);
	}

	{
		const bool bHasData = CachedConvexPlanes.Num() > 0;
		FRDGBufferRef ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionConvexPlanes"),
			sizeof(FGPUConvexPlane),
			bHasData ? CachedConvexPlanes.Num() : 1,
			bHasData ? CachedConvexPlanes.GetData() : &DummyPlane,
			bHasData ? CachedConvexPlanes.Num() * sizeof(FGPUConvexPlane) : sizeof(FGPUConvexPlane),
			ERDGInitialDataFlags::NoCopy
		);
		ConvexPlanesSRV = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	}

	// Use the pass builder
	FGPUFluidSimulatorPassBuilder::AddPrimitiveCollisionPass(
		GraphBuilder,
		ParticlesUAV,
		SpheresSRV,
		CapsulesSRV,
		BoxesSRV,
		ConvexesSRV,
		ConvexPlanesSRV,
		CachedSpheres.Num(),
		CachedCapsules.Num(),
		CachedBoxes.Num(),
		CachedConvexHeaders.Num(),
		CurrentParticleCount,
		Params.ParticleRadius,
		PrimitiveCollisionThreshold
	);
}

void FGPUFluidSimulator::AddFinalizePositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFinalizePositionsCS> ComputeShader(ShaderMap);

	FFinalizePositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFinalizePositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->VelocityDamping = VelocityDamping;
	PassParameters->MaxVelocity = MaxVelocity;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FFinalizePositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::FinalizePositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// GPU Particle Spawn System Implementation
//=============================================================================

void FGPUFluidSimulator::AddSpawnRequest(const FVector3f& Position, const FVector3f& Velocity, float Mass)
{
	FScopeLock Lock(&SpawnRequestLock);

	FGPUSpawnRequest Request;
	Request.Position = Position;
	Request.Velocity = Velocity;
	Request.Mass = Mass;
	Request.Radius = DefaultSpawnRadius;

	PendingSpawnRequests.Add(Request);
	bHasPendingSpawnRequests.store(true);

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("AddSpawnRequest: Pos=(%.2f, %.2f, %.2f), Vel=(%.2f, %.2f, %.2f)"),
		Position.X, Position.Y, Position.Z, Velocity.X, Velocity.Y, Velocity.Z);
}

void FGPUFluidSimulator::AddSpawnRequests(const TArray<FGPUSpawnRequest>& Requests)
{
	if (Requests.Num() == 0)
	{
		return;
	}

	FScopeLock Lock(&SpawnRequestLock);

	PendingSpawnRequests.Append(Requests);
	bHasPendingSpawnRequests.store(true);

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("AddSpawnRequests: Added %d spawn requests (total pending: %d)"),
		Requests.Num(), PendingSpawnRequests.Num());
}

void FGPUFluidSimulator::ClearSpawnRequests()
{
	FScopeLock Lock(&SpawnRequestLock);
	PendingSpawnRequests.Empty();
	bHasPendingSpawnRequests.store(false);
}

int32 FGPUFluidSimulator::GetPendingSpawnCount() const
{
	FScopeLock Lock(&SpawnRequestLock);
	return PendingSpawnRequests.Num();
}

void FGPUFluidSimulator::AddSpawnParticlesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef ParticleCounterUAV,
	const TArray<FGPUSpawnRequest>& SpawnRequests)
{
	if (SpawnRequests.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSpawnParticlesCS> ComputeShader(ShaderMap);

	// Create spawn request buffer
	// IMPORTANT: Do NOT use NoCopy - SpawnRequests is temporary data that may be
	// invalidated before RDG pass executes. RDG must copy the data.
	FRDGBufferRef SpawnRequestBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidSpawnRequests"),
		sizeof(FGPUSpawnRequest),
		SpawnRequests.Num(),
		SpawnRequests.GetData(),
		SpawnRequests.Num() * sizeof(FGPUSpawnRequest),
		ERDGInitialDataFlags::None
	);

	FSpawnParticlesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSpawnParticlesCS::FParameters>();
	PassParameters->SpawnRequests = GraphBuilder.CreateSRV(SpawnRequestBuffer);
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCounter = ParticleCounterUAV;
	PassParameters->SpawnRequestCount = SpawnRequests.Num();
	PassParameters->MaxParticleCount = MaxParticleCount;
	PassParameters->NextParticleID = NextParticleID.load();
	PassParameters->DefaultRadius = DefaultSpawnRadius;
	PassParameters->DefaultMass = 1.0f;

	const uint32 NumGroups = FMath::DivideAndRoundUp(SpawnRequests.Num(), FSpawnParticlesCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SpawnParticles(%d)", SpawnRequests.Num()),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);

	// Update next particle ID (atomic increment)
	NextParticleID.fetch_add(SpawnRequests.Num());

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("SpawnParticlesPass: Spawned %d particles (NextID: %d)"),
		SpawnRequests.Num(), NextParticleID.load());
}

//=============================================================================
// FGPUFluidSimulationTask Implementation
//=============================================================================

void FGPUFluidSimulationTask::Execute(
	FGPUFluidSimulator* Simulator,
	const FGPUFluidSimulationParams& Params,
	int32 NumSubsteps)
{
	if (!Simulator || !Simulator->IsReady())
	{
		return;
	}

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
