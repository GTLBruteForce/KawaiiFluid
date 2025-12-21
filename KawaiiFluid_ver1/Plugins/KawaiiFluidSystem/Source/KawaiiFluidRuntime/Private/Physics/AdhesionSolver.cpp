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
	float DetachThreshold)
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

	for (FFluidParticle& Particle : Particles)
	{
		FVector TotalAdhesionForce = FVector::ZeroVector;
		AActor* ClosestColliderActor = nullptr;
		float ClosestDistance = AdhesionRadius;

		for (UFluidCollider* Collider : Colliders)
		{
			if (!Collider || !Collider->IsColliderEnabled())
			{
				continue;
			}

			// 콜라이더에서 최근접점과 법선 얻기
			FVector ClosestPoint;
			FVector Normal;
			float Distance;

			if (Collider->GetClosestPoint(Particle.Position, ClosestPoint, Normal, Distance))
			{
				// 충돌 마진: 실제로 표면에 닿은 입자에만 접착력 적용
				const float CollisionMargin = 5.0f;  // FluidCollider와 동일한 값

				if (Distance <= CollisionMargin)
				{
					// 접착력 계산 (충돌한 입자에만)
					FVector AdhesionForce = ComputeAdhesionForce(
						Particle.Position,
						ClosestPoint,
						Normal,
						Distance,
						AdhesionStrength,
						AdhesionRadius
					);

					TotalAdhesionForce += AdhesionForce;

					// 디버그
					static int32 AdhesionDebugCounter = 0;
					if (++AdhesionDebugCounter % 1000 == 0)
					{
						UE_LOG(LogTemp, Warning, TEXT("AdhesionSolver: Distance=%.2f, Force=(%.2f, %.2f, %.2f)"),
							Distance, AdhesionForce.X, AdhesionForce.Y, AdhesionForce.Z);
					}

					// 가장 가까운 콜라이더 추적
					if (Distance < ClosestDistance)
					{
						ClosestDistance = Distance;
						ClosestColliderActor = Collider->GetOwner();
					}
				}
			}
		}

		// 접착력 적용 (속도에 추가)
		float ForceMagnitude = TotalAdhesionForce.Size();
		Particle.Velocity += TotalAdhesionForce;

		// 접착 상태 업데이트
		UpdateAttachmentState(Particle, ClosestColliderActor, ForceMagnitude, DetachThreshold);
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

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		FFluidParticle& Particle = Particles[i];
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

		Particle.Velocity += CohesionForce;
	}
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
	float DetachThreshold)
{
	if (ColliderActor)
	{
		if (!Particle.bIsAttached)
		{
			// 새로 접착
			Particle.bIsAttached = true;
			Particle.AttachedActor = ColliderActor;
		}
		else if (Particle.AttachedActor.Get() != ColliderActor)
		{
			// 다른 오브젝트로 이동
			Particle.AttachedActor = ColliderActor;
		}
	}
	else
	{
		// 콜라이더 근처에 없으면 무조건 접착 해제
		if (Particle.bIsAttached)
		{
			Particle.bIsAttached = false;
			Particle.AttachedActor.Reset();
		}
	}
}
