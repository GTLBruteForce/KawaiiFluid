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
		// Distance-based dynamic smoothing parameters
		SHADER_PARAMETER(float, SmoothingWorldScale)
		SHADER_PARAMETER(float, SmoothingMinRadius)
		SHADER_PARAMETER(float, SmoothingMaxRadius)
		SHADER_PARAMETER(float, FocalLengthPixels)
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
// Narrow-Range Filter Separable (Horizontal Pass) - 32x faster than 2D
//=============================================================================

class FFluidNarrowRangeFilterHorizontalCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidNarrowRangeFilterHorizontalCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidNarrowRangeFilterHorizontalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER(float, BlurDepthFalloff)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, NarrowRangeThresholdRatio)
		SHADER_PARAMETER(float, NarrowRangeClampRatio)
		SHADER_PARAMETER(float, NarrowRangeGrazingBoost)
		SHADER_PARAMETER(float, SmoothingWorldScale)
		SHADER_PARAMETER(float, SmoothingMinRadius)
		SHADER_PARAMETER(float, SmoothingMaxRadius)
		SHADER_PARAMETER(float, FocalLengthPixels)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidNarrowRangeFilterHorizontalCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "NarrowRangeFilterHorizontalCS",
                        SF_Compute);

//=============================================================================
// Narrow-Range Filter Separable (Vertical Pass)
//=============================================================================

class FFluidNarrowRangeFilterVerticalCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidNarrowRangeFilterVerticalCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidNarrowRangeFilterVerticalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER(float, BlurDepthFalloff)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, NarrowRangeThresholdRatio)
		SHADER_PARAMETER(float, NarrowRangeClampRatio)
		SHADER_PARAMETER(float, NarrowRangeGrazingBoost)
		SHADER_PARAMETER(float, SmoothingWorldScale)
		SHADER_PARAMETER(float, SmoothingMinRadius)
		SHADER_PARAMETER(float, SmoothingMaxRadius)
		SHADER_PARAMETER(float, FocalLengthPixels)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidNarrowRangeFilterVerticalCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "NarrowRangeFilterVerticalCS",
                        SF_Compute);

//=============================================================================
// Narrow-Range Filter Separable (Diagonal1 Pass: ↘)
//=============================================================================

class FFluidNarrowRangeFilterDiagonal1CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidNarrowRangeFilterDiagonal1CS);
	SHADER_USE_PARAMETER_STRUCT(FFluidNarrowRangeFilterDiagonal1CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER(float, BlurDepthFalloff)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, NarrowRangeThresholdRatio)
		SHADER_PARAMETER(float, NarrowRangeClampRatio)
		SHADER_PARAMETER(float, NarrowRangeGrazingBoost)
		SHADER_PARAMETER(float, SmoothingWorldScale)
		SHADER_PARAMETER(float, SmoothingMinRadius)
		SHADER_PARAMETER(float, SmoothingMaxRadius)
		SHADER_PARAMETER(float, FocalLengthPixels)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidNarrowRangeFilterDiagonal1CS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "NarrowRangeFilterDiagonal1CS",
                        SF_Compute);

//=============================================================================
// Narrow-Range Filter Separable (Diagonal2 Pass: ↙)
//=============================================================================

class FFluidNarrowRangeFilterDiagonal2CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidNarrowRangeFilterDiagonal2CS);
	SHADER_USE_PARAMETER_STRUCT(FFluidNarrowRangeFilterDiagonal2CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER(float, BlurDepthFalloff)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, NarrowRangeThresholdRatio)
		SHADER_PARAMETER(float, NarrowRangeClampRatio)
		SHADER_PARAMETER(float, NarrowRangeGrazingBoost)
		SHADER_PARAMETER(float, SmoothingWorldScale)
		SHADER_PARAMETER(float, SmoothingMinRadius)
		SHADER_PARAMETER(float, SmoothingMaxRadius)
		SHADER_PARAMETER(float, FocalLengthPixels)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidNarrowRangeFilterDiagonal2CS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "NarrowRangeFilterDiagonal2CS",
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
// Velocity Gaussian Blur (Separable - Horizontal Pass)
// Smooths velocity texture to soften foam boundaries between particles.
//=============================================================================

