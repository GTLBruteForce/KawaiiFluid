// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidSmoothingPass.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ScreenPass.h"
#include "ShaderCore.h"
#include "PipelineStateCache.h"
#include "CommonRenderResources.h"

//=============================================================================
// Bilateral Blur Compute Shader (간단하게 Compute로 변경)
//=============================================================================

class FFluidBilateralBlurCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidBilateralBlurCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidBilateralBlurCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(FVector2f, InverseTextureSize)
		SHADER_PARAMETER(FIntPoint, BlurDirection)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER(float, BlurDepthFalloff)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 8);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidBilateralBlurCS, "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "BilateralBlurCS", SF_Compute);

//=============================================================================
// Smoothing Pass Implementation
//=============================================================================

void RenderFluidSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float BlurRadius,
	float DepthFalloff)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidSmoothingPass");

	check(InputDepthTexture);

	// RenderTarget → Compute Shader 호환을 위해 텍스처 복사
	FRDGTextureDesc CopyDesc = InputDepthTexture->Desc;
	CopyDesc.Flags = TexCreate_ShaderResource | TexCreate_UAV;
	FRDGTextureRef CopiedDepthTexture = GraphBuilder.CreateTexture(CopyDesc, TEXT("FluidDepthCopy"));
	AddCopyTexturePass(GraphBuilder, InputDepthTexture, CopiedDepthTexture);

	FIntPoint TextureSize = CopiedDepthTexture->Desc.Extent;

	// Create intermediate texture for horizontal pass output
	FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
		TextureSize,
		PF_R32_FLOAT,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef IntermediateTexture = GraphBuilder.CreateTexture(IntermediateDesc, TEXT("FluidDepthIntermediate"));

	// Create final smoothed texture
	OutSmoothedDepthTexture = GraphBuilder.CreateTexture(IntermediateDesc, TEXT("FluidDepthSmoothed"));

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFluidBilateralBlurCS> ComputeShader(GlobalShaderMap);

	//=============================================================================
	// Pass 1: Horizontal Blur
	//=============================================================================
	{
		FFluidBilateralBlurCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFluidBilateralBlurCS::FParameters>();

		PassParameters->InputTexture = CopiedDepthTexture;  // 복사본 사용
		PassParameters->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
		PassParameters->InverseTextureSize = FVector2f(1.0f / TextureSize.X, 1.0f / TextureSize.Y);
		PassParameters->BlurDirection = FIntPoint(1, 0); // Horizontal
		PassParameters->BlurRadius = BlurRadius;
		PassParameters->BlurDepthFalloff = DepthFalloff;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(IntermediateTexture);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HorizontalBlur"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(TextureSize, 8));
	}

	//=============================================================================
	// Pass 2: Vertical Blur
	//=============================================================================
	{
		FFluidBilateralBlurCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFluidBilateralBlurCS::FParameters>();

		PassParameters->InputTexture = IntermediateTexture;
		PassParameters->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
		PassParameters->InverseTextureSize = FVector2f(1.0f / TextureSize.X, 1.0f / TextureSize.Y);
		PassParameters->BlurDirection = FIntPoint(0, 1); // Vertical
		PassParameters->BlurRadius = BlurRadius;
		PassParameters->BlurDepthFalloff = DepthFalloff;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutSmoothedDepthTexture);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VerticalBlur"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(TextureSize, 8));
	}
}
