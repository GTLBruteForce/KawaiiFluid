// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/FluidSimulator.h"
#include "Core/SpatialHash.h"
#include "Physics/DensityConstraint.h"
#include "Physics/ViscositySolver.h"
#include "Physics/AdhesionSolver.h"
#include "Collision/FluidCollider.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Components/FluidInteractionComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "DrawDebugHelpers.h"

// 프로파일링용 STAT 그룹
DECLARE_STATS_GROUP(TEXT("KawaiiFluid"), STATGROUP_KawaiiFluid, STATCAT_Advanced);

DECLARE_CYCLE_STAT(TEXT("Fluid Tick"), STAT_FluidTick, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Predict Positions"), STAT_PredictPositions, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Update Neighbors"), STAT_UpdateNeighbors, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Solve Density"), STAT_SolveDensity, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Cache Collider Shapes"), STAT_CacheColliderShapes, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Update Attached Particle Positions"), STAT_UpdateAttachedParticlePositions, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Handle Collisions"), STAT_HandleCollisions, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("World Collision"), STAT_WorldCollision, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Finalize Positions"), STAT_FinalizePositions, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Apply Viscosity"), STAT_ApplyViscosity, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Apply Adhesion"), STAT_ApplyAdhesion, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Debug Rendering"), STAT_DebugRendering, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Update Render Data"), STAT_UpdateRenderData, STATGROUP_KawaiiFluid);
DECLARE_DWORD_COUNTER_STAT(TEXT("Particle Count"), STAT_ParticleCount, STATGROUP_KawaiiFluid);

AFluidSimulator::AFluidSimulator()
{
	PrimaryActorTick.bCanEverTick = true;

	// 기본값 설정
	FluidType = EFluidType::Slime;
	MaxParticles = 10000;
	bSimulationEnabled = true;

	// 물리 파라미터 기본값 (언리얼 단위: cm)
	RestDensity = 1000.0f;
	ParticleMass = 1.0f;
	SmoothingRadius = 20.0f;  // 20cm (입자 반경의 약 4배)
	SubstepDeltaTime = 1.0f / 120.0f;  // 120Hz substep
	MaxSubsteps = 8;
	Gravity = FVector(0.0f, 0.0f, -980.0f);
	Compliance = 0.0001f;  // XPBD compliance

	// 점성 기본값 (슬라임)
	ViscosityCoefficient = 0.5f;

	// 접착력 기본값 (언리얼 단위: cm)
	AdhesionStrength = 0.5f;
	AdhesionRadius = 25.0f;  // 25cm
	DetachThreshold = 500.0f;

	// 내부 변수 초기화
	AccumulatedExternalForce = FVector::ZeroVector;
	NextParticleID = 0;
	AccumulatedTime = 0.0f;

	// 충돌 이벤트 시스템 초기화
	EventCountThisFrame = 0;
	CurrentGameTime = 0.0f;

	// 디버그 렌더링 기본값
	bEnableDebugRendering = true;
	DebugParticleRadius = 5.0f;
	bSpawnOnBeginPlay = true;
	AutoSpawnCount = 100;
	AutoSpawnRadius = 50.0f;

	// 월드 콜리전 기본값
	bUseWorldCollision = true;
	// ECC_GameTraceChannel1 = Project Settings에서 첫 번째로 만든 커스텀 채널 (Fluid)
	CollisionChannel = ECC_GameTraceChannel1;

	// 디버그 메시 컴포넌트 생성
	DebugMeshComponent = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("DebugMeshComponent"));
	DebugMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);  // 콜리전 비활성화
	RootComponent = DebugMeshComponent;

	// 기본 Sphere 메시 로드
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		DebugMeshComponent->SetStaticMesh(SphereMesh.Object);
	}

	// 기본 머티리얼 설정 (반투명 파란색)
	static ConstructorHelpers::FObjectFinder<UMaterial> DefaultMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (DefaultMaterial.Succeeded())
	{
		DebugMeshComponent->SetMaterial(0, DefaultMaterial.Object);
	}
}

AFluidSimulator::~AFluidSimulator()
{
	// TUniquePtr 멤버들(SpatialHash, DensityConstraint, ViscositySolver, AdhesionSolver)이
	// 여기서 자동으로 파괴됨. 빈 본문이지만 .cpp에 정의해야 완전한 타입 정의가 보장됨.
}

