// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Collision/FluidCollider.h"
#include "BoxFluidCollider.generated.h"

/**
 * 박스 형태 유체 콜라이더
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UBoxFluidCollider : public UFluidCollider
{
	GENERATED_BODY()

public:
	UBoxFluidCollider();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Box")
	FVector BoxExtent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Box")
	FVector LocalOffset;

	virtual bool GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const override;
	virtual bool IsPointInside(const FVector& Point) const override;

	/** Optimized Signed Distance function for box */
	virtual float GetSignedDistance(const FVector& Point, FVector& OutGradient) const override;

private:
	FVector WorldToLocal(const FVector& WorldPoint) const;
	FVector LocalToWorld(const FVector& LocalPoint) const;
	FVector GetBoxCenter() const;
};
