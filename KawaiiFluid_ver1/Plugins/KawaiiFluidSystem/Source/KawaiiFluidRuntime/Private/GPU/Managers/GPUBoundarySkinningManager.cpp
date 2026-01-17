// Copyright KawaiiFluid Team. All Rights Reserved.
// FGPUBoundarySkinningManager - GPU Boundary Skinning and Adhesion System

#include "GPU/Managers/GPUBoundarySkinningManager.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUBoundarySkinning, Log, All);
DEFINE_LOG_CATEGORY(LogGPUBoundarySkinning);

// Boundary spatial hash constants for Flex-style adhesion
static constexpr int32 BOUNDARY_HASH_SIZE = 65536;  // 2^16 cells
static constexpr int32 BOUNDARY_MAX_PARTICLES_PER_CELL = 16;

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUBoundarySkinningManager::FGPUBoundarySkinningManager()
	: bIsInitialized(false)
	, TotalLocalBoundaryParticleCount(0)
	, WorldBoundaryBufferCapacity(0)
	, bBoundarySkinningDataDirty(false)
	, StaticBoundaryParticleCount(0)
	, StaticBoundaryBufferCapacity(0)
	, bStaticBoundaryEnabled(false)
	, bStaticBoundaryDirty(true)
	, bStaticZOrderValid(false)
{
}

FGPUBoundarySkinningManager::~FGPUBoundarySkinningManager()
{
	Release();
}

//=============================================================================
// Lifecycle
//=============================================================================

void FGPUBoundarySkinningManager::Initialize()
{
	bIsInitialized = true;
	UE_LOG(LogGPUBoundarySkinning, Log, TEXT("GPUBoundarySkinningManager initialized"));
}

void FGPUBoundarySkinningManager::Release()
{
	FScopeLock Lock(&BoundarySkinningLock);

	// Release Skinned boundary data
	BoundarySkinningDataMap.Empty();
	PersistentLocalBoundaryBuffers.Empty();
	PersistentWorldBoundaryBuffer.SafeRelease();
	PreviousWorldBoundaryBuffer.SafeRelease();
	WorldBoundaryBufferCapacity = 0;
	TotalLocalBoundaryParticleCount = 0;
	bHasPreviousFrame = false;
	bBoundarySkinningDataDirty = false;

	// Release Static boundary data
	PersistentStaticBoundaryBuffer.SafeRelease();
	PersistentStaticZOrderSorted.SafeRelease();
	PersistentStaticCellStart.SafeRelease();
	PersistentStaticCellEnd.SafeRelease();
	PendingStaticBoundaryParticles.Empty();
	StaticBoundaryParticleCount = 0;
	StaticBoundaryBufferCapacity = 0;
	bStaticBoundaryEnabled = false;
	bStaticBoundaryDirty = true;
	bStaticZOrderValid = false;

	// Clear AABB data
	BoundaryOwnerAABBs.Empty();
	CombinedBoundaryAABB = FGPUBoundaryOwnerAABB();
	bBoundaryAABBDirty = true;

	bIsInitialized = false;

	UE_LOG(LogGPUBoundarySkinning, Log, TEXT("GPUBoundarySkinningManager released"));
}

//=============================================================================
// Static Boundary Particles (StaticMesh colliders - Persistent GPU)
//=============================================================================

void FGPUBoundarySkinningManager::UploadStaticBoundaryParticles(const TArray<FGPUBoundaryParticle>& Particles)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&BoundarySkinningLock);

	if (Particles.Num() == 0)
	{
		ClearStaticBoundaryParticles();
		return;
	}

	// Store particles for GPU upload (will be uploaded in next RDG pass)
	PendingStaticBoundaryParticles = Particles;
	StaticBoundaryParticleCount = Particles.Num();
	bStaticBoundaryDirty = true;
	bStaticZOrderValid = false;
	bStaticBoundaryEnabled = true;

	UE_LOG(LogGPUBoundarySkinning, Log, TEXT("Static boundary particles queued for upload: Count=%d"), StaticBoundaryParticleCount);
}

void FGPUBoundarySkinningManager::ClearStaticBoundaryParticles()
{
	FScopeLock Lock(&BoundarySkinningLock);

	PendingStaticBoundaryParticles.Empty();
	PersistentStaticBoundaryBuffer.SafeRelease();
	PersistentStaticZOrderSorted.SafeRelease();
	PersistentStaticCellStart.SafeRelease();
	PersistentStaticCellEnd.SafeRelease();
	StaticBoundaryParticleCount = 0;
	StaticBoundaryBufferCapacity = 0;
	bStaticBoundaryDirty = true;
	bStaticZOrderValid = false;

	UE_LOG(LogGPUBoundarySkinning, Log, TEXT("Static boundary particles cleared"));
}

