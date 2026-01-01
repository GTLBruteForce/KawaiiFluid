// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"
#include "ScreenPass.h"
#include "Rendering/FluidRenderingParameters.h"
#include "Rendering/Shading/IKawaiiMetaballShadingPass.h"

// Forward declarations
class FRDGBuilder;
class FSceneView;
class UKawaiiFluidMetaballRenderer;

/**
 * Interface for Metaball Rendering Pipelines
 *
 * A Pipeline handles surface computation (how the fluid surface is determined):
 * - ScreenSpace: Depth → Smoothing → Normal → Thickness passes
 * - RayMarching: Direct SDF ray marching from particles
 *
 * Each Pipeline delegates final shading to an IKawaiiMetaballShadingPass.
 */
class IKawaiiMetaballRenderingPipeline
{
public:
	virtual ~IKawaiiMetaballRenderingPipeline() = default;

	/**
	 * Execute the rendering pipeline
	 *
	 * @param GraphBuilder     RDG builder for pass registration
	 * @param View             Scene view for rendering
	 * @param RenderParams     Fluid rendering parameters
	 * @param Renderers        Array of renderers to process
	 * @param SceneDepthTexture Scene depth texture
	 * @param SceneColorTexture Scene color texture
	 * @param Output           Final render target
	 */
	virtual void Execute(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) = 0;

	/** Get the pipeline type */
	virtual EMetaballPipelineType GetPipelineType() const = 0;

	/** Set the shading pass for final rendering */
	void SetShadingPass(TSharedPtr<IKawaiiMetaballShadingPass> InShadingPass)
	{
		ShadingPass = InShadingPass;
	}

	/** Get the current shading pass */
	TSharedPtr<IKawaiiMetaballShadingPass> GetShadingPass() const
	{
		return ShadingPass;
	}

protected:
	/** The shading pass used for final rendering */
	TSharedPtr<IKawaiiMetaballShadingPass> ShadingPass;
};
