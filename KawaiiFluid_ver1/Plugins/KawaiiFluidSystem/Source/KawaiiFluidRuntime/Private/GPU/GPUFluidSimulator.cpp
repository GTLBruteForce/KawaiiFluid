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
		OutParticle.ClusterID = GPUParticle.ClusterID;

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
	CachedBoneTransforms = Primitives.BoneTransforms;

	// Check if we have any primitives
	if (Primitives.IsEmpty())
	{
		bCollisionPrimitivesValid = false;
		bBoneTransformsValid = false;
		return;
	}

	bCollisionPrimitivesValid = true;
	bBoneTransformsValid = CachedBoneTransforms.Num() > 0;

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Cached collision primitives: Spheres=%d, Capsules=%d, Boxes=%d, Convexes=%d, Planes=%d, BoneTransforms=%d"),
		CachedSpheres.Num(), CachedCapsules.Num(), CachedBoxes.Num(), CachedConvexHeaders.Num(), CachedConvexPlanes.Num(), CachedBoneTransforms.Num());
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

			// Check if detailed GPU stats are enabled (game thread flag)
			const bool bNeedReadback = GetFluidStatsCollector().IsDetailedGPUEnabled();

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

	// Pass 3: Build Spatial Hash using Persistent Buffers (GPU clear + build, no CPU upload)
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Building with ParticleCount=%d, Radius=%.2f, CellSize=%.2f"),
		CurrentParticleCount, Params.ParticleRadius, Params.CellSize);

	FRDGBufferRef CellCountsBuffer;
	FRDGBufferRef ParticleIndicesBuffer;

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

	FRDGBufferSRVRef CellCountsSRVLocal = GraphBuilder.CreateSRV(CellCountsBuffer);
	FRDGBufferUAVRef CellCountsUAVLocal = GraphBuilder.CreateUAV(CellCountsBuffer);
	FRDGBufferSRVRef ParticleIndicesSRVLocal = GraphBuilder.CreateSRV(ParticleIndicesBuffer);
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

	// Pass 4-5: XPBD Density Constraint Solver (Density + Position correction per iteration)
	// Each iteration:
	//   1. Compute Density and Lambda (based on current predicted positions)
	//   2. Apply position corrections (modifies predicted positions)
	// Lambda accumulates across iterations via XPBD formulation
	for (int32 i = 0; i < Params.SolverIterations; ++i)
	{
		// Compute Density and Lambda (XPBD: Δλ = (-C - α̃·λ_prev) / (|∇C|² + α̃))
		AddComputeDensityPass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal, Params);

		// Apply position corrections: Δx = (λ_i + λ_j + s_corr) · ∇W / ρ₀
		AddSolvePressurePass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal, Params);
	}

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
	AddApplyViscosityPass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal, Params);

	// Pass 8.5: Apply Cohesion (surface tension between particles)
	AddApplyCohesionPass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal, Params);

	// Pass 8.6: Apply Stack Pressure (weight transfer from stacked attached particles)
	if (Params.StackPressureScale > 0.0f && IsAdhesionEnabled() && PersistentAttachmentBuffer.IsValid())
	{
		FRDGBufferRef AttachmentBufferForStackPressure = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsStackPressure"));
		FRDGBufferSRVRef AttachmentSRVForStackPressure = GraphBuilder.CreateSRV(AttachmentBufferForStackPressure);
		AddStackPressurePass(GraphBuilder, ParticlesUAVLocal, AttachmentSRVForStackPressure, CellCountsSRVLocal, ParticleIndicesSRVLocal, Params);
	}

	// Pass 9: Clear just-detached flag at end of frame
	AddClearDetachedFlagPass(GraphBuilder, ParticlesUAVLocal);

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

		if (Axis1Buffer && Axis2Buffer && Axis3Buffer)
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
				FRDGBufferRef DummyAttachmentBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUParticleAttachment), 1),
					TEXT("DummyAttachmentBuffer"));
				AnisotropyParams.AttachmentsSRV = GraphBuilder.CreateSRV(DummyAttachmentBuffer);
			}

			AnisotropyParams.CellCountsSRV = CellCountsSRVLocal;
			AnisotropyParams.ParticleIndicesSRV = ParticleIndicesSRVLocal;
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
			// Density-based anisotropy needs wider neighbor search than simulation
			// Use 2.5x smoothing radius to find enough neighbors for reliable covariance
			AnisotropyParams.SmoothingRadius = Params.SmoothingRadius * 2.5f;
			AnisotropyParams.CellSize = Params.CellSize;

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
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("PredictPositions: Gravity=(%.2f, %.2f, %.2f), DeltaTime=%.4f, Particles=%d"),Params.Gravity.X, Params.Gravity.Y, Params.Gravity.Z, Params.DeltaTime, CurrentParticleCount);
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
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->CellSize = Params.CellSize;
	// Tensile Instability Correction (PBF Eq.13-14)
	PassParameters->bEnableTensileInstability = Params.bEnableTensileInstability;
	PassParameters->TensileK = Params.TensileK;
	PassParameters->TensileN = Params.TensileN;
	PassParameters->InvW_DeltaQ = Params.InvW_DeltaQ;

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

