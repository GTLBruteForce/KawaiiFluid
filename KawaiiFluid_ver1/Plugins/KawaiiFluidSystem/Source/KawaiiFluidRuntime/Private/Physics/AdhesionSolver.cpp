// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Physics/AdhesionSolver.h"
#include "Physics/SPHKernels.h"
#include "Collision/FluidCollider.h"

FAdhesionSolver::FAdhesionSolver()
{
}

void FAdhesionSolver::Apply(
	TArray<FFluidParticle>& Particles,
	const TArray<UFluidCollider*>& Colliders,
	float AdhesionStrength,
	float AdhesionRadius,
	float DetachThreshold,
	float ColliderContactOffset)
{
	// 디버그: AdhesionSolver 호출 확인
	static int32 ApplyDebugCounter = 0;
	if (++ApplyDebugCounter % 1000 == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("AdhesionSolver::Apply - Colliders: %d, Strength: %.2f, Radius: %.2f"),
			Colliders.Num(), AdhesionStrength, AdhesionRadius);
	}

	if (AdhesionStrength <= 0.0f || Colliders.Num() == 0)
	{
		return;
	}

	// 결과 저장용 구조체
	struct FAdhesionResult
	{
		FVector Force;
		AActor* ClosestActor;
		float ForceMagnitude;
		FName BoneName;
		FTransform BoneTransform;
		FVector ParticlePosition;  // 로컬 오프셋 계산용
		FVector SurfaceNormal;     // 표면 미끄러짐 계산용
	};

	TArray<FAdhesionResult> Results;
	Results.SetNum(Particles.Num());

	// 병렬 계산 (읽기만 하므로 안전)
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		const FFluidParticle& Particle = Particles[i];
		FVector TotalAdhesionForce = FVector::ZeroVector;
		AActor* ClosestColliderActor = nullptr;
		float ClosestDistance = FLT_MAX;
		FName ClosestBoneName = NAME_None;
		FTransform ClosestBoneTransform = FTransform::Identity;
		FVector ClosestSurfaceNormal = FVector::UpVector;
		FVector ClosestSurfacePoint = FVector::ZeroVector;

		for (UFluidCollider* Collider : Colliders)
		{
			if (!Collider || !Collider->IsColliderEnabled())
			{
				continue;
			}

			// 콜라이더에서 최근접점과 법선, 본 정보 얻기
			FVector SurfacePoint;
			FVector Normal;
			float Distance;
			FName BoneName;
			FTransform BoneTransform;

			if (Collider->GetClosestPointWithBone(Particle.Position, SurfacePoint, Normal, Distance, BoneName, BoneTransform))
			{
				const float AdjustedDistance = FMath::Max(0.0f, Distance - ColliderContactOffset);

				// 가장 가까운 콜라이더 추적 (거리 무관하게)
				if (AdjustedDistance < ClosestDistance)
				{
					ClosestDistance = AdjustedDistance;
					ClosestColliderActor = Collider->GetOwner();
					ClosestBoneName = BoneName;
					ClosestBoneTransform = BoneTransform;
					ClosestSurfaceNormal = Normal;
					ClosestSurfacePoint = SurfacePoint;
				}
			}
		}

		// 접착력 계산: 상태에 따라 다른 마진 적용
		// 이미 붙어있는 입자 (위에서 떨어지며 몸에 닿음): 엄격한 마진
		const float AttachMargin_Attached = 5.0f;
		const float MaintainMargin_Attached = 15.0f;
		const float MaintainMargin_NearGround = 5.0f;  // 바닥 근처일 때 감소된 마진
		// 안 붙어있던 입자 (바닥에서 몸에 새로 붙음): 여유로운 마진
		const float AttachMargin_New = 10.0f;

		bool bShouldApplyAdhesion = false;
		bool bSameActor = (Particle.bIsAttached && Particle.AttachedActor.Get() == ClosestColliderActor);

		if (Particle.bIsAttached)
		{
			if (bSameActor)
			{
				// 같은 액터에 접착 유지 (바닥 근처면 마진 감소)
				float EffectiveMaintainMargin = Particle.bNearGround ? MaintainMargin_NearGround : MaintainMargin_Attached;
				bShouldApplyAdhesion = (ClosestDistance <= EffectiveMaintainMargin);
			}
			else
			{
				// 다른 액터(바닥 등)가 더 가까움: 기존 접착 해제하고 새로 접착 판단
				bShouldApplyAdhesion = (ClosestDistance <= AttachMargin_Attached);
			}
		}
		else if (!Particle.bJustDetached)
		{
			// 새로 접착: 여유로운 마진 (바닥에서 몸에 붙는 경우 등 대응)
			// bJustDetached가 true면 이번 프레임에 분리된 것이므로 재접착 안함
			bShouldApplyAdhesion = (ClosestDistance <= AttachMargin_New);
		}

		if (bShouldApplyAdhesion && ClosestColliderActor)
		{
			if (bSameActor && ClosestDistance > AttachMargin_Attached)
			{
				// 같은 액터에서 멀어진 경우: 강한 복귀 힘 적용 (빠른 이동 대응)
				FVector ToSurface = ClosestSurfacePoint - Particle.Position;
				float ToSurfaceLen = ToSurface.Size();
				if (ToSurfaceLen > KINDA_SMALL_NUMBER)
				{
					FVector Direction = ToSurface / ToSurfaceLen;
					// 거리에 비례하는 강한 복귀 힘 (스프링처럼)
					float RecoveryStrength = FMath::Min(ClosestDistance * 0.5f, 50.0f);
					TotalAdhesionForce = Direction * RecoveryStrength;
				}
			}
			else
			{
				// 일반 접착력 계산 (표면 가까이 있을 때)
				FVector AdhesionForce = ComputeAdhesionForce(
					Particle.Position,
					ClosestSurfacePoint,
					ClosestSurfaceNormal,
					ClosestDistance,
					AdhesionStrength,
					AdhesionRadius
				);

				TotalAdhesionForce = AdhesionForce;
			}
		}
		else
		{
			// 디버그: 분리 원인 로깅
			static int32 DistanceLogCounter = 0;
			if (Particle.bIsAttached && ++DistanceLogCounter % 200 == 1)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Distance] Particle %d: Dist=%.1f, SameActor=%d, Bone=%s"),
					Particle.ParticleID, ClosestDistance, bSameActor ? 1 : 0, *Particle.AttachedBoneName.ToString());
			}
			// 접착 범위 밖이면 콜라이더 정보 클리어
			ClosestColliderActor = nullptr;
		}

		Results[i].Force = TotalAdhesionForce;
		Results[i].ClosestActor = ClosestColliderActor;
		Results[i].ForceMagnitude = TotalAdhesionForce.Size();
		Results[i].BoneName = ClosestBoneName;
		Results[i].BoneTransform = ClosestBoneTransform;
		Results[i].ParticlePosition = Particle.Position;
		Results[i].SurfaceNormal = ClosestSurfaceNormal;
	});

	// 순차 적용 (상태 변경이 있으므로)
	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		Particles[i].Velocity += Results[i].Force;
		UpdateAttachmentState(
			Particles[i],
			Results[i].ClosestActor,
			Results[i].ForceMagnitude,
			DetachThreshold,
			Results[i].BoneName,
			Results[i].BoneTransform,
			Results[i].ParticlePosition,
			Results[i].SurfaceNormal
		);
		// 프레임 끝에서 분리 플래그 리셋 (다음 프레임에는 재접착 가능)
		Particles[i].bJustDetached = false;
	}
}

