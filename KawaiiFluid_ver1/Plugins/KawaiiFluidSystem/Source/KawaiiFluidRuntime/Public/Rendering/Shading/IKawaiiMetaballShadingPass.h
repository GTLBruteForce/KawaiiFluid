// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "ScreenPass.h"
#include "Rendering/FluidRenderingParameters.h"

/**
 * Intermediate textures produced by ScreenSpace pipeline
 * These are passed to ShadingPass for final rendering
 */
struct FMetaballIntermediateTextures
{
	/** Smoothed depth texture (bilateral filtered) */
	FRDGTextureRef SmoothedDepthTexture = nullptr;

	/** View-space normal texture */
	FRDGTextureRef NormalTexture = nullptr;

	/** Accumulated thickness texture */
	FRDGTextureRef ThicknessTexture = nullptr;

	/** GBuffer textures (optional, for GBuffer shading mode) */
	FRDGTextureRef GBufferATexture = nullptr;
	FRDGTextureRef GBufferBTexture = nullptr;
	FRDGTextureRef GBufferCTexture = nullptr;
	FRDGTextureRef GBufferDTexture = nullptr;

	bool IsValid() const
	{
		return SmoothedDepthTexture != nullptr &&
		       NormalTexture != nullptr &&
		       ThicknessTexture != nullptr;
	}
};

/**
 * SDF Volume data for Ray Marching optimization
 * Baked 3D SDF texture for O(1) distance lookup
 */
struct FSDFVolumeData
{
	/** Whether to use SDF Volume optimization */
	bool bUseSDFVolume = false;

	/** SDF Volume texture SRV */
	FRDGTextureSRVRef SDFVolumeTextureSRV = nullptr;

	/** Volume bounds in world space */
	FVector3f VolumeMin = FVector3f::ZeroVector;
	FVector3f VolumeMax = FVector3f::ZeroVector;

	/** Volume resolution (voxels per dimension) */
	FIntVector VolumeResolution = FIntVector(64, 64, 64);

	bool IsValid() const
	{
		return bUseSDFVolume && SDFVolumeTextureSRV != nullptr;
	}
};

class FSceneView;
class FRDGBuilder;

/**
 * Metaball Shading Pass Interface
 *
 * Each shading mode (PostProcess, GBuffer, Opaque, Translucent) implements this interface.
 * Each implementation provides both ScreenSpace and RayMarching rendering methods.
 *
 * The Pipeline calls the appropriate method based on its type:
 * - ScreenSpace Pipeline -> RenderForScreenSpacePipeline()
 * - RayMarching Pipeline -> RenderForRayMarchingPipeline()
 */
class IKawaiiMetaballShadingPass
{
public:
	virtual ~IKawaiiMetaballShadingPass() = default;

	//========================================
	// Rendering Methods (called by Pipeline)
	//========================================

	/**
	 * Render using ScreenSpace pipeline intermediate textures
	 * Called by FKawaiiMetaballScreenSpacePipeline::Execute()
	 *
	 * @param GraphBuilder RDG builder for pass execution
	 * @param View Scene view for rendering
	 * @param RenderParams Rendering parameters
	 * @param IntermediateTextures Depth/Normal/Thickness textures from ScreenSpace passes
	 * @param SceneDepthTexture Scene depth for depth testing
	 * @param SceneColorTexture Scene color for refraction sampling
	 * @param Output Final render target
	 */
	virtual void RenderForScreenSpacePipeline(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FMetaballIntermediateTextures& IntermediateTextures,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) = 0;

	/**
	 * Render using RayMarching pipeline particle buffer
	 * Called by FKawaiiMetaballRayMarchPipeline::Execute()
	 *
	 * @param GraphBuilder RDG builder for pass execution
	 * @param View Scene view for rendering
	 * @param RenderParams Rendering parameters
	 * @param ParticleBufferSRV Particle positions buffer
	 * @param ParticleCount Number of particles
	 * @param ParticleRadius Average particle radius
	 * @param SceneDepthTexture Scene depth for depth testing
	 * @param SceneColorTexture Scene color for refraction sampling
	 * @param Output Final render target
	 */
	virtual void RenderForRayMarchingPipeline(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		FRDGBufferSRVRef ParticleBufferSRV,
		int32 ParticleCount,
		float ParticleRadius,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) = 0;

	//========================================
	// Mode Query
	//========================================

	/** Get the shading mode this pass implements */
	virtual EMetaballShadingMode GetShadingMode() const = 0;

	/** Check if this shading mode supports ScreenSpace pipeline */
	virtual bool SupportsScreenSpacePipeline() const { return true; }

	/** Check if this shading mode supports RayMarching pipeline */
	virtual bool SupportsRayMarchingPipeline() const { return true; }

	//========================================
	// SDF Volume Data (for RayMarching optimization)
	//========================================

	/** Set SDF Volume data for optimized ray marching */
	virtual void SetSDFVolumeData(const FSDFVolumeData& InSDFVolumeData)
	{
		SDFVolumeData = InSDFVolumeData;
	}

	/** Get current SDF Volume data */
	const FSDFVolumeData& GetSDFVolumeData() const { return SDFVolumeData; }

	/** Check if using SDF Volume optimization */
	bool IsUsingSDFVolume() const { return SDFVolumeData.IsValid(); }

protected:
	/** Cached SDF Volume data */
	FSDFVolumeData SDFVolumeData;
};

// Backwards compatibility alias
using IKawaiiFluidShadingPass = IKawaiiMetaballShadingPass;
