// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/FluidInteractionComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/SpatialHash.h"
#include "Collision/MeshFluidCollider.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Components/SkeletalMeshComponent.h"
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
