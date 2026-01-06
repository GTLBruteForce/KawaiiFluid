// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Collision/FluidCollider.h"
#include "SphereFluidCollider.generated.h"

/**
 * 구체 형태 유체 콜라이더
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API USphereFluidCollider : public UFluidCollider
{
	GENERATED_BODY()

public:
	USphereFluidCollider();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Sphere")
	float Radius;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Sphere")
	FVector LocalOffset;

	virtual bool GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const override;
	virtual bool IsPointInside(const FVector& Point) const override;

	/** Optimized Signed Distance function for sphere */
	virtual float GetSignedDistance(const FVector& Point, FVector& OutGradient) const override;

private:
	FVector GetSphereCenter() const;
};
