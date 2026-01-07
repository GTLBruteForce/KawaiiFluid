// Copyright KawaiiFluid Team. All Rights Reserved.

#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "GlobalShader.h"

//=============================================================================
// Shader Implementations
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FPredictPositionsCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidPredictPositions.usf",
	"PredictPositionsCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FComputeDensityCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidComputeDensity.usf",
	"ComputeDensityCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FSolvePressureCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidSolvePressure.usf",
	"SolvePressureCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FApplyViscosityCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidApplyViscosity.usf",
	"ApplyViscosityCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FApplyCohesionCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidApplyCohesion.usf",
	"ApplyCohesionCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FBoundsCollisionCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidBoundsCollision.usf",
	"BoundsCollisionCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FDistanceFieldCollisionCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidDistanceFieldCollision.usf",
	"DistanceFieldCollisionCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FPrimitiveCollisionCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidPrimitiveCollision.usf",
	"PrimitiveCollisionCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FFinalizePositionsCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidFinalizePositions.usf",
	"FinalizePositionsCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FExtractPositionsCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidExtractPositions.usf",
	"ExtractPositionsCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FExtractRenderDataCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidExtractRenderData.usf",
	"ExtractRenderDataCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FExtractRenderDataWithBoundsCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidExtractRenderDataWithBounds.usf",
	"ExtractRenderDataWithBoundsCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FExtractRenderDataSoACS,
	"/Plugin/KawaiiFluidSystem/Private/FluidExtractRenderData.usf",
	"ExtractRenderDataSoACS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FCopyParticlesCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidCopyParticles.usf",
	"CopyParticlesCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FSpawnParticlesCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidSpawnParticles.usf",
	"SpawnParticlesCS", SF_Compute);

//=============================================================================
// Stream Compaction Shaders (Phase 2 - Per-Polygon Collision)
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FAABBMarkCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidAABBMark.usf",
	"AABBMarkCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FPrefixSumBlockCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidPrefixSum.usf",
	"PrefixSumBlockCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FScanBlockSumsCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidPrefixSum.usf",
	"ScanBlockSumsCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FAddBlockOffsetsCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidPrefixSum.usf",
	"AddBlockOffsetsCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FCompactCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidCompact.usf",
	"CompactCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FWriteTotalCountCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidCompact.usf",
	"WriteTotalCountCS", SF_Compute);

//=============================================================================
// Per-Polygon Collision Correction Shader
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FApplyCorrectionsCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidApplyCorrections.usf",
	"ApplyCorrectionsCS", SF_Compute);

//=============================================================================
// Attachment Updates Shader
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FApplyAttachmentUpdatesCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidApplyAttachmentUpdates.usf",
	"ApplyAttachmentUpdatesCS", SF_Compute);

//=============================================================================
// GPU Adhesion Shaders (Bone-based attachment)
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FAdhesionCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidAdhesion.usf",
	"AdhesionCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FUpdateAttachedPositionsCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidAdhesion.usf",
	"UpdateAttachedPositionsCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FClearDetachedFlagCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidAdhesion.usf",
	"ClearDetachedFlagCS", SF_Compute);

//=============================================================================
// Pass Builder Implementation
//=============================================================================

