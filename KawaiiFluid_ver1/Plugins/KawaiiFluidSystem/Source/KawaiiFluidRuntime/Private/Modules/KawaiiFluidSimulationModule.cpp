// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Modules/KawaiiFluidSimulationModule.h"
#include "Core/SpatialHash.h"
#include "Collision/FluidCollider.h"
#include "Components/FluidInteractionComponent.h"
#include "Components/KawaiiFluidComponent.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "GPU/GPUFluidSimulator.h"

UKawaiiFluidSimulationModule::UKawaiiFluidSimulationModule()
{
}

#if WITH_EDITOR
void UKawaiiFluidSimulationModule::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Preset 변경 시
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, Preset))
	{
		bRuntimePresetDirty = true;
		// SpatialHash 재구성
		if (Preset && SpatialHash.IsValid())
		{
			SpatialHash = MakeShared<FSpatialHash>(Preset->SmoothingRadius);
		}
	}
	// Override 값 변경 시
	else if (PropertyName.ToString().StartsWith(TEXT("bOverride_")) ||
	         PropertyName.ToString().StartsWith(TEXT("Override_")))
	{
		bRuntimePresetDirty = true;
		// SmoothingRadius override 시 SpatialHash도 갱신
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, Override_SmoothingRadius) ||
		    PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, bOverride_SmoothingRadius))
		{
			if (bOverride_SmoothingRadius && SpatialHash.IsValid())
			{
				SpatialHash = MakeShared<FSpatialHash>(Override_SmoothingRadius);
			}
			else if (!bOverride_SmoothingRadius && Preset && SpatialHash.IsValid())
			{
				SpatialHash = MakeShared<FSpatialHash>(Preset->SmoothingRadius);
			}
		}
	}
}
#endif

void UKawaiiFluidSimulationModule::Initialize(UKawaiiFluidPresetDataAsset* InPreset)
{
	if (bIsInitialized)
	{
		return;
	}

	Preset = InPreset;
	bRuntimePresetDirty = true;

	// SpatialHash 초기화 (Independent 모드용)
	float CellSize = 20.0f;
	if (Preset)
	{
		CellSize = Preset->SmoothingRadius;
	}
	InitializeSpatialHash(CellSize);

	bIsInitialized = true;

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidSimulationModule initialized"));
}

void UKawaiiFluidSimulationModule::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	Particles.Empty();
	Colliders.Empty();
	InteractionComponents.Empty();
	SpatialHash.Reset();
	Preset = nullptr;
	RuntimePreset = nullptr;

	bIsInitialized = false;

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidSimulationModule shutdown"));
}

void UKawaiiFluidSimulationModule::SetPreset(UKawaiiFluidPresetDataAsset* InPreset)
{
	Preset = InPreset;
	bRuntimePresetDirty = true;

	// SpatialHash 재구성
	if (Preset && SpatialHash.IsValid())
	{
		SpatialHash = MakeShared<FSpatialHash>(Preset->SmoothingRadius);
	}
}

void UKawaiiFluidSimulationModule::InitializeSpatialHash(float CellSize)
{
	SpatialHash = MakeShared<FSpatialHash>(CellSize);
}

bool UKawaiiFluidSimulationModule::HasAnyOverride() const
{
	return bOverride_RestDensity || bOverride_Compliance || bOverride_SmoothingRadius ||
	       bOverride_ViscosityCoefficient || bOverride_Gravity || bOverride_AdhesionStrength ||
	       bOverride_ParticleRadius;
}

UKawaiiFluidPresetDataAsset* UKawaiiFluidSimulationModule::GetEffectivePreset()
{
	if (!HasAnyOverride())
	{
		return Preset;
	}

	if (bRuntimePresetDirty)
	{
		UpdateRuntimePreset();
	}

	return RuntimePreset ? RuntimePreset : Preset.Get();
}

void UKawaiiFluidSimulationModule::UpdateRuntimePreset()
{
	if (!Preset)
	{
		return;
	}

	// RuntimePreset이 없으면 생성
	if (!RuntimePreset)
	{
		RuntimePreset = DuplicateObject<UKawaiiFluidPresetDataAsset>(Preset, GetTransientPackage());
	}
	else
	{
		// 기존 RuntimePreset을 베이스 Preset으로 리셋
		RuntimePreset->RestDensity = Preset->RestDensity;
		RuntimePreset->Compliance = Preset->Compliance;
		RuntimePreset->SmoothingRadius = Preset->SmoothingRadius;
		RuntimePreset->ViscosityCoefficient = Preset->ViscosityCoefficient;
		RuntimePreset->Gravity = Preset->Gravity;
		RuntimePreset->AdhesionStrength = Preset->AdhesionStrength;
		RuntimePreset->ParticleRadius = Preset->ParticleRadius;
	}

	// Override 적용
	if (bOverride_RestDensity)
	{
		RuntimePreset->RestDensity = Override_RestDensity;
	}
	if (bOverride_Compliance)
	{
		RuntimePreset->Compliance = Override_Compliance;
	}
	if (bOverride_SmoothingRadius)
	{
		RuntimePreset->SmoothingRadius = Override_SmoothingRadius;
	}
	if (bOverride_ViscosityCoefficient)
	{
		RuntimePreset->ViscosityCoefficient = Override_ViscosityCoefficient;
	}
	if (bOverride_Gravity)
	{
		RuntimePreset->Gravity = Override_Gravity;
	}
	if (bOverride_AdhesionStrength)
	{
		RuntimePreset->AdhesionStrength = Override_AdhesionStrength;
	}
	if (bOverride_ParticleRadius)
	{
		RuntimePreset->ParticleRadius = Override_ParticleRadius;
	}

	bRuntimePresetDirty = false;
}

