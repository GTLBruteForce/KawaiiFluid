// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Collision/FluidCollider.h"
#include "BoxFluidCollider.generated.h"

/**
 * @brief Box-shaped fluid collider.
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
