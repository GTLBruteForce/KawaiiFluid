// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/FluidInteractionComponent.h"
#include "Components/KawaiiFluidComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Core/SpatialHash.h"
#include "Collision/MeshFluidCollider.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidParticle.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/PositionVertexBuffer.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

UFluidInteractionComponent::UFluidInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	TargetSubsystem = nullptr;
	bCanAttachFluid = true;
	AdhesionMultiplier = 1.0f;
	DragAlongStrength = 0.5f;
	bAutoCreateCollider = true;

	AttachedParticleCount = 0;
	bIsWet = false;

	AutoCollider = nullptr;
}

void UFluidInteractionComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITOR
	// 에디터 모드에서 Subsystem에 등록 (브러시 모드용)
	UWorld* World = GetWorld();
	if (World && !World->IsGameWorld())
	{
		if (!TargetSubsystem)
		{
			TargetSubsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>();
		}

		if (bAutoCreateCollider && !AutoCollider)
		{
			CreateAutoCollider();
		}

		RegisterWithSimulator();
	}
#endif
}

void UFluidInteractionComponent::OnUnregister()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (World && !World->IsGameWorld())
	{
		UnregisterFromSimulator();
	}
#endif

	Super::OnUnregister();
}

void UFluidInteractionComponent::BeginPlay()
{
	Super::BeginPlay();

	// Find subsystem automatically
	if (!TargetSubsystem)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			TargetSubsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>();
		}
	}

	if (bAutoCreateCollider)
	{
		CreateAutoCollider();
	}

	RegisterWithSimulator();

	// 경계 입자 생성 (Flex-style Adhesion)
	if (bEnableBoundaryParticles)
	{
		GenerateBoundaryParticles();
	}
}

void UFluidInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromSimulator();

	Super::EndPlay(EndPlayReason);
}

void UFluidInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!TargetSubsystem)
	{
		return;
	}

	// 기존: 붙은 파티클 추적
	int32 PrevCount = AttachedParticleCount;
	UpdateAttachedParticleCount();

	if (AttachedParticleCount > 0 && PrevCount == 0)
	{
		bIsWet = true;
		OnFluidAttached.Broadcast(AttachedParticleCount);
	}
	else if (AttachedParticleCount == 0 && PrevCount > 0)
	{
		bIsWet = false;
		OnFluidDetached.Broadcast();
	}

	// 새로운: Collider 충돌 감지
	if (bEnableCollisionDetection && AutoCollider)
	{
		DetectCollidingParticles();
		
		// 트리거 이벤트 발생 조건
		bool bIsColliding = (CollidingParticleCount >= MinParticleCountForTrigger);
		
		// Enter 이벤트
		if (bIsColliding && !bWasColliding)
		{
			if (OnFluidColliding.IsBound())
			{
				OnFluidColliding.Broadcast(CollidingParticleCount);
			}
		}
		// Exit 이벤트
		else if (!bIsColliding && bWasColliding)
		{
			if (OnFluidStopColliding.IsBound())
			{
				OnFluidStopColliding.Broadcast();
			}
		}
		
		bWasColliding = bIsColliding;
	}

	// 본 레벨 추적은 FluidSimulator::UpdateAttachedParticlePositions()에서 처리

	// Per-Polygon Collision AABB 디버그 시각화
	if (bUsePerPolygonCollision && bDrawPerPolygonAABB)
	{
		FBox AABB = GetPerPolygonFilterAABB();
		if (AABB.IsValid)
		{
			DrawDebugBox(
				GetWorld(),
				AABB.GetCenter(),
				AABB.GetExtent(),
				FColor::Cyan,
				false,  // bPersistentLines
				-1.0f,  // LifeTime (매 프레임 갱신)
				0,      // DepthPriority
				2.0f    // Thickness
			);
		}
	}

	// GPU Collision Feedback 처리 (Particle -> Player Interaction)
	if (bEnableForceFeedback)
	{
		// GPU 피드백 자동 활성화 (첫 틱에서)
		EnableGPUCollisionFeedbackIfNeeded();

		ProcessCollisionFeedback(DeltaTime);
	}

	// 경계 입자 업데이트 및 디버그 표시 (Flex-style Adhesion)
	if (bEnableBoundaryParticles && bBoundaryParticlesInitialized)
	{
		UpdateBoundaryParticlePositions();

		if (bShowBoundaryParticles)
		{
			DrawDebugBoundaryParticles();
		}
	}
}

void UFluidInteractionComponent::CreateAutoCollider()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	AutoCollider = NewObject<UMeshFluidCollider>(Owner);
	if (AutoCollider)
	{
		AutoCollider->RegisterComponent();
		AutoCollider->bAllowAdhesion = bCanAttachFluid;
		AutoCollider->AdhesionMultiplier = AdhesionMultiplier;

		// TargetMeshComponent 자동 설정
		// 우선순위: SkeletalMeshComponent > CapsuleComponent > StaticMeshComponent
		USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		if (SkelMesh)
		{
			AutoCollider->TargetMeshComponent = SkelMesh;
		}
		else
		{
			UCapsuleComponent* Capsule = Owner->FindComponentByClass<UCapsuleComponent>();
			if (Capsule)
			{
				AutoCollider->TargetMeshComponent = Capsule;
			}
			else
			{
				UStaticMeshComponent* StaticMesh = Owner->FindComponentByClass<UStaticMeshComponent>();
				if (StaticMesh)
				{
					AutoCollider->TargetMeshComponent = StaticMesh;
				}
			}
		}
	}
}

void UFluidInteractionComponent::RegisterWithSimulator()
{
	if (TargetSubsystem)
	{
		if (AutoCollider)
		{
			TargetSubsystem->RegisterGlobalCollider(AutoCollider);
		}
		TargetSubsystem->RegisterGlobalInteractionComponent(this);
	}
}

void UFluidInteractionComponent::UnregisterFromSimulator()
{
	if (TargetSubsystem)
	{
		if (AutoCollider)
		{
			TargetSubsystem->UnregisterGlobalCollider(AutoCollider);
		}
		TargetSubsystem->UnregisterGlobalInteractionComponent(this);
	}
}

void UFluidInteractionComponent::UpdateAttachedParticleCount()
{
	AActor* Owner = GetOwner();
	int32 Count = 0;

	if (TargetSubsystem)
	{
		// Iterate all Modules from subsystem
		for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
		{
			if (!Module) continue;
			for (const FFluidParticle& Particle : Module->GetParticles())
			{
				if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
				{
					++Count;
				}
			}
		}
	}

	AttachedParticleCount = Count;
}

void UFluidInteractionComponent::DetachAllFluid()
{
	AActor* Owner = GetOwner();

	auto DetachFromParticles = [Owner](TArray<FFluidParticle>& Particles)
	{
		for (FFluidParticle& Particle : Particles)
		{
			if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
			{
				Particle.bIsAttached = false;
				Particle.AttachedActor.Reset();
				Particle.AttachedBoneName = NAME_None;
				Particle.AttachedLocalOffset = FVector::ZeroVector;
			}
		}
	};

	if (TargetSubsystem)
	{
		for (auto Module : TargetSubsystem->GetAllModules())
		{
			if (!Module) continue;
			DetachFromParticles(Module->GetParticlesMutable());
		}
	}

	AttachedParticleCount = 0;
	bIsWet = false;
}

void UFluidInteractionComponent::PushFluid(FVector Direction, float Force)
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	FVector NormalizedDir = Direction.GetSafeNormal();
	FVector OwnerLocation = Owner->GetActorLocation();

	auto PushParticles = [Owner, NormalizedDir, OwnerLocation, Force](TArray<FFluidParticle>& Particles)
	{
		for (FFluidParticle& Particle : Particles)
		{
			float Distance = FVector::Dist(Particle.Position, OwnerLocation);

			if (Distance < 200.0f)
			{
				float FallOff = 1.0f - (Distance / 200.0f);
				Particle.Velocity += NormalizedDir * Force * FallOff;

				if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
				{
					Particle.bIsAttached = false;
					Particle.AttachedActor.Reset();
					Particle.AttachedBoneName = NAME_None;
					Particle.AttachedLocalOffset = FVector::ZeroVector;
				}
			}
		}
	};

	if (TargetSubsystem)
	{
		for (auto Module : TargetSubsystem->GetAllModules())
		{
			if (!Module) continue;
			PushParticles(Module->GetParticlesMutable());
		}
	}
}