void AFluidSimulator::BeginPlay()
{
	Super::BeginPlay();

	InitializeSolvers();
	InitializeRenderResource();
	ApplyFluidTypePreset(FluidType);

	// FluidRendererSubsystem에 등록
	if (UWorld* World = GetWorld())
	{
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->RegisterSimulator(this);
			
			const TCHAR* ModeStr = (RenderingMode == EKawaiiFluidRenderingMode::SSFR) ? TEXT("SSFR") :
			                       (RenderingMode == EKawaiiFluidRenderingMode::DebugMesh) ? TEXT("DebugMesh") : TEXT("Both");
			
			UE_LOG(LogTemp, Log, TEXT("FluidSimulator registered: %s (Mode: %s)"), *GetName(), ModeStr);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem not found!"));
		}
	}

	// 자동 스폰
	if (bSpawnOnBeginPlay && AutoSpawnCount > 0)
	{
		SpawnParticles(GetActorLocation(), AutoSpawnCount, AutoSpawnRadius);
	}
}

void AFluidSimulator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Delegate 명시적 정리
	OnParticleHit.Clear();
	
	// 쿨다운 맵 정리
	ParticleLastEventTime.Empty();
	
	// 이벤트 비활성화
	bEnableParticleHitEvents = false;
	
	// FluidRendererSubsystem에서 등록 해제
	if (UWorld* World = GetWorld())
	{
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->UnregisterSimulator(this);
			UE_LOG(LogTemp, Log, TEXT("FluidSimulator unregistered from RendererSubsystem: %s"), *GetName());
		}
	}

	Super::EndPlay(EndPlayReason);
}

void AFluidSimulator::BeginDestroy()
{
	// 렌더 리소스 정리
	if (RenderResource.IsValid())
	{
		// 렌더 스레드에서 리소스 해제 후 SharedPtr 해제
		ENQUEUE_RENDER_COMMAND(ReleaseFluidRenderResource)(
			[RenderResource = MoveTemp(RenderResource)](FRHICommandListImmediate& RHICmdList) mutable
			{
				if (RenderResource.IsValid())
				{
					RenderResource->ReleaseResource();
					// 렌더 스레드에서 SharedPtr 해제 (안전하도록 렌더 스레드에서 실행)
					RenderResource.Reset();
				}
			}
		);
	}

	Super::BeginDestroy();
}

void AFluidSimulator::InitializeSolvers()
{
	SpatialHash = MakeShared<FSpatialHash>(SmoothingRadius);
	DensityConstraint = MakeShared<FDensityConstraint>(RestDensity, SmoothingRadius, Compliance);
	ViscositySolver = MakeShared<FViscositySolver>();
	AdhesionSolver = MakeShared<FAdhesionSolver>();
}

void AFluidSimulator::InitializeRenderResource()
{
	// GPU 렌더 리소스 생성 및 초기화
	RenderResource = MakeShared<FKawaiiFluidRenderResource>();
	
	// 렌더 스레드에서 리소스 등록
	ENQUEUE_RENDER_COMMAND(InitFluidRenderResource)(
		[RenderResourcePtr = RenderResource.Get()](FRHICommandListImmediate& RHICmdList)
		{
			RenderResourcePtr->InitResource(RHICmdList);
		}
	);
}

void AFluidSimulator::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_FluidTick);
	TRACE_CPUPROFILER_EVENT_SCOPE(FluidSimulator_Tick);

	Super::Tick(DeltaTime);

	SET_DWORD_STAT(STAT_ParticleCount, Particles.Num());

	// 이벤트 카운터 리셋 (매 프레임 시작 시)
	EventCountThisFrame = 0;
	CurrentGameTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	if (!bSimulationEnabled || Particles.Num() == 0)
	{
		return;
	}
	

	// Accumulator 방식: 고정 dt로 시뮬레이션
	// 시간 빚 방지: 최대 처리 가능한 시간만 누적
	const float MaxAllowedTime = SubstepDeltaTime * MaxSubsteps;
	AccumulatedTime += FMath::Min(DeltaTime, MaxAllowedTime);

	// 콜라이더 충돌 형상 캐싱 (프레임당 한 번)
	{
		SCOPE_CYCLE_COUNTER(STAT_CacheColliderShapes);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_CacheColliderShapes);
		CacheColliderShapes();
	}

	// PBF 시뮬레이션 루프
	// 0. 붙은 입자 위치 업데이트 (본 추적 - 물리 시뮬레이션 전에 실행해야 중력 효과 유지)
	{	
		SCOPE_CYCLE_COUNTER(STAT_UpdateAttachedParticlePositions);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_UpdateAttachedPositions);
		UpdateAttachedParticlePositions();
	}
	
	// Small Steps 루프 (고정 dt)
	while (AccumulatedTime >= SubstepDeltaTime)
	{
		// 1. 외력 적용 & 위치 예측
		{
			SCOPE_CYCLE_COUNTER(STAT_PredictPositions);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_PredictPositions);
			PredictPositions(SubstepDeltaTime);
		}
		// 2. 이웃 탐색
		{
			SCOPE_CYCLE_COUNTER(STAT_UpdateNeighbors);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_UpdateNeighbors);
			UpdateNeighbors();
		}

		// 3. 밀도 제약 해결 (1회) - XPBD
		{
			SCOPE_CYCLE_COUNTER(STAT_SolveDensity);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_SolveDensity);
			SolveDensityConstraints(SubstepDeltaTime);
		}

		// 4. 충돌 처리
		{
			SCOPE_CYCLE_COUNTER(STAT_HandleCollisions);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_HandleCollisions);
			HandleCollisions();
		}

		// 월드 콜리전
		if (bUseWorldCollision)
		{
			SCOPE_CYCLE_COUNTER(STAT_WorldCollision);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_WorldCollision);
			HandleWorldCollision();
		}

		// 5. 속도 업데이트 & 위치 확정
		{
			SCOPE_CYCLE_COUNTER(STAT_FinalizePositions);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_FinalizePositions);
			FinalizePositions(SubstepDeltaTime);
		}

		// 6. 점성 적용
		{
			SCOPE_CYCLE_COUNTER(STAT_ApplyViscosity);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_ApplyViscosity);
			ApplyViscosity();
		}

		// 7. 접착력 적용
		{
			SCOPE_CYCLE_COUNTER(STAT_ApplyAdhesion);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_ApplyAdhesion);
			ApplyAdhesion();
		}

		AccumulatedTime -= SubstepDeltaTime;
	}

	// 외력 리셋
	AccumulatedExternalForce = FVector::ZeroVector;

	// GPU 렌더 데이터 업데이트
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateRenderData);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_UpdateRenderData);
		UpdateRenderData();
	}

	// 디버그 렌더링 업데이트 (렌더링 모드에 따라 조건부)
	if (ShouldUseDebugMesh())
	{
		SCOPE_CYCLE_COUNTER(STAT_DebugRendering);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_DebugRendering);
		UpdateDebugInstances();
	}

	// 외력 리셋
	AccumulatedExternalForce = FVector::ZeroVector;
}

