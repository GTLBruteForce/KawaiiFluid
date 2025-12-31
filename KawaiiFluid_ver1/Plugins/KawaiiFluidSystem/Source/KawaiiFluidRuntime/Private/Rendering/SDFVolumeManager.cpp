// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/SDFVolumeManager.h"
#include "Rendering/Shaders/SDFBakeShaders.h"
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

FRDGTextureSRVRef FSDFVolumeManager::BakeSDFVolume(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticleBufferSRV,
	int32 ParticleCount,
	float ParticleRadius,
	float SDFSmoothness,
	const FVector3f& VolumeMin,
	const FVector3f& VolumeMax)
{
	// Cache volume bounds
	CachedVolumeMin = VolumeMin;
	CachedVolumeMax = VolumeMax;

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
	PassParameters->ParticlePositions = ParticleBufferSRV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;
	PassParameters->SDFSmoothness = SDFSmoothness;
	PassParameters->VolumeMin = VolumeMin;
	PassParameters->VolumeMax = VolumeMax;
	PassParameters->VolumeResolution = VolumeResolution;
	PassParameters->SDFVolume = SDFVolumeUAV;

	// Get compute shader
	TShaderMapRef<FSDFBakeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch group counts
	const int32 ThreadGroupSize = FSDFBakeCS::ThreadGroupSize;
	FIntVector GroupCount(
		FMath::DivideAndRoundUp(VolumeResolution.X, ThreadGroupSize),
		FMath::DivideAndRoundUp(VolumeResolution.Y, ThreadGroupSize),
		FMath::DivideAndRoundUp(VolumeResolution.Z, ThreadGroupSize));

	// Add compute pass (Async Compute for parallel execution with graphics)
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SDFBake_Async(%dx%dx%d)", VolumeResolution.X, VolumeResolution.Y, VolumeResolution.Z),
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