void UFluidInteractionComponent::DetectCollidingParticles()
{
	if (!AutoCollider)
	{
		CollidingParticleCount = 0;
		return;
	}

	// 캐시 갱신
	AutoCollider->CacheCollisionShapes();
	if (!AutoCollider->IsCacheValid())
	{
		CollidingParticleCount = 0;
		return;
	}

	AActor* Owner = GetOwner();
	int32 Count = 0;
	FBox ColliderBounds = AutoCollider->GetCachedBounds();

	if (TargetSubsystem)
	{
		TArray<int32> CandidateIndices;

		for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
		{
			if (!Module) continue;

			FSpatialHash* SpatialHash = Module->GetSpatialHash();
			const TArray<FFluidParticle>& Particles = Module->GetParticles();

			if (SpatialHash)
			{
				// SpatialHash로 바운딩 박스 내 파티클만 쿼리
				SpatialHash->QueryBox(ColliderBounds, CandidateIndices);

				for (int32 Idx : CandidateIndices)
				{
					if (Idx < 0 || Idx >= Particles.Num()) continue;

					const FFluidParticle& Particle = Particles[Idx];

					// 1. 이미 붙어있으면 충돌 중
					if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
					{
						++Count;
						continue;
					}

					// 2. 정밀 체크 (후보만)
					if (AutoCollider->IsPointInside(Particle.Position))
					{
						++Count;
					}
				}
			}
			else
			{
				// SpatialHash 없으면 기존 방식 (폴백)
				for (const FFluidParticle& Particle : Particles)
				{
					if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
					{
						++Count;
						continue;
					}

					if (AutoCollider->IsPointInside(Particle.Position))
					{
						++Count;
					}
				}
			}
		}
	}

	CollidingParticleCount = Count;
}

FBox UFluidInteractionComponent::GetPerPolygonFilterAABB() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return FBox(ForceInit);
	}

	FBox ActorBounds(ForceInit);

	// SkeletalMeshComponent가 있으면 그 바운딩 박스 사용 (더 정확함)
	if (USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>())
	{
		ActorBounds = SkelMesh->Bounds.GetBox();
	}
	else
	{
		// 없으면 Actor 전체 바운딩 박스 사용
		ActorBounds = Owner->GetComponentsBoundingBox(true);
	}

	// 패딩 적용
	if (PerPolygonAABBPadding > 0.0f && ActorBounds.IsValid)
	{
		ActorBounds = ActorBounds.ExpandBy(PerPolygonAABBPadding);
	}

	return ActorBounds;
}

//=============================================================================
// GPU Collision Feedback Implementation (Particle -> Player Interaction)
//=============================================================================

void UFluidInteractionComponent::ProcessCollisionFeedback(float DeltaTime)
{
	AActor* Owner = GetOwner();
	const int32 MyOwnerID = Owner ? Owner->GetUniqueID() : 0;

	if (!TargetSubsystem)
	{
		// 접촉 없음 → 힘 감쇠
		SmoothedForce = FMath::VInterpTo(SmoothedForce, FVector::ZeroVector, DeltaTime, ForceSmoothingSpeed);
		CurrentFluidForce = SmoothedForce;
		CurrentContactCount = 0;
		CurrentAveragePressure = 0.0f;
		return;
	}

	// GPUSimulator에서 피드백 가져오기 (첫 번째 GPU 모드 모듈에서)
	FGPUFluidSimulator* GPUSimulator = nullptr;
for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (Module && Module->GetGPUSimulator())
		{
			GPUSimulator = Module->GetGPUSimulator();
			break;
		}
	}

	if (!GPUSimulator)
	{
		// GPUSimulator 없음 → 힘 감쇠
		SmoothedForce = FMath::VInterpTo(SmoothedForce, FVector::ZeroVector, DeltaTime, ForceSmoothingSpeed);
		CurrentFluidForce = SmoothedForce;
		CurrentContactCount = 0;
		CurrentAveragePressure = 0.0f;
		return;
	}

	// =====================================================
	// 새로운 접근법: 콜라이더별 카운트 버퍼 사용
	// GPU에서 콜라이더 인덱스별로 충돌 카운트를 집계하고,
	// OwnerID로 필터링하여 이 액터의 콜라이더와 충돌한 입자 수를 얻음
	// =====================================================
	const int32 OwnerContactCount = GPUSimulator->GetContactCountForOwner(MyOwnerID);

	// Debug: 콜라이더 카운트 로그
	static int32 DebugLogCounter = 0;
	if (++DebugLogCounter % 60 == 0)  // 60프레임마다 한 번 로그
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidInteraction: OwnerID=%d, ContactCount=%d, TotalColliders=%d"),
			MyOwnerID, OwnerContactCount, GPUSimulator->GetTotalColliderCount());
	}

	// 유체 태그별 카운트 초기화 및 설정
	CurrentFluidTagCounts.Empty();
	if (OwnerContactCount > 0)
	{
		// 현재는 기본 태그로 처리 (향후 태그 시스템 확장 가능)
		CurrentFluidTagCounts.FindOrAdd(NAME_None) = OwnerContactCount;
	}

	// 콜라이더 접촉 카운트로 이벤트 트리거
	CurrentContactCount = OwnerContactCount;

	// =====================================================
	// 힘 계산: 상세 피드백이 필요한 경우에만 처리
	// (피드백이 비활성화되어도 카운트 기반 이벤트는 동작)
	// =====================================================
	if (GPUSimulator->IsCollisionFeedbackEnabled())
	{
		TArray<FGPUCollisionFeedback> AllFeedback;
		int32 FeedbackCount = 0;
		GPUSimulator->GetAllCollisionFeedback(AllFeedback, FeedbackCount);

		// 본별 힘 처리 (Per-Bone Force)
		if (bEnablePerBoneForce)
		{
			ProcessPerBoneForces(DeltaTime, AllFeedback, FeedbackCount);

			// 본 충돌 이벤트 처리 (Niagara Spawning용)
			ProcessBoneCollisionEvents(DeltaTime, AllFeedback, FeedbackCount);
		}

		if (FeedbackCount > 0)
		{
			// 항력 계산 파라미터
			const float ParticleRadius = 3.0f;  // cm (기본값)
			const float ParticleArea = PI * ParticleRadius * ParticleRadius;  // cm²
			const float AreaInM2 = ParticleArea * 0.0001f;  // m² (cm² → m²)

			// 캐릭터/오브젝트 속도 가져오기
			FVector BodyVelocity = FVector::ZeroVector;
			if (Owner)
			{
				if (UCharacterMovementComponent* MovementComp = Owner->FindComponentByClass<UCharacterMovementComponent>())
				{
					BodyVelocity = MovementComp->Velocity;
				}
				else if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
				{
					BodyVelocity = RootPrimitive->GetPhysicsLinearVelocity();
				}
			}
			const FVector BodyVelocityInMS = BodyVelocity * 0.01f;

			FVector ForceAccum = FVector::ZeroVector;
			float DensitySum = 0.0f;
			int32 ForceContactCount = 0;

			for (int32 i = 0; i < FeedbackCount; ++i)
			{
				const FGPUCollisionFeedback& Feedback = AllFeedback[i];

				// ColliderOwnerID 필터링: 이 액터의 콜라이더와 충돌한 피드백만 힘 계산에 사용
				if (Feedback.ColliderOwnerID != 0 && Feedback.ColliderOwnerID != MyOwnerID)
				{
					continue;
				}

				// 파티클 속도 (cm/s → m/s)
				FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
				FVector ParticleVelocityInMS = ParticleVelocity * 0.01f;

				// 상대 속도: v_rel = u_fluid - v_body
				FVector RelativeVelocity = ParticleVelocityInMS - BodyVelocityInMS;
				float RelativeSpeed = RelativeVelocity.Size();

				DensitySum += Feedback.Density;
				ForceContactCount++;

				if (RelativeSpeed < SMALL_NUMBER)
				{
					continue;
				}

				// 항력 공식: F = ½ρCdA|v|²
				float DragMagnitude = 0.5f * Feedback.Density * DragCoefficient * AreaInM2 * RelativeSpeed * RelativeSpeed;
				FVector DragDirection = RelativeVelocity / RelativeSpeed;
				ForceAccum += DragDirection * DragMagnitude;
			}

			// 힘을 cm 단위로 변환
			ForceAccum *= 100.0f;

			// 스무딩 적용
			FVector TargetForce = ForceAccum * DragForceMultiplier;
			SmoothedForce = FMath::VInterpTo(SmoothedForce, TargetForce, DeltaTime, ForceSmoothingSpeed);
			CurrentFluidForce = SmoothedForce;
			CurrentAveragePressure = (ForceContactCount > 0) ? (DensitySum / ForceContactCount) : 0.0f;
		}
		else
		{
			// 피드백 없음 → 힘 감쇠
			SmoothedForce = FMath::VInterpTo(SmoothedForce, FVector::ZeroVector, DeltaTime, ForceSmoothingSpeed);
			CurrentFluidForce = SmoothedForce;
			CurrentAveragePressure = 0.0f;
		}
	}
	else
	{
		// 상세 피드백 비활성화 → 힘 없음, 카운트만 사용
		SmoothedForce = FMath::VInterpTo(SmoothedForce, FVector::ZeroVector, DeltaTime, ForceSmoothingSpeed);
		CurrentFluidForce = SmoothedForce;
		CurrentAveragePressure = 0.0f;
	}

	// 이벤트 브로드캐스트
	if (OnFluidForceUpdate.IsBound())
	{
		OnFluidForceUpdate.Broadcast(CurrentFluidForce, CurrentAveragePressure, CurrentContactCount);
	}

	// 유체 태그 이벤트 업데이트 (OnFluidEnter/OnFluidExit)
	UpdateFluidTagEvents();
}