void AFluidSimulator::PredictPositions(float DeltaTime)
{
	const FVector TotalForce = Gravity + AccumulatedExternalForce;

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		FFluidParticle& Particle = Particles[i];

		FVector AppliedForce = TotalForce;

		// 접착된 입자는 표면 접선 방향 중력만 적용 (미끄러짐 효과)
		if (Particle.bIsAttached)
		{
			const FVector& Normal = Particle.AttachedSurfaceNormal;
			// 접선 중력 = 전체 중력 - 법선 방향 성분
			// TangentGravity = Gravity - (Gravity · Normal) * Normal
			float NormalComponent = FVector::DotProduct(Gravity, Normal);
			FVector TangentGravity = Gravity - NormalComponent * Normal;
			AppliedForce = TangentGravity + AccumulatedExternalForce;
		}

		Particle.Velocity += AppliedForce * DeltaTime;
		Particle.PredictedPosition = Particle.Position + Particle.Velocity * DeltaTime;
	});
}

void AFluidSimulator::UpdateNeighbors()
{
	// 공간 해시 재구축 (순차 - 해시맵 쓰기)
	TArray<FVector> Positions;
	Positions.Reserve(Particles.Num());

	// 각 입자의 위치 수집
	for (const FFluidParticle& Particle : Particles)
	{
		Positions.Add(Particle.PredictedPosition);
	}

	// 3D 격자에 넣기 
	SpatialHash->BuildFromPositions(Positions);

	// 각 입자의 이웃 캐싱 (병렬 - 읽기만)
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		SpatialHash->GetNeighbors(
			Particles[i].PredictedPosition,
			SmoothingRadius,
			Particles[i].NeighborIndices
		);
	});
}

void AFluidSimulator::SolveDensityConstraints(float DeltaTime)
{
	if (DensityConstraint.IsValid())
	{
		DensityConstraint->Solve(Particles, SmoothingRadius, RestDensity, Compliance, DeltaTime);
	}
}

void AFluidSimulator::CacheColliderShapes()
{
	for (UFluidCollider* Collider : Colliders)
	{
		if (Collider && Collider->IsColliderEnabled())
		{
			Collider->CacheCollisionShapes();
		}
	}
}

void AFluidSimulator::HandleCollisions()
{
	for (UFluidCollider* Collider : Colliders)
	{
		if (Collider && Collider->IsColliderEnabled())
		{
			Collider->ResolveCollisions(Particles);
		}
	}
}

