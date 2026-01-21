// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/FluidParticle.h"
#include "FluidCollider.generated.h"

/**
 * 유체 콜라이더 베이스 클래스
 *
 * 유체 입자와 상호작용하는 충돌체의 기본 인터페이스
 */
UCLASS(Abstract, BlueprintType, Blueprintable, ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UFluidCollider : public UActorComponent
{
	GENERATED_BODY()

public:
	UFluidCollider();

	/** 콜라이더 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider")
	bool bColliderEnabled;

	/** 마찰 계수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Friction;

	/** 반발 계수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Restitution;

	/** 접착 가능 여부 */
	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider")
	//bool bAllowAdhesion;

	/** 접착력 배율 */
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

	/** 충돌 형상 캐싱 (프레임당 한 번 호출) */
	virtual void CacheCollisionShapes() {}

	/** 캐시된 바운딩 박스 반환 */
	virtual FBox GetCachedBounds() const { return FBox(ForceInit); }

	/** 캐시가 유효한지 */
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

	/** 최근접점과 함께 해당 본 이름, 본 트랜스폼도 반환 (스켈레탈 메시용) */
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