void UFluidInteractionComponent::UpdateFluidTagEvents()
{
	// 현재 프레임에서 충분한 파티클과 충돌 중인 태그 확인
	TSet<FName> CurrentlyColliding;
	for (const auto& Pair : CurrentFluidTagCounts)
	{
		if (Pair.Value >= MinParticleCountForFluidEvent)
		{
			CurrentlyColliding.Add(Pair.Key);
		}
	}

	// Exit 이벤트: 이전에 충돌 중이었지만 지금은 아닌 경우
	for (auto& Pair : PreviousFluidTagStates)
	{
		if (Pair.Value && !CurrentlyColliding.Contains(Pair.Key))
		{
			if (OnFluidExit.IsBound())
			{
				OnFluidExit.Broadcast(Pair.Key);
			}
			Pair.Value = false;
		}
	}

	// Enter 이벤트: 이전에 충돌 중이 아니었지만 지금은 충돌 중인 경우
	for (const FName& Tag : CurrentlyColliding)
	{
		bool* bWasCollidingWithTag = PreviousFluidTagStates.Find(Tag);
		if (!bWasCollidingWithTag || !(*bWasCollidingWithTag))
		{
			int32* CountPtr = CurrentFluidTagCounts.Find(Tag);
			int32 Count = CountPtr ? *CountPtr : 0;

			if (OnFluidEnter.IsBound())
			{
				OnFluidEnter.Broadcast(Tag, Count);
			}
			PreviousFluidTagStates.FindOrAdd(Tag) = true;
		}
	}
}

void UFluidInteractionComponent::ApplyFluidForceToCharacterMovement(float ForceScale)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	UCharacterMovementComponent* MovementComp = Owner->FindComponentByClass<UCharacterMovementComponent>();
	if (!MovementComp)
	{
		return;
	}

	// 힘 적용 (AddForce는 가속도로 변환됨)
	FVector ScaledForce = CurrentFluidForce * ForceScale;
	if (!ScaledForce.IsNearlyZero())
	{
		MovementComp->AddForce(ScaledForce);
	}
}

bool UFluidInteractionComponent::IsCollidingWithFluidTag(FName FluidTag) const
{
	const bool* bIsColliding = PreviousFluidTagStates.Find(FluidTag);
	return bIsColliding && *bIsColliding;
}

void UFluidInteractionComponent::EnableGPUCollisionFeedbackIfNeeded()
{
	// 이미 활성화되었으면 스킵
	if (bGPUFeedbackEnabled)
	{
		return;
	}

	if (!TargetSubsystem)
	{
		return;
	}

	// 모든 GPU 모듈에서 피드백 활성화
for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (Module)
		{
			FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
			if (GPUSimulator)
			{
				GPUSimulator->SetCollisionFeedbackEnabled(true);
				bGPUFeedbackEnabled = true;
				UE_LOG(LogTemp, Log, TEXT("FluidInteractionComponent: GPU Collision Feedback Enabled"));
			}
		}
	}
}

//=============================================================================
// Per-Bone Force Implementation
//=============================================================================

void UFluidInteractionComponent::InitializeBoneNameCache()
{
	if (bBoneNameCacheInitialized)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh || !SkelMesh->GetSkeletalMeshAsset())
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetSkeletalMeshAsset()->GetRefSkeleton();
	const int32 BoneCount = RefSkeleton.GetNum();

	BoneIndexToNameCache.Empty(BoneCount);
	for (int32 i = 0; i < BoneCount; ++i)
	{
		BoneIndexToNameCache.Add(i, RefSkeleton.GetBoneName(i));
	}

	bBoneNameCacheInitialized = true;
}

