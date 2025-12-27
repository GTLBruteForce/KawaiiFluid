// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FSceneView;
class UFluidRendererSubsystem;
class FRDGTexture;
class UKawaiiFluidSSFRRenderer;
typedef FRDGTexture* FRDGTextureRef;

/**
 * Fluid Depth 렌더링 패스 (Legacy path)
 * 파티클을 depth buffer에 렌더링
 * Subsystem에서 모든 렌더러를 수집하여 렌더링
 */
void RenderFluidDepthPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutLinearDepthTexture);

/**
 * Fluid Depth 렌더링 패스 (Batched path)
 * 지정된 렌더러 리스트만 렌더링 (배치 최적화용)
 */
void RenderFluidDepthPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const TArray<UKawaiiFluidSSFRRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutLinearDepthTexture);
