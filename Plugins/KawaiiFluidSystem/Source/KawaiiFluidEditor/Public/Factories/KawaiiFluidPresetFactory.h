// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "KawaiiFluidPresetFactory.generated.h"

/**
 * Factory for creating KawaiiFluidPresetDataAsset in Content Browser
 */
UCLASS()
class KAWAIIFLUIDEDITOR_API UKawaiiFluidPresetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UKawaiiFluidPresetFactory();

	//~ Begin UFactory Interface
	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual uint32 GetMenuCategories() const override;
	virtual FText GetDisplayName() const override;
	//~ End UFactory Interface
};
