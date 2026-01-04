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
	float Margin)
{
	// Create output buffer for bounds: [0] = Min, [1] = Max
	FRDGBufferRef BoundsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), 2),
		TEXT("ParticleBoundsBuffer"));

	FRDGBufferUAVRef BoundsBufferUAV = GraphBuilder.CreateUAV(BoundsBuffer);

	// Setup compute shader parameters
	FBoundsReductionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundsReductionCS::FParameters>();
	PassParameters->RenderParticles = ParticleBufferSRV;
	PassParameters->ParticleCount = static_cast<uint32>(ParticleCount);
	PassParameters->ParticleRadius = ParticleRadius;
	PassParameters->BoundsMargin = Margin;
	PassParameters->OutputBounds = BoundsBufferUAV;

	// Get compute shader
	TShaderMapRef<FBoundsReductionCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Dispatch single group of 256 threads (grid-stride loop handles all particles)
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("CalculateParticleBounds(%d particles)", ParticleCount),
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
	FRDGBufferRef BoundsBuffer)
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

	// Setup compute shader parameters
	// Note: We need a modified version of SDFBake shader that reads bounds from buffer
	// For now, we use the standard BakeSDFVolume with cached bounds from previous frame
	// This works because GPU bounds are calculated and cached for next frame use

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
	PassParameters->RenderParticles = ParticleBufferSRV;
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

	// Add compute pass (must run after bounds calculation)
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SDFBake_WithGPUBounds(%dx%dx%d)", VolumeResolution.X, VolumeResolution.Y, VolumeResolution.Z),
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
	PassParameters->RenderParticles = ParticleBufferSRV;
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
