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
 * 4. Delegate to ShadingPass for ray marching and shading
 *
 * Best for small-to-medium particle counts with SDF-based rendering.
 * Supports SDF Volume optimization for improved performance.
 */
class FKawaiiMetaballRayMarchPipeline : public IKawaiiMetaballRenderingPipeline
{
public:
	FKawaiiMetaballRayMarchPipeline() = default;
	virtual ~FKawaiiMetaballRayMarchPipeline() = default;

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
		return EMetaballPipelineType::RayMarching;
	}

private:
	/** SDF Volume Manager for optimized ray marching */
	FSDFVolumeManager SDFVolumeManager;
};
