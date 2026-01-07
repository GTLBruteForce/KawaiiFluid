// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/SDFVolumeManager.h"
#include "Rendering/Shaders/SDFBakeShaders.h"
#include "Rendering/Shaders/BoundsReductionShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

FSDFVolumeManager::FSDFVolumeManager()
{
}

FSDFVolumeManager::~FSDFVolumeManager()
{
}

FRDGBufferRef FSDFVolumeManager::CalculateGPUBounds(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticleBufferSRV,
	int32 ParticleCount,
	float ParticleRadius,
	float Margin,
	FRDGBufferSRVRef PositionBufferSRV)  // SoA: optional, nullptr = use AoS
{
	// Create output buffer for bounds: [0] = Min, [1] = Max
	FRDGBufferRef BoundsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), 2),
		TEXT("ParticleBoundsBuffer"));

	FRDGBufferUAVRef BoundsBufferUAV = GraphBuilder.CreateUAV(BoundsBuffer);

	// Determine if using SoA mode
	const bool bUseSoA = (PositionBufferSRV != nullptr);

	// Setup compute shader parameters
	FBoundsReductionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundsReductionCS::FParameters>();
	if (bUseSoA)
	{
		PassParameters->RenderPositions = PositionBufferSRV;
	}
	else
	{
		PassParameters->RenderParticles = ParticleBufferSRV;
	}
	PassParameters->ParticleCount = static_cast<uint32>(ParticleCount);
	PassParameters->ParticleRadius = ParticleRadius;
	PassParameters->BoundsMargin = Margin;
	PassParameters->OutputBounds = BoundsBufferUAV;

	// Get compute shader with permutation
	FBoundsReductionCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FBoundsReductionUseSoADim>(bUseSoA);
	TShaderMapRef<FBoundsReductionCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

	// Dispatch single group of 256 threads (grid-stride loop handles all particles)
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("CalculateParticleBounds(%d particles, SoA=%d)", ParticleCount, bUseSoA ? 1 : 0),
		ERDGPassFlags::AsyncCompute | ERDGPassFlags::NeverCull,
		ComputeShader,
		PassParameters,
		FIntVector(1, 1, 1));  // Single group

	return BoundsBuffer;
}

FRDGTextureSRVRef FSDFVolumeManager::BakeSDFVolumeWithGPUBounds(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticleBufferSRV,
	int32 ParticleCount,
	float ParticleRadius,
	float SDFSmoothness,
	FRDGBufferRef BoundsBuffer,
	FRDGBufferSRVRef PositionBufferSRV,
	FRDGBufferSRVRef CellDataSRV,
	FRDGBufferSRVRef ParticleIndicesSRV,
	float SpatialHashCellSize)
{
	// Create 3D texture for SDF volume
	FRDGTextureDesc SDFVolumeDesc = FRDGTextureDesc::Create3D(
		FIntVector(VolumeResolution.X, VolumeResolution.Y, VolumeResolution.Z),
		PF_R16F,  // 16-bit float for SDF distance values
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef SDFVolumeTexture = GraphBuilder.CreateTexture(SDFVolumeDesc, TEXT("SDFVolumeTexture"));
	FRDGTextureUAVRef SDFVolumeUAV = GraphBuilder.CreateUAV(SDFVolumeTexture);

	// Create SRV for bounds buffer to read in SDF bake shader
	FRDGBufferSRVRef BoundsBufferSRV = GraphBuilder.CreateSRV(BoundsBuffer);

	// Determine if using SoA mode
	const bool bUseSoA = (PositionBufferSRV != nullptr);

	// Use cached GPU bounds if available
	FVector3f VolumeMin = LastGPUBoundsMin;
	FVector3f VolumeMax = LastGPUBoundsMax;

	// Fallback to large default bounds if no cached bounds
	if (!bHasValidGPUBounds)
	{
		VolumeMin = FVector3f(-1000.0f, -1000.0f, -1000.0f);
		VolumeMax = FVector3f(1000.0f, 1000.0f, 1000.0f);
	}

	// Cache bounds for shader parameters
	CachedVolumeMin = VolumeMin;
	CachedVolumeMax = VolumeMax;

	FSDFBakeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSDFBakeCS::FParameters>();
	if (bUseSoA)
	{
		PassParameters->RenderPositions = PositionBufferSRV;
	}
	else
	{
		PassParameters->RenderParticles = ParticleBufferSRV;
	}
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;
	PassParameters->SDFSmoothness = SDFSmoothness;
	PassParameters->VolumeMin = VolumeMin;
	PassParameters->VolumeMax = VolumeMax;
	PassParameters->VolumeResolution = VolumeResolution;
	PassParameters->SDFVolume = SDFVolumeUAV;

	// Spatial Hash parameters
	PassParameters->CellData = CellDataSRV;
	PassParameters->SpatialHashParticleIndices = ParticleIndicesSRV;
	PassParameters->SpatialHashCellSize = SpatialHashCellSize;

	// Get compute shader with permutation
	FSDFBakeCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FSDFBakeUseSoADim>(bUseSoA);
	PermutationVector.Set<FSDFBakeUseSpatialHashDim>(SpatialHashCellSize > 0.0f);
	TShaderMapRef<FSDFBakeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

	// Calculate dispatch group counts
	const int32 ThreadGroupSize = FSDFBakeCS::ThreadGroupSize;
	FIntVector GroupCount(
		FMath::DivideAndRoundUp(VolumeResolution.X, ThreadGroupSize),
		FMath::DivideAndRoundUp(VolumeResolution.Y, ThreadGroupSize),
		FMath::DivideAndRoundUp(VolumeResolution.Z, ThreadGroupSize));

	// Add compute pass (must run after bounds calculation)
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SDFBake_WithGPUBounds(%dx%dx%d, SoA=%d, Hash=%d)", VolumeResolution.X, VolumeResolution.Y, VolumeResolution.Z, bUseSoA ? 1 : 0, CellDataSRV != nullptr ? 1 : 0),
		ERDGPassFlags::AsyncCompute | ERDGPassFlags::NeverCull,
		ComputeShader,
		PassParameters,
		GroupCount);

	// Create and return SRV for ray marching
	return GraphBuilder.CreateSRV(FRDGTextureSRVDesc(SDFVolumeTexture));
}

