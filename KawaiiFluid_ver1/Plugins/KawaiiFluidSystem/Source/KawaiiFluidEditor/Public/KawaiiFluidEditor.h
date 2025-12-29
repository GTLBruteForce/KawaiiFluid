// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "AssetTypeCategories.h"

class IAssetTypeActions;

/**
 * KawaiiFluid Editor Module
 * Provides custom asset editors and tools for the fluid system
 */
class FKawaiiFluidEditorModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	/** Get the module instance */
	static FKawaiiFluidEditorModule& Get();

	/** Get custom asset type category */
	EAssetTypeCategories::Type GetAssetCategory() const { return FluidAssetCategory; }

private:
	/** Register asset type actions */
	void RegisterAssetTypeActions();

	/** Unregister asset type actions */
	void UnregisterAssetTypeActions();

	/** Register custom property type customizations */
	void RegisterPropertyCustomizations();

	/** Unregister property customizations */
	void UnregisterPropertyCustomizations();

private:
	/** Registered asset type actions */
	TArray<TSharedPtr<IAssetTypeActions>> RegisteredAssetTypeActions;

	/** Custom asset type category for fluid assets */
	EAssetTypeCategories::Type FluidAssetCategory {};
};