void FGPUBoundarySkinningManager::ExecuteStaticBoundaryZOrderSort(FRDGBuilder& GraphBuilder, const FGPUFluidSimulationParams& Params)
{
	FScopeLock Lock(&BoundarySkinningLock);

	if (!bStaticBoundaryEnabled || StaticBoundaryParticleCount <= 0)
	{
		return;
	}

	// Check if we need to upload pending particles to GPU
	if (bStaticBoundaryDirty && PendingStaticBoundaryParticles.Num() > 0)
	{
		// Upload to GPU buffer
		const int32 ParticleCount = PendingStaticBoundaryParticles.Num();

		// Reallocate if needed
		if (StaticBoundaryBufferCapacity < ParticleCount)
		{
			PersistentStaticBoundaryBuffer.SafeRelease();
			PersistentStaticZOrderSorted.SafeRelease();
			PersistentStaticCellStart.SafeRelease();
			PersistentStaticCellEnd.SafeRelease();
			StaticBoundaryBufferCapacity = ParticleCount;
		}

		// Create or reuse static boundary buffer
		FRDGBufferRef StaticBoundaryBuffer;
		if (PersistentStaticBoundaryBuffer.IsValid())
		{
			StaticBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(PersistentStaticBoundaryBuffer, TEXT("GPUFluid.StaticBoundaryParticles"));
		}
		else
		{
			StaticBoundaryBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), ParticleCount),
				TEXT("GPUFluid.StaticBoundaryParticles"));
		}

		// Upload data
		GraphBuilder.QueueBufferUpload(
			StaticBoundaryBuffer,
			PendingStaticBoundaryParticles.GetData(),
			ParticleCount * sizeof(FGPUBoundaryParticle));

		// Extract to persistent buffer
		GraphBuilder.QueueBufferExtraction(StaticBoundaryBuffer, &PersistentStaticBoundaryBuffer);

		// Clear pending data after upload
		PendingStaticBoundaryParticles.Empty();

		UE_LOG(LogGPUBoundarySkinning, Log, TEXT("Static boundary particles uploaded to GPU: Count=%d"), ParticleCount);
	}

	// Perform Z-Order sorting if dirty
	if (bStaticBoundaryDirty || !bStaticZOrderValid)
	{
		if (!PersistentStaticBoundaryBuffer.IsValid())
		{
			return;
		}

		RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid::StaticBoundaryZOrderSort");

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		const int32 CellCount = GridResolutionPresetHelper::GetMaxCells(GridResolutionPreset);
		const int32 ParticleCount = StaticBoundaryParticleCount;

		// Register source buffer
		FRDGBufferRef SourceBuffer = GraphBuilder.RegisterExternalBuffer(
			PersistentStaticBoundaryBuffer, TEXT("GPUFluid.StaticBoundarySource"));

		// Create transient buffers for sorting
		FRDGBufferRef MortonCodesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount),
			TEXT("GPUFluid.StaticBoundaryMortonCodes"));
		FRDGBufferRef MortonCodesTempBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount),
			TEXT("GPUFluid.StaticBoundaryMortonCodesTemp"));
		FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount),
			TEXT("GPUFluid.StaticBoundarySortIndices"));
		FRDGBufferRef IndicesTempBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount),
			TEXT("GPUFluid.StaticBoundarySortIndicesTemp"));

		// Create or reuse persistent Z-Order buffers
		FRDGBufferRef SortedBuffer;
		FRDGBufferRef CellStartBuffer;
		FRDGBufferRef CellEndBuffer;

		if (PersistentStaticZOrderSorted.IsValid())
		{
			SortedBuffer = GraphBuilder.RegisterExternalBuffer(PersistentStaticZOrderSorted, TEXT("GPUFluid.StaticSortedBoundary"));
		}
		else
		{
			SortedBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), ParticleCount),
				TEXT("GPUFluid.StaticSortedBoundary"));
		}

		if (PersistentStaticCellStart.IsValid())
		{
			CellStartBuffer = GraphBuilder.RegisterExternalBuffer(PersistentStaticCellStart, TEXT("GPUFluid.StaticCellStart"));
		}
		else
		{
			CellStartBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CellCount),
				TEXT("GPUFluid.StaticCellStart"));
		}

		if (PersistentStaticCellEnd.IsValid())
		{
			CellEndBuffer = GraphBuilder.RegisterExternalBuffer(PersistentStaticCellEnd, TEXT("GPUFluid.StaticCellEnd"));
		}
		else
		{
			CellEndBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CellCount),
				TEXT("GPUFluid.StaticCellEnd"));
		}

		// Pass 1: Compute Morton codes
		{
			FComputeBoundaryMortonCodesCS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));
			TShaderMapRef<FComputeBoundaryMortonCodesCS> ComputeShader(ShaderMap, PermutationVector);

			FComputeBoundaryMortonCodesCS::FParameters* PassParams =
				GraphBuilder.AllocParameters<FComputeBoundaryMortonCodesCS::FParameters>();
			PassParams->BoundaryParticlesIn = GraphBuilder.CreateSRV(SourceBuffer);
			PassParams->BoundaryMortonCodes = GraphBuilder.CreateUAV(MortonCodesBuffer);
			PassParams->BoundaryParticleIndices = GraphBuilder.CreateUAV(IndicesBuffer);
			PassParams->BoundaryParticleCount = ParticleCount;
			PassParams->BoundsMin = ZOrderBoundsMin;
			PassParams->CellSize = Params.CellSize;

			const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FComputeBoundaryMortonCodesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::StaticComputeMortonCodes(%d)", ParticleCount),
				ComputeShader,
				PassParams,
				FIntVector(NumGroups, 1, 1));
		}

		// Pass 2: Radix sort (using multi-pass histogram-based radix sort)
		{
			const int32 GridAxisBits = GridResolutionPresetHelper::GetAxisBits(GridResolutionPreset);
			const int32 MortonCodeBits = GridAxisBits * 3;
			int32 RadixSortPasses = (MortonCodeBits + GPU_RADIX_BITS - 1) / GPU_RADIX_BITS;
			if (RadixSortPasses % 2 != 0) RadixSortPasses++;

			const int32 NumBlocks = FMath::DivideAndRoundUp(ParticleCount, GPU_RADIX_ELEMENTS_PER_GROUP);
			const int32 RequiredHistogramSize = GPU_RADIX_SIZE * NumBlocks;

			FRDGBufferRef Histogram = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RequiredHistogramSize),
				TEXT("StaticBoundaryRadixSort.Histogram")
			);
			FRDGBufferRef BucketOffsets = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE),
				TEXT("StaticBoundaryRadixSort.BucketOffsets")
			);

			FRDGBufferRef Keys[2] = { MortonCodesBuffer, MortonCodesTempBuffer };
			FRDGBufferRef Values[2] = { IndicesBuffer, IndicesTempBuffer };
			int32 BufferIndex = 0;

			for (int32 Pass = 0; Pass < RadixSortPasses; ++Pass)
			{
				const int32 BitOffset = Pass * GPU_RADIX_BITS;
				const int32 SrcIndex = BufferIndex;
				const int32 DstIndex = BufferIndex ^ 1;

				// Histogram
				{
					TShaderMapRef<FRadixSortHistogramCS> HistogramShader(ShaderMap);
					FRadixSortHistogramCS::FParameters* HistogramParams = GraphBuilder.AllocParameters<FRadixSortHistogramCS::FParameters>();
					HistogramParams->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
					HistogramParams->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
					HistogramParams->Histogram = GraphBuilder.CreateUAV(Histogram);
					HistogramParams->ElementCount = ParticleCount;
					HistogramParams->BitOffset = BitOffset;
					HistogramParams->NumGroups = NumBlocks;

					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("StaticBoundaryRadix::Histogram"), HistogramShader, HistogramParams, FIntVector(NumBlocks, 1, 1));
				}

				// Global Prefix Sum
				{
					TShaderMapRef<FRadixSortGlobalPrefixSumCS> PrefixSumShader(ShaderMap);
					FRadixSortGlobalPrefixSumCS::FParameters* GlobalPrefixParams = GraphBuilder.AllocParameters<FRadixSortGlobalPrefixSumCS::FParameters>();
					GlobalPrefixParams->Histogram = GraphBuilder.CreateUAV(Histogram);
					GlobalPrefixParams->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);
					GlobalPrefixParams->NumGroups = NumBlocks;

					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("StaticBoundaryRadix::GlobalPrefixSum"), PrefixSumShader, GlobalPrefixParams, FIntVector(1, 1, 1));
				}

				// Bucket Prefix Sum
				{
					TShaderMapRef<FRadixSortBucketPrefixSumCS> BucketSumShader(ShaderMap);
					FRadixSortBucketPrefixSumCS::FParameters* BucketPrefixParams = GraphBuilder.AllocParameters<FRadixSortBucketPrefixSumCS::FParameters>();
					BucketPrefixParams->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);

					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("StaticBoundaryRadix::BucketPrefixSum"), BucketSumShader, BucketPrefixParams, FIntVector(1, 1, 1));
				}

				// Scatter
				{
					TShaderMapRef<FRadixSortScatterCS> ScatterShader(ShaderMap);
					FRadixSortScatterCS::FParameters* ScatterParams = GraphBuilder.AllocParameters<FRadixSortScatterCS::FParameters>();
					ScatterParams->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
					ScatterParams->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
					ScatterParams->KeysOut = GraphBuilder.CreateUAV(Keys[DstIndex]);
					ScatterParams->ValuesOut = GraphBuilder.CreateUAV(Values[DstIndex]);
					ScatterParams->HistogramSRV = GraphBuilder.CreateSRV(Histogram);
					ScatterParams->GlobalOffsetsSRV = GraphBuilder.CreateSRV(BucketOffsets);
					ScatterParams->ElementCount = ParticleCount;
					ScatterParams->BitOffset = BitOffset;

					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("StaticBoundaryRadix::Scatter"), ScatterShader, ScatterParams, FIntVector(NumBlocks, 1, 1));
				}

				BufferIndex ^= 1;
			}

			MortonCodesBuffer = Keys[BufferIndex];
			IndicesBuffer = Values[BufferIndex];
		}

		// Final sorted indices buffer is now in IndicesBuffer
		FRDGBufferRef FinalIndicesBuffer = IndicesBuffer;
		FRDGBufferRef FinalMortonBuffer = MortonCodesBuffer;

		// Pass 3: Clear cell indices
		{
			FComputeBoundaryCellStartEndCS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));
			TShaderMapRef<FClearBoundaryCellIndicesCS> ClearShader(ShaderMap, PermutationVector);

			FClearBoundaryCellIndicesCS::FParameters* ClearParams =
				GraphBuilder.AllocParameters<FClearBoundaryCellIndicesCS::FParameters>();
			ClearParams->BoundaryCellStart = GraphBuilder.CreateUAV(CellStartBuffer);
			ClearParams->BoundaryCellEnd = GraphBuilder.CreateUAV(CellEndBuffer);

			const int32 NumGroups = FMath::DivideAndRoundUp(CellCount, FClearBoundaryCellIndicesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::StaticClearCellIndices"),
				ClearShader,
				ClearParams,
				FIntVector(NumGroups, 1, 1));
		}

		// Pass 4: Reorder particles
		{
			TShaderMapRef<FReorderBoundaryParticlesCS> ReorderShader(ShaderMap);

			FReorderBoundaryParticlesCS::FParameters* ReorderParams =
				GraphBuilder.AllocParameters<FReorderBoundaryParticlesCS::FParameters>();
			ReorderParams->OldBoundaryParticles = GraphBuilder.CreateSRV(SourceBuffer);
			ReorderParams->SortedBoundaryIndices = GraphBuilder.CreateSRV(FinalIndicesBuffer);
			ReorderParams->SortedBoundaryParticles = GraphBuilder.CreateUAV(SortedBuffer);
			ReorderParams->BoundaryParticleCount = ParticleCount;

			const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FReorderBoundaryParticlesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::StaticReorderParticles(%d)", ParticleCount),
				ReorderShader,
				ReorderParams,
				FIntVector(NumGroups, 1, 1));
		}

		// Pass 5: Compute cell start/end
		{
			// FinalMortonBuffer is already defined from Pass 2

			FComputeBoundaryCellStartEndCS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));
			TShaderMapRef<FComputeBoundaryCellStartEndCS> CellShader(ShaderMap, PermutationVector);

			FComputeBoundaryCellStartEndCS::FParameters* CellParams =
				GraphBuilder.AllocParameters<FComputeBoundaryCellStartEndCS::FParameters>();
			CellParams->SortedBoundaryMortonCodes = GraphBuilder.CreateSRV(FinalMortonBuffer);
			CellParams->BoundaryCellStart = GraphBuilder.CreateUAV(CellStartBuffer);
			CellParams->BoundaryCellEnd = GraphBuilder.CreateUAV(CellEndBuffer);
			CellParams->BoundaryParticleCount = ParticleCount;

			const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FComputeBoundaryCellStartEndCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::StaticComputeCellStartEnd(%d)", ParticleCount),
				CellShader,
				CellParams,
				FIntVector(NumGroups, 1, 1));
		}

		// Extract persistent buffers
		GraphBuilder.QueueBufferExtraction(SortedBuffer, &PersistentStaticZOrderSorted);
		GraphBuilder.QueueBufferExtraction(CellStartBuffer, &PersistentStaticCellStart);
		GraphBuilder.QueueBufferExtraction(CellEndBuffer, &PersistentStaticCellEnd);

		bStaticBoundaryDirty = false;
		bStaticZOrderValid = true;

		UE_LOG(LogGPUBoundarySkinning, Log, TEXT("Static boundary Z-Order sort completed: %d particles"), ParticleCount);
	}
}

