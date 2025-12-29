// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "IFluidCompositePass.h"
#include "RenderGraphResources.h"

/**
 * Ray Marching SDF rendering pass
 *
 * Implements ray marching through metaball SDF field for smooth fluid surfaces.
 * Best suited for slime-like fluids with:
 * - Fresnel reflection
 * - Subsurface scattering (SSS) for jelly effect
 * - Refraction
 * - Specular highlights
 *
 * Unlike Custom/GBuffer modes, this doesn't use intermediate Depth/Normal/Thickness
 * passes - everything is computed in a single ray marching pass.
 */
class FFluidRayMarchComposite : public IFluidCompositePass
{
public:
	virtual ~FFluidRayMarchComposite() = default;

	/**
	 * Set particle data for SDF calculation
	 * Must be called before RenderComposite
	 *
	 * @param InParticleBufferSRV Particle positions buffer (StructuredBuffer<FVector3f>)
	 * @param InParticleCount Number of particles
	 * @param InParticleRadius Particle radius for SDF
	 */
	void SetParticleData(
		FRDGBufferSRVRef InParticleBufferSRV,
		int32 InParticleCount,
		float InParticleRadius);

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
		return ESSFRRenderingMode::RayMarching;
	}

private:
	/** Particle buffer SRV for shader access */
	FRDGBufferSRVRef ParticleBufferSRV = nullptr;

	/** Number of particles */
	int32 ParticleCount = 0;

	/** Particle radius for SDF calculation */
	float ParticleRadius = 5.0f;
};
