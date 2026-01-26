// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/FluidParticle.h"
#include "FluidCollider.generated.h"

/**
 * @brief Base class for fluid colliders.
 * @details Provides the base interface for collision objects that interact with fluid particles.
 */
UCLASS(Abstract, BlueprintType, Blueprintable, ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UFluidCollider : public UActorComponent
{
	GENERATED_BODY()

public:
	UFluidCollider();

	/** Enable/disable collider */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider")
	bool bColliderEnabled;

	/** Friction coefficient (0 = no friction, 1 = maximum friction) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Friction;

	/** Restitution coefficient (0 = no bounce, 1 = full elastic bounce) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Restitution;

	/** Allow adhesion to this collider */
	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider")
	//bool bAllowAdhesion;

	/** Adhesion force multiplier */
	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider", meta = (ClampMin = "0.0", ClampMax = "2.0", EditCondition = "bAllowAdhesion"))
	//float AdhesionMultiplier;

	UFUNCTION(BlueprintCallable, Category = "Fluid Collider")
	bool IsColliderEnabled() const { return bColliderEnabled; }

	/**
	 * Resolve collisions for all particles
	 * @param Particles - Particle array
	 * @param SubstepDT - Substep delta time (for Position back-calculation)
	 */
	virtual void ResolveCollisions(TArray<FFluidParticle>& Particles, float SubstepDT);

	/** Cache collision shapes (called once per frame) */
	virtual void CacheCollisionShapes() {}

	/** Get cached bounding box */
	virtual FBox GetCachedBounds() const { return FBox(ForceInit); }

	/** Check if cached data is valid */
	virtual bool IsCacheValid() const { return false; }

	UFUNCTION(BlueprintCallable, Category = "Fluid Collider")
	virtual bool GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const;

	/**
	 * Get Signed Distance to the collider surface
	 * Positive = outside, Negative = inside, Zero = on surface
	 * @param Point - Query point in world space
	 * @param OutGradient - Gradient (surface normal pointing outward)
	 * @return Signed distance to surface
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Collider")
	virtual float GetSignedDistance(const FVector& Point, FVector& OutGradient) const;

	/** Get closest point along with bone name and transform (for skeletal mesh colliders) */
	UFUNCTION(BlueprintCallable, Category = "Fluid Collider")
	virtual bool GetClosestPointWithBone(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance, FName& OutBoneName, FTransform& OutBoneTransform) const;

	UFUNCTION(BlueprintCallable, Category = "Fluid Collider")
	virtual bool IsPointInside(const FVector& Point) const;

protected:
	virtual void BeginPlay() override;

	/**
	 * Resolve collision for a single particle using SDF
	 * @param Particle - Particle to resolve
	 * @param SubstepDT - Substep delta time (for Position back-calculation)
	 */
	virtual void ResolveParticleCollision(FFluidParticle& Particle, float SubstepDT);
};