void FGPUFluidSimulator::AddApplyCohesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if cohesion is disabled
	if (Params.CohesionStrength <= 0.0f)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyCohesionCS> ComputeShader(ShaderMap);

	FApplyCohesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyCohesionCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->CohesionStrength = Params.CohesionStrength;
	PassParameters->CellSize = Params.CellSize;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FApplyCohesionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyCohesion"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddStackPressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InAttachmentSRV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if stack pressure is disabled or no attachments
	if (Params.StackPressureScale <= 0.0f || !InAttachmentSRV)
	{
		return;
	}

	// Skip if no bone colliders (no attachments possible)
	if (!bBoneTransformsValid || CachedBoneTransforms.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FStackPressureCS> ComputeShader(ShaderMap);

	// Create collision primitive buffers (same as Adhesion pass)
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(4, 1);

	FRDGBufferRef SpheresBuffer = CachedSpheres.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Spheres"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), sizeof(FGPUCollisionSphere) * CachedSpheres.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheres"));

	FRDGBufferRef CapsulesBuffer = CachedCapsules.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Capsules"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), sizeof(FGPUCollisionCapsule) * CachedCapsules.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsules"));

	FRDGBufferRef BoxesBuffer = CachedBoxes.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Boxes"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), sizeof(FGPUCollisionBox) * CachedBoxes.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxes"));

	FRDGBufferRef ConvexesBuffer = CachedConvexHeaders.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Convexes"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), sizeof(FGPUCollisionConvex) * CachedConvexHeaders.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexes"));

	FRDGBufferRef ConvexPlanesBuffer = CachedConvexPlanes.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_ConvexPlanes"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), sizeof(FGPUConvexPlane) * CachedConvexPlanes.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanes"));

	FStackPressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStackPressureCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->Attachments = InAttachmentSRV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;

	// Collision primitives for surface normal calculation
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);

	// Parameters
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->StackPressureScale = Params.StackPressureScale;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Gravity = FVector3f(Params.Gravity);
	PassParameters->DeltaTime = Params.DeltaTime;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FStackPressureCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::StackPressure"),
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

	// OBB parameters
	PassParameters->BoundsCenter = Params.BoundsCenter;
	PassParameters->BoundsExtent = Params.BoundsExtent;
	PassParameters->BoundsRotation = Params.BoundsRotation;
	PassParameters->bUseOBB = Params.bUseOBB;

	// Legacy AABB parameters
	PassParameters->BoundsMin = Params.BoundsMin;
	PassParameters->BoundsMax = Params.BoundsMax;

	// Collision response
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

	// Create collision feedback buffers (for particle -> player interaction)
	FRDGBufferRef FeedbackBuffer = nullptr;
	FRDGBufferRef CounterBuffer = nullptr;

	// Create feedback buffer (persistent across frames for extraction)
	if (bCollisionFeedbackEnabled)
	{
		// Create or reuse feedback buffer
		if (CollisionFeedbackBuffer.IsValid())
		{
			FeedbackBuffer = GraphBuilder.RegisterExternalBuffer(CollisionFeedbackBuffer, TEXT("GPUCollisionFeedback"));
		}
		else
		{
			FRDGBufferDesc FeedbackDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUCollisionFeedback), MAX_COLLISION_FEEDBACK);
			FeedbackBuffer = GraphBuilder.CreateBuffer(FeedbackDesc, TEXT("GPUCollisionFeedback"));
		}

		// Create counter buffer (reset each frame)
		if (CollisionCounterBuffer.IsValid())
		{
			CounterBuffer = GraphBuilder.RegisterExternalBuffer(CollisionCounterBuffer, TEXT("GPUCollisionCounter"));
		}
		else
		{
			FRDGBufferDesc CounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
			CounterBuffer = GraphBuilder.CreateBuffer(CounterDesc, TEXT("GPUCollisionCounter"));
		}

		// Clear counter at start of frame
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CounterBuffer), 0);
	}
	else
	{
		// Create dummy buffers when feedback is disabled
		FRDGBufferDesc DummyFeedbackDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUCollisionFeedback), 1);
		FeedbackBuffer = GraphBuilder.CreateBuffer(DummyFeedbackDesc, TEXT("GPUCollisionFeedbackDummy"));

		FRDGBufferDesc DummyCounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		CounterBuffer = GraphBuilder.CreateBuffer(DummyCounterDesc, TEXT("GPUCollisionCounterDummy"));
	}

	// Create collider contact count buffer (항상 생성 - 간단한 충돌 감지용)
	FRDGBufferRef ContactCountBuffer = nullptr;
	if (ColliderContactCountBuffer.IsValid())
	{
		ContactCountBuffer = GraphBuilder.RegisterExternalBuffer(ColliderContactCountBuffer, TEXT("ColliderContactCounts"));
	}
	else
	{
		FRDGBufferDesc ContactCountDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MAX_COLLIDER_COUNT);
		ContactCountBuffer = GraphBuilder.CreateBuffer(ContactCountDesc, TEXT("ColliderContactCounts"));
	}

	// Clear contact counts at start of frame
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ContactCountBuffer), 0);

	// Dispatch primitive collision shader directly (with feedback buffers)
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FPrimitiveCollisionCS> ComputeShader(GlobalShaderMap);

	FPrimitiveCollisionCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FPrimitiveCollisionCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = Params.ParticleRadius;
	PassParameters->CollisionThreshold = PrimitiveCollisionThreshold;

	PassParameters->CollisionSpheres = SpheresSRV;
	PassParameters->SphereCount = CachedSpheres.Num();

	PassParameters->CollisionCapsules = CapsulesSRV;
	PassParameters->CapsuleCount = CachedCapsules.Num();

	PassParameters->CollisionBoxes = BoxesSRV;
	PassParameters->BoxCount = CachedBoxes.Num();

	PassParameters->CollisionConvexes = ConvexesSRV;
	PassParameters->ConvexCount = CachedConvexHeaders.Num();

	PassParameters->ConvexPlanes = ConvexPlanesSRV;

	// Collision feedback parameters
	PassParameters->CollisionFeedback = GraphBuilder.CreateUAV(FeedbackBuffer);
	PassParameters->CollisionCounter = GraphBuilder.CreateUAV(CounterBuffer);
	PassParameters->MaxCollisionFeedback = MAX_COLLISION_FEEDBACK;
	PassParameters->bEnableCollisionFeedback = bCollisionFeedbackEnabled ? 1 : 0;

	// Collider contact count parameters (항상 활성화 - 간단한 충돌 감지용)
	PassParameters->ColliderContactCounts = GraphBuilder.CreateUAV(ContactCountBuffer);
	PassParameters->MaxColliderCount = MAX_COLLIDER_COUNT;

	const int32 ThreadGroupSize = FPrimitiveCollisionCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PrimitiveCollision(%d particles, %d primitives, feedback=%s)",
			CurrentParticleCount, CachedSpheres.Num() + CachedCapsules.Num() + CachedBoxes.Num() + CachedConvexHeaders.Num(),
			bCollisionFeedbackEnabled ? TEXT("ON") : TEXT("OFF")),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));

	// Extract feedback buffers for next frame (only if feedback is enabled)
	if (bCollisionFeedbackEnabled)
	{
		GraphBuilder.QueueBufferExtraction(
			FeedbackBuffer,
			&CollisionFeedbackBuffer,
			ERHIAccess::UAVCompute
		);

		GraphBuilder.QueueBufferExtraction(
			CounterBuffer,
			&CollisionCounterBuffer,
			ERHIAccess::UAVCompute
		);
	}

	// Always extract collider contact count buffer (간단한 충돌 감지용)
	GraphBuilder.QueueBufferExtraction(
		ContactCountBuffer,
		&ColliderContactCountBuffer,
		ERHIAccess::UAVCompute
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
	PassParameters->MaxVelocity = MaxVelocity;  // Safety clamp (50000 cm/s = 500 m/s)
	PassParameters->GlobalDamping = Params.GlobalDamping;

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

	//UE_LOG(LogGPUFluidSimulator, Log, TEXT("AddSpawnRequests: Added %d spawn requests (total pending: %d)"),Requests.Num(), PendingSpawnRequests.Num());
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
	PassParameters->DefaultMass = DefaultSpawnMass;

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

	//UE_LOG(LogGPUFluidSimulator, Log, TEXT("SpawnParticlesPass: Spawned %d particles (NextID: %d)"), SpawnRequests.Num(), NextParticleID.load());
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

//=============================================================================
// Stream Compaction Implementation (Phase 2 - Per-Polygon Collision)
//=============================================================================

void FGPUFluidSimulator::AllocateStreamCompactionBuffers(FRHICommandListImmediate& RHICmdList)
{
	if (bStreamCompactionBuffersAllocated || MaxParticleCount <= 0)
	{
		return;
	}

	const int32 BlockSize = 256;
	const int32 NumBlocks = FMath::DivideAndRoundUp(MaxParticleCount, BlockSize);

	// Marked flags buffer (uint per particle)
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_MarkedFlags"));
		MarkedFlagsBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(uint32), MaxParticleCount * sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource, CreateInfo);
		MarkedFlagsSRV = RHICmdList.CreateShaderResourceView(MarkedFlagsBufferRHI);
		MarkedFlagsUAV = RHICmdList.CreateUnorderedAccessView(MarkedFlagsBufferRHI, false, false);
	}

	// Marked AABB index buffer (int per particle)
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_MarkedAABBIndex"));
		MarkedAABBIndexBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(int32), MaxParticleCount * sizeof(int32),
			BUF_UnorderedAccess | BUF_ShaderResource, CreateInfo);
		MarkedAABBIndexSRV = RHICmdList.CreateShaderResourceView(MarkedAABBIndexBufferRHI);
		MarkedAABBIndexUAV = RHICmdList.CreateUnorderedAccessView(MarkedAABBIndexBufferRHI, false, false);
	}

	// Prefix sums buffer
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_PrefixSums"));
		PrefixSumsBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(uint32), MaxParticleCount * sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource, CreateInfo);
		PrefixSumsSRV = RHICmdList.CreateShaderResourceView(PrefixSumsBufferRHI);
		PrefixSumsUAV = RHICmdList.CreateUnorderedAccessView(PrefixSumsBufferRHI, false, false);
	}

	// Block sums buffer
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_BlockSums"));
		BlockSumsBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(uint32), NumBlocks * sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource, CreateInfo);
		BlockSumsSRV = RHICmdList.CreateShaderResourceView(BlockSumsBufferRHI);
		BlockSumsUAV = RHICmdList.CreateUnorderedAccessView(BlockSumsBufferRHI, false, false);
	}

	// Compacted candidates buffer (worst case: all particles)
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_CompactedCandidates"));
		CompactedCandidatesBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(FGPUCandidateParticle), MaxParticleCount * sizeof(FGPUCandidateParticle),
			BUF_UnorderedAccess | BUF_ShaderResource, CreateInfo);
		CompactedCandidatesUAV = RHICmdList.CreateUnorderedAccessView(CompactedCandidatesBufferRHI, false, false);
	}

	// Total count buffer (single uint)
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_TotalCount"));
		TotalCountBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(uint32), sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource, CreateInfo);
		TotalCountUAV = RHICmdList.CreateUnorderedAccessView(TotalCountBufferRHI, false, false);
	}

	// Staging buffers for readback
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_TotalCountStaging"));
		TotalCountStagingBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(uint32), sizeof(uint32),
			BUF_None, ERHIAccess::CopyDest, CreateInfo);
	}
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_CandidatesStaging"));
		CandidatesStagingBufferRHI = RHICmdList.CreateStructuredBuffer(
			sizeof(FGPUCandidateParticle), MaxParticleCount * sizeof(FGPUCandidateParticle),
			BUF_None, ERHIAccess::CopyDest, CreateInfo);
	}

	bStreamCompactionBuffersAllocated = true;
	UE_LOG(LogGPUFluidSimulator, Log, TEXT("Stream Compaction buffers allocated (MaxParticles=%d, NumBlocks=%d)"), MaxParticleCount, NumBlocks);
}

