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
 * 5. Shading - Apply PostProcess shading (Blinn-Phong, Fresnel, Beer's Law)
 *
 * Supports only PostProcess ShadingMode.
 * For GBuffer/Translucent, use RayMarchingPipeline instead.
 */
class FKawaiiMetaballScreenSpacePipeline : public IKawaiiMetaballRenderingPipeline
{
public:
	FKawaiiMetaballScreenSpacePipeline() = default;
	virtual ~FKawaiiMetaballScreenSpacePipeline() = default;

	//========================================
	// IKawaiiMetaballRenderingPipeline Interface
	//========================================

	/** Execute at PostBasePass timing - generates intermediate textures */
	virtual void ExecutePostBasePass(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture,
		FScreenPassRenderTarget Output) override;

	/** Execute at PrePostProcess timing - not used for ScreenSpace pipeline */
	virtual void ExecutePrePostProcess(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output,
		FRDGTextureRef GBufferATexture = nullptr,
		FRDGTextureRef GBufferDTexture = nullptr) override;

	/** Prepare intermediate textures for Tonemap shading (depth, normal, thickness) */
	virtual void PrepareForTonemap(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture) override;

	/** Execute at Tonemap timing - applies PostProcess shading */
	virtual void ExecuteTonemap(
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

private:
	/** Cached intermediate textures from PostBasePass for use in Tonemap */
	FMetaballIntermediateTextures CachedIntermediateTextures;

	// Note: Shading methods are in KawaiiScreenSpaceShadingImpl.h/cpp
	// This pipeline delegates to KawaiiScreenSpaceShading::* namespace functions
};
