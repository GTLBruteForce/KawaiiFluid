// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Collision/SphereFluidCollider.h"

USphereFluidCollider::USphereFluidCollider()
{
	Radius = 50.0f;
	LocalOffset = FVector::ZeroVector;
}

bool USphereFluidCollider::GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const
{
	FVector Center = GetSphereCenter();
	FVector ToPoint = Point - Center;
	float DistanceToCenter = ToPoint.Size();

	if (DistanceToCenter < KINDA_SMALL_NUMBER)
	{
		OutNormal = FVector::UpVector;
		OutClosestPoint = Center + OutNormal * Radius;
		OutDistance = Radius;
		return true;
	}

	OutNormal = ToPoint / DistanceToCenter;
	OutClosestPoint = Center + OutNormal * Radius;
	OutDistance = DistanceToCenter - Radius;

	return true;
}

bool USphereFluidCollider::IsPointInside(const FVector& Point) const
{
	FVector Center = GetSphereCenter();
	float DistanceSq = FVector::DistSquared(Point, Center);

	return DistanceSq <= Radius * Radius;
}

float USphereFluidCollider::GetSignedDistance(const FVector& Point, FVector& OutGradient) const
{
	FVector Center = GetSphereCenter();
	FVector ToPoint = Point - Center;
	float DistanceToCenter = ToPoint.Size();

	if (DistanceToCenter < KINDA_SMALL_NUMBER)
	{
		// At center: gradient points up, distance is -Radius (deepest inside)
		OutGradient = FVector::UpVector;
		return -Radius;
	}

	// Gradient always points outward from center
	OutGradient = ToPoint / DistanceToCenter;

	// Signed distance: positive outside, negative inside
	return DistanceToCenter - Radius;
}

FVector USphereFluidCollider::GetSphereCenter() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return LocalOffset;
	}

	return Owner->GetActorLocation() + Owner->GetActorRotation().RotateVector(LocalOffset);
}
