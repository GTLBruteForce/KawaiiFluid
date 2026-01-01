// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/Shading/IKawaiiMetaballShadingPass.h"

/**
 * Translucent Shading Pass (Experimental)
 *
 * Placeholder for future translucent rendering mode.
 * Currently not implemented.
 *
 * Supports both ScreenSpace and RayMarching pipelines.
 */
class FKawaiiTranslucentShading : public IKawaiiMetaballShadingPass
{
public:
	FKawaiiTranslucentShading() = default;
	virtual ~FKawaiiTranslucentShading() = default;

	//========================================
	// IKawaiiMetaballShadingPass Interface
	//========================================

	/** Render using ScreenSpace pipeline intermediate textures */
	virtual void RenderForScreenSpacePipeline(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FMetaballIntermediateTextures& IntermediateTextures,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) override;

	/** Render using RayMarching pipeline particle buffer */
	virtual void RenderForRayMarchingPipeline(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		FRDGBufferSRVRef ParticleBufferSRV,
		int32 ParticleCount,
		float ParticleRadius,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) override;

	virtual EMetaballShadingMode GetShadingMode() const override
	{
		return EMetaballShadingMode::Translucent;
	}

	virtual bool SupportsScreenSpacePipeline() const override { return false; }
	virtual bool SupportsRayMarchingPipeline() const override { return true; }  // Translucent mode only supports RayMarching
};
