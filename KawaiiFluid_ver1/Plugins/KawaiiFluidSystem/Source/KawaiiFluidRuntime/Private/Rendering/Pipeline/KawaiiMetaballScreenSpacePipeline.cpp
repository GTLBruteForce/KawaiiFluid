// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Pipeline/KawaiiMetaballScreenSpacePipeline.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/FluidDepthPass.h"
#include "Rendering/FluidSmoothingPass.h"
#include "Rendering/FluidNormalPass.h"
#include "Rendering/FluidThicknessPass.h"

// Separated shading implementation
#include "Rendering/Shading/KawaiiScreenSpaceShadingImpl.h"
#include "Rendering/FluidSurfaceDecorationPass.h"
#include "Rendering/FluidFlowAccumulationPass.h"

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
	float BlurRadius = static_cast<float>(RenderParams.SmoothingRadius);

	// Calculate DepthFalloff considering anisotropy
	// When anisotropy is enabled, ellipsoids become flat and create larger depth jumps at edges
	// We need to increase DepthFalloff to accommodate these larger depth differences
	float DepthFalloff = AverageParticleRadius * 0.7f;
	if (RenderParams.AnisotropyParams.bEnabled)
	{
		// Anisotropy can stretch particles up to AnisotropyMax (default 2.5) ratio
		// This creates depth jumps proportional to the stretch, so multiply DepthFalloff accordingly
		float AnisotropyMultiplier = FMath::Max(1.0f, RenderParams.AnisotropyParams.AnisotropyMax);
		DepthFalloff *= AnisotropyMultiplier * 2.0f;
	}
	int32 NumIterations = 3;

	// 1. Depth Pass (outputs linear depth + screen-space velocity + occlusion mask)
	FRDGTextureRef DepthTexture = nullptr;
	FRDGTextureRef VelocityTexture = nullptr;
	FRDGTextureRef OcclusionMaskTexture = nullptr;
	RenderFluidDepthPass(GraphBuilder, View, Renderers, SceneDepthTexture, DepthTexture, VelocityTexture, OcclusionMaskTexture);

	if (!DepthTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Depth pass failed"));
		return false;
	}

	// 2. Smoothing Pass - Narrow-Range Filter (Truong & Yuksel 2018)
	FRDGTextureRef SmoothedDepthTexture = nullptr;

	// Calculate adjusted particle radius for anisotropy
	float AdjustedParticleRadius = AverageParticleRadius;
	if (RenderParams.AnisotropyParams.bEnabled)
	{
		float AnisotropyMultiplier = FMath::Max(1.0f, RenderParams.AnisotropyParams.AnisotropyMax);
		AdjustedParticleRadius *= AnisotropyMultiplier;
	}

	RenderFluidNarrowRangeSmoothingPass(GraphBuilder, View, DepthTexture, SmoothedDepthTexture,
	                                    BlurRadius, AdjustedParticleRadius,
	                                    RenderParams.NarrowRangeThresholdRatio,
	                                    RenderParams.NarrowRangeClampRatio,
	                                    NumIterations,
	                                    RenderParams.NarrowRangeGrazingBoost);

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

	// 5. Thickness Smoothing Pass - smooth out individual particle profiles
	FRDGTextureRef SmoothedThicknessTexture = nullptr;
	RenderFluidThicknessSmoothingPass(GraphBuilder, View, ThicknessTexture, SmoothedThicknessTexture,
	                                  BlurRadius, 2);  // 2 iterations for thickness

	if (!SmoothedThicknessTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballScreenSpacePipeline: Thickness smoothing pass failed"));
		SmoothedThicknessTexture = ThicknessTexture;  // Fallback to unsmoothed
	}

	// Build output
	OutIntermediateTextures.SmoothedDepthTexture = SmoothedDepthTexture;
	OutIntermediateTextures.NormalTexture = NormalTexture;
	OutIntermediateTextures.ThicknessTexture = SmoothedThicknessTexture;
	OutIntermediateTextures.VelocityTexture = VelocityTexture;
	OutIntermediateTextures.OcclusionMaskTexture = OcclusionMaskTexture;

	return true;
}

