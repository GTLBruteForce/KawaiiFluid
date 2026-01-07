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

	// Containment bounds for GPU collision (supports OBB with rotation)
	if (bEnableContainment)
	{
		// OBB parameters (Center, Extent, Rotation)
		Params.BoundsCenter = ContainmentCenter;
		Params.BoundsExtent = ContainmentExtent;
		Params.BoundsRotation = ContainmentRotation;

		// Also compute AABB from OBB for legacy/fallback
		// For rotated box, compute the axis-aligned bounding box
		FVector RotatedExtents[8];
		for (int32 i = 0; i < 8; ++i)
		{
			FVector Corner(
				(i & 1) ? ContainmentExtent.X : -ContainmentExtent.X,
				(i & 2) ? ContainmentExtent.Y : -ContainmentExtent.Y,
				(i & 4) ? ContainmentExtent.Z : -ContainmentExtent.Z
			);
			RotatedExtents[i] = ContainmentRotation.RotateVector(Corner);
		}

		FVector AABBMin = RotatedExtents[0];
		FVector AABBMax = RotatedExtents[0];
		for (int32 i = 1; i < 8; ++i)
		{
			AABBMin = AABBMin.ComponentMin(RotatedExtents[i]);
			AABBMax = AABBMax.ComponentMax(RotatedExtents[i]);
		}

		Params.WorldBounds = FBox(ContainmentCenter + AABBMin, ContainmentCenter + AABBMax);
		Params.BoundsRestitution = ContainmentRestitution;
		Params.BoundsFriction = ContainmentFriction;
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
	const float Mass = Preset ? Preset->ParticleMass : 1.0f;
	const float Radius = Preset ? Preset->ParticleRadius : 5.0f;
	const int32 ParticleID = NextParticleID++;

	// GPU mode: send spawn request directly to GPU (no CPU Particles array)
	if (bGPUSimulationActive && CachedGPUSimulator)
	{
		FGPUSpawnRequest Request;
		Request.Position = FVector3f(Position);
		Request.Velocity = FVector3f(Velocity);
		Request.Mass = Mass;
		Request.Radius = Radius;

		// Use batch API to preserve Radius value
		TArray<FGPUSpawnRequest> Requests;
		Requests.Add(Request);
		CachedGPUSimulator->AddSpawnRequests(Requests);

		static int32 GPUSpawnLogCounter = 0;
		if (++GPUSpawnLogCounter % 60 == 1)
		{
			UE_LOG(LogTemp, Log, TEXT("SpawnParticle: GPU path - PendingCount=%d, Simulator=%p"),
				CachedGPUSimulator->GetPendingSpawnCount(), CachedGPUSimulator);
		}
		return ParticleID;
	}

	// Debug: CPU path should not be used in GPU mode
	static int32 CPUSpawnLogCounter = 0;
	if (++CPUSpawnLogCounter % 60 == 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnParticle: CPU path (bGPUActive=%d, Simulator=%p)"),
			bGPUSimulationActive ? 1 : 0, CachedGPUSimulator);
	}

	// CPU mode: add to Particles array
	FFluidParticle NewParticle(Position, ParticleID);
	NewParticle.Velocity = Velocity;
	NewParticle.Mass = Mass;
	Particles.Add(NewParticle);
	return ParticleID;
}

