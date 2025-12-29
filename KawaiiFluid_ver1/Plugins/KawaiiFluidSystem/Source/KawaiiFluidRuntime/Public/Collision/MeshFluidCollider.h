// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Collision/FluidCollider.h"
#include "MeshFluidCollider.generated.h"

/** 캐싱된 캡슐 데이터 */
struct FCachedCapsule
{
	FVector Start;
	FVector End;
	float Radius;
	FName BoneName;
	FTransform BoneTransform;
};

/** 캐싱된 스피어 데이터 */
struct FCachedSphere
{
	FVector Center;
	float Radius;
	FName BoneName;
	FTransform BoneTransform;
};

/** 캐싱된 박스 데이터 */
struct FCachedBox
{
	FVector Center;
	FVector Extent;  // Half extents (X, Y, Z)
	FQuat Rotation;
	FName BoneName;
	FTransform BoneTransform;
};

/**
 * 메시 기반 유체 콜라이더
 * 캐릭터나 복잡한 형태의 오브젝트와 상호작용
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UMeshFluidCollider : public UFluidCollider
{
	GENERATED_BODY()

public:
	UMeshFluidCollider();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Mesh")
	UPrimitiveComponent* TargetMeshComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Mesh")
	bool bAutoFindMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Mesh")
	bool bUseSimplifiedCollision;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Mesh")
	float CollisionMargin;

	virtual bool GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const override;
	virtual bool GetClosestPointWithBone(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance, FName& OutBoneName, FTransform& OutBoneTransform) const override;
	virtual bool IsPointInside(const FVector& Point) const override;
	virtual void CacheCollisionShapes() override;

	/** 캐시된 바운딩 박스 반환 */
	virtual FBox GetCachedBounds() const override { return CachedBounds; }

	/** 캐시가 유효한지 */
	virtual bool IsCacheValid() const override { return bCacheValid; }

protected:
	virtual void BeginPlay() override;

private:
	void AutoFindMeshComponent();

	// 캐싱된 충돌 형상
	TArray<FCachedCapsule> CachedCapsules;
	TArray<FCachedSphere> CachedSpheres;
	TArray<FCachedBox> CachedBoxes;
	FBox CachedBounds;
	bool bCacheValid;
};
