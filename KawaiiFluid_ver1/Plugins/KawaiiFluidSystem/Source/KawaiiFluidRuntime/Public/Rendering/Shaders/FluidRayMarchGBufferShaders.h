// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphDefinitions.h"
#include "Core/KawaiiRenderParticle.h"

/**
 * @brief Parameter structure for Ray Marching SDF → G-Buffer output
 *
 * Ray marches through SDF field to find surface, then writes to G-Buffer MRT
 * for integration with Lumen/VSM deferred lighting pipeline.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidRayMarchGBufferParameters, )
	//========================================
	// Particle Data (FKawaiiRenderParticle: Position, Velocity, Radius, Padding)
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FKawaiiRenderParticle>, RenderParticles)
	SHADER_PARAMETER(int32, ParticleCount)
	SHADER_PARAMETER(float, ParticleRadius)

	//========================================
	// Ray Marching Parameters
	//========================================
	SHADER_PARAMETER(float, SDFSmoothness)
	SHADER_PARAMETER(int32, MaxRayMarchSteps)
	SHADER_PARAMETER(float, RayMarchHitThreshold)
	SHADER_PARAMETER(float, RayMarchMaxDistance)

	//========================================
	// Material Parameters (for G-Buffer)
	//========================================
	SHADER_PARAMETER(FVector3f, FluidBaseColor)
	SHADER_PARAMETER(float, Metallic)
	SHADER_PARAMETER(float, Roughness)
	SHADER_PARAMETER(float, AbsorptionCoefficient)

	//========================================
	// Scene Textures
	//========================================
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FluidSceneDepthTex)
	SHADER_PARAMETER_SAMPLER(SamplerState, FluidSceneTextureSampler)

	//========================================
	// SDF Volume (for optimized ray marching)
	//========================================
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFVolumeTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SDFVolumeSampler)
	SHADER_PARAMETER(FVector3f, SDFVolumeMin)
	SHADER_PARAMETER(FVector3f, SDFVolumeMax)
	SHADER_PARAMETER(FIntVector, SDFVolumeResolution)

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

	//========================================
	// Render Targets (G-Buffer MRT)
	//========================================
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

/**
 * @brief Vertex shader for Ray Marching G-Buffer (fullscreen triangle)
 */
class FFluidRayMarchGBufferVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidRayMarchGBufferVS);
	SHADER_USE_PARAMETER_STRUCT(FFluidRayMarchGBufferVS, FGlobalShader);

	using FParameters = FFluidRayMarchGBufferParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * @brief Shader permutation dimension for SDF Volume optimization
 */
class FUseSDFVolumeGBufferDim : SHADER_PERMUTATION_BOOL("USE_SDF_VOLUME");

/**
 * @brief Pixel shader for Ray Marching SDF → G-Buffer output
 *
 * Performs ray marching through SDF field, outputs:
 * - GBufferA: World Normal
 * - GBufferB: Metallic, Specular, Roughness, ShadingModelID
 * - GBufferC: BaseColor, AO
 * - GBufferD: Thickness (for transparency pass)
 * - Depth: Device Z
 *
 * After this pass, Lumen/VSM will light the surface automatically.
 * Then FluidTransparencyComposite applies refraction/transparency.
 */
class FFluidRayMarchGBufferPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidRayMarchGBufferPS);
	SHADER_USE_PARAMETER_STRUCT(FFluidRayMarchGBufferPS, FGlobalShader);

	using FParameters = FFluidRayMarchGBufferParameters;
	using FPermutationDomain = TShaderPermutationDomain<FUseSDFVolumeGBufferDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("RAY_MARCH_GBUFFER"), 1);
	}
};