class FFluidVelocityGaussianBlurHorizontalCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidVelocityGaussianBlurHorizontalCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidVelocityGaussianBlurHorizontalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, VelocityInputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, VelocityOutputTexture)
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

IMPLEMENT_GLOBAL_SHADER(FFluidVelocityGaussianBlurHorizontalCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "VelocityGaussianBlurHorizontalCS",
                        SF_Compute);

//=============================================================================
// Velocity Gaussian Blur (Separable - Vertical Pass)
//=============================================================================

class FFluidVelocityGaussianBlurVerticalCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidVelocityGaussianBlurVerticalCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidVelocityGaussianBlurVerticalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, VelocityInputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(float, BlurRadius)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, VelocityOutputTexture)
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

IMPLEMENT_GLOBAL_SHADER(FFluidVelocityGaussianBlurVerticalCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "VelocityGaussianBlurVerticalCS",
                        SF_Compute);

//=============================================================================
// Thickness Downsample Compute Shader (2x -> 1x, average)
//=============================================================================

class FFluidThicknessDownsampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidThicknessDownsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidThicknessDownsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(FVector2f, FullResTextureSize)
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

IMPLEMENT_GLOBAL_SHADER(FFluidThicknessDownsampleCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "ThicknessDownsampleCS",
                        SF_Compute);

//=============================================================================
// Thickness Upsample Compute Shader (1x -> 2x, bilinear)
//=============================================================================

class FFluidThicknessUpsampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidThicknessUpsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidThicknessUpsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
		SHADER_PARAMETER(FVector2f, FullResTextureSize)
		SHADER_PARAMETER(FVector2f, HalfResTextureSize)
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

