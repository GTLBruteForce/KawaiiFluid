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
	int32 BoneIndex = -1;  // Index for GPU bone transform buffer
};

/** 캐싱된 스피어 데이터 */
struct FCachedSphere
{
	FVector Center;
	float Radius;
	FName BoneName;
	FTransform BoneTransform;
	int32 BoneIndex = -1;  // Index for GPU bone transform buffer
};

/** 캐싱된 박스 데이터 */
struct FCachedBox
{
	FVector Center;
	FVector Extent;  // Half extents (X, Y, Z)
	FQuat Rotation;
	FName BoneName;
	FTransform BoneTransform;
	int32 BoneIndex = -1;  // Index for GPU bone transform buffer
};

/** Convex 평면 데이터 */
struct FCachedConvexPlane
{
	FVector Normal;      // 외부를 향하는 단위 법선
	float Distance;      // 원점에서의 부호 있는 거리
};

/** 캐싱된 Convex Hull 데이터 */
struct FCachedConvex
{
	FVector Center;           // Bounding sphere 중심
	float BoundingRadius;     // Bounding sphere 반경
	TArray<FCachedConvexPlane> Planes;  // Convex 정의하는 평면들
	FName BoneName;
	FTransform BoneTransform;
	int32 BoneIndex = -1;  // Index for GPU bone transform buffer
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

	/** GPU 충돌용 primitive 데이터 내보내기 */
	void ExportToGPUPrimitives(
		TArray<struct FGPUCollisionSphere>& OutSpheres,
		TArray<struct FGPUCollisionCapsule>& OutCapsules,
		TArray<struct FGPUCollisionBox>& OutBoxes,
		TArray<struct FGPUCollisionConvex>& OutConvexes,
		TArray<struct FGPUConvexPlane>& OutPlanes,
		float Friction = 0.1f,
		float Restitution = 0.3f,
		int32 OwnerID = 0  // Unique ID for filtering collision feedback by owner
	) const;

	/** GPU 충돌용 primitive 데이터 내보내기 (본 트랜스폼 포함) */
	void ExportToGPUPrimitivesWithBones(
		TArray<struct FGPUCollisionSphere>& OutSpheres,
		TArray<struct FGPUCollisionCapsule>& OutCapsules,
		TArray<struct FGPUCollisionBox>& OutBoxes,
		TArray<struct FGPUCollisionConvex>& OutConvexes,
		TArray<struct FGPUConvexPlane>& OutPlanes,
		TArray<struct FGPUBoneTransform>& OutBoneTransforms,
		TMap<FName, int32>& BoneNameToIndex,  // Bone name to index mapping (shared across colliders)
		float Friction = 0.1f,
		float Restitution = 0.3f,
		int32 OwnerID = 0  // Unique ID for filtering collision feedback by owner
	) const;

protected:
	virtual void BeginPlay() override;

private:
	void AutoFindMeshComponent();

	// 캐싱된 충돌 형상
	TArray<FCachedCapsule> CachedCapsules;
	TArray<FCachedSphere> CachedSpheres;
	TArray<FCachedBox> CachedBoxes;
	TArray<FCachedConvex> CachedConvexes;
	FBox CachedBounds;
	bool bCacheValid;

	// StaticMesh용 충돌 형상 추출
	void CacheStaticMeshCollision(UStaticMeshComponent* StaticMesh);
	void CacheSkeletalMeshCollision(USkeletalMeshComponent* SkelMesh);
};
