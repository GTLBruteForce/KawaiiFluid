// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Widgets/SKawaiiFluidPreviewPlaybackControls.h"
#include "Editor/KawaiiFluidPresetAssetEditor.h"
#include "Style/KawaiiFluidEditorStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SKawaiiFluidPreviewPlaybackControls"

void SKawaiiFluidPreviewPlaybackControls::Construct(const FArguments& InArgs, TSharedPtr<FKawaiiFluidPresetAssetEditor> InEditor)
{
	EditorPtr = InEditor;

	ChildSlot
	[
		SNew(SHorizontalBox)

		// Play/Pause button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.OnClicked(this, &SKawaiiFluidPreviewPlaybackControls::OnPlayPauseClicked)
			.ToolTipText(this, &SKawaiiFluidPreviewPlaybackControls::GetPlayPauseTooltip)
			[
				SNew(STextBlock)
				.Text(this, &SKawaiiFluidPreviewPlaybackControls::GetPlayPauseButtonText)
			]
		]

		// Stop button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.OnClicked(this, &SKawaiiFluidPreviewPlaybackControls::OnStopClicked)
			.ToolTipText(LOCTEXT("StopTooltip", "Stop and Reset Simulation"))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StopButton", "Stop"))
			]
		]

		// Reset button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f)
		[
			SNew(SButton)
			.OnClicked(this, &SKawaiiFluidPreviewPlaybackControls::OnResetClicked)
			.ToolTipText(LOCTEXT("ResetTooltip", "Reset Particles (keep playing)"))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ResetButton", "Reset"))
			]
		]

		// Separator
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(8.0f, 2.0f)
		[
			SNew(SSeparator)
			.Orientation(Orient_Vertical)
		]

		// Speed label
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(2.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SpeedLabel", "Speed:"))
		]

		// Speed spinbox
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.0f, 2.0f)
		[
			SNew(SBox)
			.WidthOverride(80.0f)
			[
				SNew(SSpinBox<float>)
				.MinValue(0.0f)
				.MaxValue(4.0f)
				.MinSliderValue(0.0f)
				.MaxSliderValue(2.0f)
				.Delta(0.1f)
				.Value(this, &SKawaiiFluidPreviewPlaybackControls::GetCurrentSpeed)
				.OnValueChanged(this, &SKawaiiFluidPreviewPlaybackControls::OnSpeedChanged)
				.ToolTipText(LOCTEXT("SpeedTooltip", "Simulation Speed Multiplier"))
			]
		]

		// Speed text
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(2.0f)
		[
			SNew(STextBlock)
			.Text(this, &SKawaiiFluidPreviewPlaybackControls::GetSpeedText)
		]
	];
}

FReply SKawaiiFluidPreviewPlaybackControls::OnPlayPauseClicked()
{
	TSharedPtr<FKawaiiFluidPresetAssetEditor> Editor = EditorPtr.Pin();
	if (Editor.IsValid())
	{
		if (Editor->IsPlaying())
		{
			Editor->Pause();
		}
		else
		{
			Editor->Play();
		}
	}
	return FReply::Handled();
}

FReply SKawaiiFluidPreviewPlaybackControls::OnStopClicked()
{
	TSharedPtr<FKawaiiFluidPresetAssetEditor> Editor = EditorPtr.Pin();
	if (Editor.IsValid())
	{
		Editor->Stop();
	}
	return FReply::Handled();
}

FReply SKawaiiFluidPreviewPlaybackControls::OnResetClicked()
{
	TSharedPtr<FKawaiiFluidPresetAssetEditor> Editor = EditorPtr.Pin();
	if (Editor.IsValid())
	{
		Editor->Reset();
	}
	return FReply::Handled();
}

bool SKawaiiFluidPreviewPlaybackControls::IsPlaying() const
{
	TSharedPtr<FKawaiiFluidPresetAssetEditor> Editor = EditorPtr.Pin();
	return Editor.IsValid() && Editor->IsPlaying();
}

bool SKawaiiFluidPreviewPlaybackControls::IsPaused() const
{
	return !IsPlaying();
}

bool SKawaiiFluidPreviewPlaybackControls::CanPlay() const
{
	TSharedPtr<FKawaiiFluidPresetAssetEditor> Editor = EditorPtr.Pin();
	return Editor.IsValid() && Editor->GetEditingPreset() != nullptr;
}

FText SKawaiiFluidPreviewPlaybackControls::GetPlayPauseButtonText() const
{
	if (IsPlaying())
	{
		return LOCTEXT("PauseButton", "Pause");
	}
	else
	{
		return LOCTEXT("PlayButton", "Play");
	}
}

FText SKawaiiFluidPreviewPlaybackControls::GetPlayPauseTooltip() const
{
	if (IsPlaying())
	{
		return LOCTEXT("PauseTooltip", "Pause Simulation");
	}
	else
	{
		return LOCTEXT("PlayTooltip", "Play Simulation");
	}
}

void SKawaiiFluidPreviewPlaybackControls::OnSpeedChanged(float NewValue)
{
	TSharedPtr<FKawaiiFluidPresetAssetEditor> Editor = EditorPtr.Pin();
	if (Editor.IsValid())
	{
		Editor->SetSimulationSpeed(NewValue);
	}
}

float SKawaiiFluidPreviewPlaybackControls::GetCurrentSpeed() const
{
	TSharedPtr<FKawaiiFluidPresetAssetEditor> Editor = EditorPtr.Pin();
	if (Editor.IsValid())
	{
		return Editor->GetSimulationSpeed();
	}
	return 1.0f;
}

TOptional<float> SKawaiiFluidPreviewPlaybackControls::GetSpeedAsOptional() const
{
	return GetCurrentSpeed();
}

FText SKawaiiFluidPreviewPlaybackControls::GetSpeedText() const
{
	return FText::Format(LOCTEXT("SpeedFormat", "x"), FText::AsNumber(GetCurrentSpeed()));
}

#undef LOCTEXT_NAMESPACE
