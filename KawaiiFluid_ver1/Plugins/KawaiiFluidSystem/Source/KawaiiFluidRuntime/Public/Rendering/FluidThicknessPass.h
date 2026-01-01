// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneView;
class FRDGTexture;
class UKawaiiFluidMetaballRenderer;
typedef FRDGTexture* FRDGTextureRef;

/**
 * Fluid Thickness 렌더링 패스 (Batched path)
 * 지정된 렌더러 리스트만 렌더링 (배치 최적화용)
 */
void RenderFluidThicknessPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutThicknessTexture);
