// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUCollisionManager Implementation

#include "GPU/Managers/GPUCollisionManager.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUCollisionManager, Log, All);
DEFINE_LOG_CATEGORY(LogGPUCollisionManager);

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUCollisionManager::FGPUCollisionManager()
{
}

FGPUCollisionManager::~FGPUCollisionManager()
{
	Release();
}

//=============================================================================
// Lifecycle
//=============================================================================

void FGPUCollisionManager::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	// Create feedback manager
	FeedbackManager = MakeUnique<FGPUCollisionFeedbackManager>();
	FeedbackManager->Initialize();

	bIsInitialized = true;
	UE_LOG(LogGPUCollisionManager, Log, TEXT("FGPUCollisionManager initialized"));
}

void FGPUCollisionManager::Release()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Release feedback manager
	if (FeedbackManager.IsValid())
	{
		FeedbackManager->Release();
		FeedbackManager.Reset();
	}

	// Clear cached data
	CachedSpheres.Empty();
	CachedCapsules.Empty();
	CachedBoxes.Empty();
	CachedConvexHeaders.Empty();
	CachedConvexPlanes.Empty();
	CachedBoneTransforms.Empty();

	// Release heightmap texture
	HeightmapTextureRHI.SafeRelease();
	bHeightmapDataValid = false;

	bCollisionPrimitivesValid = false;
	bBoneTransformsValid = false;
	bIsInitialized = false;

	UE_LOG(LogGPUCollisionManager, Log, TEXT("FGPUCollisionManager released"));
}

//=============================================================================
// Collision Primitives Upload
//=============================================================================

void FGPUCollisionManager::UploadCollisionPrimitives(const FGPUCollisionPrimitives& Primitives)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&CollisionLock);

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

	UE_LOG(LogGPUCollisionManager, Verbose, TEXT("Cached collision primitives: Spheres=%d, Capsules=%d, Boxes=%d, Convexes=%d, Planes=%d, BoneTransforms=%d"),
		CachedSpheres.Num(), CachedCapsules.Num(), CachedBoxes.Num(), CachedConvexHeaders.Num(), CachedConvexPlanes.Num(), CachedBoneTransforms.Num());
}

//=============================================================================
// Bounds Collision Pass
//=============================================================================

