// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"
#include "ScreenPass.h"
#include "Rendering/FluidRenderingParameters.h"
#include "Rendering/MetaballRenderingData.h"

// Forward declarations
class FRDGBuilder;
class FSceneView;
class UKawaiiFluidMetaballRenderer;

/**
 * Interface for Metaball Rendering Pipelines
 *
 * A Pipeline handles surface computation (how the fluid surface is determined):
 * - ScreenSpace: Depth -> Smoothing -> Normal -> Thickness passes
 * - RayMarching: Direct SDF ray marching from particles
 *
 * Each Pipeline provides three execution points matching UE render callbacks:
 * - ExecutePostBasePass(): PostRenderBasePassDeferred_RenderThread (GBuffer write)
 * - ExecutePrePostProcess(): PrePostProcessPass_RenderThread (Transparency compositing)
 * - ExecuteTonemap(): SubscribeToPostProcessingPass(Tonemap) (PostProcess shading)
 *
 * The Pipeline handles ShadingMode internally via switch statements.
 */
class IKawaiiMetaballRenderingPipeline
{
public:
	virtual ~IKawaiiMetaballRenderingPipeline() = default;

	/**
	 * Execute at PostBasePass timing (PostRenderBasePassDeferred_RenderThread)
	 * Used for: GBuffer write, Translucent Stencil marking, intermediate texture generation
	 *
	 * Called for:
	 * - GBuffer mode: Write to GBuffer textures
	 * - Translucent mode: Write to GBuffer + Stencil=0x01 marking
	 * - PostProcess mode: Generate intermediate textures (depth, normal, thickness)
	 *
	 * @param GraphBuilder     RDG builder for pass registration
	 * @param View             Scene view for rendering
	 * @param RenderParams     Fluid rendering parameters (includes ShadingMode)
	 * @param Renderers        Array of renderers to process
	 * @param SceneDepthTexture Scene depth texture
	 * @param Output           Render target (may be unused for GBuffer write)
	 */
	virtual void ExecutePostBasePass(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture,
		FScreenPassRenderTarget Output) = 0;

	/**
	 * Execute at PrePostProcess timing (PrePostProcessPass_RenderThread)
	 * Used for: Transparency compositing (Translucent mode only)
	 *
	 * Called for:
	 * - Translucent mode: Apply refraction and absorption effects
	 *
	 * @param GraphBuilder     RDG builder for pass registration
	 * @param View             Scene view for rendering
	 * @param RenderParams     Fluid rendering parameters
	 * @param Renderers        Array of renderers to process
	 * @param SceneDepthTexture Scene depth texture (with Stencil marking)
	 * @param SceneColorTexture Lit scene color texture
	 * @param Output           Final render target
	 * @param GBufferATexture  GBuffer A (normals for refraction)
	 * @param GBufferDTexture  GBuffer D (thickness for absorption)
	 */
	virtual void ExecutePrePostProcess(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output,
		FRDGTextureRef GBufferATexture = nullptr,
		FRDGTextureRef GBufferDTexture = nullptr) = 0;

	/**
	 * Prepare data for Tonemap shading (called at Tonemap timing)
	 * Used for: Generating intermediate data needed by ExecuteTonemap
	 *
	 * Called for:
	 * - PostProcess mode: Generate intermediate textures/buffers
	 *   - ScreenSpace: Depth, Normal, Thickness textures
	 *   - RayMarching: Particle buffer, optional SDF volume
	 *
	 * NOTE: This is NOT the same as ExecutePostBasePass.
	 *       ExecutePostBasePass is for GBuffer/Translucent modes at PostBasePass timing.
	 *       PrepareForTonemap is for PostProcess mode at Tonemap timing.
	 *
	 * @param GraphBuilder     RDG builder for pass registration
	 * @param View             Scene view for rendering
	 * @param RenderParams     Fluid rendering parameters
	 * @param Renderers        Array of renderers to process
	 * @param SceneDepthTexture Scene depth texture
	 */
	virtual void PrepareForTonemap(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture) = 0;

	/**
	 * Execute at Tonemap timing (SubscribeToPostProcessingPass - Tonemap)
	 * Used for: PostProcess shading (PostProcess mode only)
	 *
	 * Called for:
	 * - PostProcess mode: Apply custom lighting (Blinn-Phong, Fresnel, Beer's Law)
	 *
	 * NOTE: PrepareForTonemap must be called before this to prepare intermediate data.
	 *
	 * @param GraphBuilder     RDG builder for pass registration
	 * @param View             Scene view for rendering
	 * @param RenderParams     Fluid rendering parameters
	 * @param Renderers        Array of renderers to process
	 * @param SceneDepthTexture Scene depth texture
	 * @param SceneColorTexture Scene color texture
	 * @param Output           Final render target
	 */
	virtual void ExecuteTonemap(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) = 0;

	/** Get the pipeline type */
	virtual EMetaballPipelineType GetPipelineType() const = 0;

protected:
	/**
	 * Utility: Calculate particle bounding box
	 *
	 * @param Positions Particle positions
	 * @param ParticleRadius Average particle radius
	 * @param Margin Additional margin to add
	 * @param OutMin Output minimum bounds
	 * @param OutMax Output maximum bounds
	 */
	static void CalculateParticleBoundingBox(
		const TArray<FVector3f>& Positions,
		float ParticleRadius,
		float Margin,
		FVector3f& OutMin,
		FVector3f& OutMax)
	{
		if (Positions.Num() == 0)
		{
			OutMin = FVector3f::ZeroVector;
			OutMax = FVector3f::ZeroVector;
			return;
		}

		OutMin = Positions[0];
		OutMax = Positions[0];

		for (const FVector3f& Pos : Positions)
		{
			OutMin = FVector3f::Min(OutMin, Pos);
			OutMax = FVector3f::Max(OutMax, Pos);
		}

		// Expand by particle radius + margin
		const float Expansion = ParticleRadius + Margin;
		OutMin -= FVector3f(Expansion);
		OutMax += FVector3f(Expansion);
	}
};
