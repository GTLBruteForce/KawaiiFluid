// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Core/FluidParticle.h"

UKawaiiFluidRenderingModule::UKawaiiFluidRenderingModule()
{
	// Create renderer instances as default subobjects (Instanced pattern)
	ISMRenderer = CreateDefaultSubobject<UKawaiiFluidISMRenderer>(TEXT("ISMRenderer"));
	MetaballRenderer = CreateDefaultSubobject<UKawaiiFluidMetaballRenderer>(TEXT("MetaballRenderer"));
}

void UKawaiiFluidRenderingModule::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	// PIE로 복제될 때 EditorWorld의 객체를 가리키는 stale pointer 초기화
	// BeginPlay에서 Initialize()가 다시 호출되어 올바른 값으로 설정됨
	CachedWorld = nullptr;
	CachedOwnerComponent = nullptr;
	DataProviderPtr = nullptr;

	// 자식 렌더러들도 정리 (ISMRenderer, MetaballRenderer는 subobject이므로 자동 복제됨)
	if (ISMRenderer)
	{
		ISMRenderer->Cleanup();
	}
	if (MetaballRenderer)
	{
		MetaballRenderer->Cleanup();
	}

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidRenderingModule::PostDuplicate - Cleared stale pointers (PIE=%d)"),
		bDuplicateForPIE ? 1 : 0);
}

void UKawaiiFluidRenderingModule::Initialize(UWorld* InWorld, USceneComponent* InOwnerComponent, IKawaiiFluidDataProvider* InDataProvider, UKawaiiFluidPresetDataAsset* InPreset)
{
	CachedWorld = InWorld;
	CachedOwnerComponent = InOwnerComponent;
	DataProviderPtr = InDataProvider;

	// CreateDefaultSubobject only works in CDO context.
	// If created via NewObject (e.g., editor preview), renderers will be nullptr.
	// Create them here if missing.
	if (!ISMRenderer)
	{
		ISMRenderer = NewObject<UKawaiiFluidISMRenderer>(this, TEXT("ISMRenderer"));
		UE_LOG(LogTemp, Log, TEXT("RenderingModule: Created ISMRenderer via NewObject (non-CDO context)"));
	}

	if (!MetaballRenderer)
	{
		MetaballRenderer = NewObject<UKawaiiFluidMetaballRenderer>(this, TEXT("MetaballRenderer"));
		UE_LOG(LogTemp, Log, TEXT("RenderingModule: Created MetaballRenderer via NewObject (non-CDO context)"));
	}

	// Initialize renderers
	if (ISMRenderer)
	{
		ISMRenderer->Initialize(InWorld, InOwnerComponent);
	}

	if (MetaballRenderer)
	{
		MetaballRenderer->Initialize(InWorld, InOwnerComponent, InPreset);
	}

	UE_LOG(LogTemp, Log, TEXT("RenderingModule: Initialized (ISM: %s, Metaball: %s)"),
		ISMRenderer && ISMRenderer->IsEnabled() ? TEXT("Enabled") : TEXT("Disabled"),
		MetaballRenderer && MetaballRenderer->IsEnabled() ? TEXT("Enabled") : TEXT("Disabled"));
}

void UKawaiiFluidRenderingModule::Cleanup()
{
	if (ISMRenderer)
	{
		ISMRenderer->Cleanup();
	}

	if (MetaballRenderer)
	{
		MetaballRenderer->Cleanup();
	}

	DataProviderPtr = nullptr;
	CachedWorld = nullptr;
	CachedOwnerComponent = nullptr;
}

void UKawaiiFluidRenderingModule::UpdateRenderers()
{
	if (!DataProviderPtr)
	{
		return;
	}

	// Update all enabled renderers
	if (ISMRenderer && ISMRenderer->IsEnabled())
	{
		ISMRenderer->UpdateRendering(DataProviderPtr, 0.0f);
	}

	if (MetaballRenderer && MetaballRenderer->IsEnabled())
	{
		MetaballRenderer->UpdateRendering(DataProviderPtr, 0.0f);
	}
}

int32 UKawaiiFluidRenderingModule::GetParticleCount() const
{
	if (DataProviderPtr)
	{
		return DataProviderPtr->GetParticleCount();
	}
	return 0;
}