//========================================
// Override Setters
//========================================

void UKawaiiFluidSimulationModule::SetOverride_ParticleRadius(bool bEnable, float Value)
{
	bOverride_ParticleRadius = bEnable;
	Override_ParticleRadius = Value;
	bRuntimePresetDirty = true;
}

void UKawaiiFluidSimulationModule::SetOverride_SmoothingRadius(bool bEnable, float Value)
{
	bOverride_SmoothingRadius = bEnable;
	Override_SmoothingRadius = Value;
	bRuntimePresetDirty = true;

	// SpatialHash 재생성
	if (SpatialHash.IsValid())
	{
		float CellSize = bEnable ? Value : (Preset ? Preset->SmoothingRadius : 20.0f);
		SpatialHash = MakeShared<FSpatialHash>(CellSize);
	}
}

void UKawaiiFluidSimulationModule::SetOverride_RestDensity(bool bEnable, float Value)
{
	bOverride_RestDensity = bEnable;
	Override_RestDensity = Value;
	bRuntimePresetDirty = true;
}

void UKawaiiFluidSimulationModule::SetOverride_Compliance(bool bEnable, float Value)
{
	bOverride_Compliance = bEnable;
	Override_Compliance = Value;
	bRuntimePresetDirty = true;
}

void UKawaiiFluidSimulationModule::SetOverride_ViscosityCoefficient(bool bEnable, float Value)
{
	bOverride_ViscosityCoefficient = bEnable;
	Override_ViscosityCoefficient = Value;
	bRuntimePresetDirty = true;
}

void UKawaiiFluidSimulationModule::SetOverride_Gravity(bool bEnable, FVector Value)
{
	bOverride_Gravity = bEnable;
	Override_Gravity = Value;
	bRuntimePresetDirty = true;
}

void UKawaiiFluidSimulationModule::SetOverride_AdhesionStrength(bool bEnable, float Value)
{
	bOverride_AdhesionStrength = bEnable;
	Override_AdhesionStrength = Value;
	bRuntimePresetDirty = true;
}

AActor* UKawaiiFluidSimulationModule::GetOwnerActor() const
{
	if (UActorComponent* OwnerComp = Cast<UActorComponent>(GetOuter()))
	{
		return OwnerComp->GetOwner();
	}
	return nullptr;
}

FKawaiiFluidSimulationParams UKawaiiFluidSimulationModule::BuildSimulationParams() const
{
	FKawaiiFluidSimulationParams Params;

	// 외력
	Params.ExternalForce = AccumulatedExternalForce;

	// 콜라이더 / 상호작용 컴포넌트
	Params.Colliders = Colliders;
	Params.InteractionComponents = InteractionComponents;

	// Preset에서 콜리전 설정 가져오기
	if (Preset)
	{
		Params.CollisionChannel = Preset->CollisionChannel;
		Params.ParticleRadius = GetParticleRadius();  // Use getter to respect override
	}

	// Context - Module에서 직접 접근 (Outer 체인 활용)
	Params.World = GetWorld();
	Params.IgnoreActor = GetOwnerActor();
	Params.bUseWorldCollision = bUseWorldCollision;

	// GPU Simulation - get from owner component
	if (UKawaiiFluidComponent* OwnerComp = Cast<UKawaiiFluidComponent>(GetOuter()))
	{
		Params.bUseGPUSimulation = OwnerComp->bUseGPUSimulation;
	}

	// Event Settings
	Params.bEnableCollisionEvents = bEnableCollisionEvents;
	Params.MinVelocityForEvent = MinVelocityForEvent;
	Params.MaxEventsPerFrame = MaxEventsPerFrame;
	Params.EventCooldownPerParticle = EventCooldownPerParticle;

	if (bEnableCollisionEvents)
	{
		// 쿨다운 추적용 맵 연결 (const_cast 필요 - mutable 대안)
		Params.ParticleLastEventTimePtr = const_cast<TMap<int32, float>*>(&ParticleLastEventTime);

		// 현재 게임 시간
		if (UWorld* World = GetWorld())
		{
			Params.CurrentGameTime = World->GetTimeSeconds();
		}

		// 콜백 바인딩
		if (OnCollisionEventCallback.IsBound())
		{
			Params.OnCollisionEvent = OnCollisionEventCallback;
		}
	}

	return Params;
}

