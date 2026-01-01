// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Factories/FluidPresetFactory.h"
#include "KawaiiFluidEditor.h"
#include "Data/KawaiiFluidPresetDataAsset.h"

#define LOCTEXT_NAMESPACE "FluidPresetFactory"

UFluidPresetFactory::UFluidPresetFactory()
{
	SupportedClass = UKawaiiFluidPresetDataAsset::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UFluidPresetFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	return NewObject<UKawaiiFluidPresetDataAsset>(InParent, InClass, InName, Flags);
}

uint32 UFluidPresetFactory::GetMenuCategories() const
{
	return FKawaiiFluidEditorModule::Get().GetAssetCategory();
}

FText UFluidPresetFactory::GetDisplayName() const
{
	return LOCTEXT("FactoryDisplayName", "Fluid Preset");
}

#undef LOCTEXT_NAMESPACE