void AFluidSimulator::HandleWorldCollision()
{
	UWorld* World = GetWorld();
	if (!World || Particles.Num() == 0)
	{
		return;
	}

	const float CellSize = SpatialHash->GetCellSize();
	const float ParticleRadius = DebugParticleRadius;

	// 1. SpatialHash에서 셀들 가져와서 배열로 변환 (Physics Lock 밖에서 준비)
	const auto& Grid = SpatialHash->GetGrid();

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;
	QueryParams.AddIgnoredActor(this);

	// 2. 셀 기반 broad-phase - 셀 배열 준비
	struct FCellQueryData
	{
		FVector CellCenter;
		FVector CellExtent;
		TArray<int32> ParticleIndices;
	};

	TArray<FCellQueryData> CellQueries;
	CellQueries.Reserve(Grid.Num());

	for (const auto& Pair : Grid)
	{
		FCellQueryData CellData;
		CellData.CellCenter = FVector(Pair.Key) * CellSize + FVector(CellSize * 0.5f);
		CellData.CellExtent = FVector(CellSize * 0.5f);
		CellData.ParticleIndices = Pair.Value;
		CellQueries.Add(MoveTemp(CellData));
	}

	// 3. 셀 Overlap 체크 - 병렬
	TArray<uint8> CellCollisionResults;
	CellCollisionResults.SetNumZeroed(CellQueries.Num());

	ParallelFor(CellQueries.Num(), [&](int32 CellIdx)
	{
		const FCellQueryData& CellData = CellQueries[CellIdx];
		if (World->OverlapBlockingTestByChannel(
			CellData.CellCenter, FQuat::Identity, CollisionChannel,
			FCollisionShape::MakeBox(CellData.CellExtent), QueryParams))
		{
			CellCollisionResults[CellIdx] = 1;
		}
	});

	// 4. 충돌 후보 수집
	TArray<int32> CollisionParticleIndices;
	CollisionParticleIndices.Reserve(Particles.Num());

	for (int32 CellIdx = 0; CellIdx < CellQueries.Num(); ++CellIdx)
	{
		if (CellCollisionResults[CellIdx])
		{
			CollisionParticleIndices.Append(CellQueries[CellIdx].ParticleIndices);
		}
	}

	if (CollisionParticleIndices.Num() == 0)
	{
		return;
	}

	// 3. Physics Scene Read Lock - Sweep 쿼리만 (최소 범위)
	FPhysScene* PhysScene = World->GetPhysicsScene();
	if (!PhysScene)
	{
		return;
	}

	FPhysicsCommand::ExecuteRead(PhysScene, [&]()
	{
		ParallelFor(CollisionParticleIndices.Num(), [&](int32 j)
		{
			const int32 i = CollisionParticleIndices[j];
			FFluidParticle& Particle = Particles[i];

			// Sweep
			FCollisionQueryParams LocalParams;
			LocalParams.bTraceComplex = false;
			LocalParams.bReturnPhysicalMaterial = false;
			LocalParams.AddIgnoredActor(this);

			FHitResult HitResult;
			bool bHit = World->SweepSingleByChannel(
				HitResult,
				Particle.Position,
				Particle.PredictedPosition,
				FQuat::Identity,
				CollisionChannel,
				FCollisionShape::MakeSphere(ParticleRadius),
				LocalParams
			);

			if (bHit && HitResult.bBlockingHit)
			{
				FVector CollisionPos = HitResult.Location + HitResult.ImpactNormal * 0.01f;

				Particle.PredictedPosition = CollisionPos;
				Particle.Position = CollisionPos;

				float VelDotNormal = FVector::DotProduct(Particle.Velocity, HitResult.ImpactNormal);
				if (VelDotNormal < 0.0f)
				{
					Particle.Velocity -= VelDotNormal * HitResult.ImpactNormal;
				}

				// 충돌 이벤트 발생 조건 체크
				if (bEnableParticleHitEvents)
				{
					const float Speed = Particle.Velocity.Size();
					
					// 조건: 최소 속도, 최대 이벤트 수, 쿨다운 체크
					if (Speed >= MinVelocityForEvent && 
						EventCountThisFrame < MaxEventsPerFrame)
					{
						// 쿨다운 체크 (동일 파티클 연속 이벤트 방지)
						bool bCanEmitEvent = true;
						
						if (EventCooldownPerParticle > 0.0f)
						{
							const float* LastEventTime = ParticleLastEventTime.Find(Particle.ParticleID);
							if (LastEventTime && (CurrentGameTime - *LastEventTime) < EventCooldownPerParticle)
							{
								bCanEmitEvent = false;
							}
						}
						
						if (bCanEmitEvent)
						{
							// TWeakObjectPtr로 안전하게 캡처
							TWeakObjectPtr<AFluidSimulator> WeakThis(this);
							
							// 이벤트 데이터 캡처 (게임 스레드로 전달)
							const int32 ParticleIdx = Particle.ParticleID;
							TWeakObjectPtr<AActor> WeakHitActor(HitResult.GetActor());
							const FVector HitLoc = HitResult.Location;
							const FVector HitNorm = HitResult.ImpactNormal;
							const float HitSpeed = Speed;
							const float CooldownTime = EventCooldownPerParticle;
							
							// 게임 스레드에서 Delegate 호출
							AsyncTask(ENamedThreads::GameThread, [WeakThis, ParticleIdx, WeakHitActor, HitLoc, HitNorm, HitSpeed, CooldownTime]()
							{
								// 안전성 체크: Simulator가 유효한지
								AFluidSimulator* Simulator = WeakThis.Get();
								if (!Simulator || !IsValid(Simulator))
								{
									return;  // Actor 파괴됨
								}
								
								// 충돌 Actor 체크
								AActor* HitActor = WeakHitActor.Get();
								
								// Delegate 호출
								if (Simulator->OnParticleHit.IsBound())
								{
									Simulator->OnParticleHit.Broadcast(ParticleIdx, HitActor, HitLoc, HitNorm, HitSpeed);
								}
								
								// 쿨다운 기록
								if (CooldownTime > 0.0f)
								{
									if (UWorld* World = Simulator->GetWorld())
									{
										float CurrentTime = World->GetTimeSeconds();
										Simulator->ParticleLastEventTime.Add(ParticleIdx, CurrentTime);
									}
								}
							});
							
							// 이벤트 카운터 증가 (근사치, Physics Thread에서)
							EventCountThisFrame++;
						}
					}
				}

				// 월드 콜리전에 닿으면 캐릭터에서 접착 해제
				// (충돌한 액터가 접착된 액터와 다른 경우에만)
				if (Particle.bIsAttached)
				{
					AActor* HitActor = HitResult.GetActor();
					if (HitActor != Particle.AttachedActor.Get())
					{
						Particle.bIsAttached = false;
						Particle.AttachedActor.Reset();
						Particle.AttachedBoneName = NAME_None;
						Particle.AttachedLocalOffset = FVector::ZeroVector;
						Particle.AttachedSurfaceNormal = FVector::UpVector;
					}
				}
			}
			// 추가: 붙어있는 입자가 바닥 근처에 있으면 분리 (Sweep 없이도)
			else if (Particle.bIsAttached)
			{
				// 아래 방향으로 짧은 라인트레이스로 바닥 감지
				const float FloorCheckDistance = 3.0f;
				FHitResult FloorHit;
				bool bNearFloor = World->LineTraceSingleByChannel(
					FloorHit,
					Particle.Position,
					Particle.Position - FVector(0, 0, FloorCheckDistance),
					CollisionChannel,
					LocalParams
				);

				if (bNearFloor && FloorHit.GetActor() != Particle.AttachedActor.Get())
				{
					Particle.bIsAttached = false;
					Particle.AttachedActor.Reset();
					Particle.AttachedBoneName = NAME_None;
					Particle.AttachedLocalOffset = FVector::ZeroVector;
					Particle.AttachedSurfaceNormal = FVector::UpVector;
				}
			}
		});
	});

	// 추가: 캐릭터에 붙은 입자가 바닥에 닿으면 분리
	// (캐릭터를 무시하고 바닥만 감지)
	const float FloorDetachDistance = 5.0f;   // 바닥에 닿음 (분리)
	const float FloorNearDistance = 20.0f;    // 바닥 근처 (접착 유지 마진 감소)

	for (FFluidParticle& Particle : Particles)
	{
		if (!Particle.bIsAttached)
		{
			Particle.bNearGround = false;
			continue;
		}

		// 접착된 캐릭터를 무시하고 바닥만 감지
		FCollisionQueryParams FloorQueryParams;
		FloorQueryParams.bTraceComplex = false;
		FloorQueryParams.AddIgnoredActor(this);
		if (Particle.AttachedActor.IsValid())
		{
			FloorQueryParams.AddIgnoredActor(Particle.AttachedActor.Get());
		}

		// 바닥 근처 체크 (더 넓은 범위)
		FHitResult FloorHit;
		bool bNearFloor = World->LineTraceSingleByChannel(
			FloorHit,
			Particle.Position,
			Particle.Position - FVector(0, 0, FloorNearDistance),
			CollisionChannel,
			FloorQueryParams
		);

		Particle.bNearGround = bNearFloor;

		// 바닥에 닿으면 무조건 캐릭터에서 분리
		if (bNearFloor && FloorHit.Distance <= FloorDetachDistance)
		{
			Particle.bIsAttached = false;
			Particle.AttachedActor.Reset();
			Particle.AttachedBoneName = NAME_None;
			Particle.AttachedLocalOffset = FVector::ZeroVector;
			Particle.AttachedSurfaceNormal = FVector::UpVector;
			Particle.bJustDetached = true;  // 같은 프레임 재접착 방지
		}
	}
}

