// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "IFluidCompositePass.h"

/**
 * Custom rendering pass
 * Implements custom lighting with Blinn-Phong, Fresnel, Beer's Law
 *
 * This wraps the existing FluidComposite.usf shader logic
 */
class FFluidCustomComposite : public IFluidCompositePass
{
public:
	virtual ~FFluidCustomComposite() = default;

	virtual void RenderComposite(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FFluidRenderingParameters& RenderParams,
		const FFluidIntermediateTextures& IntermediateTextures,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneColorTexture,
		FScreenPassRenderTarget Output) override;

	virtual ESSFRRenderingMode GetRenderingMode() const override
	{
		return ESSFRRenderingMode::Custom;
	}
};
