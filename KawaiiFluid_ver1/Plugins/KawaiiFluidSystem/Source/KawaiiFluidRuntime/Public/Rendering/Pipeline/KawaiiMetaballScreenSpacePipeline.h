// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/Pipeline/IKawaiiMetaballRenderingPipeline.h"

/**
 * ScreenSpace Pipeline for Metaball Rendering
 *
 * Surface computation method:
 * 1. Depth Pass - Render fluid particles to depth buffer
 * 2. Smoothing Pass - Bilateral filter on depth for smooth surface
 * 3. Normal Pass - Reconstruct normals from smoothed depth
 * 4. Thickness Pass - Accumulate particle thickness
 * 5. Shading Pass - Delegate to IKawaiiMetaballShadingPass
 *
 * Best for moderate particle counts with high visual quality.
 */
class FKawaiiMetaballScreenSpacePipeline : public IKawaiiMetaballRenderingPipeline
{
public:
	FKawaiiMetaballScreenSpacePipeline() = default;
	virtual ~FKawaiiMetaballScreenSpacePipeline() = default;

	//========================================
	// IKawaiiMetaballRenderingPipeline Interface
	//========================================

	virtual void Execute(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) override;

	virtual EMetaballPipelineType GetPipelineType() const override
	{
		return EMetaballPipelineType::ScreenSpace;
	}
};
