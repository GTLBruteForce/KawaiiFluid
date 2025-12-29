// Copyright Epic Games, Inc. All Rights Reserved.

#include "Viewport/FluidPresetEditorViewportClient.h"
#include "Viewport/SFluidPresetEditorViewport.h"
#include "Preview/FluidPreviewScene.h"
#include "AdvancedPreviewScene.h"

FFluidPresetEditorViewportClient::FFluidPresetEditorViewportClient(
	TSharedRef<FFluidPreviewScene> InPreviewScene,
	TSharedRef<SFluidPresetEditorViewport> InViewportWidget)
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

	// Enable grid
	DrawHelper.bDrawGrid = true;
	DrawHelper.bDrawPivot = false;
	DrawHelper.bDrawWorldBox = false;
	DrawHelper.GridColorAxis = FColor(80, 80, 80);
	DrawHelper.GridColorMajor = FColor(40, 40, 40);
	DrawHelper.GridColorMinor = FColor(20, 20, 20);

	// Visibility settings
	EngineShowFlags.SetGrid(true);
	EngineShowFlags.SetAntiAliasing(true);
}

FFluidPresetEditorViewportClient::~FFluidPresetEditorViewportClient()
{
}

void FFluidPresetEditorViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// Preview scene tick is handled by the asset editor
}

void FFluidPresetEditorViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	FEditorViewportClient::Draw(View, PDI);
}

bool FFluidPresetEditorViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	bool bHandled = false;

	// Handle keyboard shortcuts
	if (EventArgs.Event == IE_Pressed)
	{
		if (EventArgs.Key == EKeys::F)
		{
			// Focus on particles
			TSharedPtr<SFluidPresetEditorViewport> ViewportWidget = ViewportWidgetPtr.Pin();
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

void FFluidPresetEditorViewportClient::ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY)
{
	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
}

FLinearColor FFluidPresetEditorViewportClient::GetBackgroundColor() const
{
	return FLinearColor(0.1f, 0.1f, 0.12f, 1.0f);
}

void FFluidPresetEditorViewportClient::SetInitialCameraPosition()
{
	SetViewLocation(FVector(-400.0f, 0.0f, 250.0f));
	SetViewRotation(FRotator(-25.0f, 0.0f, 0.0f));
	SetLookAtLocation(FVector(0.0f, 0.0f, 100.0f));
}

void FFluidPresetEditorViewportClient::FocusOnBounds(const FBoxSphereBounds& Bounds)
{
	const float HalfFOVRadians = FMath::DegreesToRadians(ViewFOV / 2.0f);
	const float DistanceFromSphere = Bounds.SphereRadius / FMath::Tan(HalfFOVRadians);

	FVector Direction = GetViewRotation().Vector();
	FVector NewLocation = Bounds.Origin - Direction * DistanceFromSphere;

	SetViewLocation(NewLocation);
	SetLookAtLocation(Bounds.Origin);
}

