// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/KawaiiFluidComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/KawaiiFluidSSFRRenderer.h"

UKawaiiFluidComponent::UKawaiiFluidComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;  // Subsystem 시뮬레이션 이후 렌더링

	// 시뮬레이션 모듈 생성
	SimulationModule = CreateDefaultSubobject<UKawaiiFluidSimulationModule>(TEXT("SimulationModule"));

	// 렌더링 모듈 생성`
	RenderingModule = CreateDefaultSubobject<UKawaiiFluidRenderingModule>(TEXT("RenderingModule"));
}

void UKawaiiFluidComponent::BeginPlay()
{
	Super::BeginPlay();

	// 시뮬레이션 모듈 초기화
	if (SimulationModule)
	{
		// Preset 없으면 기본 생성
		if (!SimulationModule->Preset)
		{
			SimulationModule->Preset = NewObject<UKawaiiFluidPresetDataAsset>(this, NAME_None, RF_Transient);
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidComponent [%s]: No Preset assigned, using default values"), *GetName());
		}
		SimulationModule->Initialize(SimulationModule->Preset);

		// 이벤트 콜백 항상 연결 (Module에서 bEnableCollisionEvents 체크)
		SimulationModule->SetCollisionEventCallback(
			FOnModuleCollisionEvent::CreateUObject(this, &UKawaiiFluidComponent::HandleCollisionEvent)
		);
	}

	// 렌더링 모듈 초기화
	if (bEnableRendering && RenderingModule && SimulationModule)
	{
		// 1. RenderingModule 초기화 (SimulationModule을 IKawaiiFluidDataProvider로 전달)
		RenderingModule->Initialize(GetWorld(), GetOwner(), SimulationModule);

		// 2. ISM 렌더러 설정 적용
		if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
		{
			ISMRenderer->ApplySettings(ISMSettings);
		}

		// 3. SSFR 렌더러 설정 적용
		if (UKawaiiFluidSSFRRenderer* SSFRRenderer = RenderingModule->GetSSFRRenderer())
		{
			SSFRRenderer->ApplySettings(SSFRSettings);
		}

		// 4. FluidRendererSubsystem에 등록
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				RendererSubsystem->RegisterRenderingModule(RenderingModule);
			}
		}

		UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s]: Rendering initialized (ISM: %s, SSFR: %s)"),
			*GetName(),
			ISMSettings.bEnabled ? TEXT("Enabled") : TEXT("Disabled"),
			SSFRSettings.bEnabled ? TEXT("Enabled") : TEXT("Disabled"));
	}

	// Module을 Subsystem에 등록 (Component가 아닌 Module!)
	RegisterToSubsystem();

	// 자동 스폰
	if (bSpawnOnBeginPlay && AutoSpawnCount > 0 && SimulationModule)
	{
		SimulationModule->SpawnParticles(GetOwner()->GetActorLocation(), AutoSpawnCount, AutoSpawnRadius);
	}

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidComponent BeginPlay: %s"), *GetName());
}

void UKawaiiFluidComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Subsystem에서 등록 해제
	UnregisterFromSubsystem();

	// 이벤트 클리어
	OnParticleHit.Clear();

	// 렌더링 모듈 정리
	if (RenderingModule)
	{
		// FluidRendererSubsystem에서 등록 해제
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				RendererSubsystem->UnregisterRenderingModule(RenderingModule);
			}
		}

		// RenderingModule 정리
		RenderingModule->Cleanup();
		RenderingModule = nullptr;
	}

	// 시뮬레이션 모듈 정리
	if (SimulationModule)
	{
		SimulationModule->Shutdown();
	}

	Super::EndPlay(EndPlayReason);

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidComponent EndPlay: %s"), *GetName());
}

void UKawaiiFluidComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 연속 스폰 처리
	if (bContinuousSpawn)
	{
		ProcessContinuousSpawn(DeltaTime);
	}

	// 렌더링 업데이트
	if (RenderingModule)
	{
		RenderingModule->UpdateRenderers();
	}
}

//========================================
// Continuous Spawn
//========================================

void UKawaiiFluidComponent::ProcessContinuousSpawn(float DeltaTime)
{
	if (!SimulationModule || ParticlesPerSecond <= 0.0f)
	{
		return;
	}

	// 최대 파티클 수 체크
	if (MaxParticleCount > 0 && SimulationModule->GetParticleCount() >= MaxParticleCount)
	{
		return;
	}

	SpawnAccumulatedTime += DeltaTime;
	const float SpawnInterval = 1.0f / ParticlesPerSecond;

	while (SpawnAccumulatedTime >= SpawnInterval)
	{
		// 스폰 위치 계산
		FVector SpawnLocation = GetOwner()->GetActorLocation() + SpawnOffset;

		// 반경 내 랜덤 위치
		if (ContinuousSpawnRadius > 0.0f)
		{
			FVector RandomOffset = FMath::VRand() * FMath::FRandRange(0.0f, ContinuousSpawnRadius);
			SpawnLocation += RandomOffset;
		}

		// 파티클 스폰
		SimulationModule->SpawnParticle(SpawnLocation, SpawnVelocity);

		SpawnAccumulatedTime -= SpawnInterval;

		// 최대 파티클 수 체크
		if (MaxParticleCount > 0 && SimulationModule->GetParticleCount() >= MaxParticleCount)
		{
			SpawnAccumulatedTime = 0.0f;
			break;
		}
	}
}

//========================================
// Event System
//========================================

void UKawaiiFluidComponent::HandleCollisionEvent(const FKawaiiFluidCollisionEvent& Event)
{
	// Module에서 필터링 완료 후 호출됨 - 바로 브로드캐스트
	if (OnParticleHit.IsBound())
	{
		OnParticleHit.Broadcast(
			Event.ParticleIndex,
			Event.HitActor.Get(),
			Event.HitLocation,
			Event.HitNormal,
			Event.HitSpeed
		);
	}
}

//========================================
// Subsystem Registration
//========================================

void UKawaiiFluidComponent::RegisterToSubsystem()
{
	if (!SimulationModule)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			// Module을 직접 등록!
			Subsystem->RegisterModule(SimulationModule);
		}

		// RenderSubSystem , ...
	}
}

void UKawaiiFluidComponent::UnregisterFromSubsystem()
{
	if (!SimulationModule)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			// Module 등록 해제
			Subsystem->UnregisterModule(SimulationModule);
		}
		
		// RenderSubSystem , ...
	}
}
