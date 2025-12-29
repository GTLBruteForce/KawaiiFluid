// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/**
 * Fluid Thickness 렌더링을 위한 공유 파라미터 구조체
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
 * Fluid Thickness 렌더링 Vertex Shader
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
 * Fluid Thickness 렌더링 Pixel Shader
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