void FGPUFluidSimulator::ReleaseStreamCompactionBuffers()
{
	MarkedFlagsBufferRHI.SafeRelease();
	MarkedFlagsSRV.SafeRelease();
	MarkedFlagsUAV.SafeRelease();

	MarkedAABBIndexBufferRHI.SafeRelease();
	MarkedAABBIndexSRV.SafeRelease();
	MarkedAABBIndexUAV.SafeRelease();

	PrefixSumsBufferRHI.SafeRelease();
	PrefixSumsSRV.SafeRelease();
	PrefixSumsUAV.SafeRelease();

	BlockSumsBufferRHI.SafeRelease();
	BlockSumsSRV.SafeRelease();
	BlockSumsUAV.SafeRelease();

	CompactedCandidatesBufferRHI.SafeRelease();
	CompactedCandidatesUAV.SafeRelease();

	TotalCountBufferRHI.SafeRelease();
	TotalCountUAV.SafeRelease();

	FilterAABBsBufferRHI.SafeRelease();
	FilterAABBsSRV.SafeRelease();

	TotalCountStagingBufferRHI.SafeRelease();
	CandidatesStagingBufferRHI.SafeRelease();

	bStreamCompactionBuffersAllocated = false;
	bHasFilteredCandidates = false;
	FilteredCandidateCount = 0;
}