//=============================================================================
// GPU Boundary Skinning (SkeletalMesh)
//=============================================================================

void FGPUBoundarySkinningManager::UploadLocalBoundaryParticles(int32 OwnerID, const TArray<FGPUBoundaryParticleLocal>& LocalParticles)
{
	if (!bIsInitialized || LocalParticles.Num() == 0)
	{
		return;
	}

	FScopeLock Lock(&BoundarySkinningLock);

	FGPUBoundarySkinningData& SkinningData = BoundarySkinningDataMap.FindOrAdd(OwnerID);
	SkinningData.OwnerID = OwnerID;
	SkinningData.LocalParticles = LocalParticles;
	SkinningData.bLocalParticlesUploaded = false;
	bBoundarySkinningDataDirty = true;

	// Recalculate total count
	TotalLocalBoundaryParticleCount = 0;
	for (const auto& Pair : BoundarySkinningDataMap)
	{
		TotalLocalBoundaryParticleCount += Pair.Value.LocalParticles.Num();
	}

	UE_LOG(LogGPUBoundarySkinning, Log, TEXT("UploadLocalBoundaryParticles: OwnerID=%d, Count=%d, TotalCount=%d"),
		OwnerID, LocalParticles.Num(), TotalLocalBoundaryParticleCount);
}

void FGPUBoundarySkinningManager::UploadBoneTransformsForBoundary(int32 OwnerID, const TArray<FMatrix44f>& BoneTransforms, const FMatrix44f& ComponentTransform)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&BoundarySkinningLock);

	FGPUBoundarySkinningData* SkinningData = BoundarySkinningDataMap.Find(OwnerID);
	if (SkinningData)
	{
		SkinningData->BoneTransforms = BoneTransforms;
		SkinningData->ComponentTransform = ComponentTransform;
	}
}

void FGPUBoundarySkinningManager::RemoveBoundarySkinningData(int32 OwnerID)
{
	FScopeLock Lock(&BoundarySkinningLock);

	if (BoundarySkinningDataMap.Remove(OwnerID) > 0)
	{
		PersistentLocalBoundaryBuffers.Remove(OwnerID);

		TotalLocalBoundaryParticleCount = 0;
		for (const auto& Pair : BoundarySkinningDataMap)
		{
			TotalLocalBoundaryParticleCount += Pair.Value.LocalParticles.Num();
		}

		// Remove AABB for this owner and recalculate combined AABB
		BoundaryOwnerAABBs.Remove(OwnerID);
		bBoundaryAABBDirty = true;
		RecalculateCombinedAABB();

		bBoundarySkinningDataDirty = true;

		UE_LOG(LogGPUBoundarySkinning, Log, TEXT("RemoveBoundarySkinningData: OwnerID=%d, TotalCount=%d"),
			OwnerID, TotalLocalBoundaryParticleCount);
	}
}

void FGPUBoundarySkinningManager::ClearAllBoundarySkinningData()
{
	FScopeLock Lock(&BoundarySkinningLock);

	BoundarySkinningDataMap.Empty();
	PersistentLocalBoundaryBuffers.Empty();
	PersistentWorldBoundaryBuffer.SafeRelease();
	PreviousWorldBoundaryBuffer.SafeRelease();
	WorldBoundaryBufferCapacity = 0;
	TotalLocalBoundaryParticleCount = 0;
	bHasPreviousFrame = false;
	bBoundarySkinningDataDirty = true;

	// Clear AABB data
	BoundaryOwnerAABBs.Empty();
	CombinedBoundaryAABB = FGPUBoundaryOwnerAABB();
	bBoundaryAABBDirty = true;

	UE_LOG(LogGPUBoundarySkinning, Log, TEXT("ClearAllBoundarySkinningData"));
}

bool FGPUBoundarySkinningManager::IsBoundaryAdhesionEnabled() const
{
	// Boundary adhesion is enabled when:
	// 1. Adhesion is globally enabled
	// 2. AND (Skinned boundary from GPU skinning OR Static boundary from StaticMesh)
	return CachedBoundaryAdhesionParams.bEnabled != 0 &&
		(TotalLocalBoundaryParticleCount > 0 || (bStaticBoundaryEnabled && StaticBoundaryParticleCount > 0));
}

//=============================================================================
// Boundary Owner AABB (for early-out optimization)
//=============================================================================