void FKawaiiMetaballScreenSpacePipeline::ExecutePostBasePass(
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

void FKawaiiMetaballScreenSpacePipeline::PrepareRender(
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

	// IMPORTANT: Clear AccumulatedFlowTexture to prevent stale pointer from previous frame
	// RDG textures are only valid within the same frame's RDG graph
	CachedIntermediateTextures.AccumulatedFlowTexture = nullptr;

	// Flow Accumulation Pass (if enabled and using particle velocity)
	if (RenderParams.SurfaceDecoration.bEnabled &&
		RenderParams.SurfaceDecoration.FlowMap.bEnabled &&
		RenderParams.SurfaceDecoration.FlowMap.bUseParticleVelocity &&
		CachedIntermediateTextures.VelocityTexture != nullptr)
	{
		// Get previous accumulated flow from history
		FRDGTextureRef PrevAccumulatedFlowTexture = nullptr;
		if (PrevAccumulatedFlowRT.IsValid())
		{
			PrevAccumulatedFlowTexture = GraphBuilder.RegisterExternalTexture(
				PrevAccumulatedFlowRT,
				TEXT("FluidPrevAccumulatedFlow"));
		}

		// Setup flow accumulation parameters from surface decoration settings
		FFlowAccumulationParams FlowParams;
		FlowParams.VelocityScale = RenderParams.SurfaceDecoration.FlowMap.VelocityScale;
		FlowParams.FlowDecay = RenderParams.SurfaceDecoration.FlowMap.FlowDecay;
		FlowParams.MaxFlowOffset = RenderParams.SurfaceDecoration.FlowMap.MaxFlowOffset;

		// Current frame's matrices for world position reconstruction
		FlowParams.InvViewProjectionMatrix = View.ViewMatrices.GetInvViewProjectionMatrix();
		FlowParams.InvViewMatrix = View.ViewMatrices.GetInvViewMatrix();
		FlowParams.InvProjectionMatrix = View.ViewMatrices.GetInvProjectionMatrix();

		// Previous frame's view-projection for temporal reprojection
		// Use current frame's matrix if no previous data (first frame)
		FlowParams.PrevViewProjectionMatrix = bHasPrevFrameData
			? PrevViewProjectionMatrix
			: View.ViewMatrices.GetViewProjectionMatrix();

		// Run flow accumulation pass
		FRDGTextureRef AccumulatedFlowTexture = nullptr;
		RenderFluidFlowAccumulationPass(
			GraphBuilder,
			View,
			FlowParams,
			CachedIntermediateTextures.VelocityTexture,
			CachedIntermediateTextures.SmoothedDepthTexture,
			PrevAccumulatedFlowTexture,
			AccumulatedFlowTexture);

		// Store in intermediate textures
		CachedIntermediateTextures.AccumulatedFlowTexture = AccumulatedFlowTexture;

		// Extract to history for next frame
		if (AccumulatedFlowTexture)
		{
			GraphBuilder.QueueTextureExtraction(AccumulatedFlowTexture, &PrevAccumulatedFlowRT);
		}

		// Save current frame's ViewProjectionMatrix for next frame's reprojection
		PrevViewProjectionMatrix = View.ViewMatrices.GetViewProjectionMatrix();
		bHasPrevFrameData = true;
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

void FKawaiiMetaballScreenSpacePipeline::ExecuteRender(
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

	// Check if Surface Decoration is enabled
	const bool bUseSurfaceDecoration = RenderParams.SurfaceDecoration.bEnabled;

	if (bUseSurfaceDecoration)
	{
		// Surface Decoration enabled: render to intermediate texture first
		// Use same size as Output.Texture to avoid copy issues
		FIntPoint OutputTextureSize = Output.Texture->Desc.Extent;

		// Create intermediate composite result texture (same size as output)
		FRDGTextureDesc CompositeDesc = FRDGTextureDesc::Create2D(
			OutputTextureSize,
			PF_FloatRGBA,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
		FRDGTextureRef CompositeResultTexture = GraphBuilder.CreateTexture(
			CompositeDesc, TEXT("FluidCompositeResult"));

		// Create intermediate render target for composite pass (use Output's ViewRect)
		FScreenPassRenderTarget IntermediateOutput;
		IntermediateOutput.Texture = CompositeResultTexture;
		IntermediateOutput.ViewRect = Output.ViewRect;  // Same ViewRect as final output
		IntermediateOutput.LoadAction = ERenderTargetLoadAction::EClear;

		// Copy scene color to intermediate (same size, safe to use CopyTexture)
		AddCopyTexturePass(GraphBuilder, SceneColorTexture, CompositeResultTexture);

		// Render fluid composite to intermediate texture
		KawaiiScreenSpaceShading::RenderPostProcessShading(
			GraphBuilder,
			View,
			RenderParams,
			CachedIntermediateTextures,
			SceneDepthTexture,
			SceneColorTexture,
			IntermediateOutput);

		// Apply Surface Decoration and output to final target
		FRDGTextureRef DecoratedTexture = nullptr;
		RenderFluidSurfaceDecorationPass(
			GraphBuilder,
			View,
			RenderParams.SurfaceDecoration,
			CachedIntermediateTextures.SmoothedDepthTexture,
			CachedIntermediateTextures.NormalTexture,
			CachedIntermediateTextures.ThicknessTexture,
			CompositeResultTexture,
			CachedIntermediateTextures.VelocityTexture,         // Screen-space velocity
			CachedIntermediateTextures.AccumulatedFlowTexture,  // Accumulated flow UV offset
			CachedIntermediateTextures.OcclusionMaskTexture,    // Occlusion mask for depth culling
			Output.ViewRect,  // Pass the actual ViewRect where fluid was rendered
			DecoratedTexture);

		// Copy decorated result to final output (same size now)
		AddCopyTexturePass(GraphBuilder, DecoratedTexture, Output.Texture);
	}
	else
	{
		// Surface Decoration disabled: render directly to output (no overhead)
		KawaiiScreenSpaceShading::RenderPostProcessShading(
			GraphBuilder,
			View,
			RenderParams,
			CachedIntermediateTextures,
			SceneDepthTexture,
			SceneColorTexture,
			Output);
	}
}
