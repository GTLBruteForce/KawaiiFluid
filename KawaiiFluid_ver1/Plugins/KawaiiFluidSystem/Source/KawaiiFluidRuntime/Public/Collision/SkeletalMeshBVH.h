// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"

/**
 * Skinned Triangle Data
 * Represents a single triangle from the skeletal mesh with skinned vertex positions
 */
struct FSkinnedTriangle
{
	FVector V0;                   // Skinned vertex 0 (world space)
	FVector V1;                   // Skinned vertex 1 (world space)
	FVector V2;                   // Skinned vertex 2 (world space)
	FVector Normal;               // Triangle normal (computed from vertices)
	FVector Centroid;             // Triangle center for BVH sorting
	int32 TriangleIndex;          // Original triangle index in mesh
	int32 SectionIndex;           // LOD section index

	FSkinnedTriangle()
		: V0(FVector::ZeroVector)
		, V1(FVector::ZeroVector)
		, V2(FVector::ZeroVector)
		, Normal(FVector::UpVector)
		, Centroid(FVector::ZeroVector)
		, TriangleIndex(INDEX_NONE)
		, SectionIndex(0)
	{
	}

	/** Compute normal and centroid from vertices */
	void ComputeDerivedData()
	{
		Centroid = (V0 + V1 + V2) / 3.0f;
		// UE5 스켈레탈 메쉬는 CW 와인딩 → Edge2 x Edge1로 바깥 방향 법선
		FVector Edge1 = V1 - V0;
		FVector Edge2 = V2 - V0;
		Normal = FVector::CrossProduct(Edge2, Edge1).GetSafeNormal();  // 순서 뒤집음!
	}

	/** Get AABB bounds of this triangle */
	FBox GetBounds() const
	{
		FBox Bounds(ForceInit);
		Bounds += V0;
		Bounds += V1;
		Bounds += V2;
		return Bounds;
	}
};

/**
 * BVH Node
 * Binary tree node for spatial partitioning
 */
struct FBVHNode
{
	FBox Bounds;                  // AABB bounding box
	int32 LeftChild;              // Left child index (-1 = leaf)
	int32 RightChild;             // Right child index (-1 = leaf)
	int32 TriangleStartIndex;     // For leaf: start index in sorted triangle array
	int32 TriangleCount;          // For leaf: number of triangles

	FBVHNode()
		: Bounds(ForceInit)
		, LeftChild(INDEX_NONE)
		, RightChild(INDEX_NONE)
		, TriangleStartIndex(INDEX_NONE)
		, TriangleCount(0)
	{
	}

	/** Is this a leaf node? */
	bool IsLeaf() const { return LeftChild == INDEX_NONE && RightChild == INDEX_NONE; }
};

/**
 * Triangle Query Result
 * Result of a closest point query
 */
struct FTriangleQueryResult
{
	FVector ClosestPoint;         // Closest point on triangle surface
	FVector Normal;               // Triangle normal at closest point
	float Distance;               // Distance from query point to closest point
	int32 TriangleIndex;          // Index of the triangle
	bool bValid;                  // Whether result is valid

	FTriangleQueryResult()
		: ClosestPoint(FVector::ZeroVector)
		, Normal(FVector::UpVector)
		, Distance(FLT_MAX)
		, TriangleIndex(INDEX_NONE)
		, bValid(false)
	{
	}
};

/**
 * Skeletal Mesh BVH
 * Bounding Volume Hierarchy for efficient triangle queries on skinned meshes
 *
 * Usage:
 * 1. Initialize() - Build BVH from skeletal mesh (once)
 * 2. UpdateSkinnedPositions() - Update vertex positions each frame
 * 3. QueryClosestTriangle() / QuerySphere() - Query triangles
 */
class KAWAIIFLUIDRUNTIME_API FSkeletalMeshBVH
{
public:
	FSkeletalMeshBVH();
	~FSkeletalMeshBVH();

	/**
	 * Initialize BVH from skeletal mesh component
	 * @param InSkelMesh - Target skeletal mesh component
	 * @param InLODIndex - LOD level to use (0 = highest detail)
	 * @return True if initialization succeeded
	 */
	bool Initialize(USkeletalMeshComponent* InSkelMesh, int32 InLODIndex = 0);

	/**
	 * Update skinned vertex positions
	 * Call this every frame before querying
	 */
	void UpdateSkinnedPositions();

