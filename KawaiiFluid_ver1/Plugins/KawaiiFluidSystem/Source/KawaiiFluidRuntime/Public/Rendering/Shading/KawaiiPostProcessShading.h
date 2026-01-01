// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/Shading/IKawaiiMetaballShadingPass.h"

/**
 * PostProcess Shading Pass
 *
 * Custom lighting implementation with:
 * - Blinn-Phong specular
 * - Fresnel reflection
 * - Beer's Law absorption
 * - Scene color refraction
 *
 * Supports both ScreenSpace and RayMarching pipelines.
 */
class FKawaiiPostProcessShading : public IKawaiiMetaballShadingPass
{
public:
	FKawaiiPostProcessShading() = default;
	virtual ~FKawaiiPostProcessShading() = default;

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
		return EMetaballShadingMode::PostProcess;
	}

	virtual bool SupportsScreenSpacePipeline() const override { return true; }
	virtual bool SupportsRayMarchingPipeline() const override { return true; }
};
