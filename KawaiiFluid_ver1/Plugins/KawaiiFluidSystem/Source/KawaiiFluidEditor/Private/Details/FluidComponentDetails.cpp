// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Details/FluidComponentDetails.h"
#include "Components/KawaiiFluidComponent.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Brush/FluidBrushEditorMode.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "LevelEditor.h"
#include "EditorModeManager.h"

#define LOCTEXT_NAMESPACE "FluidComponentDetails"

TSharedRef<IDetailCustomization> FFluidComponentDetails::MakeInstance()
{
	return MakeShareable(new FFluidComponentDetails);
}

void FFluidComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	if (Objects.Num() != 1)
	{
		return;
	}

	TargetComponent = Cast<UKawaiiFluidComponent>(Objects[0].Get());
	if (!TargetComponent.IsValid())
	{
		return;
	}

	// Brush 카테고리
	IDetailCategoryBuilder& BrushCategory = DetailBuilder.EditCategory(
		"Brush",
		LOCTEXT("BrushCategory", "Particle Brush"),
		ECategoryPriority::Important);

	// 버튼 행
	BrushCategory.AddCustomRow(LOCTEXT("BrushButtons", "Brush Buttons"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)

		// Start 버튼
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("StartBrush", "Start Brush"))
			.ToolTipText(LOCTEXT("StartBrushTooltip", "Enter brush mode to paint particles"))
			.OnClicked(this, &FFluidComponentDetails::OnStartBrushClicked)
			.Visibility(this, &FFluidComponentDetails::GetStartVisibility)
		]

		// Stop 버튼
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("StopBrush", "Stop Brush"))
			.ToolTipText(LOCTEXT("StopBrushTooltip", "Exit brush mode"))
			.OnClicked(this, &FFluidComponentDetails::OnStopBrushClicked)
			.Visibility(this, &FFluidComponentDetails::GetStopVisibility)
		]

		// Clear 버튼
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("ClearParticles", "Clear All"))
			.ToolTipText(LOCTEXT("ClearParticlesTooltip", "Remove all particles"))
			.OnClicked(this, &FFluidComponentDetails::OnClearParticlesClicked)
		]
	];

	// 파티클 개수 표시
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
			if (TargetComponent.IsValid() && TargetComponent->GetSimulationModule())
			{
				// Per-source count: 이 컴포넌트가 소유한 파티클 수만 표시
				const int32 SourceID = TargetComponent->GetSimulationModule()->GetSourceID();
				int32 Count = TargetComponent->GetSimulationModule()->GetParticleCountForSource(SourceID);
				if (Count < 0)
				{
					return FText::FromString(TEXT("-"));  // 데이터 미준비
				}
				return FText::AsNumber(Count);
			}
			return FText::FromString(TEXT("-"));
		})
		.Font(IDetailLayoutBuilder::GetDetailFont())
	];

	// 도움말
	BrushCategory.AddCustomRow(LOCTEXT("BrushHelp", "Help"))
	.Visibility(TAttribute<EVisibility>(this, &FFluidComponentDetails::GetStopVisibility))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("BrushHelpText", "Left-click drag to paint | [ ] Resize | 1/2 Mode | ESC Exit"))
		.Font(IDetailLayoutBuilder::GetDetailFontItalic())
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.8f, 0.5f)))
	];
}

FReply FFluidComponentDetails::OnStartBrushClicked()
{
	if (!TargetComponent.IsValid())
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
			BrushMode->SetTargetComponent(TargetComponent.Get());
		}
	}

	return FReply::Handled();
}

FReply FFluidComponentDetails::OnStopBrushClicked()
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

FReply FFluidComponentDetails::OnClearParticlesClicked()
{
	if (TargetComponent.IsValid())
	{
		// 컴포넌트의 ClearAllParticles() 사용 - 렌더링도 같이 클리어됨
		TargetComponent->ClearAllParticles();
	}

	return FReply::Handled(); 
}

bool FFluidComponentDetails::IsBrushActive() const
{
	FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");

	if (TSharedPtr<ILevelEditor> Editor = LevelEditor.GetFirstLevelEditor())
	{
		return Editor->GetEditorModeManager().IsModeActive(FFluidBrushEditorMode::EM_FluidBrush);
	}

	return false;
}

EVisibility FFluidComponentDetails::GetStartVisibility() const
{
	return IsBrushActive() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility FFluidComponentDetails::GetStopVisibility() const
{
	return IsBrushActive() ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
