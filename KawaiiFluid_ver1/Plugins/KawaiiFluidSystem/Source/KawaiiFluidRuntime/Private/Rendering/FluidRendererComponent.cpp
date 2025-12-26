// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidRendererComponent.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Core/FluidSimulator.h"
#include "Engine/World.h"

UFluidRendererComponent::UFluidRendererComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UFluidRendererComponent::BeginPlay()
{
	Super::BeginPlay();

	// 부모 시뮬레이터 캐싱
	CacheOwnerSimulator();

	// 서브시스템에 등록 (렌더링 활성화 시)
	if (RenderingParameters.bEnableRendering && OwnerSimulator)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			UFluidRendererSubsystem* Subsystem = World->GetSubsystem<UFluidRendererSubsystem>();
			if (Subsystem)
			{
				Subsystem->RegisterSimulator(OwnerSimulator);
			}
		}
	}
}

void UFluidRendererComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 서브시스템에서 등록 해제
	if (OwnerSimulator)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			UFluidRendererSubsystem* Subsystem = World->GetSubsystem<UFluidRendererSubsystem>();
			if (Subsystem)
			{
				Subsystem->UnregisterSimulator(OwnerSimulator);
			}
		}
	}

	Super::EndPlay(EndPlayReason);
}

FFluidRenderingParameters UFluidRendererComponent::GetEffectiveRenderingParameters() const
{
	if (bUseLocalParameters)
	{
		return RenderingParameters;
	}

	// 글로벌 파라미터 사용
	UWorld* World = GetWorld();
	if (World)
	{
		UFluidRendererSubsystem* Subsystem = World->GetSubsystem<UFluidRendererSubsystem>();
		if (Subsystem)
		{
			return Subsystem->RenderingParameters;
		}
	}

	return RenderingParameters;
}

void UFluidRendererComponent::SetRenderingQuality(EFluidRenderingQuality Quality)
{
	RenderingParameters.Quality = Quality;

	// 품질에 따라 자동 설정 조정
	switch (Quality)
	{
	case EFluidRenderingQuality::Low:
		RenderingParameters.RenderTargetScale = 0.5f;
		RenderingParameters.BilateralFilterRadius = 12;
		RenderingParameters.SmoothingStrength = 0.3f;
		break;

	case EFluidRenderingQuality::Medium:
		RenderingParameters.RenderTargetScale = 0.75f;
		RenderingParameters.BilateralFilterRadius = 18;
		RenderingParameters.SmoothingStrength = 0.5f;
		break;

	case EFluidRenderingQuality::High:
		RenderingParameters.RenderTargetScale = 1.0f;
		RenderingParameters.BilateralFilterRadius = 25;
		RenderingParameters.SmoothingStrength = 0.7f;
		break;

	case EFluidRenderingQuality::Ultra:
		RenderingParameters.RenderTargetScale = 1.0f;
		RenderingParameters.BilateralFilterRadius = 35;
		RenderingParameters.SmoothingStrength = 0.8f;
		break;
	}
}

void UFluidRendererComponent::SetRenderingEnabled(bool bEnabled)
{
	if (RenderingParameters.bEnableRendering == bEnabled)
	{
		return;
	}

	RenderingParameters.bEnableRendering = bEnabled;

	// 서브시스템 등록/해제
	if (!OwnerSimulator)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UFluidRendererSubsystem* Subsystem = World->GetSubsystem<UFluidRendererSubsystem>();
	if (!Subsystem)
	{
		return;
	}

	if (bEnabled)
	{
		Subsystem->RegisterSimulator(OwnerSimulator);
	}
	else
	{
		Subsystem->UnregisterSimulator(OwnerSimulator);
	}
}

void UFluidRendererComponent::CacheOwnerSimulator()
{
	OwnerSimulator = Cast<AFluidSimulator>(GetOwner());

	if (!OwnerSimulator)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidRendererComponent: Owner is not a FluidSimulator!"));
	}
}
