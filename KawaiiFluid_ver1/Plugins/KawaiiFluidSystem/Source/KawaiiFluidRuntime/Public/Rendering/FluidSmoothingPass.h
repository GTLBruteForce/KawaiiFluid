// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"

class FSceneView;
class UFluidRendererSubsystem;

/**
 * Bilateral Gaussian Blur for Fluid Depth Smoothing
 *
 * Applies separable bilateral filter (horizontal + vertical passes)
 * to smooth the depth buffer while preserving sharp edges.
 */
void RenderFluidSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float BlurRadius = 5.0f,
	float DepthFalloff = 0.05f);
