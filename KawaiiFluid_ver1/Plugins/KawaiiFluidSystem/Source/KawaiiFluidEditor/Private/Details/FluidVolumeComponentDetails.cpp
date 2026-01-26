// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Details/FluidVolumeComponentDetails.h"
#include "Components/KawaiiFluidVolumeComponent.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Brush/FluidBrushEditorMode.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "LevelEditor.h"
#include "EditorModeManager.h"

#define LOCTEXT_NAMESPACE "FluidVolumeComponentDetails"

TSharedRef<IDetailCustomization> FFluidVolumeComponentDetails::MakeInstance()
{
	return MakeShareable(new FFluidVolumeComponentDetails);
}

void FFluidVolumeComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	if (Objects.Num() != 1)
	{
		return;
	}

	TargetComponent = Cast<UKawaiiFluidVolumeComponent>(Objects[0].Get());
	if (!TargetComponent.IsValid())
	{
		return;
	}

	// Get the owning Volume actor for Brush API access
	TargetVolume = Cast<AKawaiiFluidVolume>(TargetComponent->GetOwner());

	// Brush Editor category (placed above Fluid Volume categories)
	IDetailCategoryBuilder& BrushCategory = DetailBuilder.EditCategory(
		"Brush Editor",
		LOCTEXT("BrushEditorCategory", "Brush Editor"),
		ECategoryPriority::Important);

	// Button row
	BrushCategory.AddCustomRow(LOCTEXT("BrushButtons", "Brush Buttons"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)

		// Start button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("StartBrush", "Start Brush"))
			.ToolTipText(LOCTEXT("StartBrushTooltip", "Enter brush mode to paint particles"))
			.OnClicked(this, &FFluidVolumeComponentDetails::OnStartBrushClicked)
			.Visibility(this, &FFluidVolumeComponentDetails::GetStartVisibility)
		]

		// Stop button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("StopBrush", "Stop Brush"))
			.ToolTipText(LOCTEXT("StopBrushTooltip", "Exit brush mode"))
			.OnClicked(this, &FFluidVolumeComponentDetails::OnStopBrushClicked)
			.Visibility(this, &FFluidVolumeComponentDetails::GetStopVisibility)
		]

		// Clear button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("ClearParticles", "Clear All"))
			.ToolTipText(LOCTEXT("ClearParticlesTooltip", "Remove all particles"))
			.OnClicked(this, &FFluidVolumeComponentDetails::OnClearParticlesClicked)
		]
	];

	// Particle count display
	BrushCategory.AddCustomRow(LOCTEXT("ParticleCount", "Particle Count"))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("ParticleCountLabel", "Particles"))
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	[
		SNew(STextBlock)
		.Text_Lambda([this]()
		{
			if (TargetVolume.IsValid())
			{
				UKawaiiFluidSimulationModule* SimModule = TargetVolume->GetSimulationModule();
				if (SimModule)
				{
					int32 Count = SimModule->GetParticleCount();
					if (Count >= 0)
					{
						return FText::AsNumber(Count);
					}
				}
			}
			return FText::FromString(TEXT("-"));
		})
		.Font(IDetailLayoutBuilder::GetDetailFont())
	];

	// Help text
	BrushCategory.AddCustomRow(LOCTEXT("BrushHelp", "Help"))
	.Visibility(TAttribute<EVisibility>(this, &FFluidVolumeComponentDetails::GetStopVisibility))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("BrushHelpText", "Left-click drag to paint | [ ] Resize | 1/2 Mode | ESC Exit"))
		.Font(IDetailLayoutBuilder::GetDetailFontItalic())
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.8f, 0.5f)))
	];
}

FReply FFluidVolumeComponentDetails::OnStartBrushClicked()
{
	if (!TargetComponent.IsValid() || !TargetVolume.IsValid())
	{
		return FReply::Handled();
	}

	FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");

	if (TSharedPtr<ILevelEditor> Editor = LevelEditor.GetFirstLevelEditor())
	{
		FEditorModeTools& ModeTools = Editor->GetEditorModeManager();
		ModeTools.ActivateMode(FFluidBrushEditorMode::EM_FluidBrush);

		if (FFluidBrushEditorMode* BrushMode = static_cast<FFluidBrushEditorMode*>(
			ModeTools.GetActiveMode(FFluidBrushEditorMode::EM_FluidBrush)))
		{
			// Set Volume target (calls SetTargetVolume implemented in Phase 2)
			BrushMode->SetTargetVolume(TargetVolume.Get());
		}
	}

	return FReply::Handled();
}

FReply FFluidVolumeComponentDetails::OnStopBrushClicked()
{
	FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");

	if (TSharedPtr<ILevelEditor> Editor = LevelEditor.GetFirstLevelEditor())
	{
		Editor->GetEditorModeManager().DeactivateMode(FFluidBrushEditorMode::EM_FluidBrush);
	}

	if (TargetComponent.IsValid())
	{
		TargetComponent->bBrushModeActive = false;
	}

	return FReply::Handled();
}

FReply FFluidVolumeComponentDetails::OnClearParticlesClicked()
{
	if (TargetVolume.IsValid())
	{
		// Use Volume's ClearAllParticles() - clears rendering as well
		TargetVolume->ClearAllParticles();
		TargetVolume->Modify();
	}

	return FReply::Handled(); 
}

bool FFluidVolumeComponentDetails::IsBrushActive() const
{
	FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");

	if (TSharedPtr<ILevelEditor> Editor = LevelEditor.GetFirstLevelEditor())
	{
		return Editor->GetEditorModeManager().IsModeActive(FFluidBrushEditorMode::EM_FluidBrush);
	}

	return false;
}

EVisibility FFluidVolumeComponentDetails::GetStartVisibility() const
{
	return IsBrushActive() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility FFluidVolumeComponentDetails::GetStopVisibility() const
{
	return IsBrushActive() ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