FRDGTextureSRVRef FSDFVolumeManager::BakeSDFVolumeWithGPUBoundsDirect(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticleBufferSRV,
	int32 ParticleCount,
	float ParticleRadius,
	float SDFSmoothness,
	FRDGBufferSRVRef BoundsBufferSRV,
	FRDGBufferSRVRef PositionBufferSRV,
	FRDGBufferSRVRef CellDataSRV,
	FRDGBufferSRVRef ParticleIndicesSRV,
	float SpatialHashCellSize)
{
	// Create 3D texture for SDF volume
	FRDGTextureDesc SDFVolumeDesc = FRDGTextureDesc::Create3D(
		FIntVector(VolumeResolution.X, VolumeResolution.Y, VolumeResolution.Z),
		PF_R16F,  // 16-bit float for SDF distance values
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef SDFVolumeTexture = GraphBuilder.CreateTexture(SDFVolumeDesc, TEXT("SDFVolumeTexture"));
	FRDGTextureUAVRef SDFVolumeUAV = GraphBuilder.CreateUAV(SDFVolumeTexture);

	// Determine if using SoA mode
	const bool bUseSoA = (PositionBufferSRV != nullptr);

	// Use the new shader that reads bounds from buffer
	FSDFBakeWithGPUBoundsCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FSDFBakeWithGPUBoundsCS::FParameters>();
	if (bUseSoA)
	{
		PassParameters->RenderPositions = PositionBufferSRV;
	}
	else
	{
		PassParameters->RenderParticles = ParticleBufferSRV;
	}
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;
	PassParameters->SDFSmoothness = SDFSmoothness;
	PassParameters->BoundsBuffer = BoundsBufferSRV;
	PassParameters->VolumeResolution = VolumeResolution;
	PassParameters->SDFVolume = SDFVolumeUAV;

	// Spatial Hash parameters
	PassParameters->CellData = CellDataSRV;
	PassParameters->SpatialHashParticleIndices = ParticleIndicesSRV;
	PassParameters->SpatialHashCellSize = SpatialHashCellSize;

	// Get compute shader with permutation
	FSDFBakeWithGPUBoundsCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FSDFBakeUseSoADim>(bUseSoA);
	PermutationVector.Set<FSDFBakeUseSpatialHashDim>(SpatialHashCellSize > 0.0f);
	TShaderMapRef<FSDFBakeWithGPUBoundsCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

	// Calculate dispatch group counts
	const int32 ThreadGroupSize = FSDFBakeWithGPUBoundsCS::ThreadGroupSize;
	FIntVector GroupCount(
		FMath::DivideAndRoundUp(VolumeResolution.X, ThreadGroupSize),
		FMath::DivideAndRoundUp(VolumeResolution.Y, ThreadGroupSize),
		FMath::DivideAndRoundUp(VolumeResolution.Z, ThreadGroupSize));

	// Add compute pass - must run after bounds buffer is written
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SDFBake_GPUBoundsDirect(%dx%dx%d, SoA=%d, Hash=%d)", VolumeResolution.X, VolumeResolution.Y, VolumeResolution.Z, bUseSoA ? 1 : 0, CellDataSRV != nullptr ? 1 : 0),
		ERDGPassFlags::AsyncCompute | ERDGPassFlags::NeverCull,
		ComputeShader,
		PassParameters,
		GroupCount);

	// Create and return SRV for ray marching
	return GraphBuilder.CreateSRV(FRDGTextureSRVDesc(SDFVolumeTexture));
}

