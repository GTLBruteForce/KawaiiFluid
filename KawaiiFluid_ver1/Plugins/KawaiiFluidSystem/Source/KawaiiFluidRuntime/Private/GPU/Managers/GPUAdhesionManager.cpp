// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "GPU/Managers/GPUAdhesionManager.h"
#include "GPU/Managers/GPUCollisionManager.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"

DEFINE_LOG_CATEGORY_STATIC(LogGPUAdhesionManager, Log, All);

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUAdhesionManager::FGPUAdhesionManager()
	: bIsInitialized(false)
{
}

FGPUAdhesionManager::~FGPUAdhesionManager()
{
	if (bIsInitialized)
	{
		Release();
	}
}

//=============================================================================
// Lifecycle
//=============================================================================

void FGPUAdhesionManager::Initialize()
{
	if (bIsInitialized)
	{
		UE_LOG(LogGPUAdhesionManager, Warning, TEXT("Already initialized"));
		return;
	}

	bIsInitialized = true;
	UE_LOG(LogGPUAdhesionManager, Log, TEXT("GPU Adhesion Manager initialized"));
}

void FGPUAdhesionManager::Release()
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&AdhesionLock);

	// Release persistent attachment buffer
	PersistentAttachmentBuffer.SafeRelease();
	AttachmentBufferSize = 0;

	bIsInitialized = false;
	UE_LOG(LogGPUAdhesionManager, Log, TEXT("GPU Adhesion Manager released"));
}

//=============================================================================
// Adhesion Pass (Create attachments to bone colliders)
//=============================================================================

