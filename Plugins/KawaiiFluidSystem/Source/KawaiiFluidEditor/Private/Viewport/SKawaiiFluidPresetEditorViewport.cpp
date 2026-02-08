// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Viewport/SKawaiiFluidPresetEditorViewport.h"
#include "Viewport/KawaiiFluidPresetEditorViewportClient.h"
#include "Preview/KawaiiFluidPreviewScene.h"
#include "Editor/KawaiiFluidPresetAssetEditor.h"
#include "Widgets/SKawaiiFluidPreviewStatsOverlay.h"
#include "Core/FluidParticle.h"
#include "SEditorViewportToolBarMenu.h"
#include "STransformViewportToolbar.h"

void SKawaiiFluidPresetEditorViewport::Construct(const FArguments& InArgs,
                                            TSharedPtr<FKawaiiFluidPreviewScene> InPreviewScene,
                                            TSharedPtr<FKawaiiFluidPresetAssetEditor> InAssetEditor)
{
	PreviewScene = InPreviewScene;
	AssetEditorPtr = InAssetEditor;

	SEditorViewport::Construct(SEditorViewport::FArguments());
}

SKawaiiFluidPresetEditorViewport::~SKawaiiFluidPresetEditorViewport()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->Viewport = nullptr;
	}
}

void SKawaiiFluidPresetEditorViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	// Add references to prevent garbage collection
}

TSharedRef<SEditorViewport> SKawaiiFluidPresetEditorViewport::GetViewportWidget()
{
	return SharedThis(this);
}

TSharedPtr<FExtender> SKawaiiFluidPresetEditorViewport::GetExtenders() const
{
	return MakeShared<FExtender>();
}

void SKawaiiFluidPresetEditorViewport::OnFloatingButtonClicked()
{
	// Handle floating button click if needed
}

void SKawaiiFluidPresetEditorViewport::RefreshViewport()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SKawaiiFluidPresetEditorViewport::FocusOnParticles()
{
	if (!ViewportClient.IsValid() || !PreviewScene.IsValid())
	{
		return;
	}

	const TArray<FFluidParticle>& Particles = PreviewScene->GetParticles();
	if (Particles.Num() == 0)
	{
		return;
	}

	// Calculate bounds of all particles
	FBox Bounds(ForceInit);
	for (const FFluidParticle& Particle : Particles)
	{
		Bounds += Particle.Position;
	}

	// Expand bounds slightly
	Bounds = Bounds.ExpandBy(50.0f);

	ViewportClient->FocusOnBounds(FBoxSphereBounds(Bounds));
}

void SKawaiiFluidPresetEditorViewport::ResetCamera()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetInitialCameraPosition();
	}
}

TSharedRef<FEditorViewportClient> SKawaiiFluidPresetEditorViewport::MakeEditorViewportClient()
{
	ViewportClient = MakeShared<FKawaiiFluidPresetEditorViewportClient>(
		PreviewScene.ToSharedRef(),
		SharedThis(this));

	ViewportClient->SetInitialCameraPosition();

	return ViewportClient.ToSharedRef();
}

void SKawaiiFluidPresetEditorViewport::PopulateViewportOverlays(TSharedRef<SOverlay> Overlay)
{
	SEditorViewport::PopulateViewportOverlays(Overlay);

	// Add stats overlay
	Overlay->AddSlot()
		.VAlign(VAlign_Bottom)
		.HAlign(HAlign_Left)
		.Padding(10.0f)
		[
			SNew(SKawaiiFluidPreviewStatsOverlay, PreviewScene)
		];
}

void SKawaiiFluidPresetEditorViewport::BindCommands()
{
	SEditorViewport::BindCommands();

	// Bind additional commands if needed
}
