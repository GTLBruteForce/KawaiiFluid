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

int32 UKawaiiFluidSimulationModule::SpawnParticlesSphere(FVector Center, float Radius, float Spacing,
                                                         bool bJitter, float JitterAmount, FVector Velocity,
                                                         FRotator Rotation)
{
	if (Spacing <= 0.0f || Radius <= 0.0f)
	{
		return 0;
	}

	// 구체를 포함하는 박스 범위 계산
	const int32 GridSize = FMath::CeilToInt(Radius / Spacing);
	const float RadiusSq = Radius * Radius;
	const float HalfSpacing = Spacing * 0.5f;
	const float JitterRange = Spacing * JitterAmount;

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용 (구형은 위치 회전 불필요)
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;

	// 예상 파티클 수 계산 (구 부피 / 파티클 부피)
	const float EstimatedCount = (4.0f / 3.0f * PI * Radius * Radius * Radius) / (Spacing * Spacing * Spacing);
	Particles.Reserve(Particles.Num() + FMath::CeilToInt(EstimatedCount));

	// 격자 순회
	for (int32 x = -GridSize; x <= GridSize; ++x)
	{
		for (int32 y = -GridSize; y <= GridSize; ++y)
		{
			for (int32 z = -GridSize; z <= GridSize; ++z)
			{
				FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

				// 구 내부인지 확인
				if (LocalPos.SizeSquared() <= RadiusSq)
				{
					FVector SpawnPos = Center + LocalPos;

					// Jitter 적용
					if (bJitter && JitterRange > 0.0f)
					{
						SpawnPos += FVector(
							FMath::FRandRange(-JitterRange, JitterRange),
							FMath::FRandRange(-JitterRange, JitterRange),
							FMath::FRandRange(-JitterRange, JitterRange)
						);
					}

					SpawnParticle(SpawnPos, WorldVelocity);
					++SpawnedCount;
				}
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesBox(FVector Center, FVector Extent, float Spacing,
                                                      bool bJitter, float JitterAmount, FVector Velocity,
                                                      FRotator Rotation)
{
	if (Spacing <= 0.0f)
	{
		return 0;
	}

	// 각 축별 격자 개수 계산
	const int32 CountX = FMath::Max(1, FMath::CeilToInt(Extent.X * 2.0f / Spacing));
	const int32 CountY = FMath::Max(1, FMath::CeilToInt(Extent.Y * 2.0f / Spacing));
	const int32 CountZ = FMath::Max(1, FMath::CeilToInt(Extent.Z * 2.0f / Spacing));

	const float JitterRange = Spacing * JitterAmount;
	const FVector LocalStartOffset = -Extent + FVector(Spacing * 0.5f);

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	const int32 TotalCount = CountX * CountY * CountZ;
	Particles.Reserve(Particles.Num() + TotalCount);

	for (int32 x = 0; x < CountX; ++x)
	{
		for (int32 y = 0; y < CountY; ++y)
		{
			for (int32 z = 0; z < CountZ; ++z)
			{
				// 로컬 위치 계산
				FVector LocalPos = LocalStartOffset + FVector(x * Spacing, y * Spacing, z * Spacing);

				// Jitter 적용 (로컬 공간에서)
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				// 회전 적용 후 월드 위치 계산
				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);

				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesCylinder(FVector Center, float Radius, float HalfHeight, float Spacing,
                                                           bool bJitter, float JitterAmount, FVector Velocity,
                                                           FRotator Rotation)
{
	if (Spacing <= 0.0f || Radius <= 0.0f || HalfHeight <= 0.0f)
	{
		return 0;
	}

	// 원기둥을 포함하는 박스 범위 계산
	const int32 GridSizeXY = FMath::CeilToInt(Radius / Spacing);
	const int32 GridSizeZ = FMath::CeilToInt(HalfHeight / Spacing);
	const float RadiusSq = Radius * Radius;
	const float JitterRange = Spacing * JitterAmount;

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;

	// 예상 파티클 수 계산 (원기둥 부피 / 파티클 부피)
	const float EstimatedCount = (PI * Radius * Radius * HalfHeight * 2.0f) / (Spacing * Spacing * Spacing);
	Particles.Reserve(Particles.Num() + FMath::CeilToInt(EstimatedCount));

	// 격자 순회
	for (int32 x = -GridSizeXY; x <= GridSizeXY; ++x)
	{
		for (int32 y = -GridSizeXY; y <= GridSizeXY; ++y)
		{
			// XY 평면에서 원 내부인지 확인
			const float DistSqXY = x * x * Spacing * Spacing + y * y * Spacing * Spacing;
			if (DistSqXY > RadiusSq)
			{
				continue;
			}

			for (int32 z = -GridSizeZ; z <= GridSizeZ; ++z)
			{
				FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

				// Jitter 적용 (로컬 공간에서)
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				// 회전 적용 후 월드 위치 계산
				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);

				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticleDirectional(FVector Position, FVector Direction, float Speed,
                                                             float Radius, float ConeAngle)
{
	// 방향 정규화
	FVector Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = FVector(0, 0, -1);  // 기본: 아래 방향
	}

	FVector SpawnPos = Position;
	FVector SpawnVel = Dir * Speed;

	// 스트림 반경 적용 (위치 분산)
	if (Radius > 0.0f)
	{
		// 방향에 수직인 평면에서 랜덤 오프셋
		FVector Right, Up;
		Dir.FindBestAxisVectors(Right, Up);

		const float RandomAngle = FMath::FRandRange(0.0f, 2.0f * PI);
		const float RandomRadius = FMath::FRandRange(0.0f, Radius);

		SpawnPos += Right * FMath::Cos(RandomAngle) * RandomRadius;
		SpawnPos += Up * FMath::Sin(RandomAngle) * RandomRadius;
	}

	// 원뿔 분사각 적용 (속도 방향 분산)
	if (ConeAngle > 0.0f)
	{
		// 분사각 범위 내에서 랜덤 방향
		const float HalfAngleRad = FMath::DegreesToRadians(ConeAngle * 0.5f);
		const float RandomPhi = FMath::FRandRange(0.0f, 2.0f * PI);
		const float RandomTheta = FMath::FRandRange(0.0f, HalfAngleRad);

		// 로컬 좌표계에서 랜덤 방향 생성
		FVector Right, Up;
		Dir.FindBestAxisVectors(Right, Up);

		const float SinTheta = FMath::Sin(RandomTheta);
		FVector RandomDir = Dir * FMath::Cos(RandomTheta)
		                  + Right * SinTheta * FMath::Cos(RandomPhi)
		                  + Up * SinTheta * FMath::Sin(RandomPhi);

		SpawnVel = RandomDir.GetSafeNormal() * Speed;
	}

	return SpawnParticle(SpawnPos, SpawnVel);
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

//========================================
// 명시적 개수 지정 스폰 함수
//========================================

int32 UKawaiiFluidSimulationModule::SpawnParticlesSphereByCount(FVector Center, float Radius, int32 Count,
                                                                bool bJitter, float JitterAmount, FVector Velocity,
                                                                FRotator Rotation)
{
	if (Count <= 0 || Radius <= 0.0f)
	{
		return 0;
	}

	// 개수에서 간격 역계산: 구 부피 = (4/3)πr³, 파티클당 부피 = spacing³
	// Count = Volume / spacing³ → spacing = (Volume / Count)^(1/3)
	const float Volume = (4.0f / 3.0f) * PI * Radius * Radius * Radius;
	const float Spacing = FMath::Pow(Volume / Count, 1.0f / 3.0f);

	// 실제 격자 기반으로 스폰 (Spacing 기반 함수와 동일한 결과를 위해)
	const int32 GridSize = FMath::CeilToInt(Radius / Spacing);
	const float RadiusSq = Radius * Radius;
	const float JitterRange = Spacing * JitterAmount;

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용 (구형은 위치 회전 불필요)
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	Particles.Reserve(Particles.Num() + Count);

	// 격자 순회
	for (int32 x = -GridSize; x <= GridSize && SpawnedCount < Count; ++x)
	{
		for (int32 y = -GridSize; y <= GridSize && SpawnedCount < Count; ++y)
		{
			for (int32 z = -GridSize; z <= GridSize && SpawnedCount < Count; ++z)
			{
				FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

				// 구 내부인지 확인
				if (LocalPos.SizeSquared() <= RadiusSq)
				{
					FVector SpawnPos = Center + LocalPos;

					// Jitter 적용
					if (bJitter && JitterRange > 0.0f)
					{
						SpawnPos += FVector(
							FMath::FRandRange(-JitterRange, JitterRange),
							FMath::FRandRange(-JitterRange, JitterRange),
							FMath::FRandRange(-JitterRange, JitterRange)
						);
					}

					SpawnParticle(SpawnPos, WorldVelocity);
					++SpawnedCount;
				}
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesBoxByCount(FVector Center, FVector Extent, int32 Count,
                                                             bool bJitter, float JitterAmount, FVector Velocity,
                                                             FRotator Rotation)
{
	if (Count <= 0)
	{
		return 0;
	}

	// 개수에서 간격 역계산: 박스 부피 = 8 * Extent.X * Extent.Y * Extent.Z
	const float Volume = 8.0f * Extent.X * Extent.Y * Extent.Z;

	if (Volume <= 0.0f)
	{
		return 0;
	}

	// 균등 분포를 위해 각 축의 비율 유지
	// n = nx * ny * nz, 비율 유지하면 nx:ny:nz = Ex:Ey:Ez
	// spacing = (Volume / Count)^(1/3)
	const float Spacing = FMath::Pow(Volume / Count, 1.0f / 3.0f);

	const int32 CountX = FMath::Max(1, FMath::RoundToInt(Extent.X * 2.0f / Spacing));
	const int32 CountY = FMath::Max(1, FMath::RoundToInt(Extent.Y * 2.0f / Spacing));
	const int32 CountZ = FMath::Max(1, FMath::RoundToInt(Extent.Z * 2.0f / Spacing));

	const float JitterRange = Spacing * JitterAmount;
	const FVector LocalStartOffset = -Extent + FVector(Spacing * 0.5f);

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	Particles.Reserve(Particles.Num() + Count);

	for (int32 x = 0; x < CountX && SpawnedCount < Count; ++x)
	{
		for (int32 y = 0; y < CountY && SpawnedCount < Count; ++y)
		{
			for (int32 z = 0; z < CountZ && SpawnedCount < Count; ++z)
			{
				// 로컬 위치 계산
				FVector LocalPos = LocalStartOffset + FVector(x * Spacing, y * Spacing, z * Spacing);

				// Jitter 적용 (로컬 공간에서)
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				// 회전 적용 후 월드 위치 계산
				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);

				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesCylinderByCount(FVector Center, float Radius, float HalfHeight, int32 Count,
                                                                   bool bJitter, float JitterAmount, FVector Velocity,
                                                                   FRotator Rotation)
{
	if (Count <= 0 || Radius <= 0.0f || HalfHeight <= 0.0f)
	{
		return 0;
	}

	// 개수에서 간격 역계산: 원기둥 부피 = π * r² * 2h
	const float Volume = PI * Radius * Radius * HalfHeight * 2.0f;
	const float Spacing = FMath::Pow(Volume / Count, 1.0f / 3.0f);

	const int32 GridSizeXY = FMath::CeilToInt(Radius / Spacing);
	const int32 GridSizeZ = FMath::CeilToInt(HalfHeight / Spacing);
	const float RadiusSq = Radius * Radius;
	const float JitterRange = Spacing * JitterAmount;

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	Particles.Reserve(Particles.Num() + Count);

	// 격자 순회
	for (int32 x = -GridSizeXY; x <= GridSizeXY && SpawnedCount < Count; ++x)
	{
		for (int32 y = -GridSizeXY; y <= GridSizeXY && SpawnedCount < Count; ++y)
		{
			// XY 평면에서 원 내부인지 확인
			const float DistSqXY = x * x * Spacing * Spacing + y * y * Spacing * Spacing;
			if (DistSqXY > RadiusSq)
			{
				continue;
			}

			for (int32 z = -GridSizeZ; z <= GridSizeZ && SpawnedCount < Count; ++z)
			{
				FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

				// Jitter 적용 (로컬 공간에서)
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				// 회전 적용 후 월드 위치 계산
				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);

				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}