void AFluidSimulator::FinalizePositions(float DeltaTime)
{
	const float InvDeltaTime = 1.0f / DeltaTime;

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		FFluidParticle& Particle = Particles[i];
		Particle.Velocity = (Particle.PredictedPosition - Particle.Position) * InvDeltaTime;
		Particle.Position = Particle.PredictedPosition;
	});
}

void AFluidSimulator::ApplyViscosity()
{
	if (ViscositySolver.IsValid() && ViscosityCoefficient > 0.0f)
	{
		ViscositySolver->ApplyXSPH(Particles, ViscosityCoefficient, SmoothingRadius);
	}
}

void AFluidSimulator::ApplyAdhesion()
{
	if (AdhesionSolver.IsValid() && AdhesionStrength > 0.0f)
	{
		AdhesionSolver->Apply(Particles, Colliders, AdhesionStrength, AdhesionRadius, DetachThreshold);
	}
}

void AFluidSimulator::RegisterInteractionComponent(UFluidInteractionComponent* Component)
{
	if (Component && !InteractionComponents.Contains(Component))
	{
		InteractionComponents.Add(Component);
	}
}

void AFluidSimulator::UnregisterInteractionComponent(UFluidInteractionComponent* Component)
{
	InteractionComponents.Remove(Component);
}

