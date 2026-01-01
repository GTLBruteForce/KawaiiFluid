// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetTypeActions/AssetTypeActions_FluidPreset.h"
#include "KawaiiFluidEditor.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Editor/KawaiiFluidPresetAssetEditor.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions_FluidPreset"

FText FAssetTypeActions_FluidPreset::GetName() const
{
	return LOCTEXT("AssetName", "Kawaii Fluid Preset");
}

UClass* FAssetTypeActions_FluidPreset::GetSupportedClass() const
{
	return UKawaiiFluidPresetDataAsset::StaticClass();
}

FColor FAssetTypeActions_FluidPreset::GetTypeColor() const
{
	return FColor(50, 100, 200);
}

uint32 FAssetTypeActions_FluidPreset::GetCategories()
{
	return FKawaiiFluidEditorModule::Get().GetAssetCategory();
}

void FAssetTypeActions_FluidPreset::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	const EToolkitMode::Type Mode = EditWithinLevelEditor.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone;

	for (UObject* Object : InObjects)
	{
		if (UKawaiiFluidPresetDataAsset* Preset = Cast<UKawaiiFluidPresetDataAsset>(Object))
		{
			TSharedRef<FKawaiiFluidPresetAssetEditor> NewEditor = MakeShareable(new FKawaiiFluidPresetAssetEditor());
			NewEditor->InitFluidPresetEditor(Mode, EditWithinLevelEditor, Preset);
		}
	}
}

#undef LOCTEXT_NAMESPACE
