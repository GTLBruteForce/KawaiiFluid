// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Rendering/Composite/FluidTransparencyComposite.h"

class UFluidRendererSubsystem;

/**
 * SSFR 렌더링 파이프라인 인젝션을 위한 Scene View Extension
 * 언리얼 렌더링 파이프라인에 커스텀 렌더 패스 추가
 */
class KAWAIIFLUIDRUNTIME_API FFluidSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FFluidSceneViewExtension(const FAutoRegister& AutoRegister, UFluidRendererSubsystem* InSubsystem);
	virtual ~FFluidSceneViewExtension() override;

	// ISceneViewExtension interface
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	/**
	 * PostProcessing Pass 구독
	 * Tonemap: Custom mode (post-lighting)
	 */
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass Pass,
		const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;

	/**
	 * GBuffer mode rendering - called right after BasePass, before Lighting
	 * This is the correct injection point for GBuffer write
	 */
	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

	// End of ISceneViewExtension interface

private:
	/** Check if the view belongs to our Subsystem's World */
	bool IsViewFromOurWorld(const FSceneView& InView) const;

	/** Subsystem 약한 참조 */
	TWeakObjectPtr<UFluidRendererSubsystem> Subsystem;

	/** Transparency composite for Translucent shading mode */
	FFluidTransparencyComposite TransparencyComposite;
};
