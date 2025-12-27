// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "ScreenPass.h"
#include "Rendering/FluidRenderingParameters.h"

class FSceneView;
struct FFluidRenderingParameters;

/**
 * Intermediate texture bundle from shared passes (Depth/Smoothing/Normal/Thickness)
 */
struct FFluidIntermediateTextures
{
	/** Smoothed depth texture (from bilateral filter) */
	FRDGTextureRef SmoothedDepthTexture = nullptr;

	/** Reconstructed normal texture (view-space) */
	FRDGTextureRef NormalTexture = nullptr;

	/** Accumulated thickness texture */
	FRDGTextureRef ThicknessTexture = nullptr;
};

/**
 * Interface for SSFR composite/final rendering pass
 *
 * Two implementations:
 * - Custom: Custom lighting (Blinn-Phong, manual Fresnel)
 * - GBuffer: GBuffer write for Lumen/VSM (to be implemented by team member)
 */
class IFluidCompositePass
{
public:
	virtual ~IFluidCompositePass() = default;

	/**
	 * Execute composite/final rendering pass
	 *
	 * @param GraphBuilder RDG builder
	 * @param View Scene view
	 * @param RenderParams Rendering parameters (contains mode and visual settings)
	 * @param IntermediateTextures Depth/Normal/Thickness from shared passes
	 * @param SceneDepthTexture Scene depth buffer
	 * @param SceneColorTexture Scene color (for Custom mode refraction)
	 * @param Output Output render target
	 */
	virtual void RenderComposite(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FFluidIntermediateTextures& IntermediateTextures,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) = 0;

	/** Get rendering mode this pass implements */
	virtual ESSFRRenderingMode GetRenderingMode() const = 0;
};
