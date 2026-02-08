// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class FKawaiiFluidPresetAssetEditor;

/**
 * Playback control widget for fluid preview
 * Contains Play, Pause, Stop, Reset buttons and speed slider
 */
class KAWAIIFLUIDEDITOR_API SKawaiiFluidPreviewPlaybackControls : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKawaiiFluidPreviewPlaybackControls) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TSharedPtr<FKawaiiFluidPresetAssetEditor> InEditor);

private:
	/** Button callbacks */
	FReply OnPlayPauseClicked();
	FReply OnStopClicked();
	FReply OnResetClicked();

	/** State checking */
	bool IsPlaying() const;
	bool IsPaused() const;
	bool CanPlay() const;

	/** Get play/pause button text */
	FText GetPlayPauseButtonText() const;
	FText GetPlayPauseTooltip() const;

	/** Speed slider */
	void OnSpeedChanged(float NewValue);
	float GetCurrentSpeed() const;
	TOptional<float> GetSpeedAsOptional() const;
	FText GetSpeedText() const;

private:
	/** Reference to asset editor */
	TWeakPtr<FKawaiiFluidPresetAssetEditor> EditorPtr;
};
