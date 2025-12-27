// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "IFluidCompositePass.h"

/**
 * G-Buffer rendering pass (SKELETON - TO BE IMPLEMENTED)
 *
 * This pass writes fluid surface to GBuffer for Lumen/VSM integration
 *
 * IMPLEMENTATION TASKS:
 * 1. Create FluidGBufferWrite.usf shader (MRT output: GBufferA/B/C/D)
 * 2. Create FluidGBufferWriteShaders.h/cpp (shader parameter bindings)
 * 3. Implement RenderComposite() to write to GBuffer
 * 4. Add ViewExtension hook at MotionBlur pass (pre-lighting)
 * 5. Test with Lumen reflections and VSM shadows
 *
 * See implementation guide in IMPLEMENTATION_GUIDE.md
 */
class FFluidGBufferComposite : public IFluidCompositePass
{
public:
	virtual ~FFluidGBufferComposite() = default;

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
		return ESSFRRenderingMode::GBuffer;
	}
};
