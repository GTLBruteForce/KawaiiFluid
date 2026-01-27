// Copyright 2026 Team_Bruteforce. All Rights Reserved.

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
// Narrow-Range Filter with LDS Optimization (16x16 tiles, max radius 16)
//=============================================================================

class FFluidNarrowRangeFilterLDS_CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidNarrowRangeFilterLDS_CS);
	SHADER_USE_PARAMETER_STRUCT(FFluidNarrowRangeFilterLDS_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER(float, BlurDepthFalloff)
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
		// Note: THREADGROUP_SIZE is not used by LDS version (uses NR_TILE_SIZE = 16)
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 8);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidNarrowRangeFilterLDS_CS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "NarrowRangeFilterLDS_CS",
                        SF_Compute);

//=============================================================================
// Thickness Gaussian Blur (Separable - Horizontal Pass)
//=============================================================================

class FFluidThicknessGaussianBlurHorizontalCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidThicknessGaussianBlurHorizontalCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidThicknessGaussianBlurHorizontalCS, FGlobalShader);

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

IMPLEMENT_GLOBAL_SHADER(FFluidThicknessGaussianBlurHorizontalCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "ThicknessGaussianBlurHorizontalCS",
                        SF_Compute);

//=============================================================================
// Thickness Gaussian Blur (Separable - Vertical Pass)
//=============================================================================

class FFluidThicknessGaussianBlurVerticalCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidThicknessGaussianBlurVerticalCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidThicknessGaussianBlurVerticalCS, FGlobalShader);

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

IMPLEMENT_GLOBAL_SHADER(FFluidThicknessGaussianBlurVerticalCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "ThicknessGaussianBlurVerticalCS",
                        SF_Compute);

//=============================================================================
// Depth Downsample Compute Shader (2x -> 1x)
//=============================================================================

class FFluidDepthDownsampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidDepthDownsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidDepthDownsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)         // Half-res output size
		SHADER_PARAMETER(FVector2f, FullResTextureSize)  // Full-res input size for clamping
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

IMPLEMENT_GLOBAL_SHADER(FFluidDepthDownsampleCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "DepthDownsampleCS",
                        SF_Compute);

//=============================================================================
// Depth Upsample Compute Shader (1x -> 2x, Joint Bilateral)
//=============================================================================

class FFluidDepthUpsampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidDepthUpsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidDepthUpsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)      // Half-res filtered
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FullResTexture)    // Original full-res (guide)
		SHADER_PARAMETER(FVector2f, FullResTextureSize)
		SHADER_PARAMETER(FVector2f, HalfResTextureSize)
		SHADER_PARAMETER(float, ParticleRadius)
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

IMPLEMENT_GLOBAL_SHADER(FFluidDepthUpsampleCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "DepthUpsampleCS",
                        SF_Compute);

