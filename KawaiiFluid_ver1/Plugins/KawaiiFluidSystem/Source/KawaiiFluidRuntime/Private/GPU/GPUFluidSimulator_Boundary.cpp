// Copyright KawaiiFluid Team. All Rights Reserved.
// GPUFluidSimulator - Boundary Particles and Skinning Functions

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);

// Boundary spatial hash constants for Flex-style adhesion
static constexpr int32 BOUNDARY_HASH_SIZE = 65536;  // 2^16 cells
static constexpr int32 BOUNDARY_MAX_PARTICLES_PER_CELL = 16;

//=============================================================================
// Boundary Particles Upload (Flex-style Adhesion)
//=============================================================================

void FGPUFluidSimulator::UploadBoundaryParticles(const FGPUBoundaryParticles& BoundaryParticles)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&BufferLock);

	// Cache the boundary particle data (will be uploaded to GPU during simulation)
	CachedBoundaryParticles = BoundaryParticles.Particles;

	// Check if we have any boundary particles
	if (BoundaryParticles.IsEmpty())
	{
		bBoundaryParticlesValid = false;
		return;
	}

	bBoundaryParticlesValid = true;

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Cached boundary particles: Count=%d"),
		CachedBoundaryParticles.Num());
}

//=============================================================================
// GPU Boundary Skinning (Persistent Local Boundary + GPU Transform)
//=============================================================================

void FGPUFluidSimulator::UploadLocalBoundaryParticles(int32 OwnerID, const TArray<FGPUBoundaryParticleLocal>& LocalParticles)
{
	if (!bIsInitialized || LocalParticles.Num() == 0)
	{
		return;
	}

	FScopeLock Lock(&BoundarySkinningLock);

	// Find or create skinning data for this owner
	FGPUBoundarySkinningData& SkinningData = BoundarySkinningDataMap.FindOrAdd(OwnerID);
	SkinningData.OwnerID = OwnerID;
	SkinningData.LocalParticles = LocalParticles;
	SkinningData.bLocalParticlesUploaded = false;  // Mark for upload
	bBoundarySkinningDataDirty = true;

	// Recalculate total count
	TotalLocalBoundaryParticleCount = 0;
	for (const auto& Pair : BoundarySkinningDataMap)
	{
		TotalLocalBoundaryParticleCount += Pair.Value.LocalParticles.Num();
	}

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadLocalBoundaryParticles: OwnerID=%d, Count=%d, TotalCount=%d"),
		OwnerID, LocalParticles.Num(), TotalLocalBoundaryParticleCount);
}

void FGPUFluidSimulator::UploadBoneTransformsForBoundary(int32 OwnerID, const TArray<FMatrix44f>& BoneTransforms, const FMatrix44f& ComponentTransform)
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

void FGPUFluidSimulator::RemoveBoundarySkinningData(int32 OwnerID)
{
	FScopeLock Lock(&BoundarySkinningLock);

	if (BoundarySkinningDataMap.Remove(OwnerID) > 0)
	{
		// Remove persistent buffer for this owner
		PersistentLocalBoundaryBuffers.Remove(OwnerID);

		// Recalculate total count
		TotalLocalBoundaryParticleCount = 0;
		for (const auto& Pair : BoundarySkinningDataMap)
		{
			TotalLocalBoundaryParticleCount += Pair.Value.LocalParticles.Num();
		}

		bBoundarySkinningDataDirty = true;

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("RemoveBoundarySkinningData: OwnerID=%d, TotalCount=%d"),
			OwnerID, TotalLocalBoundaryParticleCount);
	}
}

void FGPUFluidSimulator::ClearAllBoundarySkinningData()
{
	FScopeLock Lock(&BoundarySkinningLock);

	BoundarySkinningDataMap.Empty();
	PersistentLocalBoundaryBuffers.Empty();
	PersistentWorldBoundaryBuffer.SafeRelease();
	WorldBoundaryBufferCapacity = 0;
	TotalLocalBoundaryParticleCount = 0;
	bBoundarySkinningDataDirty = true;

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("ClearAllBoundarySkinningData"));
}

//=============================================================================
// Boundary Adhesion Pass
//=============================================================================

