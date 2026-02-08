// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "AssetTypeActions/AssetTypeActions_KawaiiFluidPreset.h"
#include "KawaiiFluidEditor.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Editor/KawaiiFluidPresetAssetEditor.h"
#include "ThumbnailRendering/SceneThumbnailInfo.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions_FluidPreset"

FText FAssetTypeActions_KawaiiFluidPreset::GetName() const
{
	return LOCTEXT("AssetName", "Kawaii Fluid Preset");
}

UClass* FAssetTypeActions_KawaiiFluidPreset::GetSupportedClass() const
{
	return UKawaiiFluidPresetDataAsset::StaticClass();
}

FColor FAssetTypeActions_KawaiiFluidPreset::GetTypeColor() const
{
	return FColor(50, 100, 200);
}

uint32 FAssetTypeActions_KawaiiFluidPreset::GetCategories()
{
	return FKawaiiFluidEditorModule::Get().GetAssetCategory();
}

void FAssetTypeActions_KawaiiFluidPreset::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
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

UThumbnailInfo* FAssetTypeActions_KawaiiFluidPreset::GetThumbnailInfo(UObject* Asset) const
{
	UKawaiiFluidPresetDataAsset* Preset = CastChecked<UKawaiiFluidPresetDataAsset>(Asset);
	UThumbnailInfo* ThumbnailInfo = Preset->ThumbnailInfo;
	if (ThumbnailInfo == nullptr)
	{
		ThumbnailInfo = NewObject<USceneThumbnailInfo>(Preset, NAME_None, RF_Transactional);
		Preset->ThumbnailInfo = ThumbnailInfo;
	}
	return ThumbnailInfo;
}

#undef LOCTEXT_NAMESPACE
