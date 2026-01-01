// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/Shading/IKawaiiMetaballShadingPass.h"

/**
 * Opaque Shading Pass (Experimental)
 *
 * Placeholder for future opaque rendering mode.
 * Currently not implemented.
 *
 * Supports both ScreenSpace and RayMarching pipelines.
 */
class FKawaiiOpaqueShading : public IKawaiiMetaballShadingPass
{
public:
	FKawaiiOpaqueShading() = default;
	virtual ~FKawaiiOpaqueShading() = default;

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
		return EMetaballShadingMode::Opaque;
	}

	virtual bool SupportsScreenSpacePipeline() const override { return false; }
	virtual bool SupportsRayMarchingPipeline() const override { return false; }
};