void FGPUBoundarySkinningManager::UpdateBoundaryOwnerAABB(int32 OwnerID, const FGPUBoundaryOwnerAABB& AABB)
{
	FScopeLock Lock(&BoundarySkinningLock);

	BoundaryOwnerAABBs.Add(OwnerID, AABB);
	bBoundaryAABBDirty = true;
	RecalculateCombinedAABB();
}

void FGPUBoundarySkinningManager::RecalculateCombinedAABB()
{
	// Reset to invalid state
	CombinedBoundaryAABB = FGPUBoundaryOwnerAABB();

	if (BoundaryOwnerAABBs.Num() == 0)
	{
		return;
	}

	// Combine all owner AABBs
	FVector3f CombinedMin(FLT_MAX);
	FVector3f CombinedMax(-FLT_MAX);

	for (const auto& Pair : BoundaryOwnerAABBs)
	{
		const FGPUBoundaryOwnerAABB& AABB = Pair.Value;
		if (AABB.IsValid())
		{
			CombinedMin.X = FMath::Min(CombinedMin.X, AABB.Min.X);
			CombinedMin.Y = FMath::Min(CombinedMin.Y, AABB.Min.Y);
			CombinedMin.Z = FMath::Min(CombinedMin.Z, AABB.Min.Z);
			CombinedMax.X = FMath::Max(CombinedMax.X, AABB.Max.X);
			CombinedMax.Y = FMath::Max(CombinedMax.Y, AABB.Max.Y);
			CombinedMax.Z = FMath::Max(CombinedMax.Z, AABB.Max.Z);
		}
	}

	CombinedBoundaryAABB = FGPUBoundaryOwnerAABB(CombinedMin, CombinedMax);
	bBoundaryAABBDirty = false;
}

bool FGPUBoundarySkinningManager::DoesBoundaryOverlapVolume(const FVector3f& VolumeMin, const FVector3f& VolumeMax, float AdhesionRadius) const
{
	if (!CombinedBoundaryAABB.IsValid())
	{
		return false;
	}

	// Expand boundary AABB by adhesion radius for conservative test
	FGPUBoundaryOwnerAABB ExpandedBoundaryAABB = CombinedBoundaryAABB.ExpandBy(AdhesionRadius);
	FGPUBoundaryOwnerAABB VolumeAABB(VolumeMin, VolumeMax);

	return ExpandedBoundaryAABB.Intersects(VolumeAABB);
}

bool FGPUBoundarySkinningManager::ShouldSkipBoundaryAdhesionPass(const FGPUFluidSimulationParams& Params) const
{
	// If adhesion is not enabled, skip
	if (!IsBoundaryAdhesionEnabled())
	{
		return true;
	}

	// If AABB is not valid (no boundary owners), don't skip (fall through to other checks)
	if (!CombinedBoundaryAABB.IsValid())
	{
		return false;
	}

	// Check if boundary AABB overlaps with simulation volume
	// Use ZOrderBoundsMin/Max as volume bounds (set via SetBoundaryZOrderConfig)
	const float AdhesionRadius = CachedBoundaryAdhesionParams.AdhesionRadius;
	const bool bOverlaps = DoesBoundaryOverlapVolume(ZOrderBoundsMin, ZOrderBoundsMax, AdhesionRadius);

	if (!bOverlaps)
	{
		// Log skip reason (every 60 frames to avoid spam)
		static int32 SkipLogCounter = 0;
		if (++SkipLogCounter % 60 == 1)
		{
			UE_LOG(LogGPUBoundarySkinning, Log,
				TEXT("[BoundaryAdhesion] SKIPPED - No overlap between Boundary AABB [(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)] and Volume [(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)] with AdhesionRadius=%.1f"),
				CombinedBoundaryAABB.Min.X, CombinedBoundaryAABB.Min.Y, CombinedBoundaryAABB.Min.Z,
				CombinedBoundaryAABB.Max.X, CombinedBoundaryAABB.Max.Y, CombinedBoundaryAABB.Max.Z,
				ZOrderBoundsMin.X, ZOrderBoundsMin.Y, ZOrderBoundsMin.Z,
				ZOrderBoundsMax.X, ZOrderBoundsMax.Y, ZOrderBoundsMax.Z,
				AdhesionRadius);
		}
		return true;
	}

	return false;
}

//=============================================================================
// Boundary Skinning Pass
//=============================================================================

