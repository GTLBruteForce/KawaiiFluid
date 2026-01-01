// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/Shading/IKawaiiMetaballShadingPass.h"

/**
 * GBuffer Shading Pass (Legacy)
 *
 * Writes fluid surface to GBuffer for Lumen/VSM integration:
 * - GBufferA: Normal + PerObjectData
 * - GBufferB: Metallic/Specular/Roughness
 * - GBufferC: BaseColor + AO
 * - GBufferD: Custom data
 *
 * Currently only supports ScreenSpace pipeline (RayMarching is skeleton).
 */
class FKawaiiGBufferShading : public IKawaiiMetaballShadingPass
{
public:
	FKawaiiGBufferShading() = default;
	virtual ~FKawaiiGBufferShading() = default;

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

	/** Render using RayMarching pipeline - skeleton (not implemented) */
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
		return EMetaballShadingMode::GBuffer;
	}

	virtual bool SupportsScreenSpacePipeline() const override { return true; }
	virtual bool SupportsRayMarchingPipeline() const override { return false; }  // Skeleton
};
