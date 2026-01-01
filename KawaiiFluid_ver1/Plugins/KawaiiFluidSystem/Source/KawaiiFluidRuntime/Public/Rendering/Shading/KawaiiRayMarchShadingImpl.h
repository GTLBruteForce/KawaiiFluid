// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "ScreenPass.h"

struct FFluidRenderingParameters;
struct FRayMarchingPipelineData;
class FSceneView;

/**
 * RayMarching Pipeline Shading Implementation
 *
 * Contains shading functions for the RayMarching rendering pipeline.
 * These are stateless functions called by FKawaiiMetaballRayMarchPipeline.
 *
 * Supports:
 * - PostProcess shading mode (Tonemap timing)
 * - GBuffer shading mode (PostBasePass timing)
 * - Translucent shading mode (PostBasePass + PrePostProcess timing)
 */
namespace KawaiiRayMarchShading
{
	/** Stencil reference value for Translucent mode */
	static constexpr uint8 TranslucentStencilRef = 0x01;

	/**
	 * Render GBuffer shading pass
	 *
	 * Writes fluid surface data directly to GBuffer textures (A/B/C/D)
	 * for integration with UE's deferred lighting.
	 *
	 * @param GraphBuilder - RDG builder
	 * @param View - Scene view
	 * @param RenderParams - Fluid rendering parameters
	 * @param PipelineData - Cached pipeline data (particle buffer, SDF volume)
	 * @param SceneDepthTexture - Scene depth for depth testing
	 */
	void RenderGBufferShading(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FRayMarchingPipelineData& PipelineData,
		FRDGTextureRef SceneDepthTexture);

	/**
	 * Render Translucent GBuffer write pass
	 *
	 * Writes fluid surface data to GBuffer with Stencil=0x01 marking
	 * for later transparency compositing in PrePostProcess pass.
	 *
	 * @param GraphBuilder - RDG builder
	 * @param View - Scene view
	 * @param RenderParams - Fluid rendering parameters
	 * @param PipelineData - Cached pipeline data (particle buffer, SDF volume)
	 * @param SceneDepthTexture - Scene depth for depth testing and stencil writing
	 */
	void RenderTranslucentGBufferWrite(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FRayMarchingPipelineData& PipelineData,
		FRDGTextureRef SceneDepthTexture);

	/**
	 * Render Translucent transparency pass
	 *
	 * Applies transparency compositing (refraction, absorption) for pixels
	 * marked with Stencil=0x01 in the previous GBuffer write pass.
	 *
	 * @param GraphBuilder - RDG builder
	 * @param View - Scene view
	 * @param RenderParams - Fluid rendering parameters
	 * @param SceneDepthTexture - Scene depth with stencil marking
	 * @param SceneColorTexture - Lit scene color (after Lumen/VSM)
	 * @param Output - Render target to composite onto
	 * @param GBufferATexture - Normals for refraction direction
	 * @param GBufferDTexture - Custom data (thickness for Beer's Law)
	 */
	void RenderTranslucentTransparency(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output,
		FRDGTextureRef GBufferATexture,
		FRDGTextureRef GBufferDTexture);

	/**
	 * Render PostProcess shading pass
	 *
	 * Applies ray marching with custom lighting (Blinn-Phong, Fresnel, SSS)
	 * as a post-process effect at Tonemap timing.
	 *
	 * @param GraphBuilder - RDG builder
	 * @param View - Scene view
	 * @param RenderParams - Fluid rendering parameters
	 * @param PipelineData - Cached pipeline data (particle buffer, SDF volume)
	 * @param SceneDepthTexture - Scene depth for ray start position
	 * @param SceneColorTexture - Scene color for environment sampling
	 * @param Output - Render target to composite onto
	 */
	void RenderPostProcessShading(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FRayMarchingPipelineData& PipelineData,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output);
}
