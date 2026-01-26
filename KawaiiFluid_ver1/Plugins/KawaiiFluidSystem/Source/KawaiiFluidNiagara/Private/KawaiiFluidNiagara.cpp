// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "KawaiiFluidNiagara.h"
#include "NiagaraDI/NiagaraDataInterfaceKawaiiFluid.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "FKawaiiFluidNiagaraModule"

void FKawaiiFluidNiagaraModule::StartupModule()
{
	UE_LOG(LogTemp, Warning, TEXT("========================================"));
	UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidNiagara MODULE LOADING..."));
	UE_LOG(LogTemp, Warning, TEXT("========================================"));
	
	// Check Niagara module
	if (FModuleManager::Get().IsModuleLoaded("Niagara"))
	{
		UE_LOG(LogTemp, Warning, TEXT("  [OK] Niagara module is already loaded"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("  [INFO] Niagara module not loaded yet"));
	}
	
	// Check Data Interface class
	UClass* DIClass = UNiagaraDataInterfaceKawaiiFluid::StaticClass();
	if (DIClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("  [OK] UNiagaraDataInterfaceKawaiiFluid class loaded"));
		UE_LOG(LogTemp, Warning, TEXT("       - Class Name: %s"), *DIClass->GetName());
		UE_LOG(LogTemp, Warning, TEXT("       - Package: %s"), *DIClass->GetPackage()->GetName());
		UE_LOG(LogTemp, Warning, TEXT("       - Outer: %s"), DIClass->GetOuter() ? *DIClass->GetOuter()->GetName() : TEXT("None"));
		
		// Check UCLASS metadata
		if (DIClass->HasAnyClassFlags(CLASS_Abstract))
		{
			UE_LOG(LogTemp, Error, TEXT("       - [WARN] Class is ABSTRACT!"));
		}
		if (DIClass->HasAnyClassFlags(CLASS_Deprecated))
		{
			UE_LOG(LogTemp, Error, TEXT("       - [WARN] Class is DEPRECATED!"));
		}
		if (DIClass->HasAnyClassFlags(CLASS_Hidden))
		{
			UE_LOG(LogTemp, Error, TEXT("       - [WARN] Class is HIDDEN!"));
		}
		
		// Force CDO creation
		UObject* CDO = DIClass->GetDefaultObject();
		if (CDO)
		{
			UE_LOG(LogTemp, Warning, TEXT("       - CDO Created: %s"), *CDO->GetName());
		}
		
		// Check UNiagaraDataInterface inheritance
		if (DIClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
		{
			UE_LOG(LogTemp, Warning, TEXT("       - [OK] Inherits from UNiagaraDataInterface"));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("       - [FAIL] Does NOT inherit from UNiagaraDataInterface!"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("  [FAIL] UNiagaraDataInterfaceKawaiiFluid class NOT found!"));
	}
	
	// Enumerate all Data Interfaces (for debugging)
	UE_LOG(LogTemp, Warning, TEXT("  [INFO] Enumerating all UNiagaraDataInterface subclasses..."));
	TArray<UClass*> DataInterfaceClasses;
	GetDerivedClasses(UNiagaraDataInterface::StaticClass(), DataInterfaceClasses, true);
	
	int32 FoundCount = 0;
	for (UClass* FoundClass : DataInterfaceClasses)
	{
		FString ClassName = FoundClass->GetName();
		if (ClassName.Contains(TEXT("Kawaii")))
		{
			UE_LOG(LogTemp, Warning, TEXT("       - Found: %s"), *ClassName);
			FoundCount++;
		}
	}
	
	if (FoundCount == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("       - [FAIL] No Kawaii Data Interfaces found in registry!"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("       - [OK] Found %d Kawaii Data Interface(s)"), FoundCount);
	}
	
	UE_LOG(LogTemp, Warning, TEXT("========================================"));
	UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidNiagara MODULE LOADED"));
	UE_LOG(LogTemp, Warning, TEXT("========================================"));
}

void FKawaiiFluidNiagaraModule::ShutdownModule()
{
	UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidNiagara module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FKawaiiFluidNiagaraModule, KawaiiFluidNiagara)
