// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/**
 * Fluid Depth 렌더링을 위한 공유 파라미터 구조체
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidDepthParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, ParticlePositions)
	SHADER_PARAMETER(float, ParticleRadius)
	SHADER_PARAMETER(FMatrix44f, ViewMatrix)
	SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
	SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

/**
 * Fluid Depth 렌더링 Vertex Shader
 */
class FFluidDepthVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidDepthVS);
	SHADER_USE_PARAMETER_STRUCT(FFluidDepthVS, FGlobalShader);

	using FParameters = FFluidDepthParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Fluid Depth 렌더링 Pixel Shader
 */
class FFluidDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FFluidDepthPS, FGlobalShader);

	using FParameters = FFluidDepthParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
