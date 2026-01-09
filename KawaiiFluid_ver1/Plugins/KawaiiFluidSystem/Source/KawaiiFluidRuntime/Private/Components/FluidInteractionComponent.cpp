// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/FluidInteractionComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/SpatialHash.h"
#include "Collision/MeshFluidCollider.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidParticle.h"
#include "DrawDebugHelpers.h"

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
		// 우선순위: SkeletalMeshComponent > CapsuleComponent
		USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		if (SkelMesh)
		{
			AutoCollider->TargetMeshComponent = SkelMesh;
		}
		else
		{
			// SkeletalMesh가 없으면 CapsuleComponent 사용 (캐릭터의 경우)
			UCapsuleComponent* Capsule = Owner->FindComponentByClass<UCapsuleComponent>();
			if (Capsule)
			{
				AutoCollider->TargetMeshComponent = Capsule;
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
		for (auto* Module : TargetSubsystem->GetAllModules())
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

		for (auto* Module : TargetSubsystem->GetAllModules())
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
	for (auto* Module : TargetSubsystem->GetAllModules())
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

				// OwnerID 필터링: 이 액터의 콜라이더와 충돌한 피드백만 힘 계산에 사용
				if (Feedback.OwnerID != 0 && Feedback.OwnerID != MyOwnerID)
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
	for (auto* Module : TargetSubsystem->GetAllModules())
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
