// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "FluidPresetFactory.generated.h"

/**
 * Factory for creating KawaiiFluidPresetDataAsset in Content Browser
 */
UCLASS()
class KAWAIIFLUIDEDITOR_API UFluidPresetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UFluidPresetFactory();

	//~ Begin UFactory Interface
	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual uint32 GetMenuCategories() const override;
	virtual FText GetDisplayName() const override;
	//~ End UFactory Interface
};