void AFluidSimulator::UpdateAttachedParticlePositions()
{
	// 디버그: 상태 체크
	static int32 DbgCounter = 0;
	if (++DbgCounter % 120 == 0)
	{
		int32 Attached = 0, WithBone = 0, NoBone = 0;
		for (const FFluidParticle& P : Particles)
		{
			if (P.bIsAttached)
			{
				Attached++;
				if (P.AttachedBoneName != NAME_None) WithBone++;
				else NoBone++;
			}
		}
		if (Attached > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BoneTrack] InteractionComps:%d Attached:%d WithBone:%d NoBone:%d"),
				InteractionComponents.Num(), Attached, WithBone, NoBone);
		}
	}

	if (InteractionComponents.Num() == 0 || Particles.Num() == 0)
	{
		return;
	}

	// 최적화: Owner별로 붙은 입자 인덱스 그룹화 (O(P) 1회 순회)
	TMap<AActor*, TArray<int32>> OwnerToParticleIndices;

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		const FFluidParticle& Particle = Particles[i];
		if (Particle.bIsAttached && Particle.AttachedActor.IsValid() && Particle.AttachedBoneName != NAME_None)
		{
			OwnerToParticleIndices.FindOrAdd(Particle.AttachedActor.Get()).Add(i);
		}
	}

	// InteractionComponent별로 처리
	for (UFluidInteractionComponent* Interaction : InteractionComponents)
	{
		if (!Interaction)
		{
			continue;
		}

		AActor* InteractionOwner = Interaction->GetOwner();
		if (!InteractionOwner)
		{
			continue;
		}

		// 이 Owner에 붙은 입자가 있는지 확인
		TArray<int32>* ParticleIndicesPtr = OwnerToParticleIndices.Find(InteractionOwner);
		if (!ParticleIndicesPtr || ParticleIndicesPtr->Num() == 0)
		{
			continue;
		}

		// 스켈레탈 메시 찾기
		USkeletalMeshComponent* SkelMesh = InteractionOwner->FindComponentByClass<USkeletalMeshComponent>();
		if (!SkelMesh)
		{
			continue;
		}

		// 본별 그룹화 (최적화: GetBoneTransform 호출 최소화)
		TMap<FName, TArray<int32>> BoneToParticleIndices;
		for (int32 ParticleIdx : *ParticleIndicesPtr)
		{
			const FFluidParticle& Particle = Particles[ParticleIdx];
			BoneToParticleIndices.FindOrAdd(Particle.AttachedBoneName).Add(ParticleIdx);
		}

		// 본별로 트랜스폼 조회 후 입자 위치 업데이트
		static int32 PosUpdateLogCounter = 0;
		static FVector LastPelvisPos = FVector::ZeroVector;
		bool bShouldLog = (++PosUpdateLogCounter % 60 == 0);
		int32 UpdatedCount = 0;
		float MaxDelta = 0.0f;
		FName MaxDeltaBone = NAME_None;

		// pelvis 본 위치 추적
		int32 PelvisIndex = SkelMesh->GetBoneIndex(TEXT("pelvis"));
		if (PelvisIndex != INDEX_NONE && bShouldLog)
		{
			FVector CurrentPelvisPos = SkelMesh->GetBoneTransform(PelvisIndex).GetLocation();
			float PelvisMovement = (CurrentPelvisPos - LastPelvisPos).Size();
			UE_LOG(LogTemp, Warning, TEXT("[BonePos] Pelvis: (%.1f, %.1f, %.1f) Moved: %.2f"),
				CurrentPelvisPos.X, CurrentPelvisPos.Y, CurrentPelvisPos.Z, PelvisMovement);
			LastPelvisPos = CurrentPelvisPos;
		}

		for (auto& BonePair : BoneToParticleIndices)
		{
			const FName& BoneName = BonePair.Key;
			const TArray<int32>& BoneParticleIndices = BonePair.Value;

			int32 BoneIndex = SkelMesh->GetBoneIndex(BoneName);
			if (BoneIndex == INDEX_NONE)
			{
				if (bShouldLog)
				{
					UE_LOG(LogTemp, Warning, TEXT("[PosUpdate] Bone NOT FOUND: %s"), *BoneName.ToString());
				}
				continue;
			}

			FTransform CurrentBoneTransform = SkelMesh->GetBoneTransform(BoneIndex);

			for (int32 ParticleIdx : BoneParticleIndices)
			{
				FFluidParticle& Particle = Particles[ParticleIdx];

				// 이전 로컬 오프셋 기준 월드 위치 (본이 이동하기 전 입자가 있어야 할 위치)
				FVector OldWorldPosition = CurrentBoneTransform.TransformPosition(Particle.AttachedLocalOffset);

				// 본 이동에 의한 델타만 적용 (중력은 시뮬레이션에서 별도 적용)
				FVector BoneDelta = OldWorldPosition - Particle.Position;

				float DeltaSize = BoneDelta.Size();
				if (DeltaSize > MaxDelta)
				{
					MaxDelta = DeltaSize;
					MaxDeltaBone = BoneName;
				}

				// 상세 디버그: 첫 번째 입자 추적
				if (bShouldLog && ParticleIdx == BoneParticleIndices[0] && BoneName == TEXT("spine_02"))
				{
					UE_LOG(LogTemp, Warning, TEXT("[Particle] ID:%d Bone:%s LocalOffset:(%.1f,%.1f,%.1f) OldWorld:(%.1f,%.1f,%.1f) CurPos:(%.1f,%.1f,%.1f) Delta:%.2f"),
						Particle.ParticleID, *BoneName.ToString(),
						Particle.AttachedLocalOffset.X, Particle.AttachedLocalOffset.Y, Particle.AttachedLocalOffset.Z,
						OldWorldPosition.X, OldWorldPosition.Y, OldWorldPosition.Z,
						Particle.Position.X, Particle.Position.Y, Particle.Position.Z,
						DeltaSize);
				}

				Particle.Position += BoneDelta;
				//Particle.PredictedPosition += BoneDelta;
				UpdatedCount++;

				// 시뮬레이션 후 새 위치를 로컬 오프셋으로 업데이트 (흘러내림 반영)
				// → ApplyAdhesion에서 처리됨
			}
		}

		if (bShouldLog && UpdatedCount > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[PosUpdate] Updated:%d MaxDelta:%.2f Bone:%s"),
				UpdatedCount, MaxDelta, *MaxDeltaBone.ToString());
		}
	}
}

