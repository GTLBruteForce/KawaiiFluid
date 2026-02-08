// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Factories/KawaiiFluidPresetFactory.h"
#include "KawaiiFluidEditor.h"
#include "Data/KawaiiFluidPresetDataAsset.h"

#define LOCTEXT_NAMESPACE "KawaiiFluidPresetFactory"

UKawaiiFluidPresetFactory::UKawaiiFluidPresetFactory()
{
	SupportedClass = UKawaiiFluidPresetDataAsset::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UKawaiiFluidPresetFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	return NewObject<UKawaiiFluidPresetDataAsset>(InParent, InClass, InName, Flags);
}

uint32 UKawaiiFluidPresetFactory::GetMenuCategories() const
{
	return FKawaiiFluidEditorModule::Get().GetAssetCategory();
}

FText UKawaiiFluidPresetFactory::GetDisplayName() const
{
	return LOCTEXT("FactoryDisplayName", "Fluid Preset");
}

#undef LOCTEXT_NAMESPACE