void FGPUFluidSimulator::ExecuteAABBFiltering(const TArray<FGPUFilterAABB>& FilterAABBs)
{
	if (!bIsInitialized || FilterAABBs.Num() == 0 || CurrentParticleCount == 0)
	{
		bHasFilteredCandidates = false;
		FilteredCandidateCount = 0;
		return;
	}

	// Make a copy of the filter AABBs for the render thread
	TArray<FGPUFilterAABB> FilterAABBsCopy = FilterAABBs;
	FGPUFluidSimulator* Self = this;

	ENQUEUE_RENDER_COMMAND(ExecuteAABBFiltering)(
		[Self, FilterAABBsCopy](FRHICommandListImmediate& RHICmdList)
		{
			// Allocate buffers if needed
			if (!Self->bStreamCompactionBuffersAllocated)
			{
				Self->AllocateStreamCompactionBuffers(RHICmdList);
			}

			// Upload filter AABBs
			const int32 NumAABBs = FilterAABBsCopy.Num();
			if (!Self->FilterAABBsBufferRHI.IsValid() || Self->CurrentFilterAABBCount < NumAABBs)
			{
				Self->FilterAABBsBufferRHI.SafeRelease();
				Self->FilterAABBsSRV.SafeRelease();

				FRHIResourceCreateInfo CreateInfo(TEXT("StreamCompaction_FilterAABBs"));
				Self->FilterAABBsBufferRHI = RHICmdList.CreateStructuredBuffer(
					sizeof(FGPUFilterAABB), NumAABBs * sizeof(FGPUFilterAABB),
					BUF_ShaderResource, CreateInfo);
				Self->FilterAABBsSRV = RHICmdList.CreateShaderResourceView(Self->FilterAABBsBufferRHI);
				Self->CurrentFilterAABBCount = NumAABBs;
			}

			// Upload AABB data
			void* AABBData = RHICmdList.LockBuffer(Self->FilterAABBsBufferRHI, 0,
				NumAABBs * sizeof(FGPUFilterAABB), RLM_WriteOnly);
			FMemory::Memcpy(AABBData, FilterAABBsCopy.GetData(), NumAABBs * sizeof(FGPUFilterAABB));
			RHICmdList.UnlockBuffer(Self->FilterAABBsBufferRHI);

			// Get the correct particle SRV - use PersistentParticleBuffer if available (GPU simulation mode)
			FShaderResourceViewRHIRef ParticleSRVToUse = Self->ParticleSRV;
			if (Self->PersistentParticleBuffer.IsValid())
			{
				FBufferRHIRef PersistentRHI = Self->PersistentParticleBuffer->GetRHI();
				if (PersistentRHI.IsValid())
				{
					ParticleSRVToUse = RHICmdList.CreateShaderResourceView(PersistentRHI);
					UE_LOG(LogGPUFluidSimulator, Log, TEXT("AABB Filtering: Using PersistentParticleBuffer SRV (GPU simulation mode)"));
				}
			}
			else
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("AABB Filtering: PersistentParticleBuffer not valid, using fallback ParticleSRV"));
			}

			// Execute stream compaction using direct RHI dispatch
			Self->DispatchStreamCompactionShaders(RHICmdList, Self->CurrentParticleCount, NumAABBs, ParticleSRVToUse);
		}
	);
}