void UFluidInteractionComponent::ProcessPerBoneForces(float DeltaTime, const TArray<FGPUCollisionFeedback>& AllFeedback, int32 FeedbackCount)
{
	AActor* Owner = GetOwner();
	const int32 MyOwnerID = Owner ? Owner->GetUniqueID() : 0;

	// 본 이름 캐시 초기화 (첫 호출 시)
	if (!bBoneNameCacheInitialized)
	{
		InitializeBoneNameCache();
	}

	// 본별 원시 힘 집계 (이번 프레임)
	TMap<int32, FVector> RawBoneForces;
	TMap<int32, int32> BoneContactCounts;

	// 캐릭터/오브젝트 속도 가져오기
	FVector BodyVelocity = FVector::ZeroVector;
	if (Owner)
	{
		if (UCharacterMovementComponent* MovementComp = Owner->FindComponentByClass<UCharacterMovementComponent>())
		{
			BodyVelocity = MovementComp->Velocity;
		}
		else if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
		{
			BodyVelocity = RootPrimitive->GetPhysicsLinearVelocity();
		}
	}
	const FVector BodyVelocityInMS = BodyVelocity * 0.01f;  // cm/s → m/s

	// 항력 계산 파라미터
	const float ParticleRadius = 3.0f;  // cm (기본값)
	const float ParticleArea = PI * ParticleRadius * ParticleRadius;  // cm²
	const float AreaInM2 = ParticleArea * 0.0001f;  // m² (cm² → m²)

	for (int32 i = 0; i < FeedbackCount; ++i)
	{
		const FGPUCollisionFeedback& Feedback = AllFeedback[i];

		// OwnerID 필터링
		if (Feedback.ColliderOwnerID != 0 && Feedback.ColliderOwnerID != MyOwnerID)
		{
			continue;
		}

		// BoneIndex가 유효한 경우에만 처리
		if (Feedback.BoneIndex < 0)
		{
			continue;
		}

		// 파티클 속도 (cm/s → m/s)
		FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
		FVector ParticleVelocityInMS = ParticleVelocity * 0.01f;

		// 상대 속도
		FVector RelativeVelocity = ParticleVelocityInMS - BodyVelocityInMS;
		float RelativeSpeed = RelativeVelocity.Size();

		if (RelativeSpeed < SMALL_NUMBER)
		{
			continue;
		}

		// 항력 공식: F = ½ρCdA|v|²
		float DragMagnitude = 0.5f * Feedback.Density * DragCoefficient * AreaInM2 * RelativeSpeed * RelativeSpeed;
		FVector DragDirection = RelativeVelocity / RelativeSpeed;
		FVector DragForce = DragDirection * DragMagnitude;

		// cm 단위로 변환 후 배율 적용
		DragForce *= 100.0f * PerBoneForceMultiplier;

		// 본별 힘 누적
		FVector& BoneForce = RawBoneForces.FindOrAdd(Feedback.BoneIndex, FVector::ZeroVector);
		BoneForce += DragForce;

		int32& ContactCount = BoneContactCounts.FindOrAdd(Feedback.BoneIndex, 0);
		ContactCount++;
	}

	// 스무딩 적용 및 결과 저장
	// 먼저 기존 본들 감쇠
	TArray<int32> BonesToRemove;
	for (auto& Pair : SmoothedPerBoneForces)
	{
		const int32 BoneIdx = Pair.Key;
		FVector& SmoothedForceRef = Pair.Value;

		FVector* RawForce = RawBoneForces.Find(BoneIdx);
		FVector TargetForce = RawForce ? *RawForce : FVector::ZeroVector;

		SmoothedForceRef = FMath::VInterpTo(SmoothedForceRef, TargetForce, DeltaTime, PerBoneForceSmoothingSpeed);
		CurrentPerBoneForces.FindOrAdd(BoneIdx) = SmoothedForceRef;

		// 힘이 거의 0이면 제거 대상
		if (SmoothedForceRef.SizeSquared() < 0.01f && !RawForce)
		{
			BonesToRemove.Add(BoneIdx);
		}
	}

	// 새로운 본들 추가
	for (const auto& Pair : RawBoneForces)
	{
		const int32 BoneIdx = Pair.Key;
		if (!SmoothedPerBoneForces.Contains(BoneIdx))
		{
			FVector& SmoothedForceRef = SmoothedPerBoneForces.Add(BoneIdx, FVector::ZeroVector);
			SmoothedForceRef = FMath::VInterpTo(SmoothedForceRef, Pair.Value, DeltaTime, PerBoneForceSmoothingSpeed);
			CurrentPerBoneForces.FindOrAdd(BoneIdx) = SmoothedForceRef;
		}
	}

	// 0에 가까운 본 제거
	for (int32 BoneIdx : BonesToRemove)
	{
		SmoothedPerBoneForces.Remove(BoneIdx);
		CurrentPerBoneForces.Remove(BoneIdx);
	}

	// =====================================================
	// 디버그 로그: 3초마다 본별 항력 출력
	// =====================================================
	PerBoneForceDebugTimer += DeltaTime;
	if (PerBoneForceDebugTimer >= 3.0f)
	{
		PerBoneForceDebugTimer = 0.0f;

		// 피드백 데이터 분석 로그
		int32 TotalFeedback = FeedbackCount;
		int32 MatchedOwner = 0;
		int32 ValidBoneIndex = 0;
		int32 InvalidBoneIndex = 0;

		for (int32 i = 0; i < FeedbackCount; ++i)
		{
			const FGPUCollisionFeedback& Feedback = AllFeedback[i];

			if (Feedback.ColliderOwnerID == 0 || Feedback.ColliderOwnerID == MyOwnerID)
			{
				MatchedOwner++;
				if (Feedback.BoneIndex >= 0)
				{
					ValidBoneIndex++;
				}
				else
				{
					InvalidBoneIndex++;
				}
			}
		}

		// 피드백 OwnerID 샘플 수집
		TSet<int32> UniqueOwnerIDs;
		for (int32 i = 0; i < FMath::Min(FeedbackCount, 100); ++i)
		{
			UniqueOwnerIDs.Add(AllFeedback[i].ColliderOwnerID);
		}
		FString OwnerIDSamples;
		for (int32 ID : UniqueOwnerIDs)
		{
			OwnerIDSamples += FString::Printf(TEXT("%d, "), ID);
		}

		UE_LOG(LogTemp, Warning, TEXT("========== Per-Bone Force Debug (Owner: %s, ID: %d) =========="),
			Owner ? *Owner->GetName() : TEXT("None"), MyOwnerID);
		UE_LOG(LogTemp, Warning, TEXT("  Feedback: Total=%d, MatchedOwner=%d, ValidBone=%d, InvalidBone(=-1)=%d"),
			TotalFeedback, MatchedOwner, ValidBoneIndex, InvalidBoneIndex);
		UE_LOG(LogTemp, Warning, TEXT("  Feedback OwnerIDs 샘플: [%s]"), *OwnerIDSamples);

		if (CurrentPerBoneForces.Num() > 0)
		{
			for (const auto& Pair : CurrentPerBoneForces)
			{
				const int32 BoneIdx = Pair.Key;
				const FVector& Force = Pair.Value;
				const float ForceMagnitude = Force.Size();

				// 본 이름 가져오기
				FName BoneName = NAME_None;
				if (const FName* CachedName = BoneIndexToNameCache.Find(BoneIdx))
				{
					BoneName = *CachedName;
				}

				UE_LOG(LogTemp, Warning, TEXT("  [%d] %s: Force=(%.2f, %.2f, %.2f) Magnitude=%.2f"),
					BoneIdx,
					*BoneName.ToString(),
					Force.X, Force.Y, Force.Z,
					ForceMagnitude);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("  -> No active bone forces"));
		}

		UE_LOG(LogTemp, Warning, TEXT("================================================================"));
	}
}

FVector UFluidInteractionComponent::GetFluidForceForBone(int32 BoneIndex) const
{
	const FVector* Force = CurrentPerBoneForces.Find(BoneIndex);
	return Force ? *Force : FVector::ZeroVector;
}

FVector UFluidInteractionComponent::GetFluidForceForBoneByName(FName BoneName) const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return FVector::ZeroVector;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh)
	{
		return FVector::ZeroVector;
	}

	int32 BoneIndex = SkelMesh->GetBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	return GetFluidForceForBone(BoneIndex);
}

void UFluidInteractionComponent::GetActiveBoneIndices(TArray<int32>& OutBoneIndices) const
{
	OutBoneIndices.Empty(CurrentPerBoneForces.Num());
	for (const auto& Pair : CurrentPerBoneForces)
	{
		if (Pair.Value.SizeSquared() > 0.01f)
		{
			OutBoneIndices.Add(Pair.Key);
		}
	}
}

bool UFluidInteractionComponent::GetStrongestBoneForce(int32& OutBoneIndex, FVector& OutForce) const
{
	OutBoneIndex = -1;
	OutForce = FVector::ZeroVector;
	float MaxForceSq = 0.0f;

	for (const auto& Pair : CurrentPerBoneForces)
	{
		float ForceSq = Pair.Value.SizeSquared();
		if (ForceSq > MaxForceSq)
		{
			MaxForceSq = ForceSq;
			OutBoneIndex = Pair.Key;
			OutForce = Pair.Value;
		}
	}

	return OutBoneIndex >= 0;
}

//=============================================================================
// Bone Collision Events Implementation (for Niagara Spawning)
//=============================================================================

