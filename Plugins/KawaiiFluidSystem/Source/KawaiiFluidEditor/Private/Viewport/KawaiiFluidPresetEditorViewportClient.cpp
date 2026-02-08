// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Viewport/KawaiiFluidPresetEditorViewportClient.h"
#include "Viewport/SKawaiiFluidPresetEditorViewport.h"
#include "Preview/KawaiiFluidPreviewScene.h"
#include "AdvancedPreviewScene.h"

FKawaiiFluidPresetEditorViewportClient::FKawaiiFluidPresetEditorViewportClient(
	TSharedRef<FKawaiiFluidPreviewScene> InPreviewScene,
	TSharedRef<SKawaiiFluidPresetEditorViewport> InViewportWidget)
	: FEditorViewportClient(nullptr, &InPreviewScene.Get(), StaticCastSharedRef<SEditorViewport>(InViewportWidget))
	, PreviewScene(InPreviewScene)
	, ViewportWidgetPtr(InViewportWidget)
{
	// Setup viewport settings
	SetRealtime(true);

	// Camera settings
	SetViewLocation(FVector(-400.0f, 0.0f, 200.0f));
	SetViewRotation(FRotator(-20.0f, 0.0f, 0.0f));

	// Orbit camera around origin
	SetLookAtLocation(FVector(0.0f, 0.0f, 100.0f));

	// Disable grid for cleaner fluid preview (transparent fluids look better without grid showing through)
	DrawHelper.bDrawGrid = false;
	DrawHelper.bDrawPivot = false;
	DrawHelper.bDrawWorldBox = false;

	// Visibility settings
	EngineShowFlags.SetGrid(false);
	EngineShowFlags.SetAntiAliasing(true);
}

FKawaiiFluidPresetEditorViewportClient::~FKawaiiFluidPresetEditorViewportClient()
{
}

void FKawaiiFluidPresetEditorViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// Preview scene tick is handled by the asset editor
}

void FKawaiiFluidPresetEditorViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	FEditorViewportClient::Draw(View, PDI);
}

bool FKawaiiFluidPresetEditorViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	bool bHandled = false;

	// Handle keyboard shortcuts
	if (EventArgs.Event == IE_Pressed)
	{
		if (EventArgs.Key == EKeys::F)
		{
			// Focus on particles
			TSharedPtr<SKawaiiFluidPresetEditorViewport> ViewportWidget = ViewportWidgetPtr.Pin();
			if (ViewportWidget.IsValid())
			{
				ViewportWidget->FocusOnParticles();
				bHandled = true;
			}
		}
		else if (EventArgs.Key == EKeys::H)
		{
			// Reset camera to home position
			SetInitialCameraPosition(); 
			bHandled = true;
		}
	}

	if (!bHandled)
	{
		bHandled = FEditorViewportClient::InputKey(EventArgs);
	}

	return bHandled;
}

void FKawaiiFluidPresetEditorViewportClient::ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY)
{
	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
}

FLinearColor FKawaiiFluidPresetEditorViewportClient::GetBackgroundColor() const
{
	return FLinearColor(0.1f, 0.1f, 0.12f, 1.0f);
}

void FKawaiiFluidPresetEditorViewportClient::SetInitialCameraPosition()
{
	SetViewLocation(FVector(-400.0f, 0.0f, 250.0f));
	SetViewRotation(FRotator(-25.0f, 0.0f, 0.0f));
	SetLookAtLocation(FVector(0.0f, 0.0f, 100.0f));
}

void FKawaiiFluidPresetEditorViewportClient::FocusOnBounds(const FBoxSphereBounds& Bounds)
{
	const float HalfFOVRadians = FMath::DegreesToRadians(ViewFOV / 2.0f);
	const float DistanceFromSphere = Bounds.SphereRadius / FMath::Tan(HalfFOVRadians);

	FVector Direction = GetViewRotation().Vector();
	FVector NewLocation = Bounds.Origin - Direction * DistanceFromSphere;

	SetViewLocation(NewLocation);
	SetLookAtLocation(Bounds.Origin);
}