void FGPUFluidSimulator::DispatchStreamCompactionShaders(FRHICommandListImmediate& RHICmdList, int32 ParticleCount, int32 NumAABBs, FShaderResourceViewRHIRef InParticleSRV)
{
	const int32 BlockSize = 256;
	const int32 NumBlocks = FMath::DivideAndRoundUp(ParticleCount, BlockSize);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Pass 1: AABB Mark - Mark particles that are inside any AABB
	{
		TShaderMapRef<FAABBMarkCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FAABBMarkCS::FParameters Parameters;
		Parameters.Particles = InParticleSRV;  // Use the passed SRV (from PersistentParticleBuffer)
		Parameters.FilterAABBs = FilterAABBsSRV;
		Parameters.MarkedFlags = MarkedFlagsUAV;
		Parameters.MarkedAABBIndex = MarkedAABBIndexUAV;
		Parameters.ParticleCount = ParticleCount;
		Parameters.NumAABBs = NumAABBs;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FAABBMarkCS::ThreadGroupSize);
		RHICmdList.DispatchComputeShader(NumGroups, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier between passes
	RHICmdList.Transition(FRHITransitionInfo(MarkedFlagsBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 2a: Prefix Sum Block - Blelloch scan within each block
	{
		TShaderMapRef<FPrefixSumBlockCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FPrefixSumBlockCS::FParameters Parameters;
		Parameters.MarkedFlags = MarkedFlagsSRV;
		Parameters.PrefixSums = PrefixSumsUAV;
		Parameters.BlockSums = BlockSumsUAV;
		Parameters.ElementCount = ParticleCount;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		RHICmdList.DispatchComputeShader(NumBlocks, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(BlockSumsBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 2b: Scan Block Sums - Sequential scan of block sums
	{
		TShaderMapRef<FScanBlockSumsCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FScanBlockSumsCS::FParameters Parameters;
		Parameters.BlockSums = BlockSumsUAV;
		Parameters.BlockCount = NumBlocks;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		RHICmdList.DispatchComputeShader(1, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(BlockSumsBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 2c: Add Block Offsets - Add scanned block sums to each element
	{
		TShaderMapRef<FAddBlockOffsetsCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FAddBlockOffsetsCS::FParameters Parameters;
		Parameters.PrefixSums = PrefixSumsUAV;
		Parameters.BlockSums = BlockSumsUAV;
		Parameters.ElementCount = ParticleCount;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		RHICmdList.DispatchComputeShader(NumBlocks, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(PrefixSumsBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 3: Compact - Write marked particles to compacted output
	{
		TShaderMapRef<FCompactCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FCompactCS::FParameters Parameters;
		Parameters.Particles = InParticleSRV;  // Use same buffer as AABB Mark pass!
		Parameters.MarkedFlags = MarkedFlagsSRV;
		Parameters.PrefixSums = PrefixSumsSRV;
		Parameters.MarkedAABBIndex = MarkedAABBIndexSRV;
		Parameters.CompactedParticles = CompactedCandidatesUAV;
		Parameters.ParticleCount = ParticleCount;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FCompactCS::ThreadGroupSize);
		RHICmdList.DispatchComputeShader(NumGroups, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(CompactedCandidatesBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 4: Write Total Count
	{
		TShaderMapRef<FWriteTotalCountCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FWriteTotalCountCS::FParameters Parameters;
		Parameters.MarkedFlagsForCount = MarkedFlagsSRV;
		Parameters.PrefixSumsForCount = PrefixSumsSRV;
		Parameters.TotalCount = TotalCountUAV;
		Parameters.ParticleCount = ParticleCount;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		RHICmdList.DispatchComputeShader(1, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// Readback total count
	RHICmdList.Transition(FRHITransitionInfo(TotalCountBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
	RHICmdList.CopyBufferRegion(TotalCountStagingBufferRHI, 0, TotalCountBufferRHI, 0, sizeof(uint32));

	uint32* CountPtr = (uint32*)RHICmdList.LockBuffer(TotalCountStagingBufferRHI, 0, sizeof(uint32), RLM_ReadOnly);
	FilteredCandidateCount = static_cast<int32>(*CountPtr);
	RHICmdList.UnlockBuffer(TotalCountStagingBufferRHI);

	bHasFilteredCandidates = (FilteredCandidateCount > 0);

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("AABB Filtering complete: %d/%d particles matched %d AABBs"),
		FilteredCandidateCount, ParticleCount, NumAABBs);
}

bool FGPUFluidSimulator::GetFilteredCandidates(TArray<FGPUCandidateParticle>& OutCandidates)
{
	if (!bHasFilteredCandidates || FilteredCandidateCount == 0 || !CompactedCandidatesBufferRHI.IsValid())
	{
		OutCandidates.Empty();
		return false;
	}

	FGPUFluidSimulator* Self = this;
	TArray<FGPUCandidateParticle>* OutPtr = &OutCandidates;
	const int32 Count = FilteredCandidateCount;

	// Synchronous readback (blocks until GPU is ready)
	ENQUEUE_RENDER_COMMAND(GetFilteredCandidates)(
		[Self, OutPtr, Count](FRHICommandListImmediate& RHICmdList)
		{
			if (!Self->CompactedCandidatesBufferRHI.IsValid())
			{
				return;
			}

			const uint32 CopySize = Count * sizeof(FGPUCandidateParticle);

			// Transition buffer for copy
			RHICmdList.Transition(FRHITransitionInfo(Self->CompactedCandidatesBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));

			// Copy to staging buffer
			RHICmdList.CopyBufferRegion(Self->CandidatesStagingBufferRHI, 0, Self->CompactedCandidatesBufferRHI, 0, CopySize);

			// Read back
			OutPtr->SetNumUninitialized(Count);
			FGPUCandidateParticle* DataPtr = (FGPUCandidateParticle*)RHICmdList.LockBuffer(
				Self->CandidatesStagingBufferRHI, 0, CopySize, RLM_ReadOnly);
			FMemory::Memcpy(OutPtr->GetData(), DataPtr, CopySize);
			RHICmdList.UnlockBuffer(Self->CandidatesStagingBufferRHI);
		}
	);

	// Wait for render command to complete
	FlushRenderingCommands();

	return OutCandidates.Num() > 0;
}

//=============================================================================
// Per-Polygon Collision Correction Implementation
//=============================================================================

void FGPUFluidSimulator::ApplyCorrections(const TArray<FParticleCorrection>& Corrections)
{
	if (!bIsInitialized || Corrections.Num() == 0 || !PersistentParticleBuffer.IsValid())
	{
		return;
	}

	// Make a copy of corrections for the render thread
	TArray<FParticleCorrection> CorrectionsCopy = Corrections;
	FGPUFluidSimulator* Self = this;
	const int32 CorrectionCount = Corrections.Num();

	ENQUEUE_RENDER_COMMAND(ApplyPerPolygonCorrections)(
		[Self, CorrectionsCopy, CorrectionCount](FRHICommandListImmediate& RHICmdList)
		{
			if (!Self->PersistentParticleBuffer.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyCorrections: PersistentParticleBuffer not valid"));
				return;
			}

			// Create corrections buffer
			FRHIResourceCreateInfo CreateInfo(TEXT("PerPolygonCorrections"));
			FBufferRHIRef CorrectionsBufferRHI = RHICmdList.CreateStructuredBuffer(
				sizeof(FParticleCorrection),
				CorrectionCount * sizeof(FParticleCorrection),
				BUF_ShaderResource,
				CreateInfo
			);

			// Upload corrections data
			void* CorrectionData = RHICmdList.LockBuffer(CorrectionsBufferRHI, 0,
				CorrectionCount * sizeof(FParticleCorrection), RLM_WriteOnly);
			FMemory::Memcpy(CorrectionData, CorrectionsCopy.GetData(), CorrectionCount * sizeof(FParticleCorrection));
			RHICmdList.UnlockBuffer(CorrectionsBufferRHI);

			// Create SRV for corrections
			FShaderResourceViewRHIRef CorrectionsSRV = RHICmdList.CreateShaderResourceView(CorrectionsBufferRHI);

			// Create UAV for particles from PersistentParticleBuffer
			FBufferRHIRef ParticleRHI = Self->PersistentParticleBuffer->GetRHI();
			if (!ParticleRHI.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyCorrections: Failed to get ParticleRHI from PersistentParticleBuffer"));
				return;
			}
			FUnorderedAccessViewRHIRef ParticlesUAV = RHICmdList.CreateUnorderedAccessView(ParticleRHI, false, false);

			// Dispatch ApplyCorrections compute shader
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FApplyCorrectionsCS> ComputeShader(ShaderMap);
			FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
			SetComputePipelineState(RHICmdList, ShaderRHI);

			FApplyCorrectionsCS::FParameters Parameters;
			Parameters.Corrections = CorrectionsSRV;
			Parameters.Particles = ParticlesUAV;
			Parameters.CorrectionCount = CorrectionCount;
			SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

			const int32 NumGroups = FMath::DivideAndRoundUp(CorrectionCount, FApplyCorrectionsCS::ThreadGroupSize);
			RHICmdList.DispatchComputeShader(NumGroups, 1, 1);

			UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);

			UE_LOG(LogGPUFluidSimulator, Log, TEXT("ApplyCorrections: Applied %d corrections"), CorrectionCount);
		}
	);
}

void FGPUFluidSimulator::ApplyAttachmentUpdates(const TArray<FAttachedParticleUpdate>& Updates)
{
	if (!bIsInitialized || Updates.Num() == 0 || !PersistentParticleBuffer.IsValid())
	{
		return;
	}

	// Make a copy of updates for the render thread
	TArray<FAttachedParticleUpdate> UpdatesCopy = Updates;
	FGPUFluidSimulator* Self = this;
	const int32 UpdateCount = Updates.Num();

	ENQUEUE_RENDER_COMMAND(ApplyAttachmentUpdates)(
		[Self, UpdatesCopy, UpdateCount](FRHICommandListImmediate& RHICmdList)
		{
			if (!Self->PersistentParticleBuffer.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyAttachmentUpdates: PersistentParticleBuffer not valid"));
				return;
			}

			// Create updates buffer
			FRHIResourceCreateInfo CreateInfo(TEXT("AttachmentUpdates"));
			FBufferRHIRef UpdatesBufferRHI = RHICmdList.CreateStructuredBuffer(
				sizeof(FAttachedParticleUpdate),
				UpdateCount * sizeof(FAttachedParticleUpdate),
				BUF_ShaderResource,
				CreateInfo
			);

			// Upload updates data
			void* UpdateData = RHICmdList.LockBuffer(UpdatesBufferRHI, 0,
				UpdateCount * sizeof(FAttachedParticleUpdate), RLM_WriteOnly);
			FMemory::Memcpy(UpdateData, UpdatesCopy.GetData(), UpdateCount * sizeof(FAttachedParticleUpdate));
			RHICmdList.UnlockBuffer(UpdatesBufferRHI);

			// Create SRV for updates
			FShaderResourceViewRHIRef UpdatesSRV = RHICmdList.CreateShaderResourceView(UpdatesBufferRHI);

			// Create UAV for particles from PersistentParticleBuffer
			FBufferRHIRef ParticleRHI = Self->PersistentParticleBuffer->GetRHI();
			if (!ParticleRHI.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyAttachmentUpdates: Failed to get ParticleRHI from PersistentParticleBuffer"));
				return;
			}
			FUnorderedAccessViewRHIRef ParticlesUAV = RHICmdList.CreateUnorderedAccessView(ParticleRHI, false, false);

			// Dispatch ApplyAttachmentUpdates compute shader
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FApplyAttachmentUpdatesCS> ComputeShader(ShaderMap);
			FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
			SetComputePipelineState(RHICmdList, ShaderRHI);

			FApplyAttachmentUpdatesCS::FParameters Parameters;
			Parameters.AttachmentUpdates = UpdatesSRV;
			Parameters.Particles = ParticlesUAV;
			Parameters.UpdateCount = UpdateCount;
			SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

			const int32 NumGroups = FMath::DivideAndRoundUp(UpdateCount, FApplyAttachmentUpdatesCS::ThreadGroupSize);
			RHICmdList.DispatchComputeShader(NumGroups, 1, 1);

			UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);

			UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("ApplyAttachmentUpdates: Applied %d updates"), UpdateCount);
		}
	);
}

//=============================================================================
// Adhesion Pass Implementations (GPU-based bone attachment)
//=============================================================================

void FGPUFluidSimulator::AddAdhesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef AttachmentUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (!IsAdhesionEnabled() || !bBoneTransformsValid || CachedBoneTransforms.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FAdhesionCS> ComputeShader(ShaderMap);

	// Upload bone transforms
	FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidBoneTransforms"),
		sizeof(FGPUBoneTransform),
		CachedBoneTransforms.Num(),
		CachedBoneTransforms.GetData(),
		CachedBoneTransforms.Num() * sizeof(FGPUBoneTransform),
		ERDGInitialDataFlags::NoCopy
	);
	FRDGBufferSRVRef BoneTransformsSRVLocal = GraphBuilder.CreateSRV(BoneTransformsBuffer);

	// Upload collision primitives for adhesion check
	FRDGBufferRef SpheresBuffer = nullptr;
	FRDGBufferRef CapsulesBuffer = nullptr;
	FRDGBufferRef BoxesBuffer = nullptr;
	FRDGBufferRef ConvexesBuffer = nullptr;
	FRDGBufferRef ConvexPlanesBuffer = nullptr;

	if (CachedSpheres.Num() > 0)
	{
		SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionSpheres"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), CachedSpheres.Num() * sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedCapsules.Num() > 0)
	{
		CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionCapsules"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), CachedCapsules.Num() * sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedBoxes.Num() > 0)
	{
		BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionBoxes"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), CachedBoxes.Num() * sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexHeaders.Num() > 0)
	{
		ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionConvexes"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), CachedConvexHeaders.Num() * sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexPlanes.Num() > 0)
	{
		ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidConvexPlanes"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), CachedConvexPlanes.Num() * sizeof(FGPUConvexPlane),
			ERDGInitialDataFlags::NoCopy
		);
	}

	// Dummy buffers for empty arrays
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(16, 1);
	if (!SpheresBuffer) SpheresBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheres"));
	if (!CapsulesBuffer) CapsulesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsules"));
	if (!BoxesBuffer) BoxesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxes"));
	if (!ConvexesBuffer) ConvexesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexes"));
	if (!ConvexPlanesBuffer) ConvexPlanesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanes"));

	FAdhesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FAdhesionCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = Params.ParticleRadius;
	PassParameters->Attachments = AttachmentUAV;
	PassParameters->BoneTransforms = BoneTransformsSRVLocal;
	PassParameters->BoneCount = CachedBoneTransforms.Num();
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	PassParameters->AdhesionStrength = CachedAdhesionParams.AdhesionStrength;
	PassParameters->AdhesionRadius = CachedAdhesionParams.AdhesionRadius;
	PassParameters->DetachAccelThreshold = CachedAdhesionParams.DetachAccelThreshold;
	PassParameters->DetachDistanceThreshold = CachedAdhesionParams.DetachDistanceThreshold;
	PassParameters->ColliderContactOffset = CachedAdhesionParams.ColliderContactOffset;
	PassParameters->BoneVelocityScale = CachedAdhesionParams.BoneVelocityScale;
	PassParameters->SlidingFriction = CachedAdhesionParams.SlidingFriction;
	PassParameters->CurrentTime = Params.CurrentTime;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->bEnableAdhesion = CachedAdhesionParams.bEnableAdhesion;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FAdhesionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::Adhesion"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddUpdateAttachedPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef AttachmentSRV,
	FRDGBufferSRVRef InBoneTransformsSRV,
	const FGPUFluidSimulationParams& Params)
{
	// This signature is for external calls. We'll use the internal version.
}

void FGPUFluidSimulator::AddUpdateAttachedPositionsPassInternal(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef AttachmentUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (!IsAdhesionEnabled() || !bBoneTransformsValid || CachedBoneTransforms.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FUpdateAttachedPositionsCS> ComputeShader(ShaderMap);

	// Upload bone transforms
	FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidBoneTransformsUpdate"),
		sizeof(FGPUBoneTransform),
		CachedBoneTransforms.Num(),
		CachedBoneTransforms.GetData(),
		CachedBoneTransforms.Num() * sizeof(FGPUBoneTransform),
		ERDGInitialDataFlags::NoCopy
	);
	FRDGBufferSRVRef BoneTransformsSRVLocal = GraphBuilder.CreateSRV(BoneTransformsBuffer);

	// Upload collision primitives for detachment distance check
	FRDGBufferRef SpheresBuffer = nullptr;
	FRDGBufferRef CapsulesBuffer = nullptr;
	FRDGBufferRef BoxesBuffer = nullptr;
	FRDGBufferRef ConvexesBuffer = nullptr;
	FRDGBufferRef ConvexPlanesBuffer = nullptr;

	if (CachedSpheres.Num() > 0)
	{
		SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionSpheresUpdate"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), CachedSpheres.Num() * sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedCapsules.Num() > 0)
	{
		CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionCapsulesUpdate"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), CachedCapsules.Num() * sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedBoxes.Num() > 0)
	{
		BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionBoxesUpdate"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), CachedBoxes.Num() * sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexHeaders.Num() > 0)
	{
		ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionConvexesUpdate"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), CachedConvexHeaders.Num() * sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexPlanes.Num() > 0)
	{
		ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidConvexPlanesUpdate"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), CachedConvexPlanes.Num() * sizeof(FGPUConvexPlane),
			ERDGInitialDataFlags::NoCopy
		);
	}

	// Dummy buffers for empty arrays
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(16, 1);
	if (!SpheresBuffer) SpheresBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheresUpdate"));
	if (!CapsulesBuffer) CapsulesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsulesUpdate"));
	if (!BoxesBuffer) BoxesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxesUpdate"));
	if (!ConvexesBuffer) ConvexesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexesUpdate"));
	if (!ConvexPlanesBuffer) ConvexPlanesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanesUpdate"));

	FUpdateAttachedPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpdateAttachedPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->Attachments = AttachmentUAV;
	PassParameters->BoneTransforms = BoneTransformsSRVLocal;
	PassParameters->BoneCount = CachedBoneTransforms.Num();
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	PassParameters->DetachAccelThreshold = CachedAdhesionParams.DetachAccelThreshold;
	PassParameters->DetachDistanceThreshold = CachedAdhesionParams.DetachDistanceThreshold;
	PassParameters->ColliderContactOffset = CachedAdhesionParams.ColliderContactOffset;
	PassParameters->BoneVelocityScale = CachedAdhesionParams.BoneVelocityScale;
	PassParameters->SlidingFriction = CachedAdhesionParams.SlidingFriction;
	PassParameters->DeltaTime = Params.DeltaTime;

	// Gravity sliding parameters
	PassParameters->Gravity = CachedAdhesionParams.Gravity;
	PassParameters->GravitySlidingScale = CachedAdhesionParams.GravitySlidingScale;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FUpdateAttachedPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::UpdateAttachedPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddClearDetachedFlagPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV)
{
	if (!IsAdhesionEnabled())
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FClearDetachedFlagCS> ComputeShader(ShaderMap);

	FClearDetachedFlagCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearDetachedFlagCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FClearDetachedFlagCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ClearDetachedFlag"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Collision Feedback Implementation (Particle -> Player Interaction)
// GPU collision data readback for force calculation and event triggering
//=============================================================================

void FGPUFluidSimulator::AllocateCollisionFeedbackBuffers(FRHICommandListImmediate& RHICmdList)
{
	// Allocate FRHIGPUBufferReadback objects for truly async readback
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		if (FeedbackReadbacks[i] == nullptr)
		{
			FeedbackReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("CollisionFeedbackReadback_%d"), i));
		}

		if (CounterReadbacks[i] == nullptr)
		{
			CounterReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("CollisionCounterReadback_%d"), i));
		}

		if (ContactCountReadbacks[i] == nullptr)
		{
			ContactCountReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("ContactCountReadback_%d"), i));
		}
	}

	// Initialize ready feedback array
	ReadyFeedback.SetNum(MAX_COLLISION_FEEDBACK);
	ReadyFeedbackCount = 0;

	// Initialize ready contact counts array
	ReadyColliderContactCounts.SetNumZeroed(MAX_COLLIDER_COUNT);

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("Collision Feedback readback objects allocated (MaxFeedback=%d, NumBuffers=%d, MaxColliders=%d)"), MAX_COLLISION_FEEDBACK, NUM_FEEDBACK_BUFFERS, MAX_COLLIDER_COUNT);
}

void FGPUFluidSimulator::ReleaseCollisionFeedbackBuffers()
{
	CollisionFeedbackBuffer.SafeRelease();
	CollisionCounterBuffer.SafeRelease();
	ColliderContactCountBuffer.SafeRelease();

	// Delete readback objects
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		if (FeedbackReadbacks[i] != nullptr)
		{
			delete FeedbackReadbacks[i];
			FeedbackReadbacks[i] = nullptr;
		}
		if (CounterReadbacks[i] != nullptr)
		{
			delete CounterReadbacks[i];
			CounterReadbacks[i] = nullptr;
		}
		if (ContactCountReadbacks[i] != nullptr)
		{
			delete ContactCountReadbacks[i];
			ContactCountReadbacks[i] = nullptr;
		}
	}

	ContactCountFrameNumber = 0;

	ReadyFeedback.Empty();
	ReadyFeedbackCount = 0;
	ReadyColliderContactCounts.Empty();
	CurrentFeedbackWriteIndex = 0;
	CompletedFeedbackFrame.store(-1);
	FeedbackFrameNumber = 0;
}

void FGPUFluidSimulator::ProcessCollisionFeedbackReadback(FRHICommandListImmediate& RHICmdList)
{
	if (!bCollisionFeedbackEnabled)
	{
		return;
	}

	// Ensure readback objects are allocated
	if (FeedbackReadbacks[0] == nullptr)
	{
		return;  // Will be allocated in SimulateSubstep
	}

	// Read from readback that was enqueued 2 frames ago (allowing GPU latency)
	const int32 ReadIdx = (FeedbackFrameNumber - 2 + NUM_FEEDBACK_BUFFERS) % NUM_FEEDBACK_BUFFERS;

	// Only read if we have completed at least 2 frames
	if (FeedbackFrameNumber >= 2)
	{
		// Check if counter readback is ready (non-blocking!)
		if (CounterReadbacks[ReadIdx]->IsReady())
		{
			// Read counter first
			uint32 FeedbackCount = 0;
			{
				const uint32* CounterData = (const uint32*)CounterReadbacks[ReadIdx]->Lock(sizeof(uint32));
				if (CounterData)
				{
					FeedbackCount = *CounterData;
				}
				CounterReadbacks[ReadIdx]->Unlock();
			}

			// Clamp to max
			FeedbackCount = FMath::Min(FeedbackCount, (uint32)MAX_COLLISION_FEEDBACK);

			// Read feedback data if any and if ready
			if (FeedbackCount > 0 && FeedbackReadbacks[ReadIdx]->IsReady())
			{
				FScopeLock Lock(&FeedbackLock);

				const uint32 CopySize = FeedbackCount * sizeof(FGPUCollisionFeedback);
				const FGPUCollisionFeedback* FeedbackData = (const FGPUCollisionFeedback*)FeedbackReadbacks[ReadIdx]->Lock(CopySize);

				if (FeedbackData)
				{
					ReadyFeedback.SetNum(FeedbackCount);
					FMemory::Memcpy(ReadyFeedback.GetData(), FeedbackData, CopySize);
					ReadyFeedbackCount = FeedbackCount;
				}

				FeedbackReadbacks[ReadIdx]->Unlock();

				UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Collision Feedback: Read %d entries from readback %d"), FeedbackCount, ReadIdx);
			}
			else if (FeedbackCount == 0)
			{
				FScopeLock Lock(&FeedbackLock);
				ReadyFeedbackCount = 0;
			}
		}
		// If not ready yet, skip this frame (data will be available next frame)
	}

	// Note: Frame counter is incremented in SimulateSubstep AFTER EnqueueCopy, not here
}

void FGPUFluidSimulator::ProcessColliderContactCountReadback(FRHICommandListImmediate& RHICmdList)
{
	// FRHIGPUBufferReadback: Check IsReady() before Lock() to avoid flush

	// Ensure readback objects are valid
	if (ContactCountReadbacks[0] == nullptr)
	{
		return;  // Will be allocated in SimulateSubstep
	}

	// Read from readback that was enqueued 2 frames ago (allowing GPU latency)
	const int32 ReadIdx = (ContactCountFrameNumber - 2 + NUM_FEEDBACK_BUFFERS) % NUM_FEEDBACK_BUFFERS;

	// Only read if we have completed at least 2 frames
	if (ContactCountFrameNumber >= 2)
	{
		// Check if readback is ready (non-blocking!)
		if (ContactCountReadbacks[ReadIdx]->IsReady())
		{
			// Read contact counts - GPU has already completed the copy
			const uint32* CountData = (const uint32*)ContactCountReadbacks[ReadIdx]->Lock(MAX_COLLIDER_COUNT * sizeof(uint32));

			if (CountData)
			{
				FScopeLock Lock(&FeedbackLock);

				// Copy to ready array
				ReadyColliderContactCounts.SetNumUninitialized(MAX_COLLIDER_COUNT);
				for (int32 i = 0; i < MAX_COLLIDER_COUNT; ++i)
				{
					ReadyColliderContactCounts[i] = static_cast<int32>(CountData[i]);
				}
			}

			ContactCountReadbacks[ReadIdx]->Unlock();
		}
		// If not ready yet, skip this frame (data will be available next frame)
	}

	// Note: Frame counter is incremented in SimulateSubstep AFTER EnqueueCopy, not here
}

bool FGPUFluidSimulator::GetCollisionFeedbackForCollider(int32 ColliderIndex, TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	FScopeLock Lock(&FeedbackLock);

	OutFeedback.Reset();
	OutCount = 0;

	if (!bCollisionFeedbackEnabled || ReadyFeedbackCount == 0)
	{
		return false;
	}

	// Filter feedback for this collider
	for (int32 i = 0; i < ReadyFeedbackCount; ++i)
	{
		if (ReadyFeedback[i].ColliderIndex == ColliderIndex)
		{
			OutFeedback.Add(ReadyFeedback[i]);
		}
	}

	OutCount = OutFeedback.Num();
	return OutCount > 0;
}

bool FGPUFluidSimulator::GetAllCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	FScopeLock Lock(&FeedbackLock);

	OutCount = ReadyFeedbackCount;

	if (!bCollisionFeedbackEnabled || ReadyFeedbackCount == 0)
	{
		OutFeedback.Reset();
		return false;
	}

	OutFeedback.SetNum(ReadyFeedbackCount);
	FMemory::Memcpy(OutFeedback.GetData(), ReadyFeedback.GetData(), ReadyFeedbackCount * sizeof(FGPUCollisionFeedback));

	return true;
}

//=============================================================================
// Collider Contact Count API Implementation
//=============================================================================

int32 FGPUFluidSimulator::GetColliderContactCount(int32 ColliderIndex) const
{
	if (ColliderIndex < 0 || ColliderIndex >= ReadyColliderContactCounts.Num())
	{
		return 0;
	}
	return ReadyColliderContactCounts[ColliderIndex];
}

void FGPUFluidSimulator::GetAllColliderContactCounts(TArray<int32>& OutCounts) const
{
	OutCounts = ReadyColliderContactCounts;
}

int32 FGPUFluidSimulator::GetTotalColliderCount() const
{
	return CachedSpheres.Num() + CachedCapsules.Num() + CachedBoxes.Num() + CachedConvexHeaders.Num();
}

int32 FGPUFluidSimulator::GetContactCountForOwner(int32 OwnerID) const
{
	int32 TotalCount = 0;
	int32 ColliderIndex = 0;

	// Spheres: indices 0 to SphereCount-1
	for (int32 i = 0; i < CachedSpheres.Num(); ++i)
	{
		if (CachedSpheres[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
		}
		ColliderIndex++;
	}

	// Capsules: indices SphereCount to SphereCount+CapsuleCount-1
	for (int32 i = 0; i < CachedCapsules.Num(); ++i)
	{
		if (CachedCapsules[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
		}
		ColliderIndex++;
	}

	// Boxes: indices SphereCount+CapsuleCount to SphereCount+CapsuleCount+BoxCount-1
	for (int32 i = 0; i < CachedBoxes.Num(); ++i)
	{
		if (CachedBoxes[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
		}
		ColliderIndex++;
	}

	// Convexes: indices SphereCount+CapsuleCount+BoxCount to Total-1
	for (int32 i = 0; i < CachedConvexHeaders.Num(); ++i)
	{
		if (CachedConvexHeaders[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
		}
		ColliderIndex++;
	}

	return TotalCount;
}
