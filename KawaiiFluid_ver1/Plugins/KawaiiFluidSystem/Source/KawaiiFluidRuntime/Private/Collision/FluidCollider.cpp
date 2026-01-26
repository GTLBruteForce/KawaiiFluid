// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Collision/FluidCollider.h"
#include "Async/ParallelFor.h"

UFluidCollider::UFluidCollider()
{
	PrimaryComponentTick.bCanEverTick = false;

	bColliderEnabled = true;
	Friction = 0.3f;
	Restitution = 0.2f;
	//bAllowAdhesion = true;
	//AdhesionMultiplier = 1.0f;
}

void UFluidCollider::BeginPlay()
{
	Super::BeginPlay();
}

void UFluidCollider::ResolveCollisions(TArray<FFluidParticle>& Particles, float SubstepDT)
{
	if (!bColliderEnabled)
	{
		return;
	}

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		ResolveParticleCollision(Particles[i], SubstepDT);
	});
}

bool UFluidCollider::GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const
{
	return false;
}

float UFluidCollider::GetSignedDistance(const FVector& Point, FVector& OutGradient) const
{
	// Default implementation using GetClosestPoint
	FVector ClosestPoint;
	FVector Normal;
	float Distance;

	if (!GetClosestPoint(Point, ClosestPoint, Normal, Distance))
	{
		OutGradient = FVector::UpVector;
		return MAX_FLT;
	}

	OutGradient = Normal;

	// Check if inside (IsPointInside) to return negative distance
	if (IsPointInside(Point))
	{
		return -Distance;
	}

	return Distance;
}

bool UFluidCollider::GetClosestPointWithBone(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance, FName& OutBoneName, FTransform& OutBoneTransform) const
{
	// Default implementation: no bone information
	OutBoneName = NAME_None;
	OutBoneTransform = FTransform::Identity;
	return GetClosestPoint(Point, OutClosestPoint, OutNormal, OutDistance);
}

bool UFluidCollider::IsPointInside(const FVector& Point) const
{
	return false;
}

void UFluidCollider::ResolveParticleCollision(FFluidParticle& Particle, float SubstepDT)
{
	// Use SDF-based collision
	FVector Gradient;
	float SignedDistance = GetSignedDistance(Particle.PredictedPosition, Gradient);

	// Collision margin (particle radius + safety margin)
	const float CollisionMargin = 5.0f;  // 5cm

	// Collision detected if inside or within margin
	if (SignedDistance < CollisionMargin)
	{
		// Push particle to surface + margin
		float Penetration = CollisionMargin - SignedDistance;
		FVector CollisionPos = Particle.PredictedPosition + Gradient * Penetration;

		// Only modify PredictedPosition
		Particle.PredictedPosition = CollisionPos;

		// Calculate desired velocity after collision response
		// Initialize to zero - particle stops on surface by default
		FVector DesiredVelocity = FVector::ZeroVector;
		float VelDotNormal = FVector::DotProduct(Particle.Velocity, Gradient);

		// Minimum velocity threshold for applying restitution bounce
		// Prevents "popcorn" oscillation for particles resting on surfaces
		const float MinBounceVelocity = 50.0f;  // cm/s

		if (VelDotNormal < 0.0f)
		{
			// Particle moving INTO surface - apply collision response
			FVector VelNormal = Gradient * VelDotNormal;
			FVector VelTangent = Particle.Velocity - VelNormal;

			if (VelDotNormal < -MinBounceVelocity)
			{
				// Significant impact - apply full collision response
				// Normal: Restitution (0 = stick, 1 = full bounce)
				// Tangent: Friction (0 = slide, 1 = stop)
				DesiredVelocity = VelTangent * (1.0f - Friction) - VelNormal * Restitution;
			}
			else
			{
				// Low velocity contact (resting on surface) - no bounce, just slide
				DesiredVelocity = VelTangent * (1.0f - Friction);
			}
		}
		// else: VelDotNormal >= 0 means particle moving AWAY from surface
		// DesiredVelocity stays zero - particle stops on surface (same as OLD behavior)

		// Back-calculate Position so FinalizePositions derives DesiredVelocity
		// FinalizePositions: Velocity = (PredictedPosition - Position) / dt
		// Therefore: Position = PredictedPosition - DesiredVelocity * dt
		Particle.Position = Particle.PredictedPosition - DesiredVelocity * SubstepDT;
	}
}