void FAdhesionSolver::ApplyCohesion(
	TArray<FFluidParticle>& Particles,
	float CohesionStrength,
	float SmoothingRadius)
{
	if (CohesionStrength <= 0.0f)
	{
		return;
	}

	TArray<FVector> CohesionForces;
	CohesionForces.SetNum(Particles.Num());

	// 병렬 계산
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		const FFluidParticle& Particle = Particles[i];
		FVector CohesionForce = FVector::ZeroVector;

		for (int32 NeighborIdx : Particle.NeighborIndices)
		{
			if (NeighborIdx == i)
			{
				continue;
			}

			const FFluidParticle& Neighbor = Particles[NeighborIdx];
			FVector r = Particle.Position - Neighbor.Position;
			float Distance = r.Size();

			if (Distance < KINDA_SMALL_NUMBER || Distance > SmoothingRadius)
			{
				continue;
			}

			// Cohesion 커널
			float CohesionWeight = SPHKernels::Cohesion(Distance, SmoothingRadius);

			// 응집력: 이웃 방향으로 당김
			FVector Direction = -r / Distance;
			CohesionForce += CohesionStrength * CohesionWeight * Direction;
		}

		CohesionForces[i] = CohesionForce;
	});

	// 병렬 적용
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		Particles[i].Velocity += CohesionForces[i];
	});
}

