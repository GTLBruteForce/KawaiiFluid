// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Collision/CapsuleFluidCollider.h"

UCapsuleFluidCollider::UCapsuleFluidCollider()
{
	HalfHeight = 50.0f;
	Radius = 25.0f;
	LocalOffset = FVector::ZeroVector;
	LocalRotation = FRotator::ZeroRotator;
}

bool UCapsuleFluidCollider::GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const
{
	FVector Start, End;
	GetCapsuleEndpoints(Start, End);

	// Find closest point on line segment
	FVector ClosestOnLine = FMath::ClosestPointOnSegment(Point, Start, End);

	FVector ToPoint = Point - ClosestOnLine;
	float DistanceToLine = ToPoint.Size();

	if (DistanceToLine < KINDA_SMALL_NUMBER)
	{
		// Point is on the capsule axis
		OutNormal = FVector::UpVector;
		OutClosestPoint = ClosestOnLine + OutNormal * Radius;
		OutDistance = Radius;
		return true;
	}

	OutNormal = ToPoint / DistanceToLine;
	OutClosestPoint = ClosestOnLine + OutNormal * Radius;
	OutDistance = DistanceToLine - Radius;

	return true;
}

bool UCapsuleFluidCollider::IsPointInside(const FVector& Point) const
{
	FVector Start, End;
	GetCapsuleEndpoints(Start, End);

	FVector ClosestOnLine = FMath::ClosestPointOnSegment(Point, Start, End);
	float DistanceSq = FVector::DistSquared(Point, ClosestOnLine);

	return DistanceSq <= Radius * Radius;
}

float UCapsuleFluidCollider::GetSignedDistance(const FVector& Point, FVector& OutGradient) const
{
	FVector Start, End;
	GetCapsuleEndpoints(Start, End);

	// Capsule SDF: distance to line segment minus radius
	FVector ClosestOnLine = FMath::ClosestPointOnSegment(Point, Start, End);

	FVector ToPoint = Point - ClosestOnLine;
	float DistanceToLine = ToPoint.Size();

	if (DistanceToLine < KINDA_SMALL_NUMBER)
	{
		// Point is on the capsule axis
		// Gradient points in arbitrary perpendicular direction
		FVector CapsuleDir = (End - Start).GetSafeNormal();
		OutGradient = FVector::CrossProduct(CapsuleDir, FVector::UpVector);
		if (OutGradient.SizeSquared() < KINDA_SMALL_NUMBER)
		{
			OutGradient = FVector::CrossProduct(CapsuleDir, FVector::RightVector);
		}
		OutGradient.Normalize();
		return -Radius;  // Deepest inside
	}

	// Gradient points from line to point (outward)
	OutGradient = ToPoint / DistanceToLine;

	// Signed distance: positive outside, negative inside
	return DistanceToLine - Radius;
}

FVector UCapsuleFluidCollider::GetCapsuleCenter() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return LocalOffset;
	}

	return Owner->GetActorLocation() + Owner->GetActorRotation().RotateVector(LocalOffset);
}

void UCapsuleFluidCollider::GetCapsuleEndpoints(FVector& OutStart, FVector& OutEnd) const
{
	AActor* Owner = GetOwner();
	FVector Center = GetCapsuleCenter();

	// Default capsule direction is Z-up
	FVector LocalUp = FVector::UpVector;

	if (Owner)
	{
		// Apply local rotation then owner rotation
		FQuat CombinedQuat = Owner->GetActorQuat() * LocalRotation.Quaternion();
		LocalUp = CombinedQuat.RotateVector(FVector::UpVector);
	}
	else
	{
		LocalUp = LocalRotation.Quaternion().RotateVector(FVector::UpVector);
	}

	OutStart = Center - LocalUp * HalfHeight;
	OutEnd = Center + LocalUp * HalfHeight;
}

FVector UCapsuleFluidCollider::WorldToLocal(const FVector& WorldPoint) const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return WorldPoint - LocalOffset;
	}

	FVector Center = GetCapsuleCenter();
	FVector RelativePoint = WorldPoint - Center;

	FQuat CombinedQuat = Owner->GetActorQuat() * LocalRotation.Quaternion();
	return CombinedQuat.UnrotateVector(RelativePoint);
}
