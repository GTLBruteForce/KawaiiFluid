// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "KawaiiFluidRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FKawaiiFluidRuntimeModule"

void FKawaiiFluidRuntimeModule::StartupModule()
{
	// 플러그인 셰이더 디렉토리 매핑
	FString PluginShaderPath = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("KawaiiFluidSystem"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/KawaiiFluidSystem"), PluginShaderPath);

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidRuntime Module Started: Shader Directory: %s"), *PluginShaderPath);
}

void FKawaiiFluidRuntimeModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FKawaiiFluidRuntimeModule, KawaiiFluidRuntime)
