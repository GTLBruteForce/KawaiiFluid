// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class FKawaiiFluidPreviewScene;

/**
 * Stats overlay widget for fluid preview viewport
 * Displays particle count, FPS, density, etc.
 */
class KAWAIIFLUIDEDITOR_API SKawaiiFluidPreviewStatsOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKawaiiFluidPreviewStatsOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TSharedPtr<FKawaiiFluidPreviewScene> InPreviewScene);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	/** Get text for each stat */
	FText GetParticleCountText() const;
	FText GetSimulationTimeText() const;
	FText GetFPSText() const;

private:
	/** Preview scene reference */
	TWeakPtr<FKawaiiFluidPreviewScene> PreviewScenePtr;

	/** Cached values */
	float CachedFPS{};
	float FPSAccumulator{};
	int32 FrameCount{};
	int32 CachedParticleCount{};
};