void FGPUAdhesionManager::AddAdhesionPass(
	FRDGBuilder& GraphBuilder,
	const FSimulationSpatialData& SpatialData,
	FRDGBufferUAVRef AttachmentUAV,
	FGPUCollisionManager* CollisionManager,
	int32 CurrentParticleCount,
	const FGPUFluidSimulationParams& Params)
{
	if (!IsAdhesionEnabled() || !CollisionManager || !CollisionManager->AreBoneTransformsValid() || CollisionManager->GetCachedBoneTransforms().Num() == 0 || CurrentParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FAdhesionCS> ComputeShader(ShaderMap);

	// Upload bone transforms
	const TArray<FGPUBoneTransform>& BoneTransforms = CollisionManager->GetCachedBoneTransforms();
	FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidBoneTransforms"),
		sizeof(FGPUBoneTransform),
		BoneTransforms.Num(),
		BoneTransforms.GetData(),
		BoneTransforms.Num() * sizeof(FGPUBoneTransform),
		ERDGInitialDataFlags::NoCopy
	);
	FRDGBufferSRVRef BoneTransformsSRVLocal = GraphBuilder.CreateSRV(BoneTransformsBuffer);

	// Upload collision primitives for adhesion check
	FRDGBufferRef SpheresBuffer = nullptr;
	FRDGBufferRef CapsulesBuffer = nullptr;
	FRDGBufferRef BoxesBuffer = nullptr;
	FRDGBufferRef ConvexesBuffer = nullptr;
	FRDGBufferRef ConvexPlanesBuffer = nullptr;

	if (CollisionManager->GetCachedSpheres().Num() > 0)
	{
		SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionSpheres"),
			sizeof(FGPUCollisionSphere), CollisionManager->GetCachedSpheres().Num(),
			CollisionManager->GetCachedSpheres().GetData(), CollisionManager->GetCachedSpheres().Num() * sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CollisionManager->GetCachedCapsules().Num() > 0)
	{
		CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionCapsules"),
			sizeof(FGPUCollisionCapsule), CollisionManager->GetCachedCapsules().Num(),
			CollisionManager->GetCachedCapsules().GetData(), CollisionManager->GetCachedCapsules().Num() * sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CollisionManager->GetCachedBoxes().Num() > 0)
	{
		BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionBoxes"),
			sizeof(FGPUCollisionBox), CollisionManager->GetCachedBoxes().Num(),
			CollisionManager->GetCachedBoxes().GetData(), CollisionManager->GetCachedBoxes().Num() * sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CollisionManager->GetCachedConvexHeaders().Num() > 0)
	{
		ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionConvexes"),
			sizeof(FGPUCollisionConvex), CollisionManager->GetCachedConvexHeaders().Num(),
			CollisionManager->GetCachedConvexHeaders().GetData(), CollisionManager->GetCachedConvexHeaders().Num() * sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CollisionManager->GetCachedConvexPlanes().Num() > 0)
	{
		ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidConvexPlanes"),
			sizeof(FGPUConvexPlane), CollisionManager->GetCachedConvexPlanes().Num(),
			CollisionManager->GetCachedConvexPlanes().GetData(), CollisionManager->GetCachedConvexPlanes().Num() * sizeof(FGPUConvexPlane),
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
	// Bind SOA buffers
	PassParameters->Positions = GraphBuilder.CreateUAV(SpatialData.SoA_Positions, PF_R32_FLOAT);
	PassParameters->PredictedPositions = GraphBuilder.CreateUAV(SpatialData.SoA_PredictedPositions, PF_R32_FLOAT);
	PassParameters->PackedVelocities = GraphBuilder.CreateUAV(SpatialData.SoA_PackedVelocities, PF_R32G32_UINT);  // B plan
	PassParameters->Flags = GraphBuilder.CreateUAV(SpatialData.SoA_Flags, PF_R32_UINT);
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = Params.ParticleRadius;
	PassParameters->Attachments = AttachmentUAV;
	PassParameters->BoneTransforms = BoneTransformsSRVLocal;
	PassParameters->BoneCount = BoneTransforms.Num();
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CollisionManager->GetCachedSpheres().Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CollisionManager->GetCachedCapsules().Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CollisionManager->GetCachedBoxes().Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CollisionManager->GetCachedConvexHeaders().Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	PassParameters->AdhesionStrength = AdhesionParams.AdhesionStrength;
	PassParameters->AdhesionRadius = AdhesionParams.AdhesionRadius;
	PassParameters->DetachAccelThreshold = AdhesionParams.DetachAccelThreshold;
	PassParameters->DetachDistanceThreshold = AdhesionParams.DetachDistanceThreshold;
	PassParameters->ColliderContactOffset = AdhesionParams.ColliderContactOffset;
	PassParameters->BoneVelocityScale = AdhesionParams.BoneVelocityScale;
	PassParameters->SlidingFriction = AdhesionParams.SlidingFriction;
	PassParameters->CurrentTime = Params.CurrentTime;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->bEnableAdhesion = AdhesionParams.bEnableAdhesion;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FAdhesionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::Adhesion"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Update Attached Positions Pass (Move attached particles with bones)
//=============================================================================

void FGPUAdhesionManager::AddUpdateAttachedPositionsPass(
	FRDGBuilder& GraphBuilder,
	const FSimulationSpatialData& SpatialData,
	FRDGBufferUAVRef AttachmentUAV,
	FGPUCollisionManager* CollisionManager,
	int32 CurrentParticleCount,
	const FGPUFluidSimulationParams& Params)
{
	if (!IsAdhesionEnabled() || !CollisionManager || !CollisionManager->AreBoneTransformsValid() || CollisionManager->GetCachedBoneTransforms().Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FUpdateAttachedPositionsCS> ComputeShader(ShaderMap);

	// Upload bone transforms
	const TArray<FGPUBoneTransform>& BoneTransforms = CollisionManager->GetCachedBoneTransforms();
	FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidBoneTransformsUpdate"),
		sizeof(FGPUBoneTransform),
		BoneTransforms.Num(),
		BoneTransforms.GetData(),
		BoneTransforms.Num() * sizeof(FGPUBoneTransform),
		ERDGInitialDataFlags::NoCopy
	);
	FRDGBufferSRVRef BoneTransformsSRVLocal = GraphBuilder.CreateSRV(BoneTransformsBuffer);

	// Upload collision primitives for detachment distance check
	FRDGBufferRef SpheresBuffer = nullptr;
	FRDGBufferRef CapsulesBuffer = nullptr;
	FRDGBufferRef BoxesBuffer = nullptr;
	FRDGBufferRef ConvexesBuffer = nullptr;
	FRDGBufferRef ConvexPlanesBuffer = nullptr;

	if (CollisionManager->GetCachedSpheres().Num() > 0)
	{
		SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionSpheresUpdate"),
			sizeof(FGPUCollisionSphere), CollisionManager->GetCachedSpheres().Num(),
			CollisionManager->GetCachedSpheres().GetData(), CollisionManager->GetCachedSpheres().Num() * sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CollisionManager->GetCachedCapsules().Num() > 0)
	{
		CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionCapsulesUpdate"),
			sizeof(FGPUCollisionCapsule), CollisionManager->GetCachedCapsules().Num(),
			CollisionManager->GetCachedCapsules().GetData(), CollisionManager->GetCachedCapsules().Num() * sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CollisionManager->GetCachedBoxes().Num() > 0)
	{
		BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionBoxesUpdate"),
			sizeof(FGPUCollisionBox), CollisionManager->GetCachedBoxes().Num(),
			CollisionManager->GetCachedBoxes().GetData(), CollisionManager->GetCachedBoxes().Num() * sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CollisionManager->GetCachedConvexHeaders().Num() > 0)
	{
		ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionConvexesUpdate"),
			sizeof(FGPUCollisionConvex), CollisionManager->GetCachedConvexHeaders().Num(),
			CollisionManager->GetCachedConvexHeaders().GetData(), CollisionManager->GetCachedConvexHeaders().Num() * sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CollisionManager->GetCachedConvexPlanes().Num() > 0)
	{
		ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidConvexPlanesUpdate"),
			sizeof(FGPUConvexPlane), CollisionManager->GetCachedConvexPlanes().Num(),
			CollisionManager->GetCachedConvexPlanes().GetData(), CollisionManager->GetCachedConvexPlanes().Num() * sizeof(FGPUConvexPlane),
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
	// Bind SOA buffers
	PassParameters->Positions = GraphBuilder.CreateUAV(SpatialData.SoA_Positions, PF_R32_FLOAT);
	PassParameters->PredictedPositions = GraphBuilder.CreateUAV(SpatialData.SoA_PredictedPositions, PF_R32_FLOAT);
	PassParameters->PackedVelocities = GraphBuilder.CreateUAV(SpatialData.SoA_PackedVelocities, PF_R32G32_UINT);
	PassParameters->Flags = GraphBuilder.CreateUAV(SpatialData.SoA_Flags, PF_R32_UINT);
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->Attachments = AttachmentUAV;
	PassParameters->BoneTransforms = BoneTransformsSRVLocal;
	PassParameters->BoneCount = BoneTransforms.Num();
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CollisionManager->GetCachedSpheres().Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CollisionManager->GetCachedCapsules().Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CollisionManager->GetCachedBoxes().Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CollisionManager->GetCachedConvexHeaders().Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	PassParameters->DetachAccelThreshold = AdhesionParams.DetachAccelThreshold;
	PassParameters->DetachDistanceThreshold = AdhesionParams.DetachDistanceThreshold;
	PassParameters->ColliderContactOffset = AdhesionParams.ColliderContactOffset;
	PassParameters->BoneVelocityScale = AdhesionParams.BoneVelocityScale;
	PassParameters->SlidingFriction = AdhesionParams.SlidingFriction;
	PassParameters->DeltaTime = Params.DeltaTime;

	// Gravity sliding parameters
	PassParameters->Gravity = AdhesionParams.Gravity;
	PassParameters->GravitySlidingScale = AdhesionParams.GravitySlidingScale;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FUpdateAttachedPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::UpdateAttachedPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Clear Detached Flag Pass
//=============================================================================

void FGPUAdhesionManager::AddClearDetachedFlagPass(
	FRDGBuilder& GraphBuilder,
	const FSimulationSpatialData& SpatialData,
	int32 CurrentParticleCount)
{
	if (!IsAdhesionEnabled())
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FClearDetachedFlagCS> ComputeShader(ShaderMap);

	FClearDetachedFlagCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearDetachedFlagCS::FParameters>();
	PassParameters->Flags = GraphBuilder.CreateUAV(SpatialData.SoA_Flags, PF_R32_UINT);
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
// Stack Pressure Pass (Weight transfer from stacked attached particles)
//=============================================================================

void FGPUAdhesionManager::AddStackPressurePass(
	FRDGBuilder& GraphBuilder,
	const FSimulationSpatialData& SpatialData,
	FRDGBufferSRVRef InAttachmentSRV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	FGPUCollisionManager* CollisionManager,
	int32 CurrentParticleCount,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if stack pressure is disabled or no attachments
	if (Params.StackPressureScale <= 0.0f || !InAttachmentSRV)
	{
		return;
	}

	// Skip if no bone colliders (no attachments possible)
	if (!CollisionManager || !CollisionManager->AreBoneTransformsValid() || CollisionManager->GetCachedBoneTransforms().Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FStackPressureCS> ComputeShader(ShaderMap);

	// Create collision primitive buffers (same as Adhesion pass)
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(4, 1);

	FRDGBufferRef SpheresBuffer = CollisionManager->GetCachedSpheres().Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Spheres"),
			sizeof(FGPUCollisionSphere), CollisionManager->GetCachedSpheres().Num(),
			CollisionManager->GetCachedSpheres().GetData(), sizeof(FGPUCollisionSphere) * CollisionManager->GetCachedSpheres().Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheres"));

	FRDGBufferRef CapsulesBuffer = CollisionManager->GetCachedCapsules().Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Capsules"),
			sizeof(FGPUCollisionCapsule), CollisionManager->GetCachedCapsules().Num(),
			CollisionManager->GetCachedCapsules().GetData(), sizeof(FGPUCollisionCapsule) * CollisionManager->GetCachedCapsules().Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsules"));

	FRDGBufferRef BoxesBuffer = CollisionManager->GetCachedBoxes().Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Boxes"),
			sizeof(FGPUCollisionBox), CollisionManager->GetCachedBoxes().Num(),
			CollisionManager->GetCachedBoxes().GetData(), sizeof(FGPUCollisionBox) * CollisionManager->GetCachedBoxes().Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxes"));

	FRDGBufferRef ConvexesBuffer = CollisionManager->GetCachedConvexHeaders().Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Convexes"),
			sizeof(FGPUCollisionConvex), CollisionManager->GetCachedConvexHeaders().Num(),
			CollisionManager->GetCachedConvexHeaders().GetData(), sizeof(FGPUCollisionConvex) * CollisionManager->GetCachedConvexHeaders().Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexes"));

	FRDGBufferRef ConvexPlanesBuffer = CollisionManager->GetCachedConvexPlanes().Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_ConvexPlanes"),
			sizeof(FGPUConvexPlane), CollisionManager->GetCachedConvexPlanes().Num(),
			CollisionManager->GetCachedConvexPlanes().GetData(), sizeof(FGPUConvexPlane) * CollisionManager->GetCachedConvexPlanes().Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanes"));

	FStackPressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStackPressureCS::FParameters>();
	// Bind SOA buffers
	PassParameters->Positions = GraphBuilder.CreateSRV(SpatialData.SoA_Positions, PF_R32_FLOAT);
	PassParameters->PackedVelocities = GraphBuilder.CreateUAV(SpatialData.SoA_PackedVelocities, PF_R32G32_UINT);
	PassParameters->UniformParticleMass = Params.ParticleMass;
	PassParameters->Attachments = InAttachmentSRV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;

	// Collision primitives for surface normal calculation
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CollisionManager->GetCachedSpheres().Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CollisionManager->GetCachedCapsules().Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CollisionManager->GetCachedBoxes().Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CollisionManager->GetCachedConvexHeaders().Num();
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
