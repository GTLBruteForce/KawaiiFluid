// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/FluidSceneViewExtension.h"
#include "Modules/KawaiiFluidRenderingModule.h"

bool UFluidRendererSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	UWorld* World = CastChecked<UWorld>(Outer);
	const EWorldType::Type WorldType = World->WorldType;

	// Support all world types that might need fluid rendering
	// Including EditorPreview for Preset Editor viewport
	return WorldType == EWorldType::Game ||
	       WorldType == EWorldType::Editor ||
	       WorldType == EWorldType::PIE ||
	       WorldType == EWorldType::EditorPreview ||
	       WorldType == EWorldType::GamePreview;
}

void UFluidRendererSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Scene View Extension 생성 및 등록
	ViewExtension = FSceneViewExtensions::NewExtension<FFluidSceneViewExtension>(this);

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem Initialized"));
}

void UFluidRendererSubsystem::Deinitialize()
{
	// View Extension 해제
	ViewExtension.Reset();

	RegisteredRenderingModules.Empty();

	Super::Deinitialize();

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem Deinitialized"));
}

//========================================
// RenderingModule 관리
//========================================

void UFluidRendererSubsystem::RegisterRenderingModule(UKawaiiFluidRenderingModule* Module)
{
	if (!Module)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem: RegisterRenderingModule - Module is null"));
		return;
	}

	if (RegisteredRenderingModules.Contains(Module))
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem: RenderingModule already registered: %s"),
			*Module->GetName());
		return;
	}

	RegisteredRenderingModules.Add(Module);

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem: Registered RenderingModule %s (Total: %d)"),
		*Module->GetName(),
		RegisteredRenderingModules.Num());
}

void UFluidRendererSubsystem::UnregisterRenderingModule(UKawaiiFluidRenderingModule* Module)
{
	if (!Module)
	{
		return;
	}

	int32 Removed = RegisteredRenderingModules.Remove(Module);

	if (Removed > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem: Unregistered RenderingModule %s (Remaining: %d)"),
			*Module->GetName(),
			RegisteredRenderingModules.Num());
	}
}

