// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Pipeline/KawaiiMetaballScreenSpacePipeline.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/FluidDepthPass.h"
#include "Rendering/FluidSmoothingPass.h"
#include "Rendering/FluidNormalPass.h"
#include "Rendering/FluidThicknessPass.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "SceneView.h"
#include "SceneRendering.h"
#include "SceneTextures.h"

void FKawaiiMetaballScreenSpacePipeline::Execute(
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

	if (!ShadingPass)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: No ShadingPass set"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_ScreenSpace");

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
		return;
	}

	// 2. Smoothing Pass
	FRDGTextureRef SmoothedDepthTexture = nullptr;
	RenderFluidSmoothingPass(GraphBuilder, View, DepthTexture, SmoothedDepthTexture,
	                         BlurRadius, DepthFalloff, NumIterations);

	if (!SmoothedDepthTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Smoothing pass failed"));
		return;
	}

	// 3. Normal Pass
	FRDGTextureRef NormalTexture = nullptr;
	RenderFluidNormalPass(GraphBuilder, View, SmoothedDepthTexture, NormalTexture);

	if (!NormalTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Normal pass failed"));
		return;
	}

	// 4. Thickness Pass
	FRDGTextureRef ThicknessTexture = nullptr;
	RenderFluidThicknessPass(GraphBuilder, View, Renderers, SceneDepthTexture, ThicknessTexture);

	if (!ThicknessTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Thickness pass failed"));
		return;
	}

	// 5. Build intermediate textures and delegate to ShadingPass
	FMetaballIntermediateTextures IntermediateTextures;
	IntermediateTextures.SmoothedDepthTexture = SmoothedDepthTexture;
	IntermediateTextures.NormalTexture = NormalTexture;
	IntermediateTextures.ThicknessTexture = ThicknessTexture;

	// Get GBuffer textures if in GBuffer shading mode
	if (ShadingPass->GetShadingMode() == EMetaballShadingMode::GBuffer)
	{
		const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
		const FSceneTextures& SceneTexturesRef = ViewInfo.GetSceneTextures();
		IntermediateTextures.GBufferATexture = SceneTexturesRef.GBufferA;
		IntermediateTextures.GBufferBTexture = SceneTexturesRef.GBufferB;
		IntermediateTextures.GBufferCTexture = SceneTexturesRef.GBufferC;
		IntermediateTextures.GBufferDTexture = SceneTexturesRef.GBufferD;
	}

	// Delegate to ShadingPass
	ShadingPass->RenderForScreenSpacePipeline(
		GraphBuilder,
		View,
		RenderParams,
		IntermediateTextures,
		SceneDepthTexture,
		SceneColorTexture,
		Output);
}
