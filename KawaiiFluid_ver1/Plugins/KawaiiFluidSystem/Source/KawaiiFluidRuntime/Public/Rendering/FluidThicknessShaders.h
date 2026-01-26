// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/**
 * Shared parameter structure for Fluid Thickness rendering
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidThicknessParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, ParticlePositions)
	SHADER_PARAMETER(float, ParticleRadius)
	SHADER_PARAMETER(FMatrix44f, ViewMatrix)
	SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
	SHADER_PARAMETER(float, ThicknessScale)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
	SHADER_PARAMETER(FVector2f, SceneViewRect)
	SHADER_PARAMETER(FVector2f, SceneTextureSize)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

/**
 * Fluid Thickness rendering Vertex Shader
 */
class FFluidThicknessVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidThicknessVS);
	SHADER_USE_PARAMETER_STRUCT(FFluidThicknessVS, FGlobalShader);

	using FParameters = FFluidThicknessParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Fluid Thickness rendering Pixel Shader
 */
class FFluidThicknessPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidThicknessPS);
	SHADER_USE_PARAMETER_STRUCT(FFluidThicknessPS, FGlobalShader);

	using FParameters = FFluidThicknessParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
