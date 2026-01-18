// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "Rendering/Pipeline/IKawaiiMetaballRenderingPipeline.h"
#include "Rendering/RayMarching/FluidVolumeBuilder.h"

/**
 * RayMarching Pipeline for Metaball Rendering
 *
 * Volumetric rendering using 3D density volume and ray marching.
 * Implements all 8 optimization techniques from RayMarching_Rendering_Spec.md:
 *
 * 1. Sparse Volume + Z-Order hybrid - Uses Z-Order sorted particles for volume building
 * 2. Hierarchical Ray Marching - MinMax mipmap for empty space skipping
 * 3. Front-to-Back + Early Termination - Stops at alpha >= 0.99
 * 4. Occupancy Bitmask - 32³ bits (4KB) for O(1) empty block detection
 * 5. Conservative Depth Bounds - Uses scene depth to limit ray extent
 * 6. Tile-based Culling - Skips 16x16 tiles with no fluid
 * 7. Temporal Reprojection - Reuses ~90% of previous frame data
 * 8. Adaptive Step Size - Larger steps in empty regions
 *
 * Target: ~3ms at 1080p with 256³ volume
 */
class KAWAIIFLUIDRUNTIME_API FKawaiiRayMarchingPipeline : public IKawaiiMetaballRenderingPipeline
{
public:
	FKawaiiRayMarchingPipeline();
	virtual ~FKawaiiRayMarchingPipeline();

	//========================================
	// IKawaiiMetaballRenderingPipeline Interface
	//========================================

	/** Execute at PostBasePass timing - prepares volumes for ray marching */
	virtual void ExecutePostBasePass(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture) override;

	/** Execute at PrePostProcess timing - main ray marching pass */
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

	/** Prepare intermediate textures for ray marching */
	virtual void PrepareRender(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture) override;

	/** Execute at Tonemap timing - temporal blending and final composite */
	virtual void ExecuteRender(
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

	virtual const FMetaballIntermediateTextures* GetCachedIntermediateTextures() const override
	{
		return CachedIntermediateTextures.IsValid() ? &CachedIntermediateTextures : nullptr;
	}

private:
	//========================================
	// Volume Building
	//========================================

	/** Volume builder for creating density volume and optimizations */
	TUniquePtr<FFluidVolumeBuilder> VolumeBuilder;

	/** Cached volume textures from current frame */
	FFluidVolumeTextures CachedVolumeTextures;

	//========================================
	// Tile Culling
	//========================================

	/** Execute tile culling pass */
	void ExecuteTileCulling(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		FRDGTextureRef SceneDepthTexture,
		FRDGBufferRef& OutTileVisibility,
		FRDGBufferRef& OutIndirectArgs);

	//========================================
	// Ray Marching
	//========================================

	/** Execute main ray marching pass */
	void ExecuteRayMarching(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FRDGBufferRef TileVisibility,
		FRDGTextureRef& OutFluidColor,
		FRDGTextureRef& OutFluidDepth);

	//========================================
	// Temporal
	//========================================

	/** Execute temporal blending pass */
	void ExecuteTemporalBlend(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		FRDGTextureRef CurrentColor,
		FRDGTextureRef CurrentDepth,
		FScreenPassRenderTarget Output);

	/** Execute direct composite (without temporal blending) */
	void ExecuteDirectComposite(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		FRDGTextureRef FluidColor,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output);

	//========================================
	// Cached State
	//========================================

	/** Cached Z-Order input for Hybrid Mode (from PrepareRender to ExecuteRayMarching) */
	FFluidVolumeInput CachedZOrderInput;

	/** Cached intermediate textures */
	FMetaballIntermediateTextures CachedIntermediateTextures;

	/** History color texture for temporal reprojection */
	TRefCountPtr<IPooledRenderTarget> HistoryColorRT;

	/** History depth texture for temporal reprojection */
	TRefCountPtr<IPooledRenderTarget> HistoryDepthRT;

	/** Previous frame's view projection matrix */
	FMatrix PrevViewProjectionMatrix = FMatrix::Identity;

	/** Flag indicating if we have valid history data */
	bool bHasHistoryData = false;

	/** Cached shader map */
	FGlobalShaderMap* GlobalShaderMap = nullptr;
};