void UFluidInteractionComponent::ProcessBoneCollisionEvents(float DeltaTime, const TArray<FGPUCollisionFeedback>& AllFeedback, int32 FeedbackCount)
{
	AActor* Owner = GetOwner();
	const int32 MyOwnerID = Owner ? Owner->GetUniqueID() : 0;

	// 쿨다운 타이머 업데이트 (모든 본)
	TArray<int32> ExpiredCooldowns;
	for (auto& Pair : BoneEventCooldownTimers)
	{
		Pair.Value -= DeltaTime;
		if (Pair.Value <= 0.0f)
		{
			ExpiredCooldowns.Add(Pair.Key);
		}
	}
	for (int32 BoneIdx : ExpiredCooldowns)
	{
		BoneEventCooldownTimers.Remove(BoneIdx);
	}

	// 본별 접촉 카운트, 속도, FluidTag 집계
	TMap<int32, int32> NewBoneContactCounts;
	TMap<int32, FVector> BoneVelocitySums;
	TMap<int32, int32> BoneVelocityCounts;
	TMap<int32, FVector> BoneImpactOffsetSums;
	TMap<int32, int32> BoneImpactOffsetCounts;
	TMap<int32, TMap<int32, int32>> BoneSourceCounts;  // BoneIndex → (SourceID → Count)

	for (int32 i = 0; i < FeedbackCount; ++i)
	{
		const FGPUCollisionFeedback& Feedback = AllFeedback[i];

		// OwnerID 필터링
		if (Feedback.ColliderOwnerID != 0 && Feedback.ColliderOwnerID != MyOwnerID)
		{
			continue;
		}

		// BoneIndex가 유효한 경우에만 처리
		if (Feedback.BoneIndex < 0)
		{
			continue;
		}

		// 접촉 카운트 증가
		int32& ContactCount = NewBoneContactCounts.FindOrAdd(Feedback.BoneIndex, 0);
		ContactCount++;

		// 속도 합산
		FVector ParticleVel(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
		FVector& VelSum = BoneVelocitySums.FindOrAdd(Feedback.BoneIndex, FVector::ZeroVector);
		VelSum += ParticleVel;

		int32& VelCount = BoneVelocityCounts.FindOrAdd(Feedback.BoneIndex, 0);
		VelCount++;

		// ImpactOffset 합산
		FVector ImpactOffset(Feedback.ImpactOffset.X, Feedback.ImpactOffset.Y, Feedback.ImpactOffset.Z);
		FVector& OffsetSum = BoneImpactOffsetSums.FindOrAdd(Feedback.BoneIndex, FVector::ZeroVector);
		OffsetSum += ImpactOffset;

		int32& OffsetCount = BoneImpactOffsetCounts.FindOrAdd(Feedback.BoneIndex, 0);
		OffsetCount++;

		// SourceID별 카운트 (FluidTag 결정용)
		TMap<int32, int32>& SourceCounts = BoneSourceCounts.FindOrAdd(Feedback.BoneIndex);
		int32& SourceCount = SourceCounts.FindOrAdd(Feedback.ParticleSourceID, 0);
		SourceCount++;
	}

	// 평균 속도 계산
	CurrentBoneAverageVelocities.Empty();
	for (const auto& Pair : BoneVelocitySums)
	{
		int32 BoneIdx = Pair.Key;
		int32* VelCountPtr = BoneVelocityCounts.Find(BoneIdx);
		int32 VelCount = VelCountPtr ? *VelCountPtr : 1;
		CurrentBoneAverageVelocities.Add(BoneIdx, Pair.Value / FMath::Max(1, VelCount));
	}

	// 현재 접촉 카운트 저장
	CurrentBoneContactCounts = NewBoneContactCounts;

	// 충돌 이벤트 발생 (bEnableBoneCollisionEvents 활성화 시)
	if (bEnableBoneCollisionEvents && OnBoneParticleCollision.IsBound())
	{
		// 현재 충분한 접촉이 있는 본들
		TSet<int32> CurrentContactBones;
		for (const auto& Pair : CurrentBoneContactCounts)
		{
			if (Pair.Value >= MinParticleCountForBoneEvent)
			{
				CurrentContactBones.Add(Pair.Key);
			}
		}

		// 새로운 충돌 감지 (이전 프레임에 없었거나 쿨다운이 끝난 본)
		for (int32 BoneIdx : CurrentContactBones)
		{
			// 쿨다운 중이면 스킵
			if (BoneEventCooldownTimers.Contains(BoneIdx))
			{
				continue;
			}

			// 새 충돌이거나 쿨다운 만료 → 이벤트 발생
			const int32* ContactCountPtr = CurrentBoneContactCounts.Find(BoneIdx);
			int32 ContactCount = ContactCountPtr ? *ContactCountPtr : 0;

			const FVector* AvgVelPtr = CurrentBoneAverageVelocities.Find(BoneIdx);
			FVector AverageVelocity = AvgVelPtr ? *AvgVelPtr : FVector::ZeroVector;

			// 평균 ImpactOffset 계산
			FVector ImpactOffsetAverage = FVector::ZeroVector;
			const FVector* OffsetSumPtr = BoneImpactOffsetSums.Find(BoneIdx);
			const int32* OffsetCountPtr = BoneImpactOffsetCounts.Find(BoneIdx);
			if (OffsetSumPtr && OffsetCountPtr && *OffsetCountPtr > 0)
			{
				ImpactOffsetAverage = *OffsetSumPtr / *OffsetCountPtr;
			}

			// 본 이름 가져오기
			FName BoneName = GetBoneNameFromIndex(BoneIdx);

			// FluidName 결정: 가장 많이 충돌한 SourceID의 Preset에서 FluidName 가져오기
			FName FluidName = NAME_None;
			if (TargetSubsystem)
			{
				const TMap<int32, int32>* SourceCounts = BoneSourceCounts.Find(BoneIdx);
				if (SourceCounts)
				{
					int32 MaxSourceID = -1;
					int32 MaxCount = 0;
					for (const auto& SourcePair : *SourceCounts)
					{
						if (SourcePair.Value > MaxCount)
						{
							MaxCount = SourcePair.Value;
							MaxSourceID = SourcePair.Key;
						}
					}
					if (MaxSourceID >= 0)
					{
						UKawaiiFluidPresetDataAsset* Preset = TargetSubsystem->GetPresetBySourceID(MaxSourceID);
						if (Preset)
						{
							FluidName = Preset->GetFluidName();
						}
					}
				}
			}

			// 이벤트 브로드캐스트 (FluidName 포함)
			OnBoneParticleCollision.Broadcast(BoneIdx, BoneName, ContactCount, AverageVelocity, FluidName, ImpactOffsetAverage);

			// 쿨다운 설정
			BoneEventCooldownTimers.Add(BoneIdx, BoneEventCooldown);
		}

		// 이전 프레임 상태 업데이트
		PreviousContactBones = CurrentContactBones;
	}
}

int32 UFluidInteractionComponent::GetBoneContactCount(int32 BoneIndex) const
{
	const int32* Count = CurrentBoneContactCounts.Find(BoneIndex);
	return Count ? *Count : 0;
}

void UFluidInteractionComponent::GetBonesWithContacts(TArray<int32>& OutBoneIndices) const
{
	OutBoneIndices.Empty(CurrentBoneContactCounts.Num());
	for (const auto& Pair : CurrentBoneContactCounts)
	{
		if (Pair.Value > 0)
		{
			OutBoneIndices.Add(Pair.Key);
		}
	}
}

FName UFluidInteractionComponent::GetBoneNameFromIndex(int32 BoneIndex) const
{
	// 캐시에서 먼저 확인
	const FName* CachedName = BoneIndexToNameCache.Find(BoneIndex);
	if (CachedName)
	{
		return *CachedName;
	}

	// 캐시에 없으면 SkeletalMesh에서 직접 조회
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return NAME_None;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh || !SkelMesh->GetSkeletalMeshAsset())
	{
		return NAME_None;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetSkeletalMeshAsset()->GetRefSkeleton();
	if (BoneIndex >= 0 && BoneIndex < RefSkeleton.GetNum())
	{
		return RefSkeleton.GetBoneName(BoneIndex);
	}

	return NAME_None;
}

USkeletalMeshComponent* UFluidInteractionComponent::GetOwnerSkeletalMesh() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}

	return Owner->FindComponentByClass<USkeletalMeshComponent>();
}

bool UFluidInteractionComponent::GetMostContactedBone(int32& OutBoneIndex, int32& OutContactCount) const
{
	OutBoneIndex = -1;
	OutContactCount = 0;

	for (const auto& Pair : CurrentBoneContactCounts)
	{
		if (Pair.Value > OutContactCount)
		{
			OutContactCount = Pair.Value;
			OutBoneIndex = Pair.Key;
		}
	}

	return OutBoneIndex >= 0;
}

