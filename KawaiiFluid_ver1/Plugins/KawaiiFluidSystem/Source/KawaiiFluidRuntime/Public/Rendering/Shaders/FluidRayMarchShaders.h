// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphDefinitions.h"
#include "Core/KawaiiRenderParticle.h"

/**
 * @brief Shared parameter structure for Ray Marching SDF shaders
 *
 * Used by both vertex and pixel shaders for ray marching fluid rendering.
 * Supports metaball SDF, SSS (subsurface scattering), and refraction.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidRayMarchParameters, )
	//========================================
	// Particle Data (FKawaiiRenderParticle: Position, Velocity, Radius, Padding)
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FKawaiiRenderParticle>, RenderParticles)
	SHADER_PARAMETER(int32, ParticleCount)
	SHADER_PARAMETER(float, ParticleRadius)

	//========================================
	// SoA Particle Data (Memory bandwidth optimized)
	// - RenderPositions: 12B/particle (vs 32B AoS) - 62% reduction for SDF
	// - RenderVelocities: Motion blur only
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, RenderPositions)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, RenderVelocities)

	//========================================
	// Ray Marching Parameters
	//========================================
	SHADER_PARAMETER(float, SDFSmoothness)
	SHADER_PARAMETER(int32, MaxRayMarchSteps)
	SHADER_PARAMETER(float, RayMarchHitThreshold)
	SHADER_PARAMETER(float, RayMarchMaxDistance)

	//========================================
	// Appearance Parameters
	//========================================
	SHADER_PARAMETER(FLinearColor, FluidColor)
	SHADER_PARAMETER(float, FresnelStrength)
	SHADER_PARAMETER(float, RefractiveIndex)
	SHADER_PARAMETER(float, AbsorptionCoefficient)
	SHADER_PARAMETER(FLinearColor, AbsorptionColorCoefficients)
	SHADER_PARAMETER(float, SpecularStrength)
	SHADER_PARAMETER(float, SpecularRoughness)
	SHADER_PARAMETER(FLinearColor, EnvironmentLightColor)

	//========================================
	// Reflection Cubemap
	//========================================
	SHADER_PARAMETER_TEXTURE(TextureCube, ReflectionCubemap)
	SHADER_PARAMETER_SAMPLER(SamplerState, ReflectionCubemapSampler)
	SHADER_PARAMETER(float, ReflectionIntensity)
	SHADER_PARAMETER(float, ReflectionMipLevel)
	SHADER_PARAMETER(int, bUseReflectionCubemap)

	//========================================
	// SSS Parameters
	//========================================
	SHADER_PARAMETER(float, SSSIntensity)
	SHADER_PARAMETER(FLinearColor, SSSColor)

	//========================================
	// Scene Textures
	//========================================
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneTextureSampler)

	//========================================
	// SDF Volume (for optimized ray marching)
	//========================================
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFVolumeTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SDFVolumeSampler)
	SHADER_PARAMETER(FVector3f, SDFVolumeMin)      // Used when USE_GPU_BOUNDS=0
	SHADER_PARAMETER(FVector3f, SDFVolumeMax)      // Used when USE_GPU_BOUNDS=0
	SHADER_PARAMETER(FIntVector, SDFVolumeResolution)

	//========================================
	// GPU Bounds Buffer (for GPU mode - eliminates CPU readback)
	// When USE_GPU_BOUNDS=1, bounds are read from this buffer instead of uniforms
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, BoundsBuffer)  // [0]=Min, [1]=Max

	//========================================
	// Spatial Hash (for hybrid mode: SDF Volume + Spatial Hash)
	// HYBRID: SDF Volume for 90% fast approach, Spatial Hash for 10% precise final
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector2>, CellData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SpatialHashParticleIndices)
	SHADER_PARAMETER(float, SpatialHashCellSize)

	//========================================
	// Anisotropy Buffers (for ellipsoid rendering)
	// Each float4 = (axis.xyz, scale.w)
	// Precomputed by FluidAnisotropyCompute.usf
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, AnisotropyAxis1)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, AnisotropyAxis2)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, AnisotropyAxis3)

	//========================================
	// SceneDepth UV Mapping
	//========================================
	SHADER_PARAMETER(FVector2f, SceneViewRect)
	SHADER_PARAMETER(FVector2f, SceneTextureSize)

	//========================================
	// View Matrices
	//========================================
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER(FMatrix44f, InverseViewMatrix)
	SHADER_PARAMETER(FMatrix44f, InverseProjectionMatrix)
	SHADER_PARAMETER(FMatrix44f, ViewMatrix)
	SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
	SHADER_PARAMETER(FVector2f, ViewportSize)

	// Light parameters (DirectionalLightDirection, DirectionalLightColor)
	// are accessed directly from View uniform buffer in shader

	//========================================
	// Render Target
	//========================================
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

/**
 * @brief Vertex shader for Ray Marching (fullscreen triangle)
 *
 * Generates a fullscreen triangle for ray marching in pixel shader.
 */
class FFluidRayMarchVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidRayMarchVS);
	SHADER_USE_PARAMETER_STRUCT(FFluidRayMarchVS, FGlobalShader);

	using FParameters = FFluidRayMarchParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * @brief Shader permutation dimension for SDF Volume optimization
 */
class FUseSDFVolumeDim : SHADER_PERMUTATION_BOOL("USE_SDF_VOLUME");

/**
 * @brief Shader permutation dimension for Spatial Hash acceleration
 * When enabled, uses O(k) spatial hash lookup instead of O(N) particle iteration
 */
class FUseSpatialHashDim : SHADER_PERMUTATION_BOOL("USE_SPATIAL_HASH");

/**
 * @brief Shader permutation dimension for Depth output (MRT for shadow projection)
 * When enabled, outputs fluid depth to RenderTarget[1] for VSM shadow generation.
 */
class FOutputDepthDim : SHADER_PERMUTATION_BOOL("OUTPUT_DEPTH");

/**
 * @brief Shader permutation dimension for GPU Bounds buffer
 * When enabled, reads SDFVolumeMin/Max from BoundsBuffer instead of uniforms.
 * This eliminates CPU readback latency in GPU simulation mode.
 */
class FUseGPUBoundsDim : SHADER_PERMUTATION_BOOL("USE_GPU_BOUNDS");

/**
 * @brief Shader permutation dimension for SoA (Structure of Arrays) buffer layout
 * When enabled, uses separate Position buffer (12B) instead of RenderParticles (32B).
 * Provides 62% memory bandwidth reduction for SDF evaluation.
 */
class FUseSoABuffersDim : SHADER_PERMUTATION_BOOL("USE_SOA_BUFFERS");

/**
 * @brief Shader permutation dimension for Anisotropic rendering
 * When enabled, uses ellipsoid SDF (precomputed anisotropy matrices) instead of sphere SDF.
 * Produces "baduk stone" shaped particles that align with fluid flow direction.
 * Requires AnisotropyAxis1/2/3 buffers from FluidAnisotropyCompute pass.
 */
class FUseAnisotropyDim : SHADER_PERMUTATION_BOOL("USE_ANISOTROPY");

/**
 * @brief Pixel shader for Ray Marching SDF fluid rendering
 *
 * Performs ray marching through metaball SDF field to render
 * smooth fluid surfaces with:
 * - Fresnel reflection
 * - Subsurface scattering (SSS) for jelly effect
 * - Refraction
 * - Specular highlights
 * - Beer's Law absorption
 *
 * Permutation modes:
 * - USE_SDF_VOLUME=0: Original O(N) particle iteration (slowest, legacy)
 * - USE_SDF_VOLUME=1, USE_SPATIAL_HASH=0: O(1) volume texture sampling (fast)
 * - USE_SDF_VOLUME=1, USE_SPATIAL_HASH=1: HYBRID mode - SDF Volume for 90%
 *   fast ray approach + Spatial Hash for 10% precise final evaluation (best quality)
 */
class FFluidRayMarchPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidRayMarchPS);
	SHADER_USE_PARAMETER_STRUCT(FFluidRayMarchPS, FGlobalShader);

	using FParameters = FFluidRayMarchParameters;
	using FPermutationDomain = TShaderPermutationDomain<FUseSDFVolumeDim, FUseSpatialHashDim, FOutputDepthDim, FUseGPUBoundsDim, FUseSoABuffersDim, FUseAnisotropyDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// Add any defines needed for the shader
		OutEnvironment.SetDefine(TEXT("RAY_MARCH_SDF"), 1);
	}
};

//=============================================================================
// Upscale Shader (for Half Resolution rendering)
//=============================================================================

BEGIN_SHADER_PARAMETER_STRUCT(FFluidUpscaleParameters, )
	// Scaled-res fluid color texture
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)

	// Scaled-res fluid depth texture
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FluidDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, DepthSampler)

	// Full-res scene depth for depth-aware upsampling
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)

	// Resolution info
	SHADER_PARAMETER(FVector2f, InputSize)
	SHADER_PARAMETER(FVector2f, OutputSize)
	SHADER_PARAMETER(FVector2f, SceneDepthSize)

	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FFluidUpscaleVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidUpscaleVS);
	SHADER_USE_PARAMETER_STRUCT(FFluidUpscaleVS, FGlobalShader);

	using FParameters = FFluidUpscaleParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FFluidUpscalePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidUpscalePS);
	SHADER_USE_PARAMETER_STRUCT(FFluidUpscalePS, FGlobalShader);

	using FParameters = FFluidUpscaleParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
