// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class UFluidRendererSubsystem;

/**
 * SSFR 렌더링 파이프라인 인젝션을 위한 Scene View Extension
 * 언리얼 렌더링 파이프라인에 커스텀 렌더 패스 추가
 */
class KAWAIIFLUIDRUNTIME_API FFluidSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FFluidSceneViewExtension(const FAutoRegister& AutoRegister, UFluidRendererSubsystem* InSubsystem);
	virtual ~FFluidSceneViewExtension();

	// ISceneViewExtension interface
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	/**
	 * Post Opaque 렌더링 후 호출 (투명 오브젝트 전)
	 * 여기서 SSFR 파이프라인 실행
	 */
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs) override;
	// End of ISceneViewExtension interface

private:
	/** Subsystem 약한 참조 */
	TWeakObjectPtr<UFluidRendererSubsystem> Subsystem;

	/** Depth 렌더링 패스 */
	void RenderDepthPass(FRDGBuilder& GraphBuilder, const FSceneView& View);

	/** Depth Smoothing 패스 */
	void RenderSmoothingPass(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTextureRef InputDepthTexture, FRDGTextureRef& OutSmoothedDepthTexture);

	/** Normal 재구성 패스 */
	void RenderNormalPass(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTextureRef SmoothedDepthTexture, FRDGTextureRef& OutNormalTexture);

	/** Thickness 렌더링 패스 */
	void RenderThicknessPass(FRDGBuilder& GraphBuilder, const FSceneView& View);

	/** Final Shading 패스 */
	void RenderShadingPass(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs);
};
