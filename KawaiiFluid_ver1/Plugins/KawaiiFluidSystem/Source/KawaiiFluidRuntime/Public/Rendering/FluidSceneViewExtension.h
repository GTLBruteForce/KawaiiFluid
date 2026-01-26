// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class UFluidRendererSubsystem;
struct FPostProcessingInputs;

/**
 * Scene View Extension for SSFR rendering pipeline injection
 * Adds custom render passes to Unreal rendering pipeline
 */
class KAWAIIFLUIDRUNTIME_API FFluidSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FFluidSceneViewExtension(const FAutoRegister& AutoRegister, UFluidRendererSubsystem* InSubsystem);
	virtual ~FFluidSceneViewExtension() override;

	// ISceneViewExtension interface
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;

	/**
	 * Called before ViewFamily rendering on render thread
	 * Performs data extraction from GPU simulator to RenderResource
	 */
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

	/**
	 * Subscribe to PostProcessing Pass
	 * Tonemap: Custom mode (post-lighting)
	 */
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass Pass,
		const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;


	/**
	 * PrePostProcess - called after Lighting, before PostProcessing
	 * All fluid rendering (ScreenSpace, RayMarching) happens here
	 * Both GBuffer and SceneColor are at internal resolution here
	 */
	virtual void PrePostProcessPass_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessingInputs& Inputs) override;

	// End of ISceneViewExtension interface

private:
	/** Check if the view belongs to our Subsystem's World */
	bool IsViewFromOurWorld(const FSceneView& InView) const;

	/** Subsystem weak reference */
	TWeakObjectPtr<UFluidRendererSubsystem> Subsystem;
};
