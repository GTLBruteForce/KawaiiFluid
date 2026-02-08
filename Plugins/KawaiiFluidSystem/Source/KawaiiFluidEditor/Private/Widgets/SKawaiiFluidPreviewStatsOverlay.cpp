// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Widgets/SKawaiiFluidPreviewStatsOverlay.h"
#include "Preview/KawaiiFluidPreviewScene.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SKawaiiFluidPreviewStatsOverlay"

void SKawaiiFluidPreviewStatsOverlay::Construct(const FArguments& InArgs, TSharedPtr<FKawaiiFluidPreviewScene> InPreviewScene)
{
	PreviewScenePtr = InPreviewScene;
	CachedFPS = 60.0f;
	FPSAccumulator = 0.0f;
	FrameCount = 0;
	CachedParticleCount = 0;

	ChildSlot
	[
		SNew(SBox)
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			// Particle count
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(this, &SKawaiiFluidPreviewStatsOverlay::GetParticleCountText)
				.ColorAndOpacity(FLinearColor::White)
				.ShadowOffset(FVector2D(1.0f, 1.0f))
				.ShadowColorAndOpacity(FLinearColor::Black)
			]

			// Simulation time
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(this, &SKawaiiFluidPreviewStatsOverlay::GetSimulationTimeText)
				.ColorAndOpacity(FLinearColor::White)
				.ShadowOffset(FVector2D(1.0f, 1.0f))
				.ShadowColorAndOpacity(FLinearColor::Black)
			]

			// FPS
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(this, &SKawaiiFluidPreviewStatsOverlay::GetFPSText)
				.ColorAndOpacity(FLinearColor::White)
				.ShadowOffset(FVector2D(1.0f, 1.0f))
				.ShadowColorAndOpacity(FLinearColor::Black)
			]

		]
	];
}

void SKawaiiFluidPreviewStatsOverlay::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	// Update FPS (averaged over multiple frames)
	if (InDeltaTime > 0.0f)
	{
		FPSAccumulator += 1.0f / InDeltaTime;
		FrameCount++;

		if (FrameCount >= 10)
		{
			CachedFPS = FPSAccumulator / FrameCount;
			FPSAccumulator = 0.0f;
			FrameCount = 0;
		}
	}

	// Update cached values from preview scene
	TSharedPtr<FKawaiiFluidPreviewScene> PreviewScene = PreviewScenePtr.Pin();
	if (PreviewScene.IsValid())
	{
		CachedParticleCount = PreviewScene->GetParticleCount();
	}
}

FText SKawaiiFluidPreviewStatsOverlay::GetParticleCountText() const
{
	return FText::Format(LOCTEXT("ParticleCount", "Particles: {0}"), FText::AsNumber(CachedParticleCount));
}

FText SKawaiiFluidPreviewStatsOverlay::GetSimulationTimeText() const
{
	TSharedPtr<FKawaiiFluidPreviewScene> PreviewScene = PreviewScenePtr.Pin();
	float SimTime = PreviewScene.IsValid() ? PreviewScene->GetSimulationTime() : 0.0f;
	FNumberFormattingOptions Options;
	Options.MaximumFractionalDigits = 2;
	return FText::Format(LOCTEXT("SimulationTime", "Time: {0}s"), FText::AsNumber(SimTime, &Options));
}

FText SKawaiiFluidPreviewStatsOverlay::GetFPSText() const
{
	return FText::Format(LOCTEXT("FPS", "FPS: {0}"), FText::AsNumber(FMath::RoundToInt(CachedFPS)));
}

#undef LOCTEXT_NAMESPACE