void FGPUBoundarySkinningManager::AddBoundarySkinningPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef& OutWorldBoundaryBuffer,
	int32& OutBoundaryParticleCount,
	float DeltaTime)
{
	FScopeLock Lock(&BoundarySkinningLock);

	// Debug: Log DeltaTime and estimated velocity (every 60 frames)
	static int32 DebugCounter = 0;
	if (++DebugCounter % 60 == 1)
	{
		// Example: 10cm movement per frame with this DeltaTime = velocity
		float exampleMovement = 10.0f; // cm
		float estimatedVelocity = DeltaTime > 0.0001f ? exampleMovement / DeltaTime : 0.0f;
		UE_LOG(LogGPUBoundarySkinning, Warning, TEXT("[BoundaryVelocityDebug] DeltaTime=%.6f, If 10cm/frame -> Velocity=%.1f cm/s"),
			DeltaTime, estimatedVelocity);
	}

	OutWorldBoundaryBuffer = nullptr;
	OutBoundaryParticleCount = 0;

	if (TotalLocalBoundaryParticleCount <= 0 || BoundarySkinningDataMap.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FBoundarySkinningCS> SkinningShader(ShaderMap);

	// Ensure world boundary buffer is large enough
	if (WorldBoundaryBufferCapacity < TotalLocalBoundaryParticleCount)
	{
		PersistentWorldBoundaryBuffer.SafeRelease();
		PreviousWorldBoundaryBuffer.SafeRelease();
		WorldBoundaryBufferCapacity = TotalLocalBoundaryParticleCount;
		bHasPreviousFrame = false;
	}

	// Create or reuse world boundary buffer
	FRDGBufferRef WorldBoundaryBuffer;
	if (PersistentWorldBoundaryBuffer.IsValid())
	{
		WorldBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidWorldBoundaryParticles"));
	}
	else
	{
		WorldBoundaryBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), WorldBoundaryBufferCapacity),
			TEXT("GPUFluidWorldBoundaryParticles")
		);
	}
	FRDGBufferUAVRef WorldBoundaryUAV = GraphBuilder.CreateUAV(WorldBoundaryBuffer);

	// Create or reuse previous frame buffer for velocity calculation
	FRDGBufferRef PreviousBoundaryBuffer;
	if (bHasPreviousFrame && PreviousWorldBoundaryBuffer.IsValid())
	{
		PreviousBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(PreviousWorldBoundaryBuffer, TEXT("GPUFluidPreviousBoundaryParticles"));
	}
	else
	{
		// Create dummy buffer for first frame (velocity will be 0)
		// Must use CreateStructuredBuffer with initial data so RDG marks it as "produced"
		// Otherwise RDG validation fails: "has a read dependency but was never written to"
		const int32 DummyCount = FMath::Max(1, WorldBoundaryBufferCapacity);
		TArray<FGPUBoundaryParticle> DummyData;
		DummyData.SetNumZeroed(DummyCount);
		PreviousBoundaryBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidPreviousBoundaryParticles_Dummy"),
			sizeof(FGPUBoundaryParticle),
			DummyCount,
			DummyData.GetData(),
			DummyCount * sizeof(FGPUBoundaryParticle),
			ERDGInitialDataFlags::NoCopy
		);
	}
	FRDGBufferSRVRef PreviousBoundarySRV = GraphBuilder.CreateSRV(PreviousBoundaryBuffer);

	int32 OutputOffset = 0;

	for (auto& Pair : BoundarySkinningDataMap)
	{
		const int32 OwnerID = Pair.Key;
		FGPUBoundarySkinningData& SkinningData = Pair.Value;

		if (SkinningData.LocalParticles.Num() == 0)
		{
			continue;
		}

		const int32 LocalParticleCount = SkinningData.LocalParticles.Num();

		// Upload or reuse local boundary particles buffer
		TRefCountPtr<FRDGPooledBuffer>& LocalBuffer = PersistentLocalBoundaryBuffers.FindOrAdd(OwnerID);
		FRDGBufferRef LocalBoundaryBuffer;

		if (!SkinningData.bLocalParticlesUploaded || !LocalBuffer.IsValid())
		{
			LocalBoundaryBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidLocalBoundaryParticles"),
				sizeof(FGPUBoundaryParticleLocal),
				LocalParticleCount,
				SkinningData.LocalParticles.GetData(),
				LocalParticleCount * sizeof(FGPUBoundaryParticleLocal),
				ERDGInitialDataFlags::NoCopy
			);
			SkinningData.bLocalParticlesUploaded = true;
			GraphBuilder.QueueBufferExtraction(LocalBoundaryBuffer, &LocalBuffer);
		}
		else
		{
			LocalBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(LocalBuffer, TEXT("GPUFluidLocalBoundaryParticles"));
		}
		FRDGBufferSRVRef LocalBoundarySRV = GraphBuilder.CreateSRV(LocalBoundaryBuffer);

		// Upload bone transforms
		FRDGBufferRef BoneTransformsBuffer;
		const int32 BoneCount = SkinningData.BoneTransforms.Num();
		if (BoneCount > 0)
		{
			BoneTransformsBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidBoneTransforms"),
				sizeof(FMatrix44f),
				BoneCount,
				SkinningData.BoneTransforms.GetData(),
				BoneCount * sizeof(FMatrix44f),
				ERDGInitialDataFlags::NoCopy
			);
		}
		else
		{
			FMatrix44f Identity = FMatrix44f::Identity;
			BoneTransformsBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidBoneTransforms"),
				sizeof(FMatrix44f),
				1,
				&Identity,
				sizeof(FMatrix44f),
				ERDGInitialDataFlags::NoCopy
			);
		}
		FRDGBufferSRVRef SkinningBoneTransformsSRV = GraphBuilder.CreateSRV(BoneTransformsBuffer);

		// Setup skinning shader parameters
		FBoundarySkinningCS::FParameters* PassParams = GraphBuilder.AllocParameters<FBoundarySkinningCS::FParameters>();
		PassParams->LocalBoundaryParticles = LocalBoundarySRV;
		PassParams->WorldBoundaryParticles = WorldBoundaryUAV;
		PassParams->PreviousWorldBoundaryParticles = PreviousBoundarySRV;
		PassParams->BoneTransforms = SkinningBoneTransformsSRV;
		PassParams->BoundaryParticleCount = LocalParticleCount;
		PassParams->BoneCount = FMath::Max(1, BoneCount);
		PassParams->OwnerID = OwnerID;
		PassParams->bHasPreviousFrame = bHasPreviousFrame ? 1 : 0;
		PassParams->ComponentTransform = SkinningData.ComponentTransform;
		PassParams->DeltaTime = DeltaTime;

		const uint32 NumGroups = FMath::DivideAndRoundUp(LocalParticleCount, FBoundarySkinningCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::BoundarySkinning(Owner=%d, Count=%d)", OwnerID, LocalParticleCount),
			SkinningShader,
			PassParams,
			FIntVector(NumGroups, 1, 1)
		);

		OutputOffset += LocalParticleCount;
	}

	// Output for same-frame access by density/adhesion passes
	OutWorldBoundaryBuffer = WorldBoundaryBuffer;
	OutBoundaryParticleCount = TotalLocalBoundaryParticleCount;

	// Store current frame as previous for next frame velocity calculation
	// Swap: Previous <- Current, then extract new Current
	PreviousWorldBoundaryBuffer = PersistentWorldBoundaryBuffer;
	GraphBuilder.QueueBufferExtraction(WorldBoundaryBuffer, &PersistentWorldBoundaryBuffer);
	bHasPreviousFrame = true;
}

//=============================================================================
// Boundary Adhesion Pass
//=============================================================================

