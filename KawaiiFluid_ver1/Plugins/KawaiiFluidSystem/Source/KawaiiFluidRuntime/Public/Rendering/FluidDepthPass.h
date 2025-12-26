// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneView;
class UFluidRendererSubsystem;
class FRDGTexture;
typedef FRDGTexture* FRDGTextureRef;

/**
 * Fluid Depth 렌더링 패스
 * 파티클을 depth buffer에 렌더링
 */
void RenderFluidDepthPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutLinearDepthTexture);
