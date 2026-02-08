// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class UKawaiiFluidPresetDataAsset;
class UThumbnailInfo;

/**
 * Asset type actions for KawaiiFluidPresetDataAsset
 * Handles double-click behavior, context menu, etc.
 */
class KAWAIIFLUIDEDITOR_API FAssetTypeActions_KawaiiFluidPreset : public FAssetTypeActions_Base
{
public:
	//~ Begin IAssetTypeActions Interface
	virtual FText GetName() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual FColor GetTypeColor() const override;
	virtual uint32 GetCategories() override;
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor = TSharedPtr<IToolkitHost>()) override;
	virtual UThumbnailInfo* GetThumbnailInfo(UObject* Asset) const override;
	virtual bool HasActions(const TArray<UObject*>& InObjects) const override { return false; }
	//~ End IAssetTypeActions Interface
};
