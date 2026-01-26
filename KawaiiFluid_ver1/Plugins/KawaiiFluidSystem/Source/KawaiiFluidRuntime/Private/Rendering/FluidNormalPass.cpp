// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/FluidNormalPass.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderCore.h"
#include "DataDrivenShaderPlatformInfo.h"

/**
 * @brief Normal reconstruction compute shader.
 */
class FFluidNormalCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidNormalCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidNormalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, InputDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(FVector2f, InverseTextureSize)
		SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
		SHADER_PARAMETER(FMatrix44f, InverseProjectionMatrix)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputNormalTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidNormalCS, "/Plugin/KawaiiFluidSystem/Private/FluidNormal.usf",
                        "ReconstructNormalCS", SF_Compute);

void RenderFluidNormalPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef SmoothedDepthTexture,
	FRDGTextureRef& OutNormalTexture)
{
	if (!SmoothedDepthTexture)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "FluidNormalPass");

	FIntPoint Extent = SmoothedDepthTexture->Desc.Extent;

	FRDGTextureDesc NormalDesc = FRDGTextureDesc::Create2D(
		Extent,
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	OutNormalTexture = GraphBuilder.CreateTexture(NormalDesc, TEXT("FluidNormalTexture"));

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidNormalCS> ComputeShader(GlobalShaderMap);
	FFluidNormalCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFluidNormalCS::FParameters>();

	PassParameters->InputDepthTexture = SmoothedDepthTexture;

	PassParameters->TextureSize = FVector2f(Extent.X, Extent.Y);
	PassParameters->InverseTextureSize = FVector2f(1.0f / Extent.X, 1.0f / Extent.Y);

	// Setup view matrices
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionNoAAMatrix());
	PassParameters->InverseProjectionMatrix =
		FMatrix44f(View.ViewMatrices.GetInvProjectionMatrix());

	PassParameters->OutputNormalTexture = GraphBuilder.CreateUAV(OutNormalTexture);

	// UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: Rendering FluidNormalPass. Extent: %d x %d"),
	//        Extent.X, Extent.Y);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FluidNormalReconstruction"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(Extent, FIntPoint(8, 8)));
}
