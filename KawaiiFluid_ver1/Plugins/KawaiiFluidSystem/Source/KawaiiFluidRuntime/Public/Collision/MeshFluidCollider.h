// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Collision/FluidCollider.h"
#include "MeshFluidCollider.generated.h"

/** Cached capsule collision data */
struct FCachedCapsule
{
	FVector Start;
	FVector End;
	float Radius;
	FName BoneName;
	FTransform BoneTransform;
	int32 BoneIndex = -1;  // Index for GPU bone transform buffer
};

/** Cached sphere collision data */
struct FCachedSphere
{
	FVector Center;
	float Radius;
	FName BoneName;
	FTransform BoneTransform;
	int32 BoneIndex = -1;  // Index for GPU bone transform buffer
};

/** Cached box collision data */
struct FCachedBox
{
	FVector Center;
	FVector Extent;  // Half extents (X, Y, Z)
	FQuat Rotation;
	FName BoneName;
	FTransform BoneTransform;
	int32 BoneIndex = -1;  // Index for GPU bone transform buffer
};

/** Convex plane data */
struct FCachedConvexPlane
{
	FVector Normal;      // Outward-facing unit normal
	float Distance;      // Signed distance from origin
};

/** Cached convex hull collision data */
struct FCachedConvex
{
	FVector Center;           // Bounding sphere center
	float BoundingRadius;     // Bounding sphere radius
	TArray<FCachedConvexPlane> Planes;  // Planes defining the convex hull
	FName BoneName;
	FTransform BoneTransform;
	int32 BoneIndex = -1;  // Index for GPU bone transform buffer
};

/**
 * @brief Mesh-based fluid collider.
 * @details Handles collision with characters or complex objects using simplified collision shapes.
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UMeshFluidCollider : public UFluidCollider
{
	GENERATED_BODY()

public:
	UMeshFluidCollider();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Collider|Mesh")
	TObjectPtr<UPrimitiveComponent> TargetMeshComponent;

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

	/** Get cached bounding box */
	virtual FBox GetCachedBounds() const override { return CachedBounds; }

	/** Check if cached data is valid */
	virtual bool IsCacheValid() const override { return bCacheValid; }

	/** Export primitive data for GPU collision */
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

	/** Export primitive data for GPU collision (with bone transforms) */
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

	// Cached collision shapes
	TArray<FCachedCapsule> CachedCapsules;
	TArray<FCachedSphere> CachedSpheres;
	TArray<FCachedBox> CachedBoxes;
	TArray<FCachedConvex> CachedConvexes;
	FBox CachedBounds;
	bool bCacheValid;

	// Extract collision shapes from StaticMesh
	void CacheStaticMeshCollision(UStaticMeshComponent* StaticMesh);
	void CacheSkeletalMeshCollision(USkeletalMeshComponent* SkelMesh);
};