//=============================================================================
// Boundary Particles Implementation (Flex-style Adhesion)
//=============================================================================

void UFluidInteractionComponent::SampleTriangleSurface(const FVector& V0, const FVector& V1, const FVector& V2,
                                                        float Spacing, TArray<FVector>& OutPoints)
{
	// 삼각형 변 길이
	FVector Edge1 = V1 - V0;
	FVector Edge2 = V2 - V0;

	float Len1 = Edge1.Size();
	float Len2 = Edge2.Size();

	if (Len1 < SMALL_NUMBER || Len2 < SMALL_NUMBER)
	{
		return;
	}

	// 삼각형 면적 기반으로 샘플링 개수 결정
	FVector Cross = FVector::CrossProduct(Edge1, Edge2);
	float Area = Cross.Size() * 0.5f;

	if (Area < Spacing * Spacing * 0.1f)
	{
		// 너무 작은 삼각형은 중심점만
		OutPoints.Add((V0 + V1 + V2) / 3.0f);
		return;
	}

	// Barycentric 좌표로 균일 샘플링
	int32 NumSamplesU = FMath::Max(1, FMath::CeilToInt(Len1 / Spacing));
	int32 NumSamplesV = FMath::Max(1, FMath::CeilToInt(Len2 / Spacing));

	for (int32 i = 0; i <= NumSamplesU; ++i)
	{
		float u = (float)i / (float)NumSamplesU;

		for (int32 j = 0; j <= NumSamplesV; ++j)
		{
			float v = (float)j / (float)NumSamplesV;

			// u + v <= 1 조건 (삼각형 내부)
			if (u + v > 1.0f)
			{
				continue;
			}

			// Barycentric 좌표로 점 계산
			FVector Point = V0 + Edge1 * u + Edge2 * v;
			OutPoints.Add(Point);
		}
	}
}

void UFluidInteractionComponent::GenerateBoundaryParticles()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// 배열 초기화
	BoundaryParticleLocalPositions.Empty();
	BoundaryParticleBoneIndices.Empty();
	BoundaryParticlePositions.Empty();
	BoundaryParticleVertexIndices.Empty();
	BoundaryParticleLocalNormals.Empty();
	bIsSkeletalMesh = false;

	// 1. 스켈레탈 메시 + Physics Asset 확인
	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (SkelMesh && SkelMesh->GetSkeletalMeshAsset())
	{
		bIsSkeletalMesh = true;
		USkeletalMesh* SkeletalMesh = SkelMesh->GetSkeletalMeshAsset();

		// Physics Asset 가져오기
		UPhysicsAsset* PhysAsset = SkeletalMesh->GetPhysicsAsset();
		if (!PhysAsset)
		{
			UE_LOG(LogTemp, Warning, TEXT("FluidInteraction: No PhysicsAsset found for SkeletalMesh '%s'. Boundary particles not generated."),
				*SkeletalMesh->GetName());
			return;
		}

		// RefSkeleton (본 인덱스 조회용)
		const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();

		// 각 BodySetup 순회
		for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
		{
			if (!BodySetup) continue;

			// 본 인덱스 찾기
			int32 BoneIndex = RefSkeleton.FindBoneIndex(BodySetup->BoneName);
			if (BoneIndex == INDEX_NONE)
			{
				UE_LOG(LogTemp, Verbose, TEXT("FluidInteraction: Bone '%s' not found in skeleton, skipping"),
					*BodySetup->BoneName.ToString());
				continue;
			}

			// AggGeom에서 모든 프리미티브 샘플링
			SampleAggGeomSurfaces(BodySetup->AggGeom, BoneIndex);
		}

		UE_LOG(LogTemp, Log, TEXT("FluidInteraction: Generated %d boundary particles from PhysicsAsset '%s' (%d bodies)"),
			BoundaryParticleLocalPositions.Num(), *PhysAsset->GetName(), PhysAsset->SkeletalBodySetups.Num());
	}
	else
	{
		// 2. 스태틱 메시 + Simple Collision 확인
		UStaticMeshComponent* StaticMeshComp = Owner->FindComponentByClass<UStaticMeshComponent>();
		if (StaticMeshComp && StaticMeshComp->GetStaticMesh())
		{
			UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh();
			UBodySetup* BodySetup = StaticMesh->GetBodySetup();

			if (!BodySetup || (BodySetup->AggGeom.SphereElems.Num() == 0 &&
			                   BodySetup->AggGeom.SphylElems.Num() == 0 &&
			                   BodySetup->AggGeom.BoxElems.Num() == 0))
			{
				UE_LOG(LogTemp, Warning, TEXT("FluidInteraction: No Simple Collision (Sphere/Capsule/Box) found for StaticMesh '%s'. Boundary particles not generated."),
					*StaticMesh->GetName());
				return;
			}

			// 스태틱 메시는 본 없음 (BoneIndex = -1)
			SampleAggGeomSurfaces(BodySetup->AggGeom, -1);

			UE_LOG(LogTemp, Log, TEXT("FluidInteraction: Generated %d boundary particles from StaticMesh Simple Collision '%s'"),
				BoundaryParticleLocalPositions.Num(), *StaticMesh->GetName());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("FluidInteraction: No SkeletalMesh or StaticMesh found. Boundary particles not generated."));
			return;
		}
	}

	// 월드 위치/노멀 배열 초기화
	BoundaryParticlePositions.SetNum(BoundaryParticleLocalPositions.Num());
	BoundaryParticleNormals.SetNum(BoundaryParticleLocalNormals.Num());
	bBoundaryParticlesInitialized = BoundaryParticleLocalPositions.Num() > 0;

	// 첫 프레임 위치 업데이트
	if (bBoundaryParticlesInitialized)
	{
		UpdateBoundaryParticlePositions();
	}
}

void UFluidInteractionComponent::UpdateBoundaryParticlePositions()
{
	AActor* Owner = GetOwner();
	if (!Owner || BoundaryParticlePositions.Num() == 0)
	{
		return;
	}

	const int32 NumParticles = BoundaryParticlePositions.Num();
	const bool bHasNormals = (BoundaryParticleLocalNormals.Num() == NumParticles);

	// 스켈레탈 메시인 경우 본 트랜스폼 적용
	if (bIsSkeletalMesh)
	{
		USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		if (SkelMesh && BoundaryParticleLocalPositions.Num() == NumParticles)
		{
			for (int32 i = 0; i < NumParticles; ++i)
			{
				int32 BoneIdx = BoundaryParticleBoneIndices[i];
				if (BoneIdx >= 0)
				{
					// 현재 본의 월드 트랜스폼으로 본 로컬 좌표를 월드로 변환
					FTransform BoneWorldTransform = SkelMesh->GetBoneTransform(BoneIdx);
					BoundaryParticlePositions[i] = BoneWorldTransform.TransformPosition(BoundaryParticleLocalPositions[i]);
					// 노멀도 변환
					if (bHasNormals)
					{
						BoundaryParticleNormals[i] = BoneWorldTransform.TransformVectorNoScale(BoundaryParticleLocalNormals[i]);
					}
				}
				else
				{
					FTransform ComponentTransform = SkelMesh->GetComponentTransform();
					BoundaryParticlePositions[i] = ComponentTransform.TransformPosition(BoundaryParticleLocalPositions[i]);
					if (bHasNormals)
					{
						BoundaryParticleNormals[i] = ComponentTransform.TransformVectorNoScale(BoundaryParticleLocalNormals[i]);
					}
				}
			}
		}
	}
	else
	{
		// 스태틱 메시 또는 다른 컴포넌트 - 로컬 → 월드 변환
		USceneComponent* RootComp = Owner->GetRootComponent();
		if (RootComp && BoundaryParticleLocalPositions.Num() == NumParticles)
		{
			FTransform ComponentTransform = RootComp->GetComponentTransform();

			for (int32 i = 0; i < NumParticles; ++i)
			{
				BoundaryParticlePositions[i] = ComponentTransform.TransformPosition(BoundaryParticleLocalPositions[i]);
				if (bHasNormals)
				{
					BoundaryParticleNormals[i] = ComponentTransform.TransformVectorNoScale(BoundaryParticleLocalNormals[i]);
				}
			}
		}
	}
}

