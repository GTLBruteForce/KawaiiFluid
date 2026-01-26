// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Kawaii Fluid Niagara integration module
 * Provides Data Interface for passing CPU simulation data to Niagara
 */
class FKawaiiFluidNiagaraModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
