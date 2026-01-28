// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Physics/AdhesionSolver.h"
#include "Physics/SPHKernels.h"
#include "Collision/FluidCollider.h"
#include "Async/ParallelFor.h"
#include "GameFramework/Actor.h"

FAdhesionSolver::FAdhesionSolver()
{
}

void FAdhesionSolver::Apply(
	TArray<FFluidParticle>& Particles,
	const TArray<TObjectPtr<UFluidCollider>>& Colliders,
	float AdhesionStrength,
	float AdhesionRadius,
	float DetachThreshold,
	float ColliderContactOffset)
{
	// Debug: Verify AdhesionSolver invocation - disabled for performance
	// static int32 ApplyDebugCounter = 0;
	// if (++ApplyDebugCounter % 1000 == 0)
	// {
	// 	UE_LOG(LogTemp, Warning, TEXT("AdhesionSolver::Apply - Colliders: %d, Strength: %.2f, Radius: %.2f"),
	// 		Colliders.Num(), AdhesionStrength, AdhesionRadius);
	// }

	if (AdhesionStrength <= 0.0f || Colliders.Num() == 0)
	{
		return;
	}

	// Structure for storing results
	struct FAdhesionResult
	{
		FVector Force;
		AActor* ClosestActor;
		float ForceMagnitude;
		FName BoneName;
		FTransform BoneTransform;
		FVector ParticlePosition;  // For local offset calculation
		FVector SurfaceNormal;     // For surface slip calculation
	};

	TArray<FAdhesionResult> Results;
	Results.SetNum(Particles.Num());

	// Parallel computation (safe as read-only)
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

			// Get closest point, normal, and bone information from collider
			FVector SurfacePoint;
			FVector Normal;
			float Distance;
			FName BoneName;
			FTransform BoneTransform;

			if (Collider->GetClosestPointWithBone(Particle.Position, SurfacePoint, Normal, Distance, BoneName, BoneTransform))
			{
				const float AdjustedDistance = FMath::Max(0.0f, Distance - ColliderContactOffset);

				// Track closest collider (regardless of distance)
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

		// Adhesion force calculation: apply different margins based on state
		// Already attached particles (falling from above onto body): strict margin
		const float AttachMargin_Attached = 5.0f;
		const float MaintainMargin_Attached = 15.0f;
		const float MaintainMargin_NearGround = 5.0f;  // Reduced margin when near ground
		// Previously unattached particles (newly attaching from floor to body): relaxed margin
		const float AttachMargin_New = 10.0f;

		bool bShouldApplyAdhesion = false;
		bool bSameActor = (Particle.bIsAttached && Particle.AttachedActor.Get() == ClosestColliderActor);

		if (Particle.bIsAttached)
		{
			if (bSameActor)
			{
				// Maintain adhesion to same actor (reduced margin if near ground)
				float EffectiveMaintainMargin = Particle.bNearGround ? MaintainMargin_NearGround : MaintainMargin_Attached;
				bShouldApplyAdhesion = (ClosestDistance <= EffectiveMaintainMargin);
			}
			else
			{
				// Different actor (floor etc.) is closer: release existing adhesion and judge new attachment
				bShouldApplyAdhesion = (ClosestDistance <= AttachMargin_Attached);
			}
		}
		else if (!Particle.bJustDetached)
		{
			// New attachment: relaxed margin (handles cases like attaching from floor to body)
			// If bJustDetached is true, particle detached this frame so prevent reattachment
			bShouldApplyAdhesion = (ClosestDistance <= AttachMargin_New);
		}

		if (bShouldApplyAdhesion && ClosestColliderActor)
		{
			if (bSameActor && ClosestDistance > AttachMargin_Attached)
			{
				// Moving away from same actor: apply strong recovery force (handles fast movement)
				FVector ToSurface = ClosestSurfacePoint - Particle.Position;
				float ToSurfaceLen = ToSurface.Size();
				if (ToSurfaceLen > KINDA_SMALL_NUMBER)
				{
					FVector Direction = ToSurface / ToSurfaceLen;
					// Strong recovery force proportional to distance (spring-like)
					float RecoveryStrength = FMath::Min(ClosestDistance * 0.5f, 50.0f);
					TotalAdhesionForce = Direction * RecoveryStrength;
				}
			}
			else
			{
				// Normal adhesion force calculation (when close to surface)
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
			// Outside adhesion range - clear collider information
			ClosestColliderActor = nullptr;
		}

		Results[i].Force = TotalAdhesionForce;
		Results[i].ClosestActor = ClosestColliderActor;
		Results[i].ForceMagnitude = TotalAdhesionForce.Size();
		Results[i].BoneName = ClosestBoneName;
		Results[i].BoneTransform = ClosestBoneTransform;
		Results[i].ParticlePosition = Particle.Position;
		Results[i].SurfaceNormal = ClosestSurfaceNormal;
	}, EParallelForFlags::Unbalanced);

	// Sequential application (due to state changes)
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
		// Reset detachment flag at frame end (allow reattachment next frame)
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

	// Parallel computation
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

			// Cohesion kernel
			float CohesionWeight = SPHKernels::Cohesion(Distance, SmoothingRadius);

			// Cohesion force: pull towards neighbors
			FVector Direction = -r / Distance;
			CohesionForce += CohesionStrength * CohesionWeight * Direction;
		}

		CohesionForces[i] = CohesionForce;
	});

	// Parallel application
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
	// Adhesion kernel value
	float AdhesionWeight = SPHKernels::Adhesion(Distance, AdhesionRadius);

	if (AdhesionWeight <= 0.0f)
	{
		return FVector::ZeroVector;
	}

	// Surface direction vector
	FVector ToSurface = SurfacePoint - ParticlePos;

	if (ToSurface.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		return FVector::ZeroVector;
	}

	ToSurface.Normalize();

	// Adhesion force: pull towards surface
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
	// Debug: Track detachment reason - disabled for performance
	// static int32 DetachLogCounter = 0;
	// if (Particle.bIsAttached && !ColliderActor)
	// {
	// 	if (++DetachLogCounter % 100 == 1)
	// 	{
	// 		UE_LOG(LogTemp, Warning, TEXT("[Detach] Particle %d detached! Was on bone: %s"),
	// 			Particle.ParticleID, *Particle.AttachedBoneName.ToString());
	// 	}
	// }

	if (ColliderActor)
	{
		if (!Particle.bIsAttached)
		{
		// New attachment
			Particle.bIsAttached = true;
			Particle.AttachedActor = TWeakObjectPtr<AActor>(ColliderActor);
			Particle.AttachedBoneName = BoneName;
			// Transform and store in bone local coordinates
			Particle.AttachedLocalOffset = BoneTransform.InverseTransformPosition(ParticlePosition);
			Particle.AttachedSurfaceNormal = SurfaceNormal;
		}
		else if (Particle.AttachedActor.Get() != ColliderActor || Particle.AttachedBoneName != BoneName)
		{
		// Moving to different object or different bone
			Particle.AttachedActor = TWeakObjectPtr<AActor>(ColliderActor);
			Particle.AttachedBoneName = BoneName;
			Particle.AttachedLocalOffset = BoneTransform.InverseTransformPosition(ParticlePosition);
			Particle.AttachedSurfaceNormal = SurfaceNormal;
		}
		else
		{
			// Continue adhering to same bone: reflect simulation-induced position changes (dripping) in local offset
			Particle.AttachedLocalOffset = BoneTransform.InverseTransformPosition(ParticlePosition);
			Particle.AttachedSurfaceNormal = SurfaceNormal;
		}
	}
	else
	{
		// Unconditionally release adhesion if not near collider
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