void FGPUCollisionManager::AddBoundsCollisionPass(
	FRDGBuilder& GraphBuilder,
	const FSimulationSpatialData& SpatialData,
	int32 ParticleCount,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FBoundsCollisionCS> ComputeShader(ShaderMap);

	FBoundsCollisionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundsCollisionCS::FParameters>();
	// Bind SOA buffers
	PassParameters->Positions = GraphBuilder.CreateUAV(SpatialData.SoA_Positions, PF_R32_FLOAT);
	PassParameters->PredictedPositions = GraphBuilder.CreateUAV(SpatialData.SoA_PredictedPositions, PF_R32_FLOAT);
	PassParameters->PackedVelocities = GraphBuilder.CreateUAV(SpatialData.SoA_PackedVelocities, PF_R32G32_UINT);  // B plan
	PassParameters->Flags = GraphBuilder.CreateUAV(SpatialData.SoA_Flags, PF_R32_UINT);
	PassParameters->ParticleCount = ParticleCount;
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

	const uint32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FBoundsCollisionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::BoundsCollision"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Primitive Collision Pass (Spheres, Capsules, Boxes, Convex)
//=============================================================================

void FGPUCollisionManager::AddPrimitiveCollisionPass(
	FRDGBuilder& GraphBuilder,
	const FSimulationSpatialData& SpatialData,
	int32 ParticleCount,
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
	FRDGBufferSRVRef BoneTransformsSRV = nullptr;

	// Dummy data for empty buffers (shader requires all SRVs to be valid)
	static FGPUCollisionSphere DummySphere;
	static FGPUCollisionCapsule DummyCapsule;
	static FGPUCollisionBox DummyBox;
	static FGPUCollisionConvex DummyConvex;
	static FGPUConvexPlane DummyPlane;
	static FGPUBoneTransform DummyBone;

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

	{
		const bool bHasData = CachedBoneTransforms.Num() > 0;
		FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionBoneTransforms"),
			sizeof(FGPUBoneTransform),
			bHasData ? CachedBoneTransforms.Num() : 1,
			bHasData ? CachedBoneTransforms.GetData() : &DummyBone,
			bHasData ? CachedBoneTransforms.Num() * sizeof(FGPUBoneTransform) : sizeof(FGPUBoneTransform),
			ERDGInitialDataFlags::NoCopy
		);
		BoneTransformsSRV = GraphBuilder.CreateSRV(BoneTransformsBuffer);
	}

	// Create Unified Collision Feedback Buffer
	// Single ByteAddressBuffer containing all feedback types with embedded counters
	// Layout: [Header:16B][BoneFeedback][SMFeedback][FISMFeedback]
	const bool bFeedbackEnabled = FeedbackManager.IsValid() && FeedbackManager->IsEnabled();
	FRDGBufferRef UnifiedFeedbackBuffer = nullptr;

	if (bFeedbackEnabled)
	{
		// Create or reuse unified feedback buffer as ByteAddressBuffer
		TRefCountPtr<FRDGPooledBuffer>& UnifiedPooled = FeedbackManager->GetUnifiedFeedbackBuffer();
		if (UnifiedPooled.IsValid())
		{
			UnifiedFeedbackBuffer = GraphBuilder.RegisterExternalBuffer(UnifiedPooled, TEXT("UnifiedCollisionFeedback"));
		}
		else
		{
			// Create ByteAddressBuffer with total unified size
			FRDGBufferDesc UnifiedDesc = FRDGBufferDesc::CreateByteAddressDesc(FGPUCollisionFeedbackManager::UNIFIED_BUFFER_SIZE);
			UnifiedFeedbackBuffer = GraphBuilder.CreateBuffer(UnifiedDesc, TEXT("UnifiedCollisionFeedback"));
		}

		// Clear header (first 16 bytes = 4 counters) at start of frame
		// Note: Only clearing header, not entire buffer (performance optimization)
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(UnifiedFeedbackBuffer), 0);
	}
	else
	{
		// Create minimal dummy buffer when feedback is disabled
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateByteAddressDesc(16);  // Minimal header size
		UnifiedFeedbackBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("UnifiedCollisionFeedbackDummy"));
	}

	// Create collider contact count buffer (unchanged, separate from unified buffer)
	FRDGBufferRef ContactCountBuffer = nullptr;
	if (FeedbackManager.IsValid())
	{
		TRefCountPtr<FRDGPooledBuffer>& ContactCountPooledBuffer = FeedbackManager->GetContactCountBuffer();
		if (ContactCountPooledBuffer.IsValid())
		{
			ContactCountBuffer = GraphBuilder.RegisterExternalBuffer(ContactCountPooledBuffer, TEXT("ColliderContactCounts"));
		}
		else
		{
			FRDGBufferDesc ContactCountDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FGPUCollisionFeedbackManager::MAX_COLLIDER_COUNT);
			ContactCountBuffer = GraphBuilder.CreateBuffer(ContactCountDesc, TEXT("ColliderContactCounts"));
		}
	}
	else
	{
		FRDGBufferDesc ContactCountDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FGPUCollisionFeedbackManager::MAX_COLLIDER_COUNT);
		ContactCountBuffer = GraphBuilder.CreateBuffer(ContactCountDesc, TEXT("ColliderContactCounts"));
	}

	// Clear contact counts at start of frame
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ContactCountBuffer), 0);

	// Dispatch primitive collision shader directly (with unified feedback buffer)
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FPrimitiveCollisionCS> ComputeShader(GlobalShaderMap);

	FPrimitiveCollisionCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FPrimitiveCollisionCS::FParameters>();

	// Bind SOA buffers
	PassParameters->Positions = GraphBuilder.CreateUAV(SpatialData.SoA_Positions, PF_R32_FLOAT);
	PassParameters->PredictedPositions = GraphBuilder.CreateUAV(SpatialData.SoA_PredictedPositions, PF_R32_FLOAT);
	PassParameters->PackedVelocities = GraphBuilder.CreateUAV(SpatialData.SoA_PackedVelocities, PF_R32G32_UINT);  // B plan
	PassParameters->PackedDensityLambda = GraphBuilder.CreateSRV(SpatialData.SoA_PackedDensityLambda, PF_R32_UINT);  // B plan
	PassParameters->SourceIDs = GraphBuilder.CreateSRV(SpatialData.SoA_SourceIDs, PF_R32_SINT);
	PassParameters->Flags = GraphBuilder.CreateUAV(SpatialData.SoA_Flags, PF_R32_UINT);
	PassParameters->ParticleCount = ParticleCount;
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
	PassParameters->BoneTransforms = BoneTransformsSRV;
	PassParameters->BoneCount = CachedBoneTransforms.Num();

	// Unified feedback buffer (ByteAddressBuffer with embedded counters)
	PassParameters->UnifiedFeedbackBuffer = GraphBuilder.CreateUAV(UnifiedFeedbackBuffer);
	PassParameters->bEnableCollisionFeedback = bFeedbackEnabled ? 1 : 0;

	// Collider contact count parameters (unchanged)
	PassParameters->ColliderContactCounts = GraphBuilder.CreateUAV(ContactCountBuffer);
	PassParameters->MaxColliderCount = FGPUCollisionFeedbackManager::MAX_COLLIDER_COUNT;

	const int32 ThreadGroupSize = FPrimitiveCollisionCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PrimitiveCollision(%d particles, %d primitives, feedback=%s)",
			ParticleCount, CachedSpheres.Num() + CachedCapsules.Num() + CachedBoxes.Num() + CachedConvexHeaders.Num(),
			bFeedbackEnabled ? TEXT("ON") : TEXT("OFF")),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));

	// Single unified buffer extraction (replaces 6 separate extractions)
	if (bFeedbackEnabled && FeedbackManager.IsValid())
	{
		GraphBuilder.QueueBufferExtraction(
			UnifiedFeedbackBuffer,
			&FeedbackManager->GetUnifiedFeedbackBuffer(),
			ERHIAccess::UAVCompute
		);
	}

	// Always extract collider contact count buffer (if manager valid)
	if (FeedbackManager.IsValid())
	{
		GraphBuilder.QueueBufferExtraction(
			ContactCountBuffer,
			&FeedbackManager->GetContactCountBuffer(),
			ERHIAccess::UAVCompute
		);
	}
}

