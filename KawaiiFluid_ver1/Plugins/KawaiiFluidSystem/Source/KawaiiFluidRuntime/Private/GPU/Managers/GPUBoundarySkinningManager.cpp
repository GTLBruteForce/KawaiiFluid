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
	, bBoundaryParticlesValid(false)
	, TotalLocalBoundaryParticleCount(0)
	, WorldBoundaryBufferCapacity(0)
	, bBoundarySkinningDataDirty(false)
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

	BoundarySkinningDataMap.Empty();
	PersistentLocalBoundaryBuffers.Empty();
	PersistentWorldBoundaryBuffer.SafeRelease();
	PreviousWorldBoundaryBuffer.SafeRelease();
	WorldBoundaryBufferCapacity = 0;
	TotalLocalBoundaryParticleCount = 0;
	bHasPreviousFrame = false;

	CachedBoundaryParticles.Empty();
	bBoundaryParticlesValid = false;
	bBoundarySkinningDataDirty = false;
	bIsInitialized = false;

	UE_LOG(LogGPUBoundarySkinning, Log, TEXT("GPUBoundarySkinningManager released"));
}

//=============================================================================
// Boundary Particles Upload (Legacy CPU path)
//=============================================================================

void FGPUBoundarySkinningManager::UploadBoundaryParticles(const FGPUBoundaryParticles& BoundaryParticles)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&BoundarySkinningLock);

	CachedBoundaryParticles = BoundaryParticles.Particles;

	if (BoundaryParticles.IsEmpty())
	{
		bBoundaryParticlesValid = false;
		return;
	}

	bBoundaryParticlesValid = true;

	UE_LOG(LogGPUBoundarySkinning, Verbose, TEXT("Cached boundary particles: Count=%d"), CachedBoundaryParticles.Num());
}

//=============================================================================
// GPU Boundary Skinning
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

	UE_LOG(LogGPUBoundarySkinning, Log, TEXT("ClearAllBoundarySkinningData"));
}

bool FGPUBoundarySkinningManager::IsBoundaryAdhesionEnabled() const
{
	return CachedBoundaryAdhesionParams.bEnabled != 0 &&
		(CachedBoundaryParticles.Num() > 0 || TotalLocalBoundaryParticleCount > 0);
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
		PreviousBoundaryBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), FMath::Max(1, WorldBoundaryBufferCapacity)),
			TEXT("GPUFluidPreviousBoundaryParticles_Dummy")
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
	int32 InSameFrameBoundaryCount)
{
	// Priority: 1) Same-frame buffer, 2) Persistent buffer, 3) CPU fallback
	const bool bUseSameFrameBuffer = InSameFrameBoundaryBuffer != nullptr && InSameFrameBoundaryCount > 0;
	const bool bUseGPUSkinning = !bUseSameFrameBuffer && IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();
	const bool bUseCPUBoundary = !bUseSameFrameBuffer && !bUseGPUSkinning && CachedBoundaryParticles.Num() > 0;

	if (!IsBoundaryAdhesionEnabled() || (!bUseSameFrameBuffer && !bUseGPUSkinning && !bUseCPUBoundary) || CurrentParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	int32 BoundaryParticleCount;
	FRDGBufferRef BoundaryParticleBuffer;
	FRDGBufferSRVRef BoundaryParticlesSRV;

	if (bUseSameFrameBuffer)
	{
		// Use same-frame buffer created in AddBoundarySkinningPass (works on first frame!)
		BoundaryParticleCount = InSameFrameBoundaryCount;
		BoundaryParticleBuffer = InSameFrameBoundaryBuffer;
		BoundaryParticlesSRV = GraphBuilder.CreateSRV(BoundaryParticleBuffer);
	}
	else if (bUseGPUSkinning)
	{
		BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		BoundaryParticleBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_Adhesion"));
		BoundaryParticlesSRV = GraphBuilder.CreateSRV(BoundaryParticleBuffer);
	}
	else
	{
		BoundaryParticleCount = CachedBoundaryParticles.Num();
		BoundaryParticleBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles"),
			sizeof(FGPUBoundaryParticle),
			BoundaryParticleCount,
			CachedBoundaryParticles.GetData(),
			BoundaryParticleCount * sizeof(FGPUBoundaryParticle),
			ERDGInitialDataFlags::NoCopy
		);
		BoundaryParticlesSRV = GraphBuilder.CreateSRV(BoundaryParticleBuffer);
	}

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
		const bool bCanUseZOrder = bUseBoundaryZOrder && bBoundaryZOrderValid
			&& PersistentSortedBoundaryBuffer.IsValid()
			&& PersistentBoundaryCellStart.IsValid()
			&& PersistentBoundaryCellEnd.IsValid();

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
		if (bCanUseZOrder)
		{
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

void FGPUBoundarySkinningManager::ExecuteBoundaryZOrderSort(
	FRDGBuilder& GraphBuilder,
	const FGPUFluidSimulationParams& Params,
	FRDGBufferRef InSameFrameBoundaryBuffer,
	int32 InSameFrameBoundaryCount)
{
	FScopeLock Lock(&BoundarySkinningLock);

	// Priority: 1) Same-frame buffer, 2) Persistent buffer, 3) CPU fallback
	const bool bUseSameFrameBuffer = InSameFrameBoundaryBuffer != nullptr && InSameFrameBoundaryCount > 0;
	const bool bUseGPUSkinning = !bUseSameFrameBuffer && IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();
	const bool bUseCPUBoundary = !bUseSameFrameBuffer && !bUseGPUSkinning && CachedBoundaryParticles.Num() > 0;

	if (!bUseBoundaryZOrder || (!bUseSameFrameBuffer && !bUseGPUSkinning && !bUseCPUBoundary))
	{
		bBoundaryZOrderValid = false;
		return;
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
	else if (bUseGPUSkinning)
	{
		BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		SourceBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(
			PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_ZOrderSource"));
	}
	else
	{
		BoundaryParticleCount = CachedBoundaryParticles.Num();
		SourceBoundaryBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles_CPU"),
			sizeof(FGPUBoundaryParticle),
			BoundaryParticleCount,
			CachedBoundaryParticles.GetData(),
			BoundaryParticleCount * sizeof(FGPUBoundaryParticle),
			ERDGInitialDataFlags::NoCopy
		);
	}

	if (BoundaryParticleCount <= 0)
	{
		bBoundaryZOrderValid = false;
		return;
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

	// Extract persistent buffers
	GraphBuilder.QueueBufferExtraction(SortedBoundaryBuffer, &PersistentSortedBoundaryBuffer);
	GraphBuilder.QueueBufferExtraction(BoundaryCellStartBuffer, &PersistentBoundaryCellStart);
	GraphBuilder.QueueBufferExtraction(BoundaryCellEndBuffer, &PersistentBoundaryCellEnd);

	bBoundaryZOrderValid = true;
	bBoundaryZOrderDirty = false;

	UE_LOG(LogGPUBoundarySkinning, Verbose,
		TEXT("BoundaryZOrderSort completed: %d particles, Preset=%d"),
		BoundaryParticleCount, static_cast<int32>(GridResolutionPreset));
}