void AFluidSimulator::SpawnParticles(FVector Location, int32 Count, float SpawnRadius)
{
	int32 ActualCount = FMath::Min(Count, MaxParticles - Particles.Num());

	for (int32 i = 0; i < ActualCount; ++i)
	{
		// 랜덤 위치 생성 (구 내부)
		FVector RandomOffset = FMath::VRand() * FMath::FRandRange(0.0f, SpawnRadius);
		FVector SpawnPos = Location + RandomOffset;

		FFluidParticle NewParticle(SpawnPos, NextParticleID++);
		NewParticle.Mass = ParticleMass;

		Particles.Add(NewParticle);
	}
}

void AFluidSimulator::ApplyExternalForce(FVector Force)
{
	AccumulatedExternalForce += Force;
}

void AFluidSimulator::ApplyForceToParticle(int32 ParticleIndex, FVector Force)
{
	if (Particles.IsValidIndex(ParticleIndex))
	{
		Particles[ParticleIndex].Velocity += Force;
	}
}

void AFluidSimulator::RegisterCollider(UFluidCollider* Collider)
{
	if (Collider && !Colliders.Contains(Collider))
	{
		Colliders.Add(Collider);
	}
}

void AFluidSimulator::UnregisterCollider(UFluidCollider* Collider)
{
	Colliders.Remove(Collider);
}

TArray<FVector> AFluidSimulator::GetParticlePositions() const
{
	TArray<FVector> Positions;
	Positions.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Positions.Add(Particle.Position);
	}

	return Positions;
}

TArray<FVector> AFluidSimulator::GetParticleVelocities() const
{
	TArray<FVector> Velocities;
	Velocities.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Velocities.Add(Particle.Velocity);
	}

	return Velocities;
}

int32 AFluidSimulator::GetParticleCount() const
{
	return Particles.Num();
}

void AFluidSimulator::ClearAllParticles()
{
	Particles.Empty();

	if (DebugMeshComponent)
	{
		DebugMeshComponent->ClearInstances();
	}
}

