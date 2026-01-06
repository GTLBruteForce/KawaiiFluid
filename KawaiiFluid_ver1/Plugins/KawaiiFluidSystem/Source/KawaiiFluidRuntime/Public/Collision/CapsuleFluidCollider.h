// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Collision/FluidCollider.h"
#include "CapsuleFluidCollider.generated.h"

/**
 * Capsule shaped fluid collider
 * Defined by two endpoints and a radius
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UCapsuleFluidCollider : public UFluidCollider
{
	GENERATED_BODY()

public:
	UCapsuleFluidCollider();

	/** Capsule half-height (from center to hemisphere center) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Capsule")
	float HalfHeight;

	/** Capsule radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Capsule")
	float Radius;

	/** Local offset from owner */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Capsule")
	FVector LocalOffset;

	/** Capsule orientation (default: Z-up) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Capsule")
	FRotator LocalRotation;

	virtual bool GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const override;
	virtual bool IsPointInside(const FVector& Point) const override;

	/** Optimized Signed Distance function for capsule */
	virtual float GetSignedDistance(const FVector& Point, FVector& OutGradient) const override;

private:
	/** Get capsule center in world space */
	FVector GetCapsuleCenter() const;

	/** Get capsule endpoints in world space */
	void GetCapsuleEndpoints(FVector& OutStart, FVector& OutEnd) const;

	/** Transform point to capsule local space */
	FVector WorldToLocal(const FVector& WorldPoint) const;
};
