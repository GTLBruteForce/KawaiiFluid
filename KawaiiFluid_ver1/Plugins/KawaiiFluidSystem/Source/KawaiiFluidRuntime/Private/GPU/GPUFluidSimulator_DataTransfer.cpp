// Copyright KawaiiFluid Team. All Rights Reserved.
// GPUFluidSimulator - Data Transfer Functions (CPU <-> GPU)

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "Core/FluidParticle.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);

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
// Stream Compaction Buffer Management
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
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_MarkedFlags"), MaxParticleCount * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		MarkedFlagsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		MarkedFlagsSRV = RHICmdList.CreateShaderResourceView(MarkedFlagsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(MarkedFlagsBufferRHI));
		MarkedFlagsUAV = RHICmdList.CreateUnorderedAccessView(MarkedFlagsBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(MarkedFlagsBufferRHI));
	}

	// Marked AABB index buffer (int per particle)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_MarkedAABBIndex"), MaxParticleCount * sizeof(int32), sizeof(int32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		MarkedAABBIndexBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		MarkedAABBIndexSRV = RHICmdList.CreateShaderResourceView(MarkedAABBIndexBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(MarkedAABBIndexBufferRHI));
		MarkedAABBIndexUAV = RHICmdList.CreateUnorderedAccessView(MarkedAABBIndexBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(MarkedAABBIndexBufferRHI));
	}

	// Prefix sums buffer
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_PrefixSums"), MaxParticleCount * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		PrefixSumsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		PrefixSumsSRV = RHICmdList.CreateShaderResourceView(PrefixSumsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(PrefixSumsBufferRHI));
		PrefixSumsUAV = RHICmdList.CreateUnorderedAccessView(PrefixSumsBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(PrefixSumsBufferRHI));
	}

	// Block sums buffer
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_BlockSums"), NumBlocks * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		BlockSumsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		BlockSumsSRV = RHICmdList.CreateShaderResourceView(BlockSumsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(BlockSumsBufferRHI));
		BlockSumsUAV = RHICmdList.CreateUnorderedAccessView(BlockSumsBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(BlockSumsBufferRHI));
	}

	// Compacted candidates buffer (worst case: all particles)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_CompactedCandidates"), MaxParticleCount * sizeof(FGPUCandidateParticle), sizeof(FGPUCandidateParticle))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		CompactedCandidatesBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		CompactedCandidatesUAV = RHICmdList.CreateUnorderedAccessView(CompactedCandidatesBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(CompactedCandidatesBufferRHI));
	}

	// Total count buffer (single uint)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_TotalCount"), sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		TotalCountBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		TotalCountUAV = RHICmdList.CreateUnorderedAccessView(TotalCountBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(TotalCountBufferRHI));
	}

	// Staging buffers for readback
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_TotalCountStaging"), sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_None)
			.SetInitialState(ERHIAccess::CopyDest);
		TotalCountStagingBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
	}
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_CandidatesStaging"), MaxParticleCount * sizeof(FGPUCandidateParticle), sizeof(FGPUCandidateParticle))
			.AddUsage(BUF_None)
			.SetInitialState(ERHIAccess::CopyDest);
		CandidatesStagingBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
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

//=============================================================================
// AABB Filtering (Stream Compaction)
//=============================================================================

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

				const FRHIBufferCreateDesc BufferDesc =
					FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_FilterAABBs"), NumAABBs * sizeof(FGPUFilterAABB), sizeof(FGPUFilterAABB))
					.AddUsage(BUF_ShaderResource)
					.SetInitialState(ERHIAccess::SRVMask);
				Self->FilterAABBsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
				Self->FilterAABBsSRV = RHICmdList.CreateShaderResourceView(Self->FilterAABBsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(Self->FilterAABBsBufferRHI));
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
					ParticleSRVToUse = RHICmdList.CreateShaderResourceView(PersistentRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(PersistentRHI));
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
			const FRHIBufferCreateDesc BufferDesc =
				FRHIBufferCreateDesc::CreateStructured(TEXT("PerPolygonCorrections"), CorrectionCount * sizeof(FParticleCorrection), sizeof(FParticleCorrection))
				.AddUsage(BUF_ShaderResource)
				.SetInitialState(ERHIAccess::SRVMask);
			FBufferRHIRef CorrectionsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

			// Upload corrections data
			void* CorrectionData = RHICmdList.LockBuffer(CorrectionsBufferRHI, 0,
				CorrectionCount * sizeof(FParticleCorrection), RLM_WriteOnly);
			FMemory::Memcpy(CorrectionData, CorrectionsCopy.GetData(), CorrectionCount * sizeof(FParticleCorrection));
			RHICmdList.UnlockBuffer(CorrectionsBufferRHI);

			// Create SRV for corrections
			FShaderResourceViewRHIRef CorrectionsSRV = RHICmdList.CreateShaderResourceView(CorrectionsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(CorrectionsBufferRHI));

			// Create UAV for particles from PersistentParticleBuffer
			FBufferRHIRef ParticleRHI = Self->PersistentParticleBuffer->GetRHI();
			if (!ParticleRHI.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyCorrections: Failed to get ParticleRHI from PersistentParticleBuffer"));
				return;
			}
			FUnorderedAccessViewRHIRef ParticlesUAV = RHICmdList.CreateUnorderedAccessView(ParticleRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(ParticleRHI));

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
			const FRHIBufferCreateDesc BufferDesc =
				FRHIBufferCreateDesc::CreateStructured(TEXT("AttachmentUpdates"), UpdateCount * sizeof(FAttachedParticleUpdate), sizeof(FAttachedParticleUpdate))
				.AddUsage(BUF_ShaderResource)
				.SetInitialState(ERHIAccess::SRVMask);
			FBufferRHIRef UpdatesBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

			// Upload updates data
			void* UpdateData = RHICmdList.LockBuffer(UpdatesBufferRHI, 0,
				UpdateCount * sizeof(FAttachedParticleUpdate), RLM_WriteOnly);
			FMemory::Memcpy(UpdateData, UpdatesCopy.GetData(), UpdateCount * sizeof(FAttachedParticleUpdate));
			RHICmdList.UnlockBuffer(UpdatesBufferRHI);

			// Create SRV for updates
			FShaderResourceViewRHIRef UpdatesSRV = RHICmdList.CreateShaderResourceView(UpdatesBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(UpdatesBufferRHI));

			// Create UAV for particles from PersistentParticleBuffer
			FBufferRHIRef ParticleRHI = Self->PersistentParticleBuffer->GetRHI();
			if (!ParticleRHI.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyAttachmentUpdates: Failed to get ParticleRHI from PersistentParticleBuffer"));
				return;
			}
			FUnorderedAccessViewRHIRef ParticlesUAV = RHICmdList.CreateUnorderedAccessView(ParticleRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(ParticleRHI));

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