void FGPUBoundarySkinningManager::AddBoundaryAdhesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	int32 CurrentParticleCount,
	const FGPUFluidSimulationParams& Params,
	FRDGBufferRef InSameFrameBoundaryBuffer,
	int32 InSameFrameBoundaryCount,
	FRDGBufferSRVRef InZOrderSortedSRV,
	FRDGBufferSRVRef InZOrderCellStartSRV,
	FRDGBufferSRVRef InZOrderCellEndSRV)
{
	// =========================================================================
	// DISPATCH-LEVEL EARLY-OUT: Skip entire pass if boundary AABB doesn't overlap volume
	// This saves GPU dispatch overhead when character is far from fluid
	// =========================================================================
	if (ShouldSkipBoundaryAdhesionPass(Params))
	{
		return;
	}

	// Boundary buffer must be provided by caller (Skinned or Static)
	if (!IsBoundaryAdhesionEnabled() || InSameFrameBoundaryBuffer == nullptr || InSameFrameBoundaryCount <= 0 || CurrentParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const int32 BoundaryParticleCount = InSameFrameBoundaryCount;
	FRDGBufferRef BoundaryParticleBuffer = InSameFrameBoundaryBuffer;
	FRDGBufferSRVRef BoundaryParticlesSRV = GraphBuilder.CreateSRV(BoundaryParticleBuffer);

	// BoundaryCellSize must be >= SmoothingRadius for proper neighbor search
	// Legacy mode searches 3x3x3 cells = BoundaryCellSize * 3 range
	// So BoundaryCellSize should be at least SmoothingRadius / 1.5 to cover the search range
	const float BoundaryCellSize = Params.SmoothingRadius;

	// Create spatial hash buffers
	FRDGBufferRef AdhesionCellCountsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BOUNDARY_HASH_SIZE),
		TEXT("GPUFluidBoundaryCellCounts")
	);
	FRDGBufferRef AdhesionParticleIndicesBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BOUNDARY_HASH_SIZE * BOUNDARY_MAX_PARTICLES_PER_CELL),
		TEXT("GPUFluidBoundaryParticleIndices")
	);

	// Pass 1: Clear spatial hash
	{
		TShaderMapRef<FClearBoundaryHashCS> ClearShader(ShaderMap);
		FClearBoundaryHashCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearBoundaryHashCS::FParameters>();
		ClearParams->RWBoundaryCellCounts = GraphBuilder.CreateUAV(AdhesionCellCountsBuffer);

		const uint32 ClearGroups = FMath::DivideAndRoundUp(BOUNDARY_HASH_SIZE, FClearBoundaryHashCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ClearBoundaryHash"),
			ClearShader,
			ClearParams,
			FIntVector(ClearGroups, 1, 1)
		);
	}

	// Pass 2: Build spatial hash
	{
		TShaderMapRef<FBuildBoundaryHashCS> BuildShader(ShaderMap);
		FBuildBoundaryHashCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FBuildBoundaryHashCS::FParameters>();
		BuildParams->BoundaryParticles = BoundaryParticlesSRV;
		BuildParams->BoundaryParticleCount = BoundaryParticleCount;
		BuildParams->BoundaryCellSize = BoundaryCellSize;
		BuildParams->RWBoundaryCellCounts = GraphBuilder.CreateUAV(AdhesionCellCountsBuffer);
		BuildParams->RWBoundaryParticleIndices = GraphBuilder.CreateUAV(AdhesionParticleIndicesBuffer);

		const uint32 BuildGroups = FMath::DivideAndRoundUp(BoundaryParticleCount, FBuildBoundaryHashCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::BuildBoundaryHash"),
			BuildShader,
			BuildParams,
			FIntVector(BuildGroups, 1, 1)
		);
	}

	// Pass 3: Boundary adhesion
	{
		// Check if Z-Order mode is enabled and valid
		// Priority: 1) Same-frame Z-Order buffers, 2) Persistent Z-Order buffers, 3) Disabled
		const bool bUseSameFrameZOrder = InZOrderSortedSRV != nullptr
			&& InZOrderCellStartSRV != nullptr
			&& InZOrderCellEndSRV != nullptr;
		const bool bUsePersistentZOrder = !bUseSameFrameZOrder
			&& bUseBoundaryZOrder && bBoundaryZOrderValid
			&& PersistentSortedBoundaryBuffer.IsValid()
			&& PersistentBoundaryCellStart.IsValid()
			&& PersistentBoundaryCellEnd.IsValid();
		const bool bCanUseZOrder = bUseSameFrameZOrder || bUsePersistentZOrder;

		// Create permutation vector for grid resolution
		FBoundaryAdhesionCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));
		TShaderMapRef<FBoundaryAdhesionCS> AdhesionShader(ShaderMap, PermutationVector);

		FBoundaryAdhesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundaryAdhesionCS::FParameters>();
		PassParameters->Particles = ParticlesUAV;
		PassParameters->ParticleCount = CurrentParticleCount;
		PassParameters->BoundaryParticles = BoundaryParticlesSRV;
		PassParameters->BoundaryParticleCount = BoundaryParticleCount;
		// Legacy Spatial Hash mode
		PassParameters->BoundaryCellCounts = GraphBuilder.CreateSRV(AdhesionCellCountsBuffer);
		PassParameters->BoundaryParticleIndices = GraphBuilder.CreateSRV(AdhesionParticleIndicesBuffer);
		PassParameters->BoundaryCellSize = BoundaryCellSize;

		// Z-Order mode (if enabled and valid)
		if (bUseSameFrameZOrder)
		{
			// Use same-frame Z-Order buffers (works for static boundary particles too!)
			PassParameters->SortedBoundaryParticles = InZOrderSortedSRV;
			PassParameters->BoundaryCellStart = InZOrderCellStartSRV;
			PassParameters->BoundaryCellEnd = InZOrderCellEndSRV;
			PassParameters->bUseBoundaryZOrder = 1;
			PassParameters->MortonBoundsMin = ZOrderBoundsMin;
			PassParameters->CellSize = Params.CellSize;
		}
		else if (bUsePersistentZOrder)
		{
			// Use persistent Z-Order buffers (fallback from previous frame)
			FRDGBufferRef SortedBuffer = GraphBuilder.RegisterExternalBuffer(
				PersistentSortedBoundaryBuffer, TEXT("GPUFluidSortedBoundaryParticles_Adhesion"));
			FRDGBufferRef CellStartBuffer = GraphBuilder.RegisterExternalBuffer(
				PersistentBoundaryCellStart, TEXT("GPUFluidBoundaryCellStart_Adhesion"));
			FRDGBufferRef CellEndBuffer = GraphBuilder.RegisterExternalBuffer(
				PersistentBoundaryCellEnd, TEXT("GPUFluidBoundaryCellEnd_Adhesion"));

			PassParameters->SortedBoundaryParticles = GraphBuilder.CreateSRV(SortedBuffer);
			PassParameters->BoundaryCellStart = GraphBuilder.CreateSRV(CellStartBuffer);
			PassParameters->BoundaryCellEnd = GraphBuilder.CreateSRV(CellEndBuffer);
			PassParameters->bUseBoundaryZOrder = 1;
			PassParameters->MortonBoundsMin = ZOrderBoundsMin;
			PassParameters->CellSize = Params.CellSize;
		}
		else
		{
			// Create dummy buffers for RDG validation when Z-Order is disabled
			FRDGBufferRef DummySortedBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
				TEXT("GPUFluidSortedBoundaryParticles_Adhesion_Dummy"));
			FGPUBoundaryParticle ZeroBoundary = {};
			GraphBuilder.QueueBufferUpload(DummySortedBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));

			FRDGBufferRef DummyCellStartBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
				TEXT("GPUFluidBoundaryCellStart_Adhesion_Dummy"));
			uint32 InvalidIndex = 0xFFFFFFFF;
			GraphBuilder.QueueBufferUpload(DummyCellStartBuffer, &InvalidIndex, sizeof(uint32));

			FRDGBufferRef DummyCellEndBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
				TEXT("GPUFluidBoundaryCellEnd_Adhesion_Dummy"));
			GraphBuilder.QueueBufferUpload(DummyCellEndBuffer, &InvalidIndex, sizeof(uint32));

			PassParameters->SortedBoundaryParticles = GraphBuilder.CreateSRV(DummySortedBuffer);
			PassParameters->BoundaryCellStart = GraphBuilder.CreateSRV(DummyCellStartBuffer);
			PassParameters->BoundaryCellEnd = GraphBuilder.CreateSRV(DummyCellEndBuffer);
			PassParameters->bUseBoundaryZOrder = 0;
			PassParameters->MortonBoundsMin = FVector3f::ZeroVector;
			PassParameters->CellSize = Params.CellSize;
		}

		// Adhesion parameters
		PassParameters->AdhesionStrength = CachedBoundaryAdhesionParams.AdhesionStrength;
		PassParameters->AdhesionRadius = CachedBoundaryAdhesionParams.AdhesionRadius;
		PassParameters->CohesionStrength = CachedBoundaryAdhesionParams.CohesionStrength;
		PassParameters->SmoothingRadius = Params.SmoothingRadius;
		PassParameters->DeltaTime = Params.DeltaTime;
		PassParameters->RestDensity = Params.RestDensity;
		PassParameters->Poly6Coeff = Params.Poly6Coeff;

		// =========================================================================
		// PARTICLE-LEVEL EARLY-OUT: Boundary AABB for per-particle culling
		// Skip adhesion calculation for particles far from boundary AABB
		// =========================================================================
		const bool bHasValidAABB = CombinedBoundaryAABB.IsValid();
		PassParameters->bUseBoundaryAABBCulling = bHasValidAABB ? 1 : 0;
		if (bHasValidAABB)
		{
			// Expand AABB by adhesion radius for conservative culling
			const FGPUBoundaryOwnerAABB ExpandedAABB = CombinedBoundaryAABB.ExpandBy(CachedBoundaryAdhesionParams.AdhesionRadius);
			PassParameters->BoundaryAABBMin = ExpandedAABB.Min;
			PassParameters->BoundaryAABBMax = ExpandedAABB.Max;
		}
		else
		{
			// Use infinite bounds (no culling)
			PassParameters->BoundaryAABBMin = FVector3f(-FLT_MAX);
			PassParameters->BoundaryAABBMax = FVector3f(FLT_MAX);
		}

		// Debug: Log adhesion pass parameters (every 60 frames)
		static int32 AdhesionDebugCounter = 0;
		if (++AdhesionDebugCounter % 60 == 1)
		{
			UE_LOG(LogGPUBoundarySkinning, Warning,
				TEXT("[BoundaryAdhesionPass] Running! AdhesionStrength=%.2f, CohesionStrength=%.2f, AdhesionRadius=%.2f, SmoothingRadius=%.2f, BoundaryCount=%d, FluidCount=%d"),
				CachedBoundaryAdhesionParams.AdhesionStrength,
				CachedBoundaryAdhesionParams.CohesionStrength,
				CachedBoundaryAdhesionParams.AdhesionRadius,
				Params.SmoothingRadius,
				BoundaryParticleCount,
				CurrentParticleCount);
		}

		const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FBoundaryAdhesionCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::BoundaryAdhesion%s", bCanUseZOrder ? TEXT(" (Z-Order)") : TEXT("")),
			AdhesionShader,
			PassParameters,
			FIntVector(NumGroups, 1, 1)
		);
	}
}

