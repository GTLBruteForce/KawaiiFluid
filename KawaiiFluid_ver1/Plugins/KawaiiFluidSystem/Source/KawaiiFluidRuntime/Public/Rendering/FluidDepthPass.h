// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneView;
class FRDGTexture;
class UKawaiiFluidMetaballRenderer;
typedef FRDGTexture* FRDGTextureRef;

/**
 * Fluid Depth 렌더링 패스 (Batched path)
 * 지정된 렌더러 리스트만 렌더링 (배치 최적화용)
 */
void RenderFluidDepthPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutLinearDepthTexture);
