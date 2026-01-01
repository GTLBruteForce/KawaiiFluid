// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/KawaiiFluidComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"

UKawaiiFluidComponent::UKawaiiFluidComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;  // Subsystem 시뮬레이션 이후 렌더링
	bTickInEditor = true;  // 에디터에서도 Tick 실행 (브러시 렌더링용)

	// 시뮬레이션 모듈 생성
	SimulationModule = CreateDefaultSubobject<UKawaiiFluidSimulationModule>(TEXT("SimulationModule"));

	// 렌더링 모듈 생성`
	RenderingModule = CreateDefaultSubobject<UKawaiiFluidRenderingModule>(TEXT("RenderingModule"));
}

void UKawaiiFluidComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITOR
	// 에디터 모드에서 렌더링 모듈 초기화 (PIE가 아닐 때만)
	UWorld* World = GetWorld();
	if (World && !World->IsGameWorld() && bEnableRendering && RenderingModule && SimulationModule)
	{
		// SimulationModule 초기화 (Preset 없으면 기본 생성)
		if (!SimulationModule->Preset)
		{
			SimulationModule->Preset = NewObject<UKawaiiFluidPresetDataAsset>(this, NAME_None, RF_Transient);
		}
		SimulationModule->Initialize(SimulationModule->Preset);

		// RenderingModule 초기화
		RenderingModule->Initialize(World, this, SimulationModule);

		// ISM/Metaball 설정 적용
		if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
		{
			ISMRenderer->ApplySettings(ISMSettings);
		}
		if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
		{
			MetaballRenderer->ApplySettings(MetaballSettings);
		}

		// FluidRendererSubsystem에 등록
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->RegisterRenderingModule(RenderingModule);
		}

		UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s]: Editor rendering initialized"), *GetName());
	}
#endif
}

void UKawaiiFluidComponent::OnUnregister()
{
#if WITH_EDITOR
	// 에디터 모드에서 정리
	UWorld* World = GetWorld();
	if (World && !World->IsGameWorld() && RenderingModule)
	{
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->UnregisterRenderingModule(RenderingModule);
		}
		RenderingModule->Cleanup();
	}
