// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphDefinitions.h"

/**
 * @brief Shared parameter structure for Ray Marching SDF shaders
 *
 * Used by both vertex and pixel shaders for ray marching fluid rendering.
 * Supports metaball SDF, SSS (subsurface scattering), and refraction.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidRayMarchParameters, )
	//========================================
	// Particle Data
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, ParticlePositions)
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
	// Appearance Parameters
	//========================================
	SHADER_PARAMETER(FLinearColor, FluidColor)
	SHADER_PARAMETER(float, FresnelStrength)
	SHADER_PARAMETER(float, RefractiveIndex)
	SHADER_PARAMETER(float, AbsorptionCoefficient)
	SHADER_PARAMETER(float, SpecularStrength)
	SHADER_PARAMETER(float, SpecularRoughness)
	SHADER_PARAMETER(FLinearColor, EnvironmentLightColor)

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
 * @brief Pixel shader for Ray Marching SDF fluid rendering
 *
 * Performs ray marching through metaball SDF field to render
 * smooth fluid surfaces with:
 * - Fresnel reflection
 * - Subsurface scattering (SSS) for jelly effect
 * - Refraction
 * - Specular highlights
 * - Beer's Law absorption
 */
class FFluidRayMarchPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidRayMarchPS);
	SHADER_USE_PARAMETER_STRUCT(FFluidRayMarchPS, FGlobalShader);

	using FParameters = FFluidRayMarchParameters;

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
