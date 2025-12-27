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
 * Fluid Thickness 렌더링 패스 (Legacy path)
 * 파티클의 두께를 누적하여 렌더링
 * Subsystem에서 모든 렌더러를 수집하여 렌더링
 */
void RenderFluidThicknessPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	FRDGTextureRef& OutThicknessTexture);

/**
 * Fluid Thickness 렌더링 패스 (Batched path)
 * 지정된 렌더러 리스트만 렌더링 (배치 최적화용)
 */
void RenderFluidThicknessPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const TArray<UKawaiiFluidSSFRRenderer*>& Renderers,
	FRDGTextureRef& OutThicknessTexture);