//=============================================================================
// Narrow-Range Filter Smoothing Pass (Truong & Yuksel 2018)
// Uses Half-Resolution filtering for ~4x performance improvement
//
// Pipeline: FullRes -> Downsample -> Filter@HalfRes -> Upsample -> FullRes
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
	RDG_EVENT_SCOPE(GraphBuilder, "FluidNarrowRangeFilter_HalfRes");
	check(InputDepthTexture);

	NumIterations = FMath::Clamp(NumIterations, 1, 10);

	// Clamp filter radius to LDS max (16)
	const float ClampedFilterRadius = FMath::Min(FilterRadius, 16.0f);

	FIntPoint FullResSize = InputDepthTexture->Desc.Extent;
	FIntPoint HalfResSize = FIntPoint(FMath::DivideAndRoundUp(FullResSize.X, 2),
	                                   FMath::DivideAndRoundUp(FullResSize.Y, 2));

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Half-res texture descriptor
	FRDGTextureDesc HalfResDesc = FRDGTextureDesc::Create2D(
		HalfResSize,
		PF_R32_FLOAT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	// Full-res texture descriptor
	FRDGTextureDesc FullResDesc = FRDGTextureDesc::Create2D(
		FullResSize,
		PF_R32_FLOAT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	//=========================================================================
	// Step 1: Downsample to half resolution
	//=========================================================================
	FRDGTextureRef HalfResDepth;
	{
		TShaderMapRef<FFluidDepthDownsampleCS> DownsampleShader(GlobalShaderMap);

		HalfResDepth = GraphBuilder.CreateTexture(HalfResDesc, TEXT("FluidDepth_HalfRes"));

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidDepthDownsampleCS::FParameters>();
		PassParameters->InputTexture = InputDepthTexture;
		PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
		PassParameters->FullResTextureSize = FVector2f(FullResSize.X, FullResSize.Y);
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(HalfResDepth);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Downsample"),
			DownsampleShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(HalfResSize, 8));
	}

	//=========================================================================
	// Step 2: Apply Narrow-Range Filter at half resolution (4x fewer pixels!)
	//=========================================================================
	TShaderMapRef<FFluidNarrowRangeFilterLDS_CS> FilterShader(GlobalShaderMap);
	const int32 LDS_TILE_SIZE = 16;

	// Half the filter radius for half resolution (maintains same world-space effect)
	const float HalfResFilterRadius = FMath::Max(ClampedFilterRadius * 0.5f, 2.0f);

	FRDGTextureRef CurrentInput = HalfResDepth;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		FRDGTextureRef IterationOutput = GraphBuilder.CreateTexture(
			HalfResDesc, TEXT("FluidDepthNR_HalfRes"));

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidNarrowRangeFilterLDS_CS::FParameters>();

		PassParameters->InputTexture = CurrentInput;
		PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
		PassParameters->BlurRadius = HalfResFilterRadius;
		PassParameters->BlurDepthFalloff = 0.0f;
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->NarrowRangeThresholdRatio = ThresholdRatio;
		PassParameters->NarrowRangeClampRatio = ClampRatio;
		PassParameters->NarrowRangeGrazingBoost = GrazingBoost;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(IterationOutput);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("NR_HalfRes_Iter%d", Iteration),
			FilterShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(HalfResSize, LDS_TILE_SIZE));

		CurrentInput = IterationOutput;
	}

	//=========================================================================
	// Step 3: Upsample back to full resolution with joint bilateral
	//=========================================================================
	FRDGTextureRef FinalOutput;
	{
		TShaderMapRef<FFluidDepthUpsampleCS> UpsampleShader(GlobalShaderMap);

		FinalOutput = GraphBuilder.CreateTexture(FullResDesc, TEXT("FluidDepth_Upsampled"));

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidDepthUpsampleCS::FParameters>();
		PassParameters->InputTexture = CurrentInput;           // Half-res filtered
		PassParameters->FullResTexture = InputDepthTexture;    // Original full-res as guide
		PassParameters->FullResTextureSize = FVector2f(FullResSize.X, FullResSize.Y);
		PassParameters->HalfResTextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(FinalOutput);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Upsample_JointBilateral"),
			UpsampleShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(FullResSize, 8));
	}

	OutSmoothedDepthTexture = FinalOutput;
}

//=============================================================================
// Thickness Smoothing Pass (Separable Gaussian Blur)
//
// Uses Horizontal + Vertical passes instead of 2D kernel
// O(n²) → O(2n): 41x41=1681 samples → 41+41=82 samples (20x faster)
//=============================================================================

void RenderFluidThicknessSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputThicknessTexture,
	FRDGTextureRef& OutSmoothedThicknessTexture,
	float BlurRadius,
	int32 NumIterations)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidThicknessSmoothing_Separable");
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
	TShaderMapRef<FFluidThicknessGaussianBlurHorizontalCS> HorizontalShader(GlobalShaderMap);
	TShaderMapRef<FFluidThicknessGaussianBlurVerticalCS> VerticalShader(GlobalShaderMap);

	FRDGTextureRef CurrentInput = InputThicknessTexture;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		// Pass 1: Horizontal blur
		FRDGTextureRef HorizontalOutput = GraphBuilder.CreateTexture(
			IntermediateDesc, TEXT("FluidThicknessBlur_H"));
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessGaussianBlurHorizontalCS::FParameters>();
			PassParameters->InputTexture = CurrentInput;
			PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
			PassParameters->BlurRadius = BlurRadius;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(HorizontalOutput);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("ThicknessBlur_H_Iter%d", Iteration),
				HorizontalShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(TextureSize, 8));
		}

		// Pass 2: Vertical blur
		FRDGTextureRef VerticalOutput = GraphBuilder.CreateTexture(
			IntermediateDesc, TEXT("FluidThicknessBlur_V"));
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessGaussianBlurVerticalCS::FParameters>();
			PassParameters->InputTexture = HorizontalOutput;
			PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
			PassParameters->BlurRadius = BlurRadius;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(VerticalOutput);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("ThicknessBlur_V_Iter%d", Iteration),
				VerticalShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(TextureSize, 8));
		}

		CurrentInput = VerticalOutput;
	}

	OutSmoothedThicknessTexture = CurrentInput;
}
