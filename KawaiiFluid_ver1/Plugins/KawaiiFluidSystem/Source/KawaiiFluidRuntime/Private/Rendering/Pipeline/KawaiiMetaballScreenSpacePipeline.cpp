// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Pipeline/KawaiiMetaballScreenSpacePipeline.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/FluidDepthPass.h"
#include "Rendering/FluidSmoothingPass.h"
#include "Rendering/FluidNormalPass.h"
#include "Rendering/FluidThicknessPass.h"

// Separated shading implementation
#include "Rendering/Shading/KawaiiScreenSpaceShadingImpl.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ScenePrivate.h"
#include "SceneRendering.h"
#include "SceneTextures.h"

// Helper function to generate intermediate textures (depth, normal, thickness)
static bool GenerateIntermediateTextures(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FMetaballIntermediateTextures& OutIntermediateTextures)
{
	// Calculate average particle radius for this batch
	float AverageParticleRadius = 10.0f;
	float TotalRadius = 0.0f;
	int ValidCount = 0;

	for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
	{
		TotalRadius += Renderer->GetCachedParticleRadius();
		ValidCount++;
	}

	if (ValidCount > 0)
	{
		AverageParticleRadius = TotalRadius / ValidCount;
	}

	// Use RenderParams for rendering parameters
	float BlurRadius = static_cast<float>(RenderParams.BilateralFilterRadius);
	float DepthFalloff = AverageParticleRadius * 0.7f;
	int32 NumIterations = 3;

	// 1. Depth Pass
	FRDGTextureRef DepthTexture = nullptr;
	RenderFluidDepthPass(GraphBuilder, View, Renderers, SceneDepthTexture, DepthTexture);

	if (!DepthTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Depth pass failed"));
		return false;
	}

	// 2. Smoothing Pass
	FRDGTextureRef SmoothedDepthTexture = nullptr;
	RenderFluidSmoothingPass(GraphBuilder, View, DepthTexture, SmoothedDepthTexture,
	                         BlurRadius, DepthFalloff, NumIterations);

	if (!SmoothedDepthTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Smoothing pass failed"));
		return false;
	}

	// 3. Normal Pass
	FRDGTextureRef NormalTexture = nullptr;
	RenderFluidNormalPass(GraphBuilder, View, SmoothedDepthTexture, NormalTexture);

	if (!NormalTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Normal pass failed"));
		return false;
	}

	// 4. Thickness Pass
	FRDGTextureRef ThicknessTexture = nullptr;
	RenderFluidThicknessPass(GraphBuilder, View, Renderers, SceneDepthTexture, ThicknessTexture);

	if (!ThicknessTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Thickness pass failed"));
		return false;
	}

	// Build output
	OutIntermediateTextures.SmoothedDepthTexture = SmoothedDepthTexture;
	OutIntermediateTextures.NormalTexture = NormalTexture;
	OutIntermediateTextures.ThicknessTexture = ThicknessTexture;

	return true;
}

void FKawaiiMetaballScreenSpacePipeline::ExecutePostBasePass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FScreenPassRenderTarget Output)
{
	if (Renderers.Num() == 0)
	{
		return;
	}

	// PostProcess mode uses PrepareForTonemap + ExecuteTonemap at Tonemap timing
	if (RenderParams.ShadingMode == EMetaballShadingMode::PostProcess)
	{
		// Nothing to do here - PostProcess mode preparation happens in PrepareForTonemap
		return;
	}

	// Translucent mode is not supported by ScreenSpace pipeline
	if (RenderParams.ShadingMode == EMetaballShadingMode::Translucent)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Translucent shading mode is not supported. Use RayMarchingPipeline."));
		return;
	}

	// GBuffer/Opaque mode - write to GBuffer
	if (RenderParams.ShadingMode == EMetaballShadingMode::GBuffer ||
		RenderParams.ShadingMode == EMetaballShadingMode::Opaque)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_ScreenSpace_PostBasePass_GBuffer");

		// Generate intermediate textures
		FMetaballIntermediateTextures IntermediateTextures;
		if (!GenerateIntermediateTextures(GraphBuilder, View, RenderParams, Renderers, SceneDepthTexture, IntermediateTextures))
		{
			return;
		}

		// Get GBuffer textures from SceneTextures
		const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
		const FSceneTextures& SceneTexturesRef = ViewInfo.GetSceneTextures();
		IntermediateTextures.GBufferATexture = SceneTexturesRef.GBufferA;
		IntermediateTextures.GBufferBTexture = SceneTexturesRef.GBufferB;
		IntermediateTextures.GBufferCTexture = SceneTexturesRef.GBufferC;
		IntermediateTextures.GBufferDTexture = SceneTexturesRef.GBufferD;

		// Call GBuffer shading
		KawaiiScreenSpaceShading::RenderGBufferShading(
			GraphBuilder,
			View,
			RenderParams,
			IntermediateTextures,
			SceneDepthTexture);

		UE_LOG(LogTemp, Log, TEXT("FKawaiiMetaballScreenSpacePipeline: GBuffer write completed"));
	}
}

void FKawaiiMetaballScreenSpacePipeline::PrepareForTonemap(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture)
{
	if (Renderers.Num() == 0)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_ScreenSpace_PrepareForTonemap");

	// Generate and cache intermediate textures for ExecuteTonemap
	if (!GenerateIntermediateTextures(GraphBuilder, View, RenderParams, Renderers, SceneDepthTexture, CachedIntermediateTextures))
	{
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("FKawaiiMetaballScreenSpacePipeline: PrepareForTonemap completed - intermediate textures cached"));
}

void FKawaiiMetaballScreenSpacePipeline::ExecutePrePostProcess(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output,
	FRDGTextureRef GBufferATexture,
	FRDGTextureRef GBufferDTexture)
{
	// ScreenSpace pipeline does not use PrePostProcess timing
	// Translucent mode requires RayMarchingPipeline
}

void FKawaiiMetaballScreenSpacePipeline::ExecuteTonemap(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	if (Renderers.Num() == 0)
	{
		return;
	}

	// Only PostProcess shading mode uses Tonemap timing
	if (RenderParams.ShadingMode != EMetaballShadingMode::PostProcess)
	{
		return;
	}

	// Validate cached intermediate textures
	if (!CachedIntermediateTextures.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Missing cached intermediate textures for Tonemap"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_ScreenSpace_Tonemap");

	// Delegate to separated shading implementation
	KawaiiScreenSpaceShading::RenderPostProcessShading(
		GraphBuilder,
		View,
		RenderParams,
		CachedIntermediateTextures,
		SceneDepthTexture,
		SceneColorTexture,
		Output);
}