IMPLEMENT_GLOBAL_SHADER(FFluidThicknessUpsampleCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSmoothing.usf", "ThicknessUpsampleCS",
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
	float ParticleRadius,
	float ThresholdRatio,
	float ClampRatio,
	int32 NumIterations,
	float GrazingBoost,
	const FDistanceBasedSmoothingParams& DistanceBasedParams)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidNarrowRangeFilter_HalfRes");
	check(InputDepthTexture);

	NumIterations = FMath::Clamp(NumIterations, 1, 10);

	// Calculate focal length in pixels for distance-based smoothing
	// FocalLength = ProjectionMatrix[0][0] * (TextureWidth / 2)
	// This converts world-space radius to screen-space pixel radius at depth z:
	// PixelRadius = WorldRadius * FocalLengthPixels / Depth
	const FMatrix ProjectionMatrix = View.ViewMatrices.GetProjectionMatrix();
	const float FocalLengthPixels = ProjectionMatrix.M[0][0] * (InputDepthTexture->Desc.Extent.X * 0.5f);

	// MaxRadius clamped to LDS limit (16 at half-res = 32 at full-res)
	const float ClampedMaxRadius = FMath::Min(static_cast<float>(DistanceBasedParams.MaxRadius), 32.0f);

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
	// Step 2: Apply Separable Narrow-Range Filter at half resolution
	// 16x faster: O(4n) instead of O(n²) - H + V + D1 + D2
	//=========================================================================
	TShaderMapRef<FFluidNarrowRangeFilterHorizontalCS> HorizontalShader(GlobalShaderMap);
	TShaderMapRef<FFluidNarrowRangeFilterVerticalCS> VerticalShader(GlobalShaderMap);
	TShaderMapRef<FFluidNarrowRangeFilterDiagonal1CS> Diagonal1Shader(GlobalShaderMap);
	TShaderMapRef<FFluidNarrowRangeFilterDiagonal2CS> Diagonal2Shader(GlobalShaderMap);

	// Half-res max radius
	const float HalfResMaxRadius = FMath::Max(ClampedMaxRadius * 0.5f, 2.0f);
	const float HalfResFocalLength = FocalLengthPixels * 0.5f;
	const float HalfResMinRadius = FMath::Max(static_cast<float>(DistanceBasedParams.MinRadius) * 0.5f, 1.0f);
	const float HalfResMaxRadiusClamped = FMath::Min(static_cast<float>(DistanceBasedParams.MaxRadius) * 0.5f, 32.0f);

	// Total pixels for diagonal dispatch
	const int32 TotalPixels = HalfResSize.X * HalfResSize.Y;

	FRDGTextureRef CurrentInput = HalfResDepth;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		// Pass 1: Horizontal →
		FRDGTextureRef HorizontalOutput = GraphBuilder.CreateTexture(
			HalfResDesc, TEXT("FluidDepthNR_H"));
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidNarrowRangeFilterHorizontalCS::FParameters>();
			PassParameters->InputTexture = CurrentInput;
			PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
			PassParameters->BlurRadius = HalfResMaxRadius;
			PassParameters->BlurDepthFalloff = 0.0f;
			PassParameters->ParticleRadius = ParticleRadius;
			PassParameters->NarrowRangeThresholdRatio = ThresholdRatio;
			PassParameters->NarrowRangeClampRatio = ClampRatio;
			PassParameters->NarrowRangeGrazingBoost = GrazingBoost;
			PassParameters->SmoothingWorldScale = DistanceBasedParams.WorldScale;
			PassParameters->SmoothingMinRadius = HalfResMinRadius;
			PassParameters->SmoothingMaxRadius = HalfResMaxRadiusClamped;
			PassParameters->FocalLengthPixels = HalfResFocalLength;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(HorizontalOutput);

			FIntVector GroupCount = FIntVector(
				FMath::DivideAndRoundUp(HalfResSize.X, 256),
				HalfResSize.Y,
				1);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("NR_Sep_H_Iter%d", Iteration),
				HorizontalShader,
				PassParameters,
				GroupCount);
		}

		// Pass 2: Vertical ↓
		FRDGTextureRef VerticalOutput = GraphBuilder.CreateTexture(
			HalfResDesc, TEXT("FluidDepthNR_V"));
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidNarrowRangeFilterVerticalCS::FParameters>();
			PassParameters->InputTexture = HorizontalOutput;
			PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
			PassParameters->BlurRadius = HalfResMaxRadius;
			PassParameters->BlurDepthFalloff = 0.0f;
			PassParameters->ParticleRadius = ParticleRadius;
			PassParameters->NarrowRangeThresholdRatio = ThresholdRatio;
			PassParameters->NarrowRangeClampRatio = ClampRatio;
			PassParameters->NarrowRangeGrazingBoost = GrazingBoost;
			PassParameters->SmoothingWorldScale = DistanceBasedParams.WorldScale;
			PassParameters->SmoothingMinRadius = HalfResMinRadius;
			PassParameters->SmoothingMaxRadius = HalfResMaxRadiusClamped;
			PassParameters->FocalLengthPixels = HalfResFocalLength;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(VerticalOutput);

			FIntVector GroupCount = FIntVector(
				HalfResSize.X,
				FMath::DivideAndRoundUp(HalfResSize.Y, 256),
				1);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("NR_Sep_V_Iter%d", Iteration),
				VerticalShader,
				PassParameters,
				GroupCount);
		}

		// Pass 3: Diagonal1 ↘
		FRDGTextureRef Diagonal1Output = GraphBuilder.CreateTexture(
			HalfResDesc, TEXT("FluidDepthNR_D1"));
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidNarrowRangeFilterDiagonal1CS::FParameters>();
			PassParameters->InputTexture = VerticalOutput;
			PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
			PassParameters->BlurRadius = HalfResMaxRadius;
			PassParameters->BlurDepthFalloff = 0.0f;
			PassParameters->ParticleRadius = ParticleRadius;
			PassParameters->NarrowRangeThresholdRatio = ThresholdRatio;
			PassParameters->NarrowRangeClampRatio = ClampRatio;
			PassParameters->NarrowRangeGrazingBoost = GrazingBoost;
			PassParameters->SmoothingWorldScale = DistanceBasedParams.WorldScale;
			PassParameters->SmoothingMinRadius = HalfResMinRadius;
			PassParameters->SmoothingMaxRadius = HalfResMaxRadiusClamped;
			PassParameters->FocalLengthPixels = HalfResFocalLength;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(Diagonal1Output);

			FIntVector GroupCount = FIntVector(
				FMath::DivideAndRoundUp(TotalPixels, 256),
				1,
				1);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("NR_Sep_D1_Iter%d", Iteration),
				Diagonal1Shader,
				PassParameters,
				GroupCount);
		}

		// Pass 4: Diagonal2 ↙
		FRDGTextureRef Diagonal2Output = GraphBuilder.CreateTexture(
			HalfResDesc, TEXT("FluidDepthNR_D2"));
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidNarrowRangeFilterDiagonal2CS::FParameters>();
			PassParameters->InputTexture = Diagonal1Output;
			PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
			PassParameters->BlurRadius = HalfResMaxRadius;
			PassParameters->BlurDepthFalloff = 0.0f;
			PassParameters->ParticleRadius = ParticleRadius;
			PassParameters->NarrowRangeThresholdRatio = ThresholdRatio;
			PassParameters->NarrowRangeClampRatio = ClampRatio;
			PassParameters->NarrowRangeGrazingBoost = GrazingBoost;
			PassParameters->SmoothingWorldScale = DistanceBasedParams.WorldScale;
			PassParameters->SmoothingMinRadius = HalfResMinRadius;
			PassParameters->SmoothingMaxRadius = HalfResMaxRadiusClamped;
			PassParameters->FocalLengthPixels = HalfResFocalLength;
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(Diagonal2Output);

			FIntVector GroupCount = FIntVector(
				FMath::DivideAndRoundUp(TotalPixels, 256),
				1,
				1);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("NR_Sep_D2_Iter%d", Iteration),
				Diagonal2Shader,
				PassParameters,
				GroupCount);
		}

		CurrentInput = Diagonal2Output;
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
	int32 NumIterations,
	bool bUseHalfRes)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidThicknessSmoothing_%s", bUseHalfRes ? TEXT("HalfRes") : TEXT("FullRes"));
	check(InputThicknessTexture);

	NumIterations = FMath::Clamp(NumIterations, 1, 5);

	FIntPoint FullResSize = InputThicknessTexture->Desc.Extent;
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	if (bUseHalfRes)
	{
		//=====================================================================
		// Half-Resolution Path: Downsample -> Blur -> Upsample
		//=====================================================================
		FIntPoint HalfResSize = FIntPoint(
			FMath::DivideAndRoundUp(FullResSize.X, 2),
			FMath::DivideAndRoundUp(FullResSize.Y, 2));

		// Half-res texture descriptor
		FRDGTextureDesc HalfResDesc = FRDGTextureDesc::Create2D(
			HalfResSize,
			PF_R16F,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV);

		// Full-res texture descriptor (for final output)
		FRDGTextureDesc FullResDesc = FRDGTextureDesc::Create2D(
			FullResSize,
			PF_R16F,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV);

		//--- Step 1: Downsample to half resolution ---
		FRDGTextureRef HalfResThickness;
		{
			TShaderMapRef<FFluidThicknessDownsampleCS> DownsampleShader(GlobalShaderMap);

			HalfResThickness = GraphBuilder.CreateTexture(HalfResDesc, TEXT("FluidThickness_HalfRes"));

			auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessDownsampleCS::FParameters>();
			PassParameters->InputTexture = InputThicknessTexture;
			PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
			PassParameters->FullResTextureSize = FVector2f(FullResSize.X, FullResSize.Y);
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(HalfResThickness);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("ThicknessDownsample"),
				DownsampleShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(HalfResSize, 8));
		}

		//--- Step 2: Blur at half resolution ---
		TShaderMapRef<FFluidThicknessGaussianBlurHorizontalCS> HorizontalShader(GlobalShaderMap);
		TShaderMapRef<FFluidThicknessGaussianBlurVerticalCS> VerticalShader(GlobalShaderMap);

		// Adjust blur radius for half resolution
		float HalfResBlurRadius = FMath::Max(BlurRadius * 0.5f, 1.0f);

		FRDGTextureRef CurrentInput = HalfResThickness;

		for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
		{
			// Horizontal blur
			FRDGTextureRef HorizontalOutput = GraphBuilder.CreateTexture(
				HalfResDesc, TEXT("FluidThicknessBlur_H"));
			{
				auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessGaussianBlurHorizontalCS::FParameters>();
				PassParameters->InputTexture = CurrentInput;
				PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
				PassParameters->BlurRadius = HalfResBlurRadius;
				PassParameters->OutputTexture = GraphBuilder.CreateUAV(HorizontalOutput);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ThicknessBlur_H_Iter%d", Iteration),
					HorizontalShader,
					PassParameters,
					FComputeShaderUtils::GetGroupCount(HalfResSize, 8));
			}

			// Vertical blur
			FRDGTextureRef VerticalOutput = GraphBuilder.CreateTexture(
				HalfResDesc, TEXT("FluidThicknessBlur_V"));
			{
				auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessGaussianBlurVerticalCS::FParameters>();
				PassParameters->InputTexture = HorizontalOutput;
				PassParameters->TextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
				PassParameters->BlurRadius = HalfResBlurRadius;
				PassParameters->OutputTexture = GraphBuilder.CreateUAV(VerticalOutput);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ThicknessBlur_V_Iter%d", Iteration),
					VerticalShader,
					PassParameters,
					FComputeShaderUtils::GetGroupCount(HalfResSize, 8));
			}

			CurrentInput = VerticalOutput;
		}

		//--- Step 3: Upsample back to full resolution ---
		FRDGTextureRef FinalOutput;
		{
			TShaderMapRef<FFluidThicknessUpsampleCS> UpsampleShader(GlobalShaderMap);

			FinalOutput = GraphBuilder.CreateTexture(FullResDesc, TEXT("FluidThickness_Upsampled"));

			auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessUpsampleCS::FParameters>();
			PassParameters->InputTexture = CurrentInput;
			PassParameters->FullResTextureSize = FVector2f(FullResSize.X, FullResSize.Y);
			PassParameters->HalfResTextureSize = FVector2f(HalfResSize.X, HalfResSize.Y);
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(FinalOutput);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("ThicknessUpsample"),
				UpsampleShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(FullResSize, 8));
		}

		OutSmoothedThicknessTexture = FinalOutput;
	}
	else
	{
		//=====================================================================
		// Full-Resolution Path: Original implementation
		//=====================================================================
		FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
			FullResSize,
			PF_R16F,
			FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV);

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
				PassParameters->TextureSize = FVector2f(FullResSize.X, FullResSize.Y);
				PassParameters->BlurRadius = BlurRadius;
				PassParameters->OutputTexture = GraphBuilder.CreateUAV(HorizontalOutput);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ThicknessBlur_H_Iter%d", Iteration),
					HorizontalShader,
					PassParameters,
					FComputeShaderUtils::GetGroupCount(FullResSize, 8));
			}

			// Pass 2: Vertical blur
			FRDGTextureRef VerticalOutput = GraphBuilder.CreateTexture(
				IntermediateDesc, TEXT("FluidThicknessBlur_V"));
			{
				auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessGaussianBlurVerticalCS::FParameters>();
				PassParameters->InputTexture = HorizontalOutput;
				PassParameters->TextureSize = FVector2f(FullResSize.X, FullResSize.Y);
				PassParameters->BlurRadius = BlurRadius;
				PassParameters->OutputTexture = GraphBuilder.CreateUAV(VerticalOutput);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("ThicknessBlur_V_Iter%d", Iteration),
					VerticalShader,
					PassParameters,
					FComputeShaderUtils::GetGroupCount(FullResSize, 8));
			}

			CurrentInput = VerticalOutput;
		}

		OutSmoothedThicknessTexture = CurrentInput;
	}
}