#endif

	Super::OnUnregister();
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

	// 렌더링 모듈 초기화 (중복 초기화 방지)
	if (bEnableRendering && RenderingModule && SimulationModule)
	{
		if (RenderingModule->IsInitialized())
		{
			// 이미 초기화됨 (에디터에서 복제된 경우 등)
			UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s]: RenderingModule already initialized, skipping"), *GetName());
		}
		else
		{
			// 1. RenderingModule 초기화 (this를 부모 컴포넌트로 전달)
			RenderingModule->Initialize(GetWorld(), this, SimulationModule);

			// 2. ISM 렌더러 설정 적용
			if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
			{
				ISMRenderer->ApplySettings(ISMSettings);
			}

			// 3. Metaball 렌더러 설정 적용
			if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
			{
				MetaballRenderer->ApplySettings(MetaballSettings);
			}

			// 4. FluidRendererSubsystem에 등록
			if (UWorld* World = GetWorld())
			{
				if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
				{
					RendererSubsystem->RegisterRenderingModule(RenderingModule);
				}
			}

			UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s]: Rendering initialized (ISM: %s, Metaball: %s)"),
				*GetName(),
				ISMSettings.bEnabled ? TEXT("Enabled") : TEXT("Disabled"),
				MetaballSettings.bEnabled ? TEXT("Enabled") : TEXT("Disabled"));
		}
	}

	// Module을 Subsystem에 등록 (Component가 아닌 Module!)
	RegisterToSubsystem();

	// 자동 스폰
	if (bSpawnOnBeginPlay && AutoSpawnCount > 0 && SimulationModule)
	{
		SimulationModule->SpawnParticles(GetComponentLocation(), AutoSpawnCount, AutoSpawnRadius);
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

	UWorld* World = GetWorld();
	const bool bIsGameWorld = World && World->IsGameWorld();

	// 연속 스폰 처리 (게임 월드에서만)
	if (bIsGameWorld && bContinuousSpawn)
	{
		ProcessContinuousSpawn(DeltaTime);
	}

	// 렌더링 업데이트 (에디터 + 게임 모두)
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
		// 스폰 위치 계산 (컴포넌트 위치 기준)
		FVector SpawnLocation = GetComponentLocation() + SpawnOffset;

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

//========================================
// Brush API
//========================================

void UKawaiiFluidComponent::AddParticlesInRadius(const FVector& WorldCenter, float Radius,
                                                  int32 Count, const FVector& Velocity,
                                                  float Randomness, const FVector& SurfaceNormal)
{
	if (!SimulationModule)
	{
		return;
	}

#if WITH_EDITOR
	// 에디터에서 데이터 수정 시 Modify() 호출 - 인스턴스 직렬화에 반영
	// 컴포넌트와 서브오브젝트 모두 마킹해야 Re-instancing 시 데이터 보존됨
	Modify();
	SimulationModule->Modify();
#endif

	// 노말 정규화 (안전)
	const FVector Normal = SurfaceNormal.GetSafeNormal();

	for (int32 i = 0; i < Count; ++i)
	{
		// 랜덤 방향 생성
		FVector RandomDir = FMath::VRand();

		// 반구 분포: 노말과 반대 방향이면 반전 (표면 위로만 생성)
		if (FVector::DotProduct(RandomDir, Normal) < 0.0f)
		{
			RandomDir = -RandomDir;
		}

		FVector RandomOffset = RandomDir * FMath::FRand() * Radius * Randomness;
		FVector SpawnPos = WorldCenter + RandomOffset;
		FVector SpawnVel = Velocity + FMath::VRand() * 20.0f * Randomness;

		SimulationModule->SpawnParticle(SpawnPos, SpawnVel);
	}
}

int32 UKawaiiFluidComponent::RemoveParticlesInRadius(const FVector& WorldCenter, float Radius)
{
	if (!SimulationModule)
	{
		return 0;
	}

#if WITH_EDITOR
	// 에디터에서 데이터 수정 시 Modify() 호출 - 인스턴스 직렬화에 반영
	// 컴포넌트와 서브오브젝트 모두 마킹해야 Re-instancing 시 데이터 보존됨
	Modify();
	SimulationModule->Modify();
#endif

	float RadiusSq = Radius * Radius;

	TArray<FFluidParticle>& Particles = SimulationModule->GetParticlesMutable();
	int32 RemovedCount = 0;

	for (int32 i = Particles.Num() - 1; i >= 0; --i)
	{
		if (FVector::DistSquared(Particles[i].Position, WorldCenter) <= RadiusSq)
		{
			Particles.RemoveAtSwap(i);
			++RemovedCount;
		}
	}

	return RemovedCount;
}

void UKawaiiFluidComponent::ClearAllParticles()
{
	if (SimulationModule)
	{
		SimulationModule->ClearAllParticles();
	}

	// 렌더링도 즉시 클리어
	if (RenderingModule)
	{
		RenderingModule->UpdateRenderers();
	}
}

//========================================
// InstanceData (Re-instancing 시 파티클 데이터 보존)
//========================================

FKawaiiFluidComponentInstanceData::FKawaiiFluidComponentInstanceData(const UKawaiiFluidComponent* SourceComponent)
	: FActorComponentInstanceData(SourceComponent)
{
	if (SourceComponent && SourceComponent->GetSimulationModule())
	{
		SavedParticles = SourceComponent->GetSimulationModule()->GetParticles();
		SavedNextParticleID = SourceComponent->GetSimulationModule()->GetNextParticleID();

		UE_LOG(LogTemp, Log, TEXT("InstanceData: Saved %d particles from %s"),
			SavedParticles.Num(), *SourceComponent->GetName());
	}
}

void FKawaiiFluidComponentInstanceData::ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase)
{
	Super::ApplyToComponent(Component, CacheApplyPhase);

	if (CacheApplyPhase == ECacheApplyPhase::PostUserConstructionScript)
	{
		if (UKawaiiFluidComponent* FluidComponent = Cast<UKawaiiFluidComponent>(Component))
		{
			if (FluidComponent->GetSimulationModule() && SavedParticles.Num() > 0)
			{
				FluidComponent->GetSimulationModule()->GetParticlesMutable() = SavedParticles;
				FluidComponent->GetSimulationModule()->SetNextParticleID(SavedNextParticleID);

				UE_LOG(LogTemp, Log, TEXT("InstanceData: Restored %d particles to %s"),
					SavedParticles.Num(), *FluidComponent->GetName());
			}
		}
	}
}

TStructOnScope<FActorComponentInstanceData> UKawaiiFluidComponent::GetComponentInstanceData() const
{
	// 에디터에서만 + 파티클이 있을 때만 저장
	if (GetSimulationModule() && GetSimulationModule()->GetParticleCount() > 0)
	{
		return MakeStructOnScope<FActorComponentInstanceData, FKawaiiFluidComponentInstanceData>(this);
	}

	return Super::GetComponentInstanceData();
}