void AFluidSimulator::ApplyFluidTypePreset(EFluidType NewType)
{
	FluidType = NewType;

	switch (NewType)
	{
	case EFluidType::Water:
		RestDensity = 1000.0f;
		ViscosityCoefficient = 0.01f;
		AdhesionStrength = 0.0f;
		Compliance = 0.0001f;  // 거의 비압축성
		break;

	case EFluidType::Honey:
		RestDensity = 1400.0f;
		ViscosityCoefficient = 0.8f;
		AdhesionStrength = 0.3f;
		Compliance = 0.001f;   // 약간 부드러움
		break;

	case EFluidType::Slime:
		RestDensity = 1200.0f;
		ViscosityCoefficient = 0.5f;
		AdhesionStrength = 0.7f;
		Compliance = 0.01f;    // 탄성 있음 (늘어남)
		break;

	case EFluidType::Custom:
		// 사용자 정의 - 값 변경 안함
		break;
	}
}

void AFluidSimulator::InitializeDebugMesh()
{
	if (!DebugMeshComponent)
	{
		return;
	}

	DebugMeshComponent->ClearInstances();
}

void AFluidSimulator::UpdateDebugInstances()
{
	if (!DebugMeshComponent)
	{
		return;
	}

	const int32 ParticleCount = Particles.Num();
	const int32 InstanceCount = DebugMeshComponent->GetInstanceCount();

	// 인스턴스 수 조정
	if (InstanceCount < ParticleCount)
	{
		// 인스턴스 추가
		for (int32 i = InstanceCount; i < ParticleCount; ++i)
		{
			DebugMeshComponent->AddInstance(FTransform::Identity);
		}
	}
	else if (InstanceCount > ParticleCount)
	{
		// 인스턴스 제거 (뒤에서부터)
		for (int32 i = InstanceCount - 1; i >= ParticleCount; --i)
		{
			DebugMeshComponent->RemoveInstance(i);
		}
	}

	// 스케일 계산 (기본 Sphere는 지름 100cm = 반지름 50cm)
	const float Scale = DebugParticleRadius / 50.0f;
	const FVector ScaleVec(Scale, Scale, Scale);

	// 각 입자 위치로 인스턴스 트랜스폼 업데이트
	for (int32 i = 0; i < ParticleCount; ++i)
	{
		FTransform InstanceTransform;
		InstanceTransform.SetLocation(Particles[i].Position);
		InstanceTransform.SetScale3D(ScaleVec);

		DebugMeshComponent->UpdateInstanceTransform(i, InstanceTransform, true, false, false);
	}

	// 일괄 업데이트
	DebugMeshComponent->MarkRenderStateDirty();
}

void AFluidSimulator::UpdateRenderData()
{
	if (!RenderResource.IsValid() || Particles.Num() == 0)
	{
		return;
	}

	// 시뮬레이션 데이터 -> 렌더링용 데이터 변환
	TArray<FKawaiiRenderParticle> RenderParticles = ConvertToRenderParticles();

	// GPU 버퍼 업데이트 (렌더 스레드로 전송)
	RenderResource->UpdateParticleData(RenderParticles);
}

TArray<FKawaiiRenderParticle> AFluidSimulator::ConvertToRenderParticles() const
{
	TArray<FKawaiiRenderParticle> RenderParticles;
	RenderParticles.Reserve(Particles.Num());

	for (const FFluidParticle& SimParticle : Particles)
	{
		FKawaiiRenderParticle RenderParticle;
		
		// FVector (double) -> FVector3f (float) 변환
		RenderParticle.Position = FVector3f(SimParticle.Position);
		RenderParticle.Velocity = FVector3f(SimParticle.Velocity);
		RenderParticle.Radius = DebugParticleRadius; // 디버그 반경 사용
		RenderParticle.Padding = 0.0f;

		RenderParticles.Add(RenderParticle);
	}

	return RenderParticles;
}

bool AFluidSimulator::IsFluidRenderResourceValid() const
{
	return RenderResource.IsValid() && RenderResource->IsValid();
}

FKawaiiFluidRenderResource* AFluidSimulator::GetFluidRenderResource() const
{
	return RenderResource.Get();
}

//========================================
// Query 함수 구현
//========================================

TArray<int32> AFluidSimulator::GetParticlesInRadius(FVector Location, float Radius) const
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

TArray<int32> AFluidSimulator::GetParticlesInBox(FVector Center, FVector Extent) const
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

TArray<int32> AFluidSimulator::GetParticlesNearActor(AActor* Actor, float Radius) const
{
	if (!Actor)
	{
		return TArray<int32>();
	}
	
	return GetParticlesInRadius(Actor->GetActorLocation(), Radius);
}

bool AFluidSimulator::GetParticleInfo(int32 ParticleIndex, FVector& OutPosition, FVector& OutVelocity, float& OutDensity) const
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