//=============================================================================
// Boundary Z-Order Sorting Pipeline
//=============================================================================

bool FGPUBoundarySkinningManager::ExecuteBoundaryZOrderSort(
	FRDGBuilder& GraphBuilder,
	const FGPUFluidSimulationParams& Params,
	FRDGBufferRef InSameFrameBoundaryBuffer,
	int32 InSameFrameBoundaryCount,
	FRDGBufferRef& OutSortedBuffer,
	FRDGBufferRef& OutCellStartBuffer,
	FRDGBufferRef& OutCellEndBuffer,
	int32& OutParticleCount)
{
	FScopeLock Lock(&BoundarySkinningLock);

	// Initialize outputs
	OutSortedBuffer = nullptr;
	OutCellStartBuffer = nullptr;
	OutCellEndBuffer = nullptr;
	OutParticleCount = 0;

	// Priority: 1) Same-frame buffer (from caller), 2) Persistent GPU skinning buffer
	// Note: Static boundary uses ExecuteStaticBoundaryZOrderSort() separately
	const bool bUseSameFrameBuffer = InSameFrameBoundaryBuffer != nullptr && InSameFrameBoundaryCount > 0;
	const bool bUseGPUSkinning = !bUseSameFrameBuffer && IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();

	if (!bUseBoundaryZOrder || (!bUseSameFrameBuffer && !bUseGPUSkinning))
	{
		bBoundaryZOrderValid = false;
		return false;
	}

	// Determine boundary particle count and source buffer
	int32 BoundaryParticleCount;
	FRDGBufferRef SourceBoundaryBuffer;

	if (bUseSameFrameBuffer)
	{
		// Use same-frame buffer created in AddBoundarySkinningPass (works on first frame!)
		BoundaryParticleCount = InSameFrameBoundaryCount;
		SourceBoundaryBuffer = InSameFrameBoundaryBuffer;
	}
	else // bUseGPUSkinning
	{
		BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		SourceBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(
			PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_ZOrderSource"));
	}

	if (BoundaryParticleCount <= 0)
	{
		bBoundaryZOrderValid = false;
		return false;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid::BoundaryZOrderSort");

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Get grid parameters from preset
	const int32 CellCount = GridResolutionPresetHelper::GetMaxCells(GridResolutionPreset);

	// Create transient buffers for sorting
	FRDGBufferRef MortonCodesBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BoundaryParticleCount),
		TEXT("GPUFluid.BoundaryMortonCodes")
	);
	FRDGBufferRef MortonCodesTempBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BoundaryParticleCount),
		TEXT("GPUFluid.BoundaryMortonCodesTemp")
	);
	FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BoundaryParticleCount),
		TEXT("GPUFluid.BoundarySortIndices")
	);
	FRDGBufferRef IndicesTempBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BoundaryParticleCount),
		TEXT("GPUFluid.BoundarySortIndicesTemp")
	);

	// Create or reuse persistent buffers
	FRDGBufferRef SortedBoundaryBuffer;
	FRDGBufferRef BoundaryCellStartBuffer;
	FRDGBufferRef BoundaryCellEndBuffer;

	if (BoundaryZOrderBufferCapacity < BoundaryParticleCount)
	{
		PersistentSortedBoundaryBuffer.SafeRelease();
		PersistentBoundaryCellStart.SafeRelease();
		PersistentBoundaryCellEnd.SafeRelease();
		BoundaryZOrderBufferCapacity = BoundaryParticleCount;
	}

	if (PersistentSortedBoundaryBuffer.IsValid())
	{
		SortedBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(
			PersistentSortedBoundaryBuffer, TEXT("GPUFluid.SortedBoundaryParticles"));
	}
	else
	{
		SortedBoundaryBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), BoundaryParticleCount),
			TEXT("GPUFluid.SortedBoundaryParticles")
		);
	}

	if (PersistentBoundaryCellStart.IsValid())
	{
		BoundaryCellStartBuffer = GraphBuilder.RegisterExternalBuffer(
			PersistentBoundaryCellStart, TEXT("GPUFluid.BoundaryCellStart"));
	}
	else
	{
		BoundaryCellStartBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CellCount),
			TEXT("GPUFluid.BoundaryCellStart")
		);
	}

	if (PersistentBoundaryCellEnd.IsValid())
	{
		BoundaryCellEndBuffer = GraphBuilder.RegisterExternalBuffer(
			PersistentBoundaryCellEnd, TEXT("GPUFluid.BoundaryCellEnd"));
	}
	else
	{
		BoundaryCellEndBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CellCount),
			TEXT("GPUFluid.BoundaryCellEnd")
		);
	}

	//=========================================================================
	// Pass 1: Compute Morton codes for boundary particles
	//=========================================================================
	{
		FComputeBoundaryMortonCodesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));
		TShaderMapRef<FComputeBoundaryMortonCodesCS> ComputeShader(ShaderMap, PermutationVector);

		FComputeBoundaryMortonCodesCS::FParameters* PassParams =
			GraphBuilder.AllocParameters<FComputeBoundaryMortonCodesCS::FParameters>();
		PassParams->BoundaryParticlesIn = GraphBuilder.CreateSRV(SourceBoundaryBuffer);
		PassParams->BoundaryMortonCodes = GraphBuilder.CreateUAV(MortonCodesBuffer);
		PassParams->BoundaryParticleIndices = GraphBuilder.CreateUAV(IndicesBuffer);
		PassParams->BoundaryParticleCount = BoundaryParticleCount;
		PassParams->BoundsMin = ZOrderBoundsMin;
		PassParams->CellSize = Params.CellSize;

		const int32 NumGroups = FMath::DivideAndRoundUp(BoundaryParticleCount, FComputeBoundaryMortonCodesCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ComputeBoundaryMortonCodes(%d)", BoundaryParticleCount),
			ComputeShader,
			PassParams,
			FIntVector(NumGroups, 1, 1)
		);
	}

	//=========================================================================
	// Pass 2: Radix Sort (reuse existing radix sort passes)
	//=========================================================================
	{
		const int32 GridAxisBits = GridResolutionPresetHelper::GetAxisBits(GridResolutionPreset);
		const int32 MortonCodeBits = GridAxisBits * 3;
		int32 RadixSortPasses = (MortonCodeBits + GPU_RADIX_BITS - 1) / GPU_RADIX_BITS;
		if (RadixSortPasses % 2 != 0) RadixSortPasses++;

		const int32 NumBlocks = FMath::DivideAndRoundUp(BoundaryParticleCount, GPU_RADIX_ELEMENTS_PER_GROUP);
		const int32 RequiredHistogramSize = GPU_RADIX_SIZE * NumBlocks;

		FRDGBufferRef Histogram = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RequiredHistogramSize),
			TEXT("BoundaryRadixSort.Histogram")
		);
		FRDGBufferRef BucketOffsets = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE),
			TEXT("BoundaryRadixSort.BucketOffsets")
		);

		FRDGBufferRef Keys[2] = { MortonCodesBuffer, MortonCodesTempBuffer };
		FRDGBufferRef Values[2] = { IndicesBuffer, IndicesTempBuffer };
		int32 BufferIndex = 0;

		for (int32 Pass = 0; Pass < RadixSortPasses; ++Pass)
		{
			const int32 BitOffset = Pass * GPU_RADIX_BITS;
			const int32 SrcIndex = BufferIndex;
			const int32 DstIndex = BufferIndex ^ 1;

			// Histogram
			{
				TShaderMapRef<FRadixSortHistogramCS> HistogramShader(ShaderMap);
				FRadixSortHistogramCS::FParameters* HistogramParams = GraphBuilder.AllocParameters<FRadixSortHistogramCS::FParameters>();
				HistogramParams->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
				HistogramParams->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
				HistogramParams->Histogram = GraphBuilder.CreateUAV(Histogram);
				HistogramParams->ElementCount = BoundaryParticleCount;
				HistogramParams->BitOffset = BitOffset;
				HistogramParams->NumGroups = NumBlocks;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BoundaryRadix::Histogram"), HistogramShader, HistogramParams, FIntVector(NumBlocks, 1, 1));
			}

			// Global Prefix Sum
			{
				TShaderMapRef<FRadixSortGlobalPrefixSumCS> PrefixSumShader(ShaderMap);
				FRadixSortGlobalPrefixSumCS::FParameters* GlobalPrefixParams = GraphBuilder.AllocParameters<FRadixSortGlobalPrefixSumCS::FParameters>();
				GlobalPrefixParams->Histogram = GraphBuilder.CreateUAV(Histogram);
				GlobalPrefixParams->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);
				GlobalPrefixParams->NumGroups = NumBlocks;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BoundaryRadix::GlobalPrefixSum"), PrefixSumShader, GlobalPrefixParams, FIntVector(1, 1, 1));
			}

			// Bucket Prefix Sum
			{
				TShaderMapRef<FRadixSortBucketPrefixSumCS> BucketSumShader(ShaderMap);
				FRadixSortBucketPrefixSumCS::FParameters* BucketPrefixParams = GraphBuilder.AllocParameters<FRadixSortBucketPrefixSumCS::FParameters>();
				BucketPrefixParams->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BoundaryRadix::BucketPrefixSum"), BucketSumShader, BucketPrefixParams, FIntVector(1, 1, 1));
			}

			// Scatter
			{
				TShaderMapRef<FRadixSortScatterCS> ScatterShader(ShaderMap);
				FRadixSortScatterCS::FParameters* ScatterParams = GraphBuilder.AllocParameters<FRadixSortScatterCS::FParameters>();
				ScatterParams->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
				ScatterParams->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
				ScatterParams->KeysOut = GraphBuilder.CreateUAV(Keys[DstIndex]);
				ScatterParams->ValuesOut = GraphBuilder.CreateUAV(Values[DstIndex]);
				ScatterParams->HistogramSRV = GraphBuilder.CreateSRV(Histogram);
				ScatterParams->GlobalOffsetsSRV = GraphBuilder.CreateSRV(BucketOffsets);
				ScatterParams->ElementCount = BoundaryParticleCount;
				ScatterParams->BitOffset = BitOffset;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BoundaryRadix::Scatter"), ScatterShader, ScatterParams, FIntVector(NumBlocks, 1, 1));
			}

			BufferIndex ^= 1;
		}

		MortonCodesBuffer = Keys[BufferIndex];
		IndicesBuffer = Values[BufferIndex];
	}

	//=========================================================================
	// Pass 3: Clear Cell Start/End
	//=========================================================================
	{
		FClearBoundaryCellIndicesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));
		TShaderMapRef<FClearBoundaryCellIndicesCS> ClearShader(ShaderMap, PermutationVector);

		FClearBoundaryCellIndicesCS::FParameters* ClearParams =
			GraphBuilder.AllocParameters<FClearBoundaryCellIndicesCS::FParameters>();
		ClearParams->BoundaryCellStart = GraphBuilder.CreateUAV(BoundaryCellStartBuffer);
		ClearParams->BoundaryCellEnd = GraphBuilder.CreateUAV(BoundaryCellEndBuffer);

		const int32 NumGroups = FMath::DivideAndRoundUp(CellCount, FClearBoundaryCellIndicesCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ClearBoundaryCellIndices"),
			ClearShader,
			ClearParams,
			FIntVector(NumGroups, 1, 1)
		);
	}

	//=========================================================================
	// Pass 4: Reorder Boundary Particles
	//=========================================================================
	{
		TShaderMapRef<FReorderBoundaryParticlesCS> ReorderShader(ShaderMap);

		FReorderBoundaryParticlesCS::FParameters* ReorderParams =
			GraphBuilder.AllocParameters<FReorderBoundaryParticlesCS::FParameters>();
		ReorderParams->OldBoundaryParticles = GraphBuilder.CreateSRV(SourceBoundaryBuffer);
		ReorderParams->SortedBoundaryIndices = GraphBuilder.CreateSRV(IndicesBuffer);
		ReorderParams->SortedBoundaryParticles = GraphBuilder.CreateUAV(SortedBoundaryBuffer);
		ReorderParams->BoundaryParticleCount = BoundaryParticleCount;

		const int32 NumGroups = FMath::DivideAndRoundUp(BoundaryParticleCount, FReorderBoundaryParticlesCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ReorderBoundaryParticles(%d)", BoundaryParticleCount),
			ReorderShader,
			ReorderParams,
			FIntVector(NumGroups, 1, 1)
		);
	}

	//=========================================================================
	// Pass 5: Compute Cell Start/End
	//=========================================================================
	{
		FComputeBoundaryCellStartEndCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));
		TShaderMapRef<FComputeBoundaryCellStartEndCS> CellStartEndShader(ShaderMap, PermutationVector);

		FComputeBoundaryCellStartEndCS::FParameters* CellParams =
			GraphBuilder.AllocParameters<FComputeBoundaryCellStartEndCS::FParameters>();
		CellParams->SortedBoundaryMortonCodes = GraphBuilder.CreateSRV(MortonCodesBuffer);
		CellParams->BoundaryCellStart = GraphBuilder.CreateUAV(BoundaryCellStartBuffer);
		CellParams->BoundaryCellEnd = GraphBuilder.CreateUAV(BoundaryCellEndBuffer);
		CellParams->BoundaryParticleCount = BoundaryParticleCount;

		const int32 NumGroups = FMath::DivideAndRoundUp(BoundaryParticleCount, FComputeBoundaryCellStartEndCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ComputeBoundaryCellStartEnd(%d)", BoundaryParticleCount),
			CellStartEndShader,
			CellParams,
			FIntVector(NumGroups, 1, 1)
		);
	}

	// Extract persistent buffers (for next frame fallback)
	GraphBuilder.QueueBufferExtraction(SortedBoundaryBuffer, &PersistentSortedBoundaryBuffer);
	GraphBuilder.QueueBufferExtraction(BoundaryCellStartBuffer, &PersistentBoundaryCellStart);
	GraphBuilder.QueueBufferExtraction(BoundaryCellEndBuffer, &PersistentBoundaryCellEnd);

	// Set same-frame output buffers (for current frame use)
	OutSortedBuffer = SortedBoundaryBuffer;
	OutCellStartBuffer = BoundaryCellStartBuffer;
	OutCellEndBuffer = BoundaryCellEndBuffer;
	OutParticleCount = BoundaryParticleCount;

	bBoundaryZOrderValid = true;
	bBoundaryZOrderDirty = false;

	UE_LOG(LogGPUBoundarySkinning, Verbose,
		TEXT("BoundaryZOrderSort completed: %d particles, Preset=%d"),
		BoundaryParticleCount, static_cast<int32>(GridResolutionPreset));

	return true;
}