void UFluidInteractionComponent::DrawDebugBoundaryParticles()
{
	UWorld* World = GetWorld();
	if (!World || BoundaryParticlePositions.Num() == 0)
	{
		return;
	}

	const int32 NumParticles = BoundaryParticlePositions.Num();
	const bool bHasNormals = (BoundaryParticleNormals.Num() == NumParticles);

	for (int32 i = 0; i < NumParticles; ++i)
	{
		const FVector& Pos = BoundaryParticlePositions[i];

		// 점 그리기
		DrawDebugPoint(
			World,
			Pos,
			BoundaryParticleDebugSize,
			BoundaryParticleDebugColor,
			false,  // bPersistentLines
			-1.0f,  // LifeTime
			SDPG_Foreground  // DepthPriority - 메쉬 앞에 항상 표시
		);

		// 노말 화살표 그리기
		if (bShowBoundaryNormals && bHasNormals)
		{
			const FVector& Normal = BoundaryParticleNormals[i];
			FVector EndPos = Pos + Normal * BoundaryNormalLength;

			DrawDebugDirectionalArrow(
				World,
				Pos,
				EndPos,
				3.0f,  // ArrowSize
				FColor::Yellow,
				false,  // bPersistentLines
				-1.0f,  // LifeTime
				SDPG_Foreground,  // DepthPriority
				1.0f   // Thickness
			);
		}
	}
}

void UFluidInteractionComponent::RegenerateBoundaryParticles()
{
	bBoundaryParticlesInitialized = false;
	bIsSkeletalMesh = false;
	BoundaryParticleLocalPositions.Empty();
	BoundaryParticleBoneIndices.Empty();
	BoundaryParticlePositions.Empty();
	BoundaryParticleVertexIndices.Empty();
	BoundaryParticleNormals.Empty();
	BoundaryParticleLocalNormals.Empty();

	if (bEnableBoundaryParticles)
	{
		GenerateBoundaryParticles();
	}
}

void UFluidInteractionComponent::CollectGPUBoundaryParticles(FGPUBoundaryParticles& OutBoundaryParticles) const
{
	if (!bBoundaryParticlesInitialized || BoundaryParticlePositions.Num() == 0)
	{
		return;
	}

	const int32 NumParticles = BoundaryParticlePositions.Num();
	OutBoundaryParticles.Particles.Reserve(OutBoundaryParticles.Particles.Num() + NumParticles);

	// Component의 고유 ID 생성
	const int32 OwnerID = GetUniqueID();

	for (int32 i = 0; i < NumParticles; ++i)
	{
		FVector3f Position = FVector3f(BoundaryParticlePositions[i]);
		FVector3f Normal = (i < BoundaryParticleNormals.Num())
			? FVector3f(BoundaryParticleNormals[i])
			: FVector3f(0.0f, 0.0f, 1.0f);

		// Psi는 경계 입자의 볼륨 기여도
		// 낮을수록 밀려나는 힘 감소, 높을수록 강하게 밀려남
		float Psi = 0.1f;

		OutBoundaryParticles.Add(Position, Normal, OwnerID, Psi);
	}
}

void UFluidInteractionComponent::CollectLocalBoundaryParticles(TArray<FGPUBoundaryParticleLocal>& OutLocalParticles) const
{
	if (!bBoundaryParticlesInitialized || BoundaryParticleLocalPositions.Num() == 0)
	{
		return;
	}

	const int32 NumParticles = BoundaryParticleLocalPositions.Num();
	OutLocalParticles.Reserve(OutLocalParticles.Num() + NumParticles);

	for (int32 i = 0; i < NumParticles; ++i)
	{
		FVector3f LocalPosition = FVector3f(BoundaryParticleLocalPositions[i]);
		FVector3f LocalNormal = (i < BoundaryParticleLocalNormals.Num())
			? FVector3f(BoundaryParticleLocalNormals[i])
			: FVector3f(0.0f, 0.0f, 1.0f);
		int32 BoneIndex = (i < BoundaryParticleBoneIndices.Num()) ? BoundaryParticleBoneIndices[i] : -1;

		// Psi는 경계 입자의 볼륨 기여도
		float Psi = 0.1f;

		OutLocalParticles.Add(FGPUBoundaryParticleLocal(LocalPosition, BoneIndex, LocalNormal, Psi));
	}
}

void UFluidInteractionComponent::CollectBoneTransformsForBoundary(TArray<FMatrix>& OutBoneTransforms, FMatrix& OutComponentTransform) const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		OutComponentTransform = FMatrix::Identity;
		return;
	}

	if (bIsSkeletalMesh)
	{
		USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		if (SkelMesh)
		{
			// 모든 본의 월드 트랜스폼 수집
			const int32 NumBones = SkelMesh->GetNumBones();
			OutBoneTransforms.SetNum(NumBones);

			for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
			{
				FTransform BoneWorldTransform = SkelMesh->GetBoneTransform(BoneIdx);
				OutBoneTransforms[BoneIdx] = BoneWorldTransform.ToMatrixWithScale();
			}

			OutComponentTransform = SkelMesh->GetComponentTransform().ToMatrixWithScale();
		}
		else
		{
			OutComponentTransform = FMatrix::Identity;
		}
	}
	else
	{
		// 스태틱 메시 또는 다른 컴포넌트
		USceneComponent* RootComp = Owner->GetRootComponent();
		if (RootComp)
		{
			OutComponentTransform = RootComp->GetComponentTransform().ToMatrixWithScale();
		}
		else
		{
			OutComponentTransform = FMatrix::Identity;
		}
		OutBoneTransforms.Empty();
	}
}

//=============================================================================
// Physics Asset / Simple Collision 기반 표면 샘플링
//=============================================================================

void UFluidInteractionComponent::SampleSphereSurface(const FKSphereElem& Sphere, int32 BoneIndex, const FTransform& LocalTransform)
{
	float Radius = Sphere.Radius;
	FVector Center = Sphere.Center;

	// 샘플 수 결정: 표면적 기반 (4πr²)
	float SurfaceArea = 4.0f * PI * Radius * Radius;
	float SpacingSq = BoundaryParticleSpacing * BoundaryParticleSpacing;
	int32 NumSamples = FMath::Max(8, FMath::CeilToInt(SurfaceArea / SpacingSq));
	NumSamples = FMath::Min(NumSamples, 200);  // 최대 제한

	// 피보나치 구 샘플링 (균일 분포)
	float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
	float AngleIncrement = 2.0f * PI * GoldenRatio;

	for (int32 i = 0; i < NumSamples; ++i)
	{
		float t = (NumSamples > 1) ? (float)i / (float)(NumSamples - 1) : 0.5f;
		float Phi = FMath::Acos(1.0f - 2.0f * t);
		float Theta = AngleIncrement * i;

		// 구면 좌표 → 카르테시안
		FVector LocalPoint(
			Radius * FMath::Sin(Phi) * FMath::Cos(Theta),
			Radius * FMath::Sin(Phi) * FMath::Sin(Theta),
			Radius * FMath::Cos(Phi)
		);

		// 프리미티브 로컬 → 본 로컬
		FVector BoneLocalPos = LocalTransform.TransformPosition(LocalPoint + Center);

		// 노멀은 중심에서 외부로
		FVector LocalNormal = LocalPoint.GetSafeNormal();
		FVector BoneLocalNormal = LocalTransform.TransformVectorNoScale(LocalNormal);

		BoundaryParticleLocalPositions.Add(BoneLocalPos);
		BoundaryParticleBoneIndices.Add(BoneIndex);
		BoundaryParticleLocalNormals.Add(BoneLocalNormal);
	}
}