void FGPUFluidSimulator::AddBoundaryAdhesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	// Check if we have boundary particles from either GPU skinning or CPU upload
	const bool bUseGPUSkinning = IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();
	const bool bUseCPUBoundary = !bUseGPUSkinning && CachedBoundaryParticles.Num() > 0;

	// Early exit if no boundary particles or adhesion disabled
	if (!IsBoundaryAdhesionEnabled() || (!bUseGPUSkinning && !bUseCPUBoundary) || CurrentParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	int32 BoundaryParticleCount;
	FRDGBufferRef BoundaryParticleBuffer;
	FRDGBufferSRVRef BoundaryParticlesSRV;

	if (bUseGPUSkinning)
	{
		// Use GPU-skinned world boundary buffer
		BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		BoundaryParticleBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_Adhesion"));
		BoundaryParticlesSRV = GraphBuilder.CreateSRV(BoundaryParticleBuffer);
	}
	else
	{
		// Use CPU-uploaded boundary particles (legacy path)
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

	// Cell size = AdhesionRadius (particles within this range can interact)
	const float BoundaryCellSize = CachedBoundaryAdhesionParams.AdhesionRadius;

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

	// Pass 3: Boundary adhesion (using spatial hash)
	{
		TShaderMapRef<FBoundaryAdhesionCS> AdhesionShader(ShaderMap);
		FBoundaryAdhesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundaryAdhesionCS::FParameters>();
		PassParameters->Particles = ParticlesUAV;
		PassParameters->ParticleCount = CurrentParticleCount;
		PassParameters->BoundaryParticles = BoundaryParticlesSRV;
		PassParameters->BoundaryParticleCount = BoundaryParticleCount;
		PassParameters->BoundaryCellCounts = GraphBuilder.CreateSRV(AdhesionCellCountsBuffer);
		PassParameters->BoundaryParticleIndices = GraphBuilder.CreateSRV(AdhesionParticleIndicesBuffer);
		PassParameters->BoundaryCellSize = BoundaryCellSize;
		PassParameters->AdhesionStrength = CachedBoundaryAdhesionParams.AdhesionStrength;
		PassParameters->AdhesionRadius = CachedBoundaryAdhesionParams.AdhesionRadius;
		PassParameters->CohesionStrength = CachedBoundaryAdhesionParams.CohesionStrength;
		PassParameters->SmoothingRadius = Params.SmoothingRadius;
		PassParameters->DeltaTime = Params.DeltaTime;
		PassParameters->RestDensity = Params.RestDensity;
		PassParameters->Poly6Coeff = Params.Poly6Coeff;

		const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FBoundaryAdhesionCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::BoundaryAdhesion"),
			AdhesionShader,
			PassParameters,
			FIntVector(NumGroups, 1, 1)
		);
	}
}

//=============================================================================
// GPU Boundary Skinning Pass
// Transforms bone-local boundary particles to world space using GPU compute
//=============================================================================

void FGPUFluidSimulator::AddBoundarySkinningPass(
	FRDGBuilder& GraphBuilder,
	const FGPUFluidSimulationParams& Params)
{
	FScopeLock Lock(&BoundarySkinningLock);

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
		WorldBoundaryBufferCapacity = TotalLocalBoundaryParticleCount;
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

	// Track output offset for concatenating multiple owners
	int32 OutputOffset = 0;

	// Process each owner's boundary particles
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
			// Upload local particles (first time or after change)
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

			// Extract pooled buffer for persistence
			GraphBuilder.QueueBufferExtraction(LocalBoundaryBuffer, &LocalBuffer);
		}
		else
		{
			// Reuse existing buffer
			LocalBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(LocalBuffer, TEXT("GPUFluidLocalBoundaryParticles"));
		}
		FRDGBufferSRVRef LocalBoundarySRV = GraphBuilder.CreateSRV(LocalBoundaryBuffer);

		// Upload bone transforms (each frame)
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
			// Create dummy buffer with identity matrix
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
		PassParams->BoneTransforms = SkinningBoneTransformsSRV;
		PassParams->BoundaryParticleCount = LocalParticleCount;
		PassParams->BoneCount = FMath::Max(1, BoneCount);
		PassParams->OwnerID = OwnerID;
		PassParams->ComponentTransform = SkinningData.ComponentTransform;

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

	// Extract world boundary buffer for use in density/viscosity passes
	GraphBuilder.QueueBufferExtraction(WorldBoundaryBuffer, &PersistentWorldBoundaryBuffer);

	// Update cached boundary particles with world-space positions
	// This is done by reading back from GPU (or we can use the skinned buffer directly in passes)
	// For now, we use the persistent world buffer in subsequent passes
}
