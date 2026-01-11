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
// 2D Bilateral Blur Compute Shader (9x9 kernel, 81 samples)
//=============================================================================

class FFluidBilateralBlur2DCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidBilateralBlur2DCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidBilateralBlur2DCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER(float, BlurDepthFalloff)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 8);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidBilateralBlur2DCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "BilateralBlur2DCS",
                        SF_Compute);

//=============================================================================
// Narrow-Range Filter Compute Shader (Truong & Yuksel, i3D 2018)
//=============================================================================

class FFluidNarrowRangeFilterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidNarrowRangeFilterCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidNarrowRangeFilterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER(float, BlurDepthFalloff)  // Unused but kept for consistency
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, NarrowRangeThresholdRatio)
		SHADER_PARAMETER(float, NarrowRangeClampRatio)
		SHADER_PARAMETER(float, NarrowRangeGrazingBoost)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 8);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidNarrowRangeFilterCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "NarrowRangeFilterCS",
                        SF_Compute);

//=============================================================================
// Thickness Gaussian Blur Compute Shader
//=============================================================================

class FFluidThicknessGaussianBlurCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidThicknessGaussianBlurCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidThicknessGaussianBlurCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 8);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidThicknessGaussianBlurCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "ThicknessGaussianBlurCS",
                        SF_Compute);

//=============================================================================
// Curvature Flow Compute Shader (van der Laan et al.)
//=============================================================================

class FFluidCurvatureFlowCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidCurvatureFlowCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidCurvatureFlowCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)  // Unused but kept for interface consistency
		SHADER_PARAMETER(float, BlurDepthFalloff)  // Unused
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, CurvatureFlowDt)
		SHADER_PARAMETER(float, CurvatureFlowThreshold)
		SHADER_PARAMETER(float, CurvatureFlowGrazingBoost)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 8);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidCurvatureFlowCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "CurvatureFlowCS",
                        SF_Compute);

//=============================================================================
// Smoothing Pass Implementation
//=============================================================================

void RenderFluidSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float BlurRadius,
	float DepthFalloff,
	int32 NumIterations)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidSmoothingPass (Multi-Iteration)");
	check(InputDepthTexture);

	// Clamp iterations to reasonable range
	NumIterations = FMath::Clamp(NumIterations, 1, 5);

	FIntPoint TextureSize = InputDepthTexture->Desc.Extent;

	FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
		TextureSize,
		PF_R32_FLOAT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFluidBilateralBlur2DCS> ComputeShader(GlobalShaderMap);

	// Current input/output for iteration loop
	FRDGTextureRef CurrentInput = InputDepthTexture;

	//=============================================================================
	// Multiple Iterations with 2D Bilateral Blur (9x9 kernel)
	//=============================================================================
	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		// Create output texture for this iteration
		FRDGTextureRef IterationOutput = GraphBuilder.CreateTexture(
			IntermediateDesc, TEXT("FluidDepth2DBlur"));

		//=============================================================================
		// 2D Bilateral Blur (single pass, no separable filtering)
		//=============================================================================
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidBilateralBlur2DCS::FParameters>();

			PassParameters->InputTexture = CurrentInput;
			PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
			PassParameters->BlurRadius = BlurRadius;
			PassParameters->BlurDepthFalloff = DepthFalloff;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(IterationOutput);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Iteration%d_2DBlur", Iteration),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(TextureSize, 8));
		}

		// Use this iteration's output as next iteration's input
		CurrentInput = IterationOutput;
	}

	// Final output
	OutSmoothedDepthTexture = CurrentInput;
}

//=============================================================================
// Narrow-Range Filter Smoothing Pass (Truong & Yuksel 2018)
//=============================================================================

void RenderFluidNarrowRangeSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float FilterRadius,
	float ParticleRadius,
	float ThresholdRatio,
	float ClampRatio,
	int32 NumIterations,
	float GrazingBoost)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidNarrowRangeFilter (Multi-Iteration)");
	check(InputDepthTexture);

	NumIterations = FMath::Clamp(NumIterations, 1, 10);

	FIntPoint TextureSize = InputDepthTexture->Desc.Extent;

	FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
		TextureSize,
		PF_R32_FLOAT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFluidNarrowRangeFilterCS> ComputeShader(GlobalShaderMap);

	FRDGTextureRef CurrentInput = InputDepthTexture;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FRDGTextureRef IterationOutput = GraphBuilder.CreateTexture(
			IntermediateDesc, TEXT("FluidDepthNarrowRange"));

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidNarrowRangeFilterCS::FParameters>();

		PassParameters->InputTexture = CurrentInput;
		PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
		PassParameters->BlurRadius = FilterRadius;
		PassParameters->BlurDepthFalloff = 0.0f;  // Unused in narrow-range
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->NarrowRangeThresholdRatio = ThresholdRatio;
		PassParameters->NarrowRangeClampRatio = ClampRatio;
		PassParameters->NarrowRangeGrazingBoost = GrazingBoost;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(IterationOutput);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Iteration%d_NarrowRange", Iteration),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(TextureSize, 8));

		CurrentInput = IterationOutput;
	}

	OutSmoothedDepthTexture = CurrentInput;
}

//=============================================================================
// Thickness Smoothing Pass (Simple Gaussian Blur)
//=============================================================================

void RenderFluidThicknessSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputThicknessTexture,
	FRDGTextureRef& OutSmoothedThicknessTexture,
	float BlurRadius,
	int32 NumIterations)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidThicknessSmoothing");
	check(InputThicknessTexture);

	NumIterations = FMath::Clamp(NumIterations, 1, 5);

	FIntPoint TextureSize = InputThicknessTexture->Desc.Extent;

	// Use R16F format to match the input thickness texture
	FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
		TextureSize,
		PF_R16F,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFluidThicknessGaussianBlurCS> ComputeShader(GlobalShaderMap);

	FRDGTextureRef CurrentInput = InputThicknessTexture;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FRDGTextureRef IterationOutput = GraphBuilder.CreateTexture(
			IntermediateDesc, TEXT("FluidThicknessBlur"));

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessGaussianBlurCS::FParameters>();

		PassParameters->InputTexture = CurrentInput;
		PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
		PassParameters->BlurRadius = BlurRadius;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(IterationOutput);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ThicknessBlur_Iteration%d", Iteration),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(TextureSize, 8));

		CurrentInput = IterationOutput;
	}

	OutSmoothedThicknessTexture = CurrentInput;
}

//=============================================================================
// Curvature Flow Smoothing Pass (van der Laan et al.)
//=============================================================================

void RenderFluidCurvatureFlowSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float ParticleRadius,
	float Dt,
	float DepthThreshold,
	int32 NumIterations,
	float GrazingBoost)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidCurvatureFlowSmoothing");
	check(InputDepthTexture);

	NumIterations = FMath::Clamp(NumIterations, 1, 200);

	FIntPoint TextureSize = InputDepthTexture->Desc.Extent;

	FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
		TextureSize,
		PF_R32_FLOAT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFluidCurvatureFlowCS> ComputeShader(GlobalShaderMap);

	FRDGTextureRef CurrentInput = InputDepthTexture;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FRDGTextureRef IterationOutput = GraphBuilder.CreateTexture(
			IntermediateDesc, TEXT("FluidDepthCurvatureFlow"));

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidCurvatureFlowCS::FParameters>();

		PassParameters->InputTexture = CurrentInput;
		PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
		PassParameters->BlurRadius = 0.0f;  // Unused
		PassParameters->BlurDepthFalloff = 0.0f;  // Unused
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->CurvatureFlowDt = Dt;
		PassParameters->CurvatureFlowThreshold = DepthThreshold;
		PassParameters->CurvatureFlowGrazingBoost = GrazingBoost;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(IterationOutput);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("CurvatureFlow_Iteration%d", Iteration),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(TextureSize, 8));

		CurrentInput = IterationOutput;
	}

	OutSmoothedDepthTexture = CurrentInput;
}