void UFluidInteractionComponent::SampleHemisphere(const FTransform& Transform, float Radius, float ZOffset,
                                                   int32 ZDirection, int32 BoneIndex, int32 NumSamples)
{
	if (NumSamples <= 0) return;

	float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
	float AngleIncrement = 2.0f * PI * GoldenRatio;

	for (int32 i = 0; i < NumSamples; ++i)
	{
		// 반구만 샘플링
		float t = (float)i / (float)(NumSamples * 2);
		if (ZDirection < 0) t = 1.0f - t;

		// t가 반구 범위를 벗어나면 스킵
		if ((ZDirection > 0 && t > 0.5f) || (ZDirection < 0 && t < 0.5f))
			continue;

		float Phi = FMath::Acos(1.0f - 2.0f * t);
		float Theta = AngleIncrement * i;

		FVector LocalPoint(
			Radius * FMath::Sin(Phi) * FMath::Cos(Theta),
			Radius * FMath::Sin(Phi) * FMath::Sin(Theta),
			Radius * FMath::Cos(Phi) + ZOffset
		);

		FVector LocalNormal = FVector(
			LocalPoint.X,
			LocalPoint.Y,
			LocalPoint.Z - ZOffset
		).GetSafeNormal();

		FVector BoneLocalPos = Transform.TransformPosition(LocalPoint);
		FVector BoneLocalNormal = Transform.TransformVectorNoScale(LocalNormal);

		BoundaryParticleLocalPositions.Add(BoneLocalPos);
		BoundaryParticleBoneIndices.Add(BoneIndex);
		BoundaryParticleLocalNormals.Add(BoneLocalNormal);
	}
}

void UFluidInteractionComponent::SampleCapsuleSurface(const FKSphylElem& Capsule, int32 BoneIndex)
{
	float Radius = Capsule.Radius;
	float HalfLength = Capsule.Length * 0.5f;
	FTransform CapsuleTransform = Capsule.GetTransform();

	// 원통 부분
	float CylinderHeight = Capsule.Length;
	float CylinderArea = 2.0f * PI * Radius * CylinderHeight;

	// 반구 2개 = 완전한 구
	float SphereArea = 4.0f * PI * Radius * Radius;

	float TotalArea = CylinderArea + SphereArea;
	float SpacingSq = BoundaryParticleSpacing * BoundaryParticleSpacing;
	int32 TotalSamples = FMath::Max(12, FMath::CeilToInt(TotalArea / SpacingSq));
	TotalSamples = FMath::Min(TotalSamples, 500);

	// 면적 비율로 샘플 분배
	float CylinderRatio = CylinderArea / TotalArea;
	int32 CylinderSamples = FMath::CeilToInt(TotalSamples * CylinderRatio);
	int32 SphereSamples = TotalSamples - CylinderSamples;

	// 원통 부분 샘플링
	int32 NumRings = FMath::Max(2, FMath::CeilToInt(CylinderHeight / BoundaryParticleSpacing));
	int32 NumSegments = FMath::Max(6, CylinderSamples / FMath::Max(1, NumRings));

	for (int32 Ring = 0; Ring <= NumRings; ++Ring)
	{
		float Z = -HalfLength + (CylinderHeight * Ring / FMath::Max(1, NumRings));

		for (int32 Seg = 0; Seg < NumSegments; ++Seg)
		{
			float Angle = 2.0f * PI * Seg / NumSegments;

			FVector LocalPoint(
				Radius * FMath::Cos(Angle),
				Radius * FMath::Sin(Angle),
				Z
			);

			FVector LocalNormal(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);

			FVector BoneLocalPos = CapsuleTransform.TransformPosition(LocalPoint);
			FVector BoneLocalNormal = CapsuleTransform.TransformVectorNoScale(LocalNormal);

			BoundaryParticleLocalPositions.Add(BoneLocalPos);
			BoundaryParticleBoneIndices.Add(BoneIndex);
			BoundaryParticleLocalNormals.Add(BoneLocalNormal);
		}
	}

	// 반구 부분 (상단/하단)
	int32 HalfSphereSamples = SphereSamples / 2;
	SampleHemisphere(CapsuleTransform, Radius, +HalfLength, +1, BoneIndex, HalfSphereSamples);
	SampleHemisphere(CapsuleTransform, Radius, -HalfLength, -1, BoneIndex, HalfSphereSamples);
}

void UFluidInteractionComponent::SampleBoxSurface(const FKBoxElem& Box, int32 BoneIndex)
{
	FVector HalfExtent(Box.X * 0.5f, Box.Y * 0.5f, Box.Z * 0.5f);
	FTransform BoxTransform = Box.GetTransform();

	// 6개 면 정의: 노멀, 거리, 축1, 축2, 크기1, 크기2
	struct FBoxFace
	{
		FVector Normal;
		float Distance;
		FVector Axis1, Axis2;
		float Size1, Size2;
	};

	TArray<FBoxFace> Faces;
	Faces.Add({ FVector( 1, 0, 0), (float)HalfExtent.X, FVector(0, 1, 0), FVector(0, 0, 1), (float)Box.Y, (float)Box.Z });
	Faces.Add({ FVector(-1, 0, 0), (float)HalfExtent.X, FVector(0, 1, 0), FVector(0, 0, 1), (float)Box.Y, (float)Box.Z });
	Faces.Add({ FVector( 0, 1, 0), (float)HalfExtent.Y, FVector(1, 0, 0), FVector(0, 0, 1), (float)Box.X, (float)Box.Z });
	Faces.Add({ FVector( 0,-1, 0), (float)HalfExtent.Y, FVector(1, 0, 0), FVector(0, 0, 1), (float)Box.X, (float)Box.Z });
	Faces.Add({ FVector( 0, 0, 1), (float)HalfExtent.Z, FVector(1, 0, 0), FVector(0, 1, 0), (float)Box.X, (float)Box.Y });
	Faces.Add({ FVector( 0, 0,-1), (float)HalfExtent.Z, FVector(1, 0, 0), FVector(0, 1, 0), (float)Box.X, (float)Box.Y });

	float SpacingSq = BoundaryParticleSpacing * BoundaryParticleSpacing;

	for (const FBoxFace& Face : Faces)
	{
		float FaceArea = Face.Size1 * Face.Size2;
		int32 NumU = FMath::Max(1, FMath::CeilToInt(Face.Size1 / BoundaryParticleSpacing));
		int32 NumV = FMath::Max(1, FMath::CeilToInt(Face.Size2 / BoundaryParticleSpacing));

		for (int32 u = 0; u <= NumU; ++u)
		{
			for (int32 v = 0; v <= NumV; ++v)
			{
				float t1 = (NumU > 0) ? ((float)u / (float)NumU - 0.5f) : 0.0f;
				float t2 = (NumV > 0) ? ((float)v / (float)NumV - 0.5f) : 0.0f;

				FVector LocalPoint = Face.Normal * Face.Distance
				                   + Face.Axis1 * (t1 * Face.Size1)
				                   + Face.Axis2 * (t2 * Face.Size2);

				FVector BoneLocalPos = BoxTransform.TransformPosition(LocalPoint);
				FVector BoneLocalNormal = BoxTransform.TransformVectorNoScale(Face.Normal);

				BoundaryParticleLocalPositions.Add(BoneLocalPos);
				BoundaryParticleBoneIndices.Add(BoneIndex);
				BoundaryParticleLocalNormals.Add(BoneLocalNormal);
			}
		}
	}
}

void UFluidInteractionComponent::SampleAggGeomSurfaces(const FKAggregateGeom& AggGeom, int32 BoneIndex)
{
	// Sphere 콜라이더
	for (const FKSphereElem& Sphere : AggGeom.SphereElems)
	{
		SampleSphereSurface(Sphere, BoneIndex, Sphere.GetTransform());
	}

	// Capsule(Sphyl) 콜라이더
	for (const FKSphylElem& Capsule : AggGeom.SphylElems)
	{
		SampleCapsuleSurface(Capsule, BoneIndex);
	}

	// Box 콜라이더
	for (const FKBoxElem& Box : AggGeom.BoxElems)
	{
		SampleBoxSurface(Box, BoneIndex);
	}
}
