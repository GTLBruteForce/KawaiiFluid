// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "ScreenPass.h"

struct FFluidRenderingParameters;
struct FMetaballIntermediateTextures;
class FSceneView;

/**
 * ScreenSpace Pipeline Shading Implementation
 *
 * Contains shading functions for the ScreenSpace rendering pipeline.
 * These are stateless functions called by FKawaiiMetaballScreenSpacePipeline.
 *
 * Supports:
 * - GBuffer shading mode (writes to GBuffer for deferred lighting)
 * - PostProcess shading mode (custom shading at Tonemap timing)
 */
namespace KawaiiScreenSpaceShading
{
	/**
	 * Render GBuffer shading pass (writes to GBuffer A/B/C/D)
	 *
	 * Writes fluid surface data to GBuffer for integration with
	 * Unreal's deferred lighting (Lumen, VSM, GI).
	 *
	 * @param GraphBuilder - RDG builder
	 * @param View - Scene view
	 * @param RenderParams - Fluid rendering parameters
	 * @param IntermediateTextures - Intermediate textures (depth, normal, thickness + GBuffer refs)
	 * @param SceneDepthTexture - Scene depth texture
	 */
	void RenderGBufferShading(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FMetaballIntermediateTextures& IntermediateTextures,
		FRDGTextureRef SceneDepthTexture);

	/**
	 * Render PostProcess shading pass
	 *
	 * Applies Blinn-Phong lighting, Fresnel, and Beer's Law absorption
	 * using intermediate textures (depth, normal, thickness) from ScreenSpace pipeline.
	 *
	 * @param GraphBuilder - RDG builder
	 * @param View - Scene view
	 * @param RenderParams - Fluid rendering parameters
	 * @param IntermediateTextures - Cached textures from PrepareForTonemap (depth, normal, thickness)
	 * @param SceneDepthTexture - Scene depth for depth comparison
	 * @param SceneColorTexture - Scene color for background sampling
	 * @param Output - Render target to composite onto
	 */
	void RenderPostProcessShading(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FMetaballIntermediateTextures& IntermediateTextures,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output);
}