int32 UKawaiiFluidSimulationModule::SpawnParticle(FVector Position, FVector Velocity)
{
	FFluidParticle NewParticle(Position, NextParticleID++);
	NewParticle.Velocity = Velocity;

	if (Preset)
	{
		NewParticle.Mass = Preset->ParticleMass;
	}
	else
	{
		NewParticle.Mass = 1.0f;
	}

	Particles.Add(NewParticle);
	return NewParticle.ParticleID;
}

void UKawaiiFluidSimulationModule::SpawnParticles(FVector Location, int32 Count, float SpawnRadius)
{
	Particles.Reserve(Particles.Num() + Count);

	for (int32 i = 0; i < Count; ++i)
	{
		FVector RandomOffset = FMath::VRand() * FMath::FRandRange(0.0f, SpawnRadius);
		FVector SpawnPos = Location + RandomOffset;
		SpawnParticle(SpawnPos);
	}
}

void UKawaiiFluidSimulationModule::ClearAllParticles()
{
	Particles.Empty();
	NextParticleID = 0;
}

TArray<FVector> UKawaiiFluidSimulationModule::GetParticlePositions() const
{
	TArray<FVector> Positions;
	Positions.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Positions.Add(Particle.Position);
	}

	return Positions;
}

TArray<FVector> UKawaiiFluidSimulationModule::GetParticleVelocities() const
{
	TArray<FVector> Velocities;
	Velocities.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Velocities.Add(Particle.Velocity);
	}

	return Velocities;
}

void UKawaiiFluidSimulationModule::ApplyExternalForce(FVector Force)
{
	AccumulatedExternalForce += Force;
}

void UKawaiiFluidSimulationModule::ApplyForceToParticle(int32 ParticleIndex, FVector Force)
{
	if (Particles.IsValidIndex(ParticleIndex))
	{
		Particles[ParticleIndex].Velocity += Force;
	}
}

void UKawaiiFluidSimulationModule::RegisterCollider(UFluidCollider* Collider)
{
	if (Collider && !Colliders.Contains(Collider))
	{
		Colliders.Add(Collider);
	}
}

void UKawaiiFluidSimulationModule::UnregisterCollider(UFluidCollider* Collider)
{
	Colliders.Remove(Collider);
}

void UKawaiiFluidSimulationModule::RegisterInteractionComponent(UFluidInteractionComponent* Component)
{
	if (Component && !InteractionComponents.Contains(Component))
	{
		InteractionComponents.Add(Component);
	}
}

void UKawaiiFluidSimulationModule::UnregisterInteractionComponent(UFluidInteractionComponent* Component)
{
	InteractionComponents.Remove(Component);
}

TArray<int32> UKawaiiFluidSimulationModule::GetParticlesInRadius(FVector Location, float Radius) const
{
	TArray<int32> Result;
	const float RadiusSq = Radius * Radius;

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		const float DistSq = FVector::DistSquared(Particles[i].Position, Location);
		if (DistSq <= RadiusSq)
		{
			Result.Add(i);
		}
	}

	return Result;
}

TArray<int32> UKawaiiFluidSimulationModule::GetParticlesInBox(FVector Center, FVector Extent) const
{
	TArray<int32> Result;
	const FBox Box(Center - Extent, Center + Extent);

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		if (Box.IsInside(Particles[i].Position))
		{
			Result.Add(i);
		}
	}

	return Result;
}

bool UKawaiiFluidSimulationModule::GetParticleInfo(int32 ParticleIndex, FVector& OutPosition, FVector& OutVelocity, float& OutDensity) const
{
	if (!Particles.IsValidIndex(ParticleIndex))
	{
		return false;
	}

	const FFluidParticle& Particle = Particles[ParticleIndex];
	OutPosition = Particle.Position;
	OutVelocity = Particle.Velocity;
	OutDensity = Particle.Density;

	return true;
}

//========================================
// IKawaiiFluidDataProvider Interface
//========================================

float UKawaiiFluidSimulationModule::GetParticleRadius() const
{
	// Override가 있으면 Override 값 반환
	if (bOverride_ParticleRadius)
	{
		return Override_ParticleRadius;
	}

	// Preset에서 실제 시뮬레이션 파티클 반경 가져오기
	if (Preset)
	{
		return Preset->ParticleRadius;
	}
	
	return 10.0f; // 기본값
}

FString UKawaiiFluidSimulationModule::GetDebugName() const
{
	AActor* Owner = GetOwnerActor();
	return FString::Printf(TEXT("SimulationModule_%s"),
		Owner ? *Owner->GetName() : TEXT("NoOwner"));
}

//========================================
// GPU Buffer Access (Phase 2)
//========================================

bool UKawaiiFluidSimulationModule::IsGPUSimulationActive() const
{
	return bGPUSimulationActive && CachedGPUSimulator != nullptr;
}

int32 UKawaiiFluidSimulationModule::GetGPUParticleCount() const
{
	if (CachedGPUSimulator && CachedGPUSimulator->IsReady())
	{
		return CachedGPUSimulator->GetParticleCount();
	}
	return 0;
}