//=============================================================================
// Collision Feedback
//=============================================================================

void FGPUCollisionManager::SetCollisionFeedbackEnabled(bool bEnabled)
{
	if (FeedbackManager.IsValid())
	{
		FeedbackManager->SetEnabled(bEnabled);
	}
}

bool FGPUCollisionManager::IsCollisionFeedbackEnabled() const
{
	return FeedbackManager.IsValid() && FeedbackManager->IsEnabled();
}

void FGPUCollisionManager::AllocateCollisionFeedbackBuffers(FRHICommandListImmediate& RHICmdList)
{
	if (FeedbackManager.IsValid())
	{
		FeedbackManager->AllocateReadbackObjects(RHICmdList);
	}
}

void FGPUCollisionManager::ReleaseCollisionFeedbackBuffers()
{
	// Manager release is handled in Release()
}

void FGPUCollisionManager::ProcessCollisionFeedbackReadback(FRHICommandListImmediate& RHICmdList)
{
	if (FeedbackManager.IsValid())
	{
		FeedbackManager->ProcessFeedbackReadback(RHICmdList);
	}
}

void FGPUCollisionManager::ProcessColliderContactCountReadback(FRHICommandListImmediate& RHICmdList)
{
	if (FeedbackManager.IsValid())
	{
		FeedbackManager->ProcessContactCountReadback(RHICmdList);
	}
}

bool FGPUCollisionManager::GetCollisionFeedbackForCollider(int32 ColliderIndex, TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	if (!FeedbackManager.IsValid())
	{
		OutFeedback.Reset();
		OutCount = 0;
		return false;
	}
	return FeedbackManager->GetFeedbackForCollider(ColliderIndex, OutFeedback, OutCount);
}

bool FGPUCollisionManager::GetAllCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	if (!FeedbackManager.IsValid())
	{
		OutFeedback.Reset();
		OutCount = 0;
		return false;
	}
	return FeedbackManager->GetAllFeedback(OutFeedback, OutCount);
}

int32 FGPUCollisionManager::GetCollisionFeedbackCount() const
{
	return FeedbackManager.IsValid() ? FeedbackManager->GetFeedbackCount() : 0;
}

bool FGPUCollisionManager::GetAllStaticMeshCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	if (!FeedbackManager.IsValid())
	{
		OutFeedback.Reset();
		OutCount = 0;
		return false;
	}
	return FeedbackManager->GetAllStaticMeshFeedback(OutFeedback, OutCount);
}

int32 FGPUCollisionManager::GetStaticMeshCollisionFeedbackCount() const
{
	return FeedbackManager.IsValid() ? FeedbackManager->GetStaticMeshFeedbackCount() : 0;
}

