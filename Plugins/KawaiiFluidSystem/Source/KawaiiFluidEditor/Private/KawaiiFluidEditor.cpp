// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "KawaiiFluidEditor.h"
#include "Style/KawaiiFluidEditorStyle.h"
#include "AssetTypeActions/AssetTypeActions_KawaiiFluidPreset.h"
#include "Brush/KawaiiFluidBrushEditorMode.h"
#include "Details/KawaiiFluidVolumeComponentDetails.h"
#include "Components/KawaiiFluidVolumeComponent.h"

#include "IAssetTools.h"
#include "AssetToolsModule.h"
#include "PropertyEditorModule.h"
#include "EditorModeRegistry.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "Thumbnail/KawaiiFluidPresetThumbnailRenderer.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Editor.h"
#include "ObjectTools.h"
#include "UObject/ObjectSaveContext.h"

#define LOCTEXT_NAMESPACE "FKawaiiFluidEditorModule"

void FKawaiiFluidEditorModule::StartupModule()
{
	// Initialize editor style
	FKawaiiFluidEditorStyle::Initialize();

	// Register custom asset category
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	FluidAssetCategory = AssetTools.RegisterAdvancedAssetCategory(
		FName(TEXT("KawaiiFluid")),
		LOCTEXT("KawaiiFluidAssetCategory", "Kawaii Fluid"));

	// Register asset type actions
	RegisterAssetTypeActions();

	// Register property customizations
	RegisterPropertyCustomizations();

	// Register Fluid Brush Editor Mode
	FEditorModeRegistry::Get().RegisterMode<FKawaiiFluidBrushEditorMode>(
		FKawaiiFluidBrushEditorMode::EM_FluidBrush,
		LOCTEXT("FluidBrushModeName", "Fluid Brush"),
		FSlateIcon(),
		false  // Do not show in toolbar
	);

	// Register custom thumbnail renderer
	UThumbnailManager::Get().RegisterCustomRenderer(
		UKawaiiFluidPresetDataAsset::StaticClass(),
		UKawaiiFluidPresetThumbnailRenderer::StaticClass());

	// Bind event for automatic thumbnail refresh on asset save
	UPackage::PreSavePackageWithContextEvent.AddRaw(this, &FKawaiiFluidEditorModule::HandleAssetPreSave);
}

void FKawaiiFluidEditorModule::ShutdownModule()
{
	// Unbind event
	UPackage::PreSavePackageWithContextEvent.RemoveAll(this);

	if (!GExitPurge && !IsEngineExitRequested() && UObjectInitialized())
	{
		UThumbnailManager::Get().UnregisterCustomRenderer(UKawaiiFluidPresetDataAsset::StaticClass());
	}
	
	// Unregister Fluid Brush Editor Mode
	FEditorModeRegistry::Get().UnregisterMode(FKawaiiFluidBrushEditorMode::EM_FluidBrush);

	// Unregister property customizations
	UnregisterPropertyCustomizations();

	// Unregister asset type actions
	UnregisterAssetTypeActions();

	// Shutdown editor style
	FKawaiiFluidEditorStyle::Shutdown();
}

FKawaiiFluidEditorModule& FKawaiiFluidEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FKawaiiFluidEditorModule>("KawaiiFluidEditor");
}

void FKawaiiFluidEditorModule::HandleAssetPreSave(UPackage* InPackage, FObjectPreSaveContext InContext)
{
	if (!InPackage)
	{
		return;
	}

	// Check if our preset asset exists inside the package
	TArray<UObject*> ObjectsInPackage;
	GetObjectsWithOuter(InPackage, ObjectsInPackage);

	for (UObject* Obj : ObjectsInPackage)
	{
		if (UKawaiiFluidPresetDataAsset* Preset = Cast<UKawaiiFluidPresetDataAsset>(Obj))
		{
			// Calling ThumbnailTools at this point physically writes the latest Draw result
			// to the thumbnail section of the .uasset file.
			ThumbnailTools::GenerateThumbnailForObjectToSaveToDisk(Preset);
		}
	}
}

void FKawaiiFluidEditorModule::RegisterAssetTypeActions()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Register Fluid Preset asset type
	TSharedPtr<FAssetTypeActions_KawaiiFluidPreset> FluidPresetActions = MakeShared<FAssetTypeActions_KawaiiFluidPreset>();
	AssetTools.RegisterAssetTypeActions(FluidPresetActions.ToSharedRef());
	RegisteredAssetTypeActions.Add(FluidPresetActions);
}

void FKawaiiFluidEditorModule::UnregisterAssetTypeActions()
{
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		for (const TSharedPtr<IAssetTypeActions>& Action : RegisteredAssetTypeActions)
		{
			if (Action.IsValid())
			{
				AssetTools.UnregisterAssetTypeActions(Action.ToSharedRef());
			}
		}
	}

	RegisteredAssetTypeActions.Empty();
}

void FKawaiiFluidEditorModule::RegisterPropertyCustomizations()
{
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	// Register KawaiiFluidVolumeComponent detail customization
	PropertyModule.RegisterCustomClassLayout(
		UKawaiiFluidVolumeComponent::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FKawaiiFluidVolumeComponentDetails::MakeInstance)
	);
}

void FKawaiiFluidEditorModule::UnregisterPropertyCustomizations()
{
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UKawaiiFluidVolumeComponent::StaticClass()->GetFName());
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FKawaiiFluidEditorModule, KawaiiFluidEditor)
