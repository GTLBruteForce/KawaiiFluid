// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/KawaiiFluidRenderController.h"
#include "Core/FluidParticle.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Components/KawaiiFluidISMComponent.h"
#include "Components/KawaiiFluidSSFRComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

UKawaiiFluidRenderController::UKawaiiFluidRenderController()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UKawaiiFluidRenderController::BeginPlay()
{
	Super::BeginPlay();

	// DataProvider 자동 찾기
	if (bAutoFindDataProvider && !DataProvider.GetInterface())
	{
		FindDataProvider();
	}

	// Renderer 자동 등록
	if (bAutoRegisterRenderers)
	{
		FindAndRegisterRenderers();
	}

	// FluidRendererSubsystem에 등록 (하이브리드 아키텍처)
	if (UWorld* World = GetWorld())
	{
		if (UFluidRendererSubsystem* Subsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			Subsystem->RegisterRenderController(this);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidRenderController: Initialized (DataProvider: %s, Renderers: %d)"),
		DataProvider.GetInterface() ? TEXT("Found") : TEXT("None"),
		ActiveRenderers.Num());
}

void UKawaiiFluidRenderController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// FluidRendererSubsystem에서 등록 해제
	if (UWorld* World = GetWorld())
	{
		if (UFluidRendererSubsystem* Subsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			Subsystem->UnregisterRenderController(this);
		}
	}

	ClearRenderers();
	Super::EndPlay(EndPlayReason);
}

void UKawaiiFluidRenderController::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bAutoUpdateRenderers)
	{
		UpdateRenderers();
	}
}

void UKawaiiFluidRenderController::SetDataProvider(TScriptInterface<IKawaiiFluidDataProvider> NewDataProvider)
{
	DataProvider = NewDataProvider;
	
	if (DataProvider.GetInterface())
	{
		UE_LOG(LogTemp, Log, TEXT("KawaiiFluidRenderController: DataProvider set to %s"),
			*DataProvider.GetObject()->GetName());
	}
}

void UKawaiiFluidRenderController::RegisterRenderer(TScriptInterface<IKawaiiFluidRenderer> Renderer)
{
	if (!Renderer.GetInterface())
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidRenderController: Attempted to register null renderer"));
		return;
	}

	if (ActiveRenderers.Contains(Renderer))
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidRenderController: Renderer already registered: %s"),
			*Renderer.GetObject()->GetName());
		return;
	}

	ActiveRenderers.Add(Renderer);
	
	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidRenderController: Renderer registered: %s (Total: %d)"),
		*Renderer.GetObject()->GetName(),
		ActiveRenderers.Num());
}

void UKawaiiFluidRenderController::UnregisterRenderer(TScriptInterface<IKawaiiFluidRenderer> Renderer)
{
	if (!Renderer.GetInterface())
	{
		return;
	}

	int32 Removed = ActiveRenderers.Remove(Renderer);
	
	if (Removed > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("KawaiiFluidRenderController: Renderer unregistered: %s (Remaining: %d)"),
			*Renderer.GetObject()->GetName(),
			ActiveRenderers.Num());
	}
}

void UKawaiiFluidRenderController::ClearRenderers()
{
	ActiveRenderers.Empty();
	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidRenderController: All renderers cleared"));
}

void UKawaiiFluidRenderController::UpdateRenderers()
{
	// DataProvider 유효성 검사
	if (!DataProvider.GetInterface())
	{
		return;
	}

	// 렌더러가 없으면 스킵
	if (ActiveRenderers.Num() == 0)
	{
		return;
	}

	// DataProvider로부터 시뮬레이션 데이터 가져오기
	const TArray<FFluidParticle>& SimParticles = DataProvider->GetParticles();
	
	if (SimParticles.Num() == 0)
	{
		return;
	}

	// 시뮬레이션 데이터 → 렌더링 데이터 변환
	RenderParticlesCache.SetNum(SimParticles.Num(), EAllowShrinking::No);
	
	float ParticleRadius = DataProvider->GetParticleRenderRadius();
	
	for (int32 i = 0; i < SimParticles.Num(); ++i)
	{
		const FFluidParticle& SimParticle = SimParticles[i];
		FKawaiiRenderParticle& RenderParticle = RenderParticlesCache[i];
		
		RenderParticle.Position = FVector3f(SimParticle.Position);
		RenderParticle.Velocity = FVector3f(SimParticle.Velocity);
		RenderParticle.Radius = ParticleRadius;
		RenderParticle.Padding = 0.0f;
	}

	// 모든 활성 렌더러에게 렌더링 요청
	for (const auto& Renderer : ActiveRenderers)
	{
		if (!Renderer.GetInterface() || !Renderer->IsEnabled())
		{
			continue;
		}

		// Primary 모드가 설정되어 있으면 해당 모드의 렌더러만 활성화
		if (!IsMatchingPrimaryMode(Renderer))
		{
			continue;
		}

		Renderer->UpdateRendering(DataProvider.GetInterface(), 0.0f);
	}
}

void UKawaiiFluidRenderController::FindDataProvider()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidRenderController: No owner actor"));
		return;
	}

	// 같은 Actor의 모든 컴포넌트 검색
	TArray<UActorComponent*> Components;
	Owner->GetComponents(Components);

	for (UActorComponent* Component : Components)
	{
		if (Component && Component != this && Component->Implements<UKawaiiFluidDataProvider>())
		{
			DataProvider = Component;
			UE_LOG(LogTemp, Log, TEXT("KawaiiFluidRenderController: DataProvider found: %s"),
				*Component->GetName());
			return;
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidRenderController: No DataProvider found in actor '%s'"),
		*Owner->GetName());
}

void UKawaiiFluidRenderController::FindAndRegisterRenderers()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// 같은 Actor의 모든 컴포넌트 검색
	TArray<UActorComponent*> Components;
	Owner->GetComponents(Components);

	int32 RegisteredCount = 0;
	for (UActorComponent* Component : Components)
	{
		if (Component && Component != this && Component->Implements<UKawaiiFluidRenderer>())
		{
			TScriptInterface<IKawaiiFluidRenderer> Renderer;
			Renderer.SetObject(Component);
			Renderer.SetInterface(Cast<IKawaiiFluidRenderer>(Component));
			
			if (Renderer.GetInterface())
			{
				RegisterRenderer(Renderer);
				RegisteredCount++;
			}
		}
	}

	if (RegisteredCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidRenderController: No renderers found in actor '%s'"),
			*Owner->GetName());
	}
}

bool UKawaiiFluidRenderController::IsMatchingPrimaryMode(const TScriptInterface<IKawaiiFluidRenderer>& Renderer) const
{
	if (!Renderer.GetInterface())
	{
		return false;
	}

	// None 모드는 모든 렌더러 허용
	if (PrimaryRenderMode == EKawaiiFluidRenderingMode::None)
	{
		return true;
	}

	// 렌더러의 모드가 Primary 모드와 일치하는지 확인
	return Renderer->GetRenderingMode() == PrimaryRenderMode;
}