bool FGPUCollisionManager::GetAllFluidInteractionSMCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	if (!FeedbackManager.IsValid())
	{
		OutFeedback.Reset();
		OutCount = 0;
		return false;
	}
	return FeedbackManager->GetAllFluidInteractionSMFeedback(OutFeedback, OutCount);
}

int32 FGPUCollisionManager::GetFluidInteractionSMCollisionFeedbackCount() const
{
	return FeedbackManager.IsValid() ? FeedbackManager->GetFluidInteractionSMFeedbackCount() : 0;
}

int32 FGPUCollisionManager::GetColliderContactCount(int32 ColliderIndex) const
{
	if (!FeedbackManager.IsValid())
	{
		return 0;
	}
	return FeedbackManager->GetContactCount(ColliderIndex);
}

void FGPUCollisionManager::GetAllColliderContactCounts(TArray<int32>& OutCounts) const
{
	if (!FeedbackManager.IsValid())
	{
		OutCounts.Empty();
		return;
	}
	FeedbackManager->GetAllContactCounts(OutCounts);
}

int32 FGPUCollisionManager::GetContactCountForOwner(int32 OwnerID) const
{
	// Debug logging (every 60 frames)
	static int32 OwnerCountDebugFrame = 0;
	const bool bLogThisFrame = (OwnerCountDebugFrame++ % 60 == 0);

	int32 TotalCount = 0;
	int32 ColliderIndex = 0;
	int32 MatchedColliders = 0;

	// Spheres: indices 0 to SphereCount-1
	for (int32 i = 0; i < CachedSpheres.Num(); ++i)
	{
		if (CachedSpheres[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	// Capsules: indices SphereCount to SphereCount+CapsuleCount-1
	for (int32 i = 0; i < CachedCapsules.Num(); ++i)
	{
		if (CachedCapsules[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	// Boxes: indices SphereCount+CapsuleCount to SphereCount+CapsuleCount+BoxCount-1
	for (int32 i = 0; i < CachedBoxes.Num(); ++i)
	{
		if (CachedBoxes[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	// Convexes: remaining indices
	for (int32 i = 0; i < CachedConvexHeaders.Num(); ++i)
	{
		if (CachedConvexHeaders[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	if (bLogThisFrame && MatchedColliders > 0)
	{
		UE_LOG(LogGPUCollisionManager, Log, TEXT("[ContactCountForOwner] OwnerID=%d, MatchedColliders=%d, TotalCount=%d"),
			OwnerID, MatchedColliders, TotalCount);
	}

	return TotalCount;
}

//=============================================================================
// Heightmap Collision (Landscape terrain)
//=============================================================================

void FGPUCollisionManager::UploadHeightmapTexture(const TArray<float>& HeightData, int32 Width, int32 Height)
{
	if (!bIsInitialized)
	{
		return;
	}

	if (HeightData.Num() != Width * Height)
	{
		UE_LOG(LogGPUCollisionManager, Warning, TEXT("Heightmap data size mismatch: %d != %d x %d"), HeightData.Num(), Width, Height);
		bHeightmapDataValid = false;
		return;
	}

	// Update parameters on game thread
	{
		FScopeLock Lock(&CollisionLock);
		HeightmapParams.TextureWidth = Width;
		HeightmapParams.TextureHeight = Height;
		HeightmapParams.UpdateInverseValues();
	}

	// Copy data for render thread
	TArray<float> HeightDataCopy = HeightData;
	FTextureRHIRef* TexturePtr = &HeightmapTextureRHI;
	bool* ValidPtr = &bHeightmapDataValid;
	FGPUHeightmapCollisionParams* ParamsPtr = &HeightmapParams;

	// Create texture on render thread
	ENQUEUE_RENDER_COMMAND(UploadHeightmapTexture)(
		[TexturePtr, ValidPtr, ParamsPtr, HeightDataCopy = MoveTemp(HeightDataCopy), Width, Height](FRHICommandListImmediate& RHICmdList)
		{
			// Release existing texture if any (prevent memory leak on re-upload)
			if (TexturePtr->IsValid())
			{
				TexturePtr->SafeRelease();
			}

			// Create R32F texture
			const FRHITextureCreateDesc Desc =
				FRHITextureCreateDesc::Create2D(TEXT("HeightmapTexture"), Width, Height, PF_R32_FLOAT)
				.SetFlags(ETextureCreateFlags::ShaderResource)
				.SetNumMips(1);

			*TexturePtr = RHICreateTexture(Desc);

			if (!TexturePtr->IsValid())
			{
				UE_LOG(LogGPUCollisionManager, Error, TEXT("Failed to create heightmap texture"));
				*ValidPtr = false;
				return;
			}

			// Upload data to texture
			uint32 DestStride = 0;
			float* DestData = (float*)RHILockTexture2D(*TexturePtr, 0, RLM_WriteOnly, DestStride, false);

			if (DestData)
			{
				const uint32 SrcRowPitch = Width * sizeof(float);
				for (int32 y = 0; y < Height; ++y)
				{
					FMemory::Memcpy(
						(uint8*)DestData + y * DestStride,
						HeightDataCopy.GetData() + y * Width,
						SrcRowPitch);
				}
				RHIUnlockTexture2D(*TexturePtr, 0, false);
				*ValidPtr = true;

				UE_LOG(LogGPUCollisionManager, Log, TEXT("Uploaded heightmap texture: %dx%d"), Width, Height);
			}
			else
			{
				UE_LOG(LogGPUCollisionManager, Error, TEXT("Failed to lock heightmap texture"));
				TexturePtr->SafeRelease();
				*ValidPtr = false;
			}
		});

	UE_LOG(LogGPUCollisionManager, Log, TEXT("Enqueued heightmap texture upload: %dx%d, WorldBounds: (%.1f,%.1f,%.1f) - (%.1f,%.1f,%.1f)"),
		Width, Height,
		HeightmapParams.WorldMin.X, HeightmapParams.WorldMin.Y, HeightmapParams.WorldMin.Z,
		HeightmapParams.WorldMax.X, HeightmapParams.WorldMax.Y, HeightmapParams.WorldMax.Z);
}

void FGPUCollisionManager::AddHeightmapCollisionPass(
	FRDGBuilder& GraphBuilder,
	const FSimulationSpatialData& SpatialData,
	int32 ParticleCount,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if heightmap collision is not enabled or no valid data
	if (!HeightmapParams.bEnabled || !bHeightmapDataValid || !HeightmapTextureRHI.IsValid())
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FHeightmapCollisionCS> ComputeShader(ShaderMap);

	// Register external texture with RDG
	FRDGTextureRef HeightmapTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(HeightmapTextureRHI, TEXT("HeightmapTexture")));
	FRDGTextureSRVRef HeightmapSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(HeightmapTexture));

	FHeightmapCollisionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHeightmapCollisionCS::FParameters>();
	// Bind SOA buffers
	PassParameters->Positions = GraphBuilder.CreateUAV(SpatialData.SoA_Positions, PF_R32_FLOAT);
	PassParameters->PredictedPositions = GraphBuilder.CreateUAV(SpatialData.SoA_PredictedPositions, PF_R32_FLOAT);
	PassParameters->PackedVelocities = GraphBuilder.CreateUAV(SpatialData.SoA_PackedVelocities, PF_R32G32_UINT);  // B plan
	PassParameters->Flags = GraphBuilder.CreateUAV(SpatialData.SoA_Flags, PF_R32_UINT);
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = HeightmapParams.ParticleRadius > 0 ? HeightmapParams.ParticleRadius : Params.ParticleRadius;

	// Heightmap texture
	PassParameters->HeightmapTexture = HeightmapSRV;
	PassParameters->HeightmapSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// World space transform parameters
	PassParameters->WorldMin = HeightmapParams.WorldMin;
	PassParameters->WorldMax = HeightmapParams.WorldMax;
	PassParameters->InvWorldExtent = HeightmapParams.InvWorldExtent;
	PassParameters->TextureWidth = HeightmapParams.TextureWidth;
	PassParameters->TextureHeight = HeightmapParams.TextureHeight;
	PassParameters->InvTextureWidth = HeightmapParams.InvTextureWidth;
	PassParameters->InvTextureHeight = HeightmapParams.InvTextureHeight;

	// Collision response parameters
	PassParameters->Friction = HeightmapParams.Friction;
	PassParameters->Restitution = HeightmapParams.Restitution;
	PassParameters->NormalStrength = HeightmapParams.NormalStrength;
	PassParameters->CollisionOffset = HeightmapParams.CollisionOffset;

	const uint32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FHeightmapCollisionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::HeightmapCollision(%dx%d)", HeightmapParams.TextureWidth, HeightmapParams.TextureHeight),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}
