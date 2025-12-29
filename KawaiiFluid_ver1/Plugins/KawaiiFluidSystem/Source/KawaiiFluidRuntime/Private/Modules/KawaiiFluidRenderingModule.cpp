// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/KawaiiFluidSSFRRenderer.h"
#include "Core/FluidParticle.h"

UKawaiiFluidRenderingModule::UKawaiiFluidRenderingModule()
{
	// Create renderer instances as default subobjects (Instanced pattern)
	ISMRenderer = CreateDefaultSubobject<UKawaiiFluidISMRenderer>(TEXT("ISMRenderer"));
	SSFRRenderer = CreateDefaultSubobject<UKawaiiFluidSSFRRenderer>(TEXT("SSFRRenderer"));
}

void UKawaiiFluidRenderingModule::Initialize(UWorld* InWorld, AActor* InOwner, IKawaiiFluidDataProvider* InDataProvider)
{
	CachedWorld = InWorld;
	CachedOwner = InOwner;
	DataProviderPtr = InDataProvider;

	// CreateDefaultSubobject only works in CDO context.
	// If created via NewObject (e.g., editor preview), renderers will be nullptr.
	// Create them here if missing.
	if (!ISMRenderer)
	{
		ISMRenderer = NewObject<UKawaiiFluidISMRenderer>(this, TEXT("ISMRenderer"));
		UE_LOG(LogTemp, Log, TEXT("RenderingModule: Created ISMRenderer via NewObject (non-CDO context)"));
	}

	if (!SSFRRenderer)
	{
		SSFRRenderer = NewObject<UKawaiiFluidSSFRRenderer>(this, TEXT("SSFRRenderer"));
		UE_LOG(LogTemp, Log, TEXT("RenderingModule: Created SSFRRenderer via NewObject (non-CDO context)"));
	}

	// Initialize renderers
	if (ISMRenderer)
	{
		ISMRenderer->Initialize(InWorld, InOwner);
	}

	if (SSFRRenderer)
	{
		SSFRRenderer->Initialize(InWorld, InOwner);
	}

	UE_LOG(LogTemp, Log, TEXT("RenderingModule: Initialized (ISM: %s, SSFR: %s)"),
		ISMRenderer && ISMRenderer->IsEnabled() ? TEXT("Enabled") : TEXT("Disabled"),
		SSFRRenderer && SSFRRenderer->IsEnabled() ? TEXT("Enabled") : TEXT("Disabled"));
}

void UKawaiiFluidRenderingModule::Cleanup()
{
	if (ISMRenderer)
	{
		ISMRenderer->Cleanup();
	}

	if (SSFRRenderer)
	{
		SSFRRenderer->Cleanup();
	}

	DataProviderPtr = nullptr;
	CachedWorld = nullptr;
	CachedOwner = nullptr;
}

void UKawaiiFluidRenderingModule::UpdateRenderers()
{
	if (!DataProviderPtr || !DataProviderPtr->IsDataValid())
	{
		return;
	}

	// Update all enabled renderers
	if (ISMRenderer && ISMRenderer->IsEnabled())
	{
		ISMRenderer->UpdateRendering(DataProviderPtr, 0.0f);
	}

	if (SSFRRenderer && SSFRRenderer->IsEnabled())
	{
		SSFRRenderer->UpdateRendering(DataProviderPtr, 0.0f);
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
