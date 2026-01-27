// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EdMode.h"

class UKawaiiFluidComponent;
class AKawaiiFluidVolume;
class UKawaiiFluidVolumeComponent;

/**
 * Fluid particle brush editor mode
 * Activated by detail panel button and operates on a specific FluidComponent target
 */
class FFluidBrushEditorMode : public FEdMode
{
public:
	static const FEditorModeID EM_FluidBrush;

	FFluidBrushEditorMode();
	virtual ~FFluidBrushEditorMode() override;

	//~ Begin FEdMode Interface
	virtual void Enter() override;
	virtual void Exit() override;
	virtual bool UsesToolkits() const override { return false; }

	virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	                      FKey Key, EInputEvent Event) override;
	virtual bool HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy,
	                         const FViewportClick& Click) override;
	virtual bool StartTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport) override;
	virtual bool EndTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport) override;
	virtual bool MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	                       int32 x, int32 y) override;
	virtual bool CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	                               int32 InMouseX, int32 InMouseY) override;
	virtual void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	                     const FSceneView* View, FCanvas* Canvas) override;
	virtual bool IsSelectionAllowed(AActor* InActor, bool bInSelected) const override { return false; }
	virtual bool ShouldDrawWidget() const override { return false; }
	virtual bool DisallowMouseDeltaTracking() const override;
	virtual void Tick(FEditorViewportClient* ViewportClient, float DeltaTime) override;
	//~ End FEdMode Interface

	/** Set target component (for KawaiiFluidComponent) */
	void SetTargetComponent(UKawaiiFluidComponent* Component);

	/** Set target volume (for KawaiiFluidVolume) */
	void SetTargetVolume(AKawaiiFluidVolume* Volume);

	/** Get target component */
	UKawaiiFluidComponent* GetTargetComponent() const { return TargetComponent.Get(); }

	/** Get target volume */
	AKawaiiFluidVolume* GetTargetVolume() const { return TargetVolume.Get(); }

	/** Check if currently targeting Volume */
	bool IsTargetingVolume() const { return TargetVolume.IsValid(); }

private:
	/** Target fluid component (KawaiiFluidComponent mode) */
	TWeakObjectPtr<UKawaiiFluidComponent> TargetComponent;

	/** Target volume (KawaiiFluidVolume mode) */
	TWeakObjectPtr<AKawaiiFluidVolume> TargetVolume;

	/** Target volume component (for BrushSettings access) */
	TWeakObjectPtr<UKawaiiFluidVolumeComponent> TargetVolumeComponent;

	/** Current brush location */
	FVector BrushLocation {};

	/** Brush hit normal (surface direction) */
	FVector BrushNormal { FVector::UpVector };

	/** Whether brush location is valid */
	bool bValidLocation = false;

	/** Whether currently painting */
	bool bPainting = false;

	/** Last stroke time */
	double LastStrokeTime = 0.0;

	/** Selection changed delegate handle */
	FDelegateHandle SelectionChangedHandle;

	/** Called on selection change */
	void OnSelectionChanged(UObject* Object);

	/** Target component owner actor (for selection change detection) */
	TWeakObjectPtr<AActor> TargetOwnerActor;

	/** Update brush location */
	bool UpdateBrushLocation(FEditorViewportClient* ViewportClient, int32 MouseX, int32 MouseY);

	/** Apply brush */
	void ApplyBrush();

	/** Draw brush preview */
	void DrawBrushPreview(FPrimitiveDrawInterface* PDI);

	/** Get brush color by mode */
	FLinearColor GetBrushColor() const;
};