	/**
	 * Query closest triangle to a point
	 * @param Point - Query point (world space)
	 * @param MaxDistance - Maximum search distance
	 * @param OutResult - Query result
	 * @return True if a triangle was found within MaxDistance
	 */
	bool QueryClosestTriangle(const FVector& Point, float MaxDistance, FTriangleQueryResult& OutResult) const;

	/**
	 * Query all triangles within a sphere
	 * @param Center - Sphere center (world space)
	 * @param Radius - Sphere radius
	 * @param OutTriangleIndices - Indices of triangles that may intersect the sphere
	 */
	void QuerySphere(const FVector& Center, float Radius, TArray<int32>& OutTriangleIndices) const;

	/**
	 * Query all triangles within an AABB
	 * @param AABB - Axis-aligned bounding box (world space)
	 * @param OutTriangleIndices - Indices of triangles that may intersect the AABB
	 */
	void QueryAABB(const FBox& AABB, TArray<int32>& OutTriangleIndices) const;

	/** Is BVH valid and ready for queries? */
	bool IsValid() const { return bIsInitialized && Nodes.Num() > 0; }

	/** Get number of triangles */
	int32 GetTriangleCount() const { return SkinnedTriangles.Num(); }

	/** Get number of BVH nodes */
	int32 GetNodeCount() const { return Nodes.Num(); }

	/** Get all triangles (for direct access) */
	const TArray<FSkinnedTriangle>& GetTriangles() const { return SkinnedTriangles; }

	/** Get triangle by index */
	const FSkinnedTriangle& GetTriangle(int32 Index) const { return SkinnedTriangles[TriangleIndicesSorted[Index]]; }

	/** Get root bounds */
	FBox GetRootBounds() const { return Nodes.Num() > 0 ? Nodes[0].Bounds : FBox(ForceInit); }

	/** Get associated skeletal mesh component */
	USkeletalMeshComponent* GetSkeletalMeshComponent() const { return SkelMeshComponent.Get(); }

	/** Clear all data */
	void Clear();

	/** Compute closest point on a triangle */
	static FVector ClosestPointOnTriangle(const FVector& Point, const FVector& V0, const FVector& V1, const FVector& V2);

private:
	/**
	 * Build BVH tree recursively (SAH-based)
	 * @param TriangleIndices - Array of triangle indices to partition
	 * @param Start - Start index in TriangleIndices
	 * @param End - End index in TriangleIndices (exclusive)
	 * @return Index of created node
	 */
	int32 BuildBVH(TArray<int32>& TriangleIndices, int32 Start, int32 End);

	/**
	 * Update node bounds recursively (bottom-up)
	 * @param NodeIndex - Node to update
	 */
	void UpdateNodeBounds(int32 NodeIndex);

	/**
	 * Query BVH recursively for sphere intersection
	 */
	void QuerySphereRecursive(int32 NodeIndex, const FVector& Center, float RadiusSq, TArray<int32>& OutTriangleIndices) const;

	/**
	 * Query BVH recursively for AABB intersection
	 */
	void QueryAABBRecursive(int32 NodeIndex, const FBox& AABB, TArray<int32>& OutTriangleIndices) const;

	/**
	 * Query BVH recursively for closest triangle
	 */
	void QueryClosestRecursive(int32 NodeIndex, const FVector& Point, float& BestDistSq, int32& BestTriangle) const;

	/**
	 * Extract triangles from skeletal mesh render data
	 */
	bool ExtractTrianglesFromMesh();

	/**
	 * Get skinned vertex position from skeletal mesh
	 * @param VertexIndex - Index in the vertex buffer
	 * @param OutPosition - Output position (world space)
	 * @return True if successful
	 */
	bool GetSkinnedVertexPosition(int32 VertexIndex, FVector& OutPosition) const;

private:
	// Skeletal mesh reference
	TWeakObjectPtr<USkeletalMeshComponent> SkelMeshComponent;

	// BVH structure
	TArray<FBVHNode> Nodes;
	TArray<FSkinnedTriangle> SkinnedTriangles;
	TArray<int32> TriangleIndicesSorted;  // Triangle indices in BVH leaf order

	// Original mesh data (for skinning)
	TArray<uint32> IndexBuffer;           // Original index buffer
	int32 LODIndex;
	int32 VertexCount;

	// State
	bool bIsInitialized;

	// Constants
	static constexpr int32 LeafTriangleThreshold = 4;  // Max triangles per leaf
	static constexpr int32 MaxTreeDepth = 32;          // Maximum BVH depth
};