void FGPUFluidSimulatorPassBuilder::AddPredictPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	int32 ParticleCount,
	float DeltaTime,
	const FVector3f& Gravity,
	const FVector3f& ExternalForce)
{
	if (ParticleCount <= 0 || !ParticlesUAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FPredictPositionsCS> ComputeShader(GlobalShaderMap);

	FPredictPositionsCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FPredictPositionsCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->DeltaTime = DeltaTime;
	PassParameters->Gravity = Gravity;
	PassParameters->ExternalForce = ExternalForce;

	const int32 ThreadGroupSize = FPredictPositionsCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PredictPositions(%d)", ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddComputeDensityPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef CellCountsSRV,
	FRDGBufferSRVRef ParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	if (Params.ParticleCount <= 0 || !ParticlesUAV || !CellCountsSRV || !ParticleIndicesSRV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FComputeDensityCS> ComputeShader(GlobalShaderMap);

	FComputeDensityCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FComputeDensityCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->CellCounts = CellCountsSRV;
	PassParameters->ParticleIndices = ParticleIndicesSRV;
	PassParameters->ParticleCount = Params.ParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Compliance = Params.Compliance;
	PassParameters->DeltaTimeSq = Params.DeltaTimeSq;

	const int32 ThreadGroupSize = FComputeDensityCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(Params.ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ComputeDensity(%d)", Params.ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddSolvePressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef CellCountsSRV,
	FRDGBufferSRVRef ParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	if (Params.ParticleCount <= 0 || !ParticlesUAV || !CellCountsSRV || !ParticleIndicesSRV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSolvePressureCS> ComputeShader(GlobalShaderMap);

	FSolvePressureCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FSolvePressureCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->CellCounts = CellCountsSRV;
	PassParameters->ParticleIndices = ParticleIndicesSRV;
	PassParameters->ParticleCount = Params.ParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;

	const int32 ThreadGroupSize = FSolvePressureCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(Params.ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SolvePressure(%d)", Params.ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddApplyViscosityPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef CellCountsSRV,
	FRDGBufferSRVRef ParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	if (Params.ParticleCount <= 0 || !ParticlesUAV || !CellCountsSRV || !ParticleIndicesSRV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyViscosityCS> ComputeShader(GlobalShaderMap);

	FApplyViscosityCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FApplyViscosityCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->CellCounts = CellCountsSRV;
	PassParameters->ParticleIndices = ParticleIndicesSRV;
	PassParameters->ParticleCount = Params.ParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->ViscosityCoefficient = Params.ViscosityCoefficient;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->CellSize = Params.CellSize;

	const int32 ThreadGroupSize = FApplyViscosityCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(Params.ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyViscosity(%d)", Params.ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddApplyCohesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef CellCountsSRV,
	FRDGBufferSRVRef ParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	if (Params.ParticleCount <= 0 || !ParticlesUAV || !CellCountsSRV || !ParticleIndicesSRV)
	{
		return;
	}

	// Skip if cohesion is disabled
	if (Params.CohesionStrength <= 0.0f)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyCohesionCS> ComputeShader(GlobalShaderMap);

	FApplyCohesionCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FApplyCohesionCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->CellCounts = CellCountsSRV;
	PassParameters->ParticleIndices = ParticleIndicesSRV;
	PassParameters->ParticleCount = Params.ParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->CohesionStrength = Params.CohesionStrength;
	PassParameters->CellSize = Params.CellSize;

	const int32 ThreadGroupSize = FApplyCohesionCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(Params.ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyCohesion(%d)", Params.ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddBoundsCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (Params.ParticleCount <= 0 || !ParticlesUAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FBoundsCollisionCS> ComputeShader(GlobalShaderMap);

	FBoundsCollisionCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FBoundsCollisionCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = Params.ParticleCount;
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

	const int32 ThreadGroupSize = FBoundsCollisionCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(Params.ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::BoundsCollision(%d)", Params.ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddDistanceFieldCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGTextureSRVRef GlobalDistanceFieldSRV,
	const FGPUDistanceFieldCollisionParams& DFParams,
	int32 ParticleCount)
{
	if (ParticleCount <= 0 || !ParticlesUAV || !DFParams.bEnabled)
	{
		return;
	}

	// Skip if no distance field texture available
	if (!GlobalDistanceFieldSRV)
	{
		UE_LOG(LogTemp, Verbose, TEXT("GPUFluid::DistanceFieldCollision skipped - no GDF texture"));
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FDistanceFieldCollisionCS> ComputeShader(GlobalShaderMap);

	FDistanceFieldCollisionCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FDistanceFieldCollisionCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = DFParams.ParticleRadius;

	// Distance Field Volume Parameters
	PassParameters->GDFVolumeCenter = DFParams.VolumeCenter;
	PassParameters->GDFVolumeExtent = DFParams.VolumeExtent;
	PassParameters->GDFVoxelSize = FVector3f(DFParams.VoxelSize);
	PassParameters->GDFMaxDistance = DFParams.MaxDistance;

	// Collision Response Parameters
	PassParameters->DFCollisionRestitution = DFParams.Restitution;
	PassParameters->DFCollisionFriction = DFParams.Friction;
	PassParameters->DFCollisionThreshold = DFParams.CollisionThreshold;

	// Global Distance Field Texture
	PassParameters->GlobalDistanceFieldTexture = GlobalDistanceFieldSRV;
	PassParameters->GlobalDistanceFieldSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	const int32 ThreadGroupSize = FDistanceFieldCollisionCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::DistanceFieldCollision(%d)", ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddPrimitiveCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef SpheresSRV,
	FRDGBufferSRVRef CapsulesSRV,
	FRDGBufferSRVRef BoxesSRV,
	FRDGBufferSRVRef ConvexesSRV,
	FRDGBufferSRVRef ConvexPlanesSRV,
	int32 SphereCount,
	int32 CapsuleCount,
	int32 BoxCount,
	int32 ConvexCount,
	int32 ParticleCount,
	float ParticleRadius,
	float CollisionThreshold)
{
	if (ParticleCount <= 0 || !ParticlesUAV)
	{
		return;
	}

	// Skip if no primitives
	if (SphereCount == 0 && CapsuleCount == 0 && BoxCount == 0 && ConvexCount == 0)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FPrimitiveCollisionCS> ComputeShader(GlobalShaderMap);

	FPrimitiveCollisionCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FPrimitiveCollisionCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;
	PassParameters->CollisionThreshold = CollisionThreshold;

	PassParameters->CollisionSpheres = SpheresSRV;
	PassParameters->SphereCount = SphereCount;

	PassParameters->CollisionCapsules = CapsulesSRV;
	PassParameters->CapsuleCount = CapsuleCount;

	PassParameters->CollisionBoxes = BoxesSRV;
	PassParameters->BoxCount = BoxCount;

	PassParameters->CollisionConvexes = ConvexesSRV;
	PassParameters->ConvexCount = ConvexCount;

	PassParameters->ConvexPlanes = ConvexPlanesSRV;

	const int32 ThreadGroupSize = FPrimitiveCollisionCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PrimitiveCollision(%d particles, %d primitives)",
			ParticleCount, SphereCount + CapsuleCount + BoxCount + ConvexCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddFinalizePositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	int32 ParticleCount,
	float DeltaTime,
	float VelocityDamping,
	float MaxVelocity)
{
	if (ParticleCount <= 0 || !ParticlesUAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFinalizePositionsCS> ComputeShader(GlobalShaderMap);

	FFinalizePositionsCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFinalizePositionsCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->DeltaTime = DeltaTime;
	PassParameters->VelocityDamping = VelocityDamping;
	PassParameters->MaxVelocity = MaxVelocity;

	const int32 ThreadGroupSize = FFinalizePositionsCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::FinalizePositions(%d)", ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddExtractPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticlesSRV,
	FRDGBufferUAVRef PositionsUAV,
	int32 ParticleCount,
	bool bUsePredictedPosition)
{
	if (ParticleCount <= 0 || !ParticlesSRV || !PositionsUAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FExtractPositionsCS> ComputeShader(GlobalShaderMap);

	FExtractPositionsCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FExtractPositionsCS::FParameters>();

	PassParameters->Particles = ParticlesSRV;
	PassParameters->Positions = PositionsUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->bUsePredictedPosition = bUsePredictedPosition ? 1 : 0;

	const int32 ThreadGroupSize = FExtractPositionsCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ExtractPositions(%d)", ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddExtractRenderDataPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef PhysicsParticlesSRV,
	FRDGBufferUAVRef RenderParticlesUAV,
	int32 ParticleCount,
	float ParticleRadius)
{
	if (ParticleCount <= 0 || !PhysicsParticlesSRV || !RenderParticlesUAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FExtractRenderDataCS> ComputeShader(GlobalShaderMap);

	FExtractRenderDataCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FExtractRenderDataCS::FParameters>();

	PassParameters->PhysicsParticles = PhysicsParticlesSRV;
	PassParameters->RenderParticles = RenderParticlesUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;

	const int32 ThreadGroupSize = FExtractRenderDataCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ExtractRenderData(%d)", ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddExtractRenderDataWithBoundsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef PhysicsParticlesSRV,
	FRDGBufferUAVRef RenderParticlesUAV,
	FRDGBufferUAVRef BoundsBufferUAV,
	int32 ParticleCount,
	float ParticleRadius,
	float BoundsMargin)
{
	if (ParticleCount <= 0 || !PhysicsParticlesSRV || !RenderParticlesUAV || !BoundsBufferUAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FExtractRenderDataWithBoundsCS> ComputeShader(GlobalShaderMap);

	FExtractRenderDataWithBoundsCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FExtractRenderDataWithBoundsCS::FParameters>();

	PassParameters->PhysicsParticles = PhysicsParticlesSRV;
	PassParameters->RenderParticles = RenderParticlesUAV;
	PassParameters->OutputBounds = BoundsBufferUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;
	PassParameters->BoundsMargin = BoundsMargin;

	// Single group of 256 threads with grid-stride loop (same as BoundsReduction)
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ExtractRenderDataWithBounds(%d)", ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(1, 1, 1));
}

void FGPUFluidSimulatorPassBuilder::AddExtractRenderDataSoAPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef PhysicsParticlesSRV,
	FRDGBufferUAVRef RenderPositionsUAV,
	FRDGBufferUAVRef RenderVelocitiesUAV,
	int32 ParticleCount,
	float ParticleRadius)
{
	if (ParticleCount <= 0 || !PhysicsParticlesSRV || !RenderPositionsUAV || !RenderVelocitiesUAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FExtractRenderDataSoACS> ComputeShader(GlobalShaderMap);

	FExtractRenderDataSoACS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FExtractRenderDataSoACS::FParameters>();

	PassParameters->PhysicsParticles = PhysicsParticlesSRV;
	PassParameters->RenderPositions = RenderPositionsUAV;
	PassParameters->RenderVelocities = RenderVelocitiesUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;

	const int32 ThreadGroupSize = FExtractRenderDataSoACS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ExtractRenderDataSoA(%d)", ParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}
