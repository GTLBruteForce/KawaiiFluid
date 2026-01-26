// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

//=============================================================================
// Anisotropy Permutation
//=============================================================================

class FUseAnisotropyDim : SHADER_PERMUTATION_BOOL("USE_ANISOTROPY");

/**
 * Shared parameter structure for Fluid Depth rendering
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidDepthParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, ParticlePositions)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, ParticleVelocities)
	SHADER_PARAMETER(float, ParticleRadius)
	SHADER_PARAMETER(FMatrix44f, ViewMatrix)
	SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
	SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
	SHADER_PARAMETER(FVector2f, SceneViewRect)
	SHADER_PARAMETER(FVector2f, SceneTextureSize)
	// Anisotropy buffers (float4: direction.xyz + scale.w)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, AnisotropyAxis1)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, AnisotropyAxis2)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, AnisotropyAxis3)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

/**
 * Fluid Depth rendering Vertex Shader
 */
class FFluidDepthVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidDepthVS);
	SHADER_USE_PARAMETER_STRUCT(FFluidDepthVS, FGlobalShader);

	using FParameters = FFluidDepthParameters;
	using FPermutationDomain = TShaderPermutationDomain<FUseAnisotropyDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

/**
 * Fluid Depth rendering Pixel Shader
 */
class FFluidDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FFluidDepthPS, FGlobalShader);

	using FParameters = FFluidDepthParameters;
	using FPermutationDomain = TShaderPermutationDomain<FUseAnisotropyDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