//=============================================================================
// Velocity Smoothing Pass (Separable Gaussian Blur)
//
// Smooths the velocity texture to soften foam boundaries between particles.
// Without smoothing, foam edges appear sharp because each particle sprite
// has a constant velocity, creating abrupt transitions at particle borders.
//
// Uses Horizontal + Vertical passes for O(2n) instead of O(n²).
//=============================================================================

/**
 * @brief Renders the velocity smoothing pass using separable Gaussian blur.
 * @param GraphBuilder RDG builder
 * @param View Scene view
 * @param InputVelocityTexture Input velocity texture (RG16F or RG32F)
 * @param OutSmoothedVelocityTexture Output smoothed velocity texture
 * @param BlurRadius Blur kernel radius in pixels
 * @param NumIterations Number of blur iterations (1-5)
 */
void RenderFluidVelocitySmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputVelocityTexture,
	FRDGTextureRef& OutSmoothedVelocityTexture,
	float BlurRadius,
	int32 NumIterations)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidVelocitySmoothing_Separable");
	check(InputVelocityTexture);

	NumIterations = FMath::Clamp(NumIterations, 1, 5);

	FIntPoint TextureSize = InputVelocityTexture->Desc.Extent;

	// Use same format as input velocity texture (typically RG16F)
	FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
		TextureSize,
		InputVelocityTexture->Desc.Format,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFluidVelocityGaussianBlurHorizontalCS> HorizontalShader(GlobalShaderMap);
	TShaderMapRef<FFluidVelocityGaussianBlurVerticalCS> VerticalShader(GlobalShaderMap);

	FRDGTextureRef CurrentInput = InputVelocityTexture;

	for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration)
	{
		// Pass 1: Horizontal blur
		FRDGTextureRef HorizontalOutput = GraphBuilder.CreateTexture(
			IntermediateDesc, TEXT("FluidVelocityBlur_H"));
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidVelocityGaussianBlurHorizontalCS::FParameters>();
			PassParameters->VelocityInputTexture = CurrentInput;
			PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
			PassParameters->BlurRadius = BlurRadius;
			PassParameters->VelocityOutputTexture = GraphBuilder.CreateUAV(HorizontalOutput);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("VelocityBlur_H_Iter%d", Iteration),
				HorizontalShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(TextureSize, 8));
		}

		// Pass 2: Vertical blur
		FRDGTextureRef VerticalOutput = GraphBuilder.CreateTexture(
			IntermediateDesc, TEXT("FluidVelocityBlur_V"));
		{
			auto* PassParameters = GraphBuilder.AllocParameters<FFluidVelocityGaussianBlurVerticalCS::FParameters>();
			PassParameters->VelocityInputTexture = HorizontalOutput;
			PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
			PassParameters->BlurRadius = BlurRadius;
			PassParameters->VelocityOutputTexture = GraphBuilder.CreateUAV(VerticalOutput);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("VelocityBlur_V_Iter%d", Iteration),
				VerticalShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(TextureSize, 8));
		}

		CurrentInput = VerticalOutput;
	}

	OutSmoothedVelocityTexture = CurrentInput;
}
