// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/Pipeline/IKawaiiMetaballRenderingPipeline.h"
#include "Rendering/SDFVolumeManager.h"

/**
 * RayMarching Pipeline for Metaball Rendering
 *
 * Surface computation method:
 * 1. Collect particles from all renderers
 * 2. Create particle buffer for GPU
 * 3. (Optional) Bake SDF to 3D volume texture for O(1) lookup
 * 4. Apply shading based on ShadingMode
 *
 * Supports all shading modes:
 * - GBuffer: Write to GBuffer textures (skeleton)
 * - PostProcess: Ray march with PostProcess shading
 * - Translucent: Two-stage (GBuffer+Stencil, then transparency)
 */
class FKawaiiMetaballRayMarchPipeline : public IKawaiiMetaballRenderingPipeline
{
public:
	FKawaiiMetaballRayMarchPipeline() = default;
	virtual ~FKawaiiMetaballRayMarchPipeline() = default;

	//========================================
	// IKawaiiMetaballRenderingPipeline Interface
	//========================================

	/** Execute at PostBasePass timing - GBuffer write or particle buffer prep */
	virtual void ExecutePostBasePass(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture) override;

	/** Execute at PrePostProcess timing - Translucent transparency compositing */
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

	/** Prepare particle buffer and SDF for Tonemap shading */
	virtual void PrepareForTonemap(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture) override;

	/** Execute at Tonemap timing - PostProcess ray march shading */
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
		return EMetaballPipelineType::RayMarching;
	}

private:
	/** SDF Volume Manager for optimized ray marching */
	FSDFVolumeManager SDFVolumeManager;

	/** Cached pipeline data from PostBasePass for use in PrePostProcess/Tonemap */
	FRayMarchingPipelineData CachedPipelineData;

	/** Pooled buffer for GPU bounds readback (double-buffered for async readback) */
	TRefCountPtr<FRDGPooledBuffer> PendingBoundsReadbackBuffer;
	bool bHasPendingBoundsReadback = false;

	//========================================
	// Particle Buffer Preparation
	//========================================

	/** Prepare particle buffer from renderers (common to all shading modes) */
	bool PrepareParticleBuffer(
		FRDGBuilder& GraphBuilder,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers);

	/** Process pending bounds readback from previous frame */
	void ProcessPendingBoundsReadback();

	// Note: Shading methods are in KawaiiRayMarchShadingImpl.h/cpp
	// This pipeline delegates to KawaiiRayMarchShading::* namespace functions
};
