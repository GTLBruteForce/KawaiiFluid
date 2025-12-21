// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/FluidSimulator.h"
#include "Core/SpatialHash.h"
#include "Physics/DensityConstraint.h"
#include "Physics/ViscositySolver.h"
#include "Physics/AdhesionSolver.h"
#include "Collision/FluidCollider.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "UObject/ConstructorHelpers.h"
#include "DrawDebugHelpers.h"

// 프로파일링용 STAT 그룹
DECLARE_STATS_GROUP(TEXT("KawaiiFluid"), STATGROUP_KawaiiFluid, STATCAT_Advanced);

DECLARE_CYCLE_STAT(TEXT("Fluid Tick"), STAT_FluidTick, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Predict Positions"), STAT_PredictPositions, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Update Neighbors"), STAT_UpdateNeighbors, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Solve Density"), STAT_SolveDensity, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Cache Collider Shapes"), STAT_CacheColliderShapes, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Handle Collisions"), STAT_HandleCollisions, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("World Collision"), STAT_WorldCollision, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Finalize Positions"), STAT_FinalizePositions, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Apply Viscosity"), STAT_ApplyViscosity, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Apply Adhesion"), STAT_ApplyAdhesion, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Debug Rendering"), STAT_DebugRendering, STATGROUP_KawaiiFluid);
DECLARE_CYCLE_STAT(TEXT("Update Render Data"), STAT_UpdateRenderData, STATGROUP_KawaiiFluid);

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
	SolverIterations = 4;
	Gravity = FVector(0.0f, 0.0f, -980.0f);
	Epsilon = 600.0f;  // 스케일에 맞게 조정

	// 점성 기본값 (슬라임)
	ViscosityCoefficient = 0.5f;

	// 접착력 기본값 (언리얼 단위: cm)
	AdhesionStrength = 0.5f;
	AdhesionRadius = 25.0f;  // 25cm
	DetachThreshold = 500.0f;

	// 내부 변수 초기화
	AccumulatedExternalForce = FVector::ZeroVector;
	NextParticleID = 0;

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

	// 자동 스폰
	if (bSpawnOnBeginPlay && AutoSpawnCount > 0)
	{
		SpawnParticles(GetActorLocation(), AutoSpawnCount, AutoSpawnRadius);
	}
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
	DensityConstraint = MakeShared<FDensityConstraint>(RestDensity, SmoothingRadius, Epsilon);
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

	if (!bSimulationEnabled || Particles.Num() == 0)
	{
		return;
	}

	// PBF 시뮬레이션 루프
	// 1. 외력 적용 & 위치 예측
	{
		SCOPE_CYCLE_COUNTER(STAT_PredictPositions);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_PredictPositions);
		PredictPositions(DeltaTime);
	}

	// 2. 이웃 탐색
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateNeighbors);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_UpdateNeighbors);
		UpdateNeighbors();
	}

	// 2.5. 콜라이더 충돌 형상 캐싱 (프레임당 한 번)
	{
		SCOPE_CYCLE_COUNTER(STAT_CacheColliderShapes);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_CacheColliderShapes);
		CacheColliderShapes();
	}

	// 3. 밀도 제약 해결 (반복)
	for (int32 Iter = 0; Iter < SolverIterations; ++Iter)
	{
		{
			SCOPE_CYCLE_COUNTER(STAT_SolveDensity);
			TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_SolveDensity);
			SolveDensityConstraints();
		}
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
	}

	// 4. 속도 업데이트 & 위치 확정
	{
		SCOPE_CYCLE_COUNTER(STAT_FinalizePositions);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_FinalizePositions);
		FinalizePositions(DeltaTime);
	}

	// 5. 점성 적용
	{
		SCOPE_CYCLE_COUNTER(STAT_ApplyViscosity);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_ApplyViscosity);
		ApplyViscosity();
	}

	// 6. 접착력 적용
	{
		SCOPE_CYCLE_COUNTER(STAT_ApplyAdhesion);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_ApplyAdhesion);
		ApplyAdhesion();
	}

	// 외력 리셋
	AccumulatedExternalForce = FVector::ZeroVector;

	// GPU 렌더 데이터 업데이트
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateRenderData);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_UpdateRenderData);
		UpdateRenderData();
	}

	// 디버그 렌더링 업데이트
	if (bEnableDebugRendering)
	{
		SCOPE_CYCLE_COUNTER(STAT_DebugRendering);
		TRACE_CPUPROFILER_EVENT_SCOPE(Fluid_DebugRendering);
		UpdateDebugInstances();

		// 실제 위치 디버그 (빨간색 = 실제 물리 위치)
		for (const FFluidParticle& Particle : Particles)
		{
			//DrawDebugPoint(GetWorld(), Particle.Position, 10.0f, FColor::Red, false, -1.0f);

			// 속도 방향 표시 (파란색 선)
			//DrawDebugLine(GetWorld(), Particle.Position, Particle.Position + Particle.Velocity * 0.1f, FColor::Blue, false, -1.0f);
		}
	}
}

void AFluidSimulator::PredictPositions(float DeltaTime)
{
	const FVector TotalForce = Gravity + AccumulatedExternalForce;

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		FFluidParticle& Particle = Particles[i];
		Particle.Velocity += TotalForce * DeltaTime;
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

void AFluidSimulator::SolveDensityConstraints()
{
	if (DensityConstraint.IsValid())
	{
		DensityConstraint->Solve(Particles, SmoothingRadius, RestDensity, Epsilon);
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
	if (!World)
	{
		return;
	}

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;
	QueryParams.AddIgnoredActor(this);  // 자기 자신 무시

	const float ParticleRadius = DebugParticleRadius;

	for (FFluidParticle& Particle : Particles)
	{
		// 스윕 테스트: 이전 위치 → 예측 위치
		FHitResult HitResult;
		bool bHit = World->SweepSingleByChannel(
			HitResult,
			Particle.Position,
			Particle.PredictedPosition,
			FQuat::Identity,
			CollisionChannel,
			FCollisionShape::MakeSphere(ParticleRadius),
			QueryParams
		);

		if (bHit && HitResult.bBlockingHit)
		{
			// 충돌 지점으로 위치 조정
			//FVector CollisionPos = HitResult.Location + HitResult.ImpactNormal * (ParticleRadius + 0.01f);

			FVector CollisionPos = HitResult.Location + HitResult.ImpactNormal * 0.01f;


			// 점성 유체: Position도 함께 업데이트하여 FinalizePositions에서 튀어오르지 않도록 함
			Particle.PredictedPosition = CollisionPos;
			Particle.Position = CollisionPos;  // ← 이게 핵심!

			// 속도의 수직 성분 제거 (표면에 달라붙음)
			float VelDotNormal = FVector::DotProduct(Particle.Velocity, HitResult.ImpactNormal);
			if (VelDotNormal < 0.0f)
			{
				Particle.Velocity -= VelDotNormal * HitResult.ImpactNormal;
			}
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
		SolverIterations = 3;
		break;

	case EFluidType::Honey:
		RestDensity = 1400.0f;
		ViscosityCoefficient = 0.8f;
		AdhesionStrength = 0.3f;
		SolverIterations = 5;
		break;

	case EFluidType::Slime:
		RestDensity = 1200.0f;
		ViscosityCoefficient = 0.5f;
		AdhesionStrength = 0.7f;
		SolverIterations = 4;
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
