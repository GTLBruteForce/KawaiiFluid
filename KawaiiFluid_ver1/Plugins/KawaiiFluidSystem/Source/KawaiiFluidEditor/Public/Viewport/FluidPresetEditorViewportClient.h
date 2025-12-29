// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"

class FFluidPreviewScene;
class SFluidPresetEditorViewport;

/**
 * Viewport client for fluid preset editor
 * Handles rendering, input, and camera control
 */
class KAWAIIFLUIDEDITOR_API FFluidPresetEditorViewportClient : public FEditorViewportClient,
                                                                public TSharedFromThis<FFluidPresetEditorViewportClient>
{
public:
	FFluidPresetEditorViewportClient(
		TSharedRef<FFluidPreviewScene> InPreviewScene,
		TSharedRef<SFluidPresetEditorViewport> InViewportWidget);

	virtual ~FFluidPresetEditorViewportClient() override;

	//~ Begin FViewportClient Interface
	virtual void Tick(float DeltaSeconds) override;
	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	//~ End FViewportClient Interface

	//~ Begin FEditorViewportClient Interface
	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override;
	virtual FLinearColor GetBackgroundColor() const override;
	virtual bool ShouldOrbitCamera() const override { return true; }
	//~ End FEditorViewportClient Interface

	/** Set initial camera position */
	void SetInitialCameraPosition();

	/** Focus on given bounds */
	void FocusOnBounds(const FBoxSphereBounds& Bounds);

	/** Get preview scene */
	TSharedPtr<FFluidPreviewScene> GetPreviewScene() const { return PreviewScene; }

private:
	/** Preview scene */
	TSharedPtr<FFluidPreviewScene> PreviewScene;

	/** Viewport widget reference */
	TWeakPtr<SFluidPresetEditorViewport> ViewportWidgetPtr;
};