void UKawaiiFluidSimulationModule::SpawnParticles(FVector Location, int32 Count, float SpawnRadius)
{
	const float Mass = Preset ? Preset->ParticleMass : 1.0f;
	const float Radius = Preset ? Preset->ParticleRadius : 5.0f;

	// GPU mode: batch spawn requests
	if (bGPUSimulationActive && CachedGPUSimulator)
	{
		TArray<FGPUSpawnRequest> SpawnRequests;
		SpawnRequests.Reserve(Count);

		for (int32 i = 0; i < Count; ++i)
		{
			FVector RandomOffset = FMath::VRand() * FMath::FRandRange(0.0f, SpawnRadius);
			FVector SpawnPos = Location + RandomOffset;

			FGPUSpawnRequest Request;
			Request.Position = FVector3f(SpawnPos);
			Request.Velocity = FVector3f::ZeroVector;
			Request.Mass = Mass;
			Request.Radius = Radius;
			SpawnRequests.Add(Request);
			NextParticleID++;
		}

		CachedGPUSimulator->AddSpawnRequests(SpawnRequests);
		return;
	}

	// CPU mode: add to Particles array
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

	// GPU mode: batch spawn requests for efficiency
	if (bGPUSimulationActive && CachedGPUSimulator)
	{
		const float Mass = Preset ? Preset->ParticleMass : 1.0f;
		const float ParticleRadius = Preset ? Preset->ParticleRadius : 5.0f;

		TArray<FGPUSpawnRequest> SpawnRequests;
		SpawnRequests.Reserve(FMath::CeilToInt(EstimatedCount));

		for (int32 x = -GridSize; x <= GridSize; ++x)
		{
			for (int32 y = -GridSize; y <= GridSize; ++y)
			{
				for (int32 z = -GridSize; z <= GridSize; ++z)
				{
					FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

					if (LocalPos.SizeSquared() <= RadiusSq)
					{
						FVector SpawnPos = Center + LocalPos;

						if (bJitter && JitterRange > 0.0f)
						{
							SpawnPos += FVector(
								FMath::FRandRange(-JitterRange, JitterRange),
								FMath::FRandRange(-JitterRange, JitterRange),
								FMath::FRandRange(-JitterRange, JitterRange)
							);
						}

						FGPUSpawnRequest Request;
						Request.Position = FVector3f(SpawnPos);
						Request.Velocity = FVector3f(WorldVelocity);
						Request.Mass = Mass;
						Request.Radius = ParticleRadius;
						SpawnRequests.Add(Request);
						NextParticleID++;
						++SpawnedCount;
					}
				}
			}
		}

		if (SpawnRequests.Num() > 0)
		{
			CachedGPUSimulator->AddSpawnRequests(SpawnRequests);
		}
		return SpawnedCount;
	}

	// CPU mode: add to Particles array
	Particles.Reserve(Particles.Num() + FMath::CeilToInt(EstimatedCount));

	for (int32 x = -GridSize; x <= GridSize; ++x)
	{
		for (int32 y = -GridSize; y <= GridSize; ++y)
		{
			for (int32 z = -GridSize; z <= GridSize; ++z)
			{
				FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

				if (LocalPos.SizeSquared() <= RadiusSq)
				{
					FVector SpawnPos = Center + LocalPos;

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

int32 UKawaiiFluidSimulationModule::SpawnParticleDirectionalHexLayer(FVector Position, FVector Direction, float Speed,
                                                                      float Radius, float Spacing, float Jitter)
{
	// 방향 정규화
	FVector Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = FVector(0, 0, -1);  // 기본: 아래 방향
	}

	// 자동 간격 계산 (SmoothingRadius * 0.5)
	if (Spacing <= 0.0f)
	{
		Spacing = Preset ? Preset->SmoothingRadius * 0.5f : 10.0f;
	}

	// Jitter 범위 제한 (0 ~ 0.5)
	Jitter = FMath::Clamp(Jitter, 0.0f, 0.5f);
	const float MaxJitterOffset = Spacing * Jitter;
	const bool bApplyJitter = Jitter > KINDA_SMALL_NUMBER;

	// 방향에 수직인 로컬 좌표계 생성
	FVector Right, Up;
	Dir.FindBestAxisVectors(Right, Up);

	// Hexagonal Packing 상수
	const float RowSpacing = Spacing * FMath::Sqrt(3.0f) * 0.5f;  // ~0.866 * Spacing
	const float RadiusSq = Radius * Radius;

	// 행 개수 계산
	const int32 NumRows = FMath::CeilToInt(Radius / RowSpacing) * 2 + 1;
	const int32 HalfRows = NumRows / 2;

	int32 SpawnedCount = 0;
	const FVector SpawnVel = Dir * Speed;

	// Hexagonal grid 순회
	for (int32 RowIdx = -HalfRows; RowIdx <= HalfRows; ++RowIdx)
	{
		const float LocalY = RowIdx * RowSpacing;
		const float LocalYSq = LocalY * LocalY;

		// 이 행에서 원 내부의 최대 X 범위 계산
		if (LocalYSq > RadiusSq)
		{
			continue;  // 원 밖의 행
		}

		const float MaxX = FMath::Sqrt(RadiusSq - LocalYSq);

		// 홀수 행은 X 오프셋 적용 (Hexagonal Packing)
		const float XOffset = (FMath::Abs(RowIdx) % 2 != 0) ? Spacing * 0.5f : 0.0f;

		// X 시작점 계산 (중심 기준 대칭)
		const int32 NumCols = FMath::FloorToInt(MaxX / Spacing);

		for (int32 ColIdx = -NumCols; ColIdx <= NumCols; ++ColIdx)
		{
			float LocalX = ColIdx * Spacing + XOffset;
			float LocalYFinal = LocalY;

			// Jitter 적용: 랜덤 오프셋 추가
			if (bApplyJitter)
			{
				LocalX += FMath::FRandRange(-MaxJitterOffset, MaxJitterOffset);
				LocalYFinal += FMath::FRandRange(-MaxJitterOffset, MaxJitterOffset);
			}

			// 원 내부 체크 (jitter 후에도 원 내부에 있는지 확인)
			if (LocalX * LocalX + LocalYFinal * LocalYFinal <= RadiusSq)
			{
				FVector SpawnPos = Position + Right * LocalX + Up * LocalYFinal;
				SpawnParticle(SpawnPos, SpawnVel);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
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

//========================================
// Containment Volume
//========================================

void UKawaiiFluidSimulationModule::SetContainment(bool bEnabled, const FVector& Center, const FVector& Extent,
                                                   const FQuat& Rotation, float Restitution, float Friction)
{
	// UPROPERTY 값들은 에디터에서 설정, Center와 Rotation은 동적으로 설정됨
	bEnableContainment = bEnabled;
	ContainmentCenter = Center;
	ContainmentExtent = Extent;
	ContainmentRotation = Rotation;
	ContainmentRestitution = FMath::Clamp(Restitution, 0.0f, 1.0f);
	ContainmentFriction = FMath::Clamp(Friction, 0.0f, 1.0f);
}

void UKawaiiFluidSimulationModule::ResolveContainmentCollisions()
{
	if (!bEnableContainment)
	{
		return;
	}

	// OBB (Oriented Bounding Box) 충돌 처리
	// 파티클을 로컬 공간으로 변환하여 AABB 충돌 체크 후 다시 월드로 변환
	const FQuat InverseRotation = ContainmentRotation.Inverse();
	const FVector LocalBoxMin = -ContainmentExtent;
	const FVector LocalBoxMax = ContainmentExtent;

	for (FFluidParticle& P : Particles)
	{
		// 월드 -> 로컬 변환
		FVector LocalPos = InverseRotation.RotateVector(P.PredictedPosition - ContainmentCenter);
		FVector LocalVel = InverseRotation.RotateVector(P.Velocity);
		bool bCollided = false;

		// 로컬 공간에서 AABB 충돌 체크
		// X축 체크
		if (LocalPos.X < LocalBoxMin.X)
		{
			LocalPos.X = LocalBoxMin.X;
			if (LocalVel.X < 0.0f)
			{
				LocalVel.X = -LocalVel.X * ContainmentRestitution;
			}
			LocalVel.Y *= (1.0f - ContainmentFriction);
			LocalVel.Z *= (1.0f - ContainmentFriction);
			bCollided = true;
		}
		else if (LocalPos.X > LocalBoxMax.X)
		{
			LocalPos.X = LocalBoxMax.X;
			if (LocalVel.X > 0.0f)
			{
				LocalVel.X = -LocalVel.X * ContainmentRestitution;
			}
			LocalVel.Y *= (1.0f - ContainmentFriction);
			LocalVel.Z *= (1.0f - ContainmentFriction);
			bCollided = true;
		}

		// Y축 체크
		if (LocalPos.Y < LocalBoxMin.Y)
		{
			LocalPos.Y = LocalBoxMin.Y;
			if (LocalVel.Y < 0.0f)
			{
				LocalVel.Y = -LocalVel.Y * ContainmentRestitution;
			}
			LocalVel.X *= (1.0f - ContainmentFriction);
			LocalVel.Z *= (1.0f - ContainmentFriction);
			bCollided = true;
		}
		else if (LocalPos.Y > LocalBoxMax.Y)
		{
			LocalPos.Y = LocalBoxMax.Y;
			if (LocalVel.Y > 0.0f)
			{
				LocalVel.Y = -LocalVel.Y * ContainmentRestitution;
			}
			LocalVel.X *= (1.0f - ContainmentFriction);
			LocalVel.Z *= (1.0f - ContainmentFriction);
			bCollided = true;
		}

		// Z축 체크
		if (LocalPos.Z < LocalBoxMin.Z)
		{
			LocalPos.Z = LocalBoxMin.Z;
			if (LocalVel.Z < 0.0f)
			{
				LocalVel.Z = -LocalVel.Z * ContainmentRestitution;
			}
			LocalVel.X *= (1.0f - ContainmentFriction);
			LocalVel.Y *= (1.0f - ContainmentFriction);
			bCollided = true;
		}
		else if (LocalPos.Z > LocalBoxMax.Z)
		{
			LocalPos.Z = LocalBoxMax.Z;
			if (LocalVel.Z > 0.0f)
			{
				LocalVel.Z = -LocalVel.Z * ContainmentRestitution;
			}
			LocalVel.X *= (1.0f - ContainmentFriction);
			LocalVel.Y *= (1.0f - ContainmentFriction);
			bCollided = true;
		}

		// 로컬 -> 월드 변환하여 Position/Velocity 업데이트
		if (bCollided)
		{
			P.PredictedPosition = ContainmentCenter + ContainmentRotation.RotateVector(LocalPos);
			P.Position = P.PredictedPosition;
			P.Velocity = ContainmentRotation.RotateVector(LocalVel);
		}
	}
}