FRDGTextureSRVRef FSDFVolumeManager::BakeSDFVolume(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticleBufferSRV,
	int32 ParticleCount,
	float ParticleRadius,
	float SDFSmoothness,
	const FVector3f& VolumeMin,
	const FVector3f& VolumeMax,
	FRDGBufferSRVRef PositionBufferSRV,
	FRDGBufferSRVRef CellDataSRV,
	FRDGBufferSRVRef ParticleIndicesSRV,
	float SpatialHashCellSize)
{
	// Cache volume bounds
	CachedVolumeMin = VolumeMin;
	CachedVolumeMax = VolumeMax;

	// Determine if using SoA mode
	const bool bUseSoA = (PositionBufferSRV != nullptr);

	// Create 3D texture for SDF volume
	FRDGTextureDesc SDFVolumeDesc = FRDGTextureDesc::Create3D(
		FIntVector(VolumeResolution.X, VolumeResolution.Y, VolumeResolution.Z),
		PF_R16F,  // 16-bit float for SDF distance values
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef SDFVolumeTexture = GraphBuilder.CreateTexture(SDFVolumeDesc, TEXT("SDFVolumeTexture"));
	FRDGTextureUAVRef SDFVolumeUAV = GraphBuilder.CreateUAV(SDFVolumeTexture);

	// Setup compute shader parameters
	FSDFBakeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSDFBakeCS::FParameters>();
	if (bUseSoA)
	{
		PassParameters->RenderPositions = PositionBufferSRV;
	}
	else
	{
		PassParameters->RenderParticles = ParticleBufferSRV;
	}
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;
	PassParameters->SDFSmoothness = SDFSmoothness;
	PassParameters->VolumeMin = VolumeMin;
	PassParameters->VolumeMax = VolumeMax;
	PassParameters->VolumeResolution = VolumeResolution;
	PassParameters->SDFVolume = SDFVolumeUAV;

	// Spatial Hash parameters
	PassParameters->CellData = CellDataSRV;
	PassParameters->SpatialHashParticleIndices = ParticleIndicesSRV;
	PassParameters->SpatialHashCellSize = SpatialHashCellSize;

	// Get compute shader with permutation
	FSDFBakeCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FSDFBakeUseSoADim>(bUseSoA);
	PermutationVector.Set<FSDFBakeUseSpatialHashDim>(SpatialHashCellSize > 0.0f);
	TShaderMapRef<FSDFBakeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

	// Calculate dispatch group counts
	const int32 ThreadGroupSize = FSDFBakeCS::ThreadGroupSize;
	FIntVector GroupCount(
		FMath::DivideAndRoundUp(VolumeResolution.X, ThreadGroupSize),
		FMath::DivideAndRoundUp(VolumeResolution.Y, ThreadGroupSize),
		FMath::DivideAndRoundUp(VolumeResolution.Z, ThreadGroupSize));

	// Add compute pass (Async Compute for parallel execution with graphics)
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SDFBake_Async(%dx%dx%d, SoA=%d, Hash=%d)", VolumeResolution.X, VolumeResolution.Y, VolumeResolution.Z, bUseSoA ? 1 : 0, CellDataSRV != nullptr ? 1 : 0),
		ERDGPassFlags::AsyncCompute | ERDGPassFlags::NeverCull,
		ComputeShader,
		PassParameters,
		GroupCount);

	// Create and return SRV for ray marching
	return GraphBuilder.CreateSRV(FRDGTextureSRVDesc(SDFVolumeTexture));
}

void CalculateParticleBoundingBox(
	const TArray<FVector3f>& Particles,
	float ParticleRadius,
	float Margin,
	FVector3f& OutMin,
	FVector3f& OutMax)
{
	if (Particles.Num() == 0)
	{
		OutMin = FVector3f::ZeroVector;
		OutMax = FVector3f::ZeroVector;
		return;
	}

	// Initialize with first particle
	OutMin = Particles[0];
	OutMax = Particles[0];

	// Find min/max across all particles
	for (const FVector3f& Pos : Particles)
	{
		OutMin.X = FMath::Min(OutMin.X, Pos.X);
		OutMin.Y = FMath::Min(OutMin.Y, Pos.Y);
		OutMin.Z = FMath::Min(OutMin.Z, Pos.Z);

		OutMax.X = FMath::Max(OutMax.X, Pos.X);
		OutMax.Y = FMath::Max(OutMax.Y, Pos.Y);
		OutMax.Z = FMath::Max(OutMax.Z, Pos.Z);
	}

	// Expand by particle radius and margin
	float Expansion = ParticleRadius + Margin;
	OutMin -= FVector3f(Expansion);
	OutMax += FVector3f(Expansion);
}