FVector FAdhesionSolver::ComputeAdhesionForce(
	const FVector& ParticlePos,
	const FVector& SurfacePoint,
	const FVector& SurfaceNormal,
	float Distance,
	float AdhesionStrength,
	float AdhesionRadius)
{
	// Adhesion 커널 값
	float AdhesionWeight = SPHKernels::Adhesion(Distance, AdhesionRadius);

	if (AdhesionWeight <= 0.0f)
	{
		return FVector::ZeroVector;
	}

	// 표면 방향 벡터
	FVector ToSurface = SurfacePoint - ParticlePos;

	if (ToSurface.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		return FVector::ZeroVector;
	}

	ToSurface.Normalize();

	// 접착력: 표면 방향으로 당김
	FVector AdhesionForce = AdhesionStrength * AdhesionWeight * ToSurface;

	return AdhesionForce;
}

void FAdhesionSolver::UpdateAttachmentState(
	FFluidParticle& Particle,
	AActor* ColliderActor,
	float Force,
	float DetachThreshold,
	FName BoneName,
	const FTransform& BoneTransform,
	const FVector& ParticlePosition,
	const FVector& SurfaceNormal)
{
	// 디버그: 분리 원인 추적
	static int32 DetachLogCounter = 0;
	if (Particle.bIsAttached && !ColliderActor)
	{
		if (++DetachLogCounter % 100 == 1)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Detach] Particle %d detached! Was on bone: %s"),
				Particle.ParticleID, *Particle.AttachedBoneName.ToString());
		}
	}

	if (ColliderActor)
	{
		if (!Particle.bIsAttached)
		{
			// 새로 접착
			Particle.bIsAttached = true;
			Particle.AttachedActor = ColliderActor;
			Particle.AttachedBoneName = BoneName;
			// 본 로컬 좌표로 변환하여 저장
			Particle.AttachedLocalOffset = BoneTransform.InverseTransformPosition(ParticlePosition);
			Particle.AttachedSurfaceNormal = SurfaceNormal;
		}
		else if (Particle.AttachedActor.Get() != ColliderActor || Particle.AttachedBoneName != BoneName)
		{
			// 다른 오브젝트 또는 다른 본으로 이동
			Particle.AttachedActor = ColliderActor;
			Particle.AttachedBoneName = BoneName;
			Particle.AttachedLocalOffset = BoneTransform.InverseTransformPosition(ParticlePosition);
			Particle.AttachedSurfaceNormal = SurfaceNormal;
		}
		else
		{
			// 같은 본에 계속 접착: 시뮬레이션에 의한 위치 변화(흘러내림)를 로컬 오프셋에 반영
			Particle.AttachedLocalOffset = BoneTransform.InverseTransformPosition(ParticlePosition);
			Particle.AttachedSurfaceNormal = SurfaceNormal;
		}
	}
	else
	{
		// 콜라이더 근처에 없으면 무조건 접착 해제
		if (Particle.bIsAttached)
		{
			Particle.bIsAttached = false;
			Particle.AttachedActor.Reset();
			Particle.AttachedBoneName = NAME_None;
			Particle.AttachedLocalOffset = FVector::ZeroVector;
			Particle.AttachedSurfaceNormal = FVector::UpVector;
		}
	}
}
