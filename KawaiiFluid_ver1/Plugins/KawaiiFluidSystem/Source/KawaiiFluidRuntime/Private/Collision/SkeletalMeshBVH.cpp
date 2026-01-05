// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Collision/SkeletalMeshBVH.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Algo/Sort.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSkeletalMeshBVH, Log, All);
DEFINE_LOG_CATEGORY(LogSkeletalMeshBVH);

FSkeletalMeshBVH::FSkeletalMeshBVH()
	: LODIndex(0)
	, VertexCount(0)
	, bIsInitialized(false)
{
}

FSkeletalMeshBVH::~FSkeletalMeshBVH()
{
	Clear();
}

void FSkeletalMeshBVH::Clear()
{
	Nodes.Empty();
	SkinnedTriangles.Empty();
	TriangleIndicesSorted.Empty();
	IndexBuffer.Empty();
	SkelMeshComponent.Reset();
	bIsInitialized = false;
	VertexCount = 0;
}

bool FSkeletalMeshBVH::Initialize(USkeletalMeshComponent* InSkelMesh, int32 InLODIndex)
{
	Clear();

	if (!InSkelMesh)
	{
		UE_LOG(LogSkeletalMeshBVH, Warning, TEXT("Initialize failed: SkelMesh is null"));
		return false;
	}

	USkeletalMesh* SkelMeshAsset = InSkelMesh->GetSkeletalMeshAsset();
	if (!SkelMeshAsset)
	{
		UE_LOG(LogSkeletalMeshBVH, Warning, TEXT("Initialize failed: SkeletalMeshAsset is null"));
		return false;
	}

	SkelMeshComponent = InSkelMesh;
	LODIndex = FMath::Clamp(InLODIndex, 0, SkelMeshAsset->GetLODNum() - 1);

	// Extract triangles from mesh
	if (!ExtractTrianglesFromMesh())
	{
		UE_LOG(LogSkeletalMeshBVH, Warning, TEXT("Initialize failed: Could not extract triangles"));
		Clear();
		return false;
	}

	if (SkinnedTriangles.Num() == 0)
	{
		UE_LOG(LogSkeletalMeshBVH, Warning, TEXT("Initialize failed: No triangles extracted"));
		Clear();
		return false;
	}

	// Build initial triangle positions (bind pose)
	UpdateSkinnedPositions();

	// Build BVH
	TriangleIndicesSorted.SetNum(SkinnedTriangles.Num());
	for (int32 i = 0; i < SkinnedTriangles.Num(); ++i)
	{
		TriangleIndicesSorted[i] = i;
	}

	Nodes.Reserve(SkinnedTriangles.Num() * 2);  // Estimate node count
	BuildBVH(TriangleIndicesSorted, 0, TriangleIndicesSorted.Num());

	bIsInitialized = true;

	UE_LOG(LogSkeletalMeshBVH, Log, TEXT("BVH initialized: %d triangles, %d nodes"),
		SkinnedTriangles.Num(), Nodes.Num());

	return true;
}

bool FSkeletalMeshBVH::ExtractTrianglesFromMesh()
{
	USkeletalMeshComponent* SkelMesh = SkelMeshComponent.Get();
	if (!SkelMesh)
	{
		return false;
	}

	USkeletalMesh* MeshAsset = SkelMesh->GetSkeletalMeshAsset();
	if (!MeshAsset)
	{
		return false;
	}

	FSkeletalMeshRenderData* RenderData = MeshAsset->GetResourceForRendering();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return false;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
	VertexCount = LODData.GetNumVertices();

	// Extract index buffer
	const FRawStaticIndexBuffer16or32Interface* IndexBufferInterface = LODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (!IndexBufferInterface)
	{
		return false;
	}

	const int32 NumIndices = IndexBufferInterface->Num();
	if (NumIndices < 3)
	{
		return false;
	}

	// Copy index buffer
	IndexBuffer.SetNum(NumIndices);
	for (int32 i = 0; i < NumIndices; ++i)
	{
		IndexBuffer[i] = IndexBufferInterface->Get(i);
	}

	// Create triangles
	const int32 NumTriangles = NumIndices / 3;
	SkinnedTriangles.SetNum(NumTriangles);

	for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
	{
		FSkinnedTriangle& Tri = SkinnedTriangles[TriIdx];
		Tri.TriangleIndex = TriIdx;
		Tri.SectionIndex = 0;  // Will be updated if needed

		// Vertex indices will be used in UpdateSkinnedPositions
	}

	return true;
}

void FSkeletalMeshBVH::UpdateSkinnedPositions()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SkeletalMeshBVH_UpdateSkinnedPositions);

	USkeletalMeshComponent* SkelMesh = SkelMeshComponent.Get();
	if (!SkelMesh || SkinnedTriangles.Num() == 0)
	{
		return;
	}

	USkeletalMesh* MeshAsset = SkelMesh->GetSkeletalMeshAsset();
	if (!MeshAsset)
	{
		return;
	}

	FSkeletalMeshRenderData* RenderData = MeshAsset->GetResourceForRendering();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
	FSkinWeightVertexBuffer& SkinWeightBuffer = *const_cast<FSkinWeightVertexBuffer*>(&LODData.SkinWeightVertexBuffer);

	// Get component transform to convert local → world space
	const FTransform ComponentTransform = SkelMesh->GetComponentTransform();

	const int32 NumTriangles = SkinnedTriangles.Num();

	// ParallelFor for performance
	ParallelFor(NumTriangles, [this, SkelMesh, &LODData, &SkinWeightBuffer, &ComponentTransform](int32 TriIdx)
	{
		FSkinnedTriangle& Tri = SkinnedTriangles[TriIdx];

		const int32 BaseIndex = TriIdx * 3;
		const uint32 Idx0 = IndexBuffer[BaseIndex + 0];
		const uint32 Idx1 = IndexBuffer[BaseIndex + 1];
		const uint32 Idx2 = IndexBuffer[BaseIndex + 2];

		// Get skinned vertex positions in component space
		FVector LocalV0 = FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(SkelMesh, Idx0, LODData, SkinWeightBuffer));
		FVector LocalV1 = FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(SkelMesh, Idx1, LODData, SkinWeightBuffer));
		FVector LocalV2 = FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(SkelMesh, Idx2, LODData, SkinWeightBuffer));

		// Transform to world space
		Tri.V0 = ComponentTransform.TransformPosition(LocalV0);
		Tri.V1 = ComponentTransform.TransformPosition(LocalV1);
		Tri.V2 = ComponentTransform.TransformPosition(LocalV2);

		// Compute derived data
		Tri.ComputeDerivedData();
	});

	// Update BVH bounds (bottom-up)
	if (Nodes.Num() > 0)
	{
		// Update from leaves to root (reverse order works for complete trees)
		for (int32 i = Nodes.Num() - 1; i >= 0; --i)
		{
			UpdateNodeBounds(i);
		}
	}
}

void FSkeletalMeshBVH::UpdateNodeBounds(int32 NodeIndex)
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	FBVHNode& Node = Nodes[NodeIndex];

	if (Node.IsLeaf())
	{
		// Leaf node: compute bounds from triangles
		Node.Bounds = FBox(ForceInit);
		const int32 End = Node.TriangleStartIndex + Node.TriangleCount;
		for (int32 i = Node.TriangleStartIndex; i < End; ++i)
		{
			if (TriangleIndicesSorted.IsValidIndex(i))
			{
				const FSkinnedTriangle& Tri = SkinnedTriangles[TriangleIndicesSorted[i]];
				Node.Bounds += Tri.V0;
				Node.Bounds += Tri.V1;
				Node.Bounds += Tri.V2;
			}
		}
	}
	else
	{
		// Internal node: compute bounds from children
		Node.Bounds = FBox(ForceInit);
		if (Nodes.IsValidIndex(Node.LeftChild))
		{
			Node.Bounds += Nodes[Node.LeftChild].Bounds;
		}
		if (Nodes.IsValidIndex(Node.RightChild))
		{
			Node.Bounds += Nodes[Node.RightChild].Bounds;
		}
	}
}

int32 FSkeletalMeshBVH::BuildBVH(TArray<int32>& TriangleIndices, int32 Start, int32 End)
{
	const int32 Count = End - Start;
	const int32 NodeIndex = Nodes.Num();
	Nodes.AddDefaulted();

	if (Count <= 0)
	{
		return NodeIndex;
	}

	// Compute bounds for this node
	FBox Bounds(ForceInit);
	for (int32 i = Start; i < End; ++i)
	{
		const FSkinnedTriangle& Tri = SkinnedTriangles[TriangleIndices[i]];
		Bounds += Tri.V0;
		Bounds += Tri.V1;
		Bounds += Tri.V2;
	}
	Nodes[NodeIndex].Bounds = Bounds;

	// Leaf condition
	if (Count <= LeafTriangleThreshold)
	{
		Nodes[NodeIndex].LeftChild = INDEX_NONE;
		Nodes[NodeIndex].RightChild = INDEX_NONE;
		Nodes[NodeIndex].TriangleStartIndex = Start;
		Nodes[NodeIndex].TriangleCount = Count;
		return NodeIndex;
	}

	// Find best split axis (longest axis)
	const FVector Extent = Bounds.GetExtent();
	int32 SplitAxis = 0;
	if (Extent.Y > Extent.X) SplitAxis = 1;
	if (Extent.Z > Extent[SplitAxis]) SplitAxis = 2;

	// Sort triangles by centroid along split axis
	::Sort(&TriangleIndices[Start], Count, [this, SplitAxis](int32 A, int32 B)
	{
		return SkinnedTriangles[A].Centroid[SplitAxis] < SkinnedTriangles[B].Centroid[SplitAxis];
	});

	// Split at median
	const int32 Mid = Start + Count / 2;

	// Build children recursively
	Nodes[NodeIndex].LeftChild = BuildBVH(TriangleIndices, Start, Mid);
	Nodes[NodeIndex].RightChild = BuildBVH(TriangleIndices, Mid, End);
	Nodes[NodeIndex].TriangleStartIndex = INDEX_NONE;
	Nodes[NodeIndex].TriangleCount = 0;

	return NodeIndex;
}

bool FSkeletalMeshBVH::QueryClosestTriangle(const FVector& Point, float MaxDistance, FTriangleQueryResult& OutResult) const
{
	OutResult = FTriangleQueryResult();

	if (!IsValid())
	{
		return false;
	}

	float BestDistSq = MaxDistance * MaxDistance;
	int32 BestTriangle = INDEX_NONE;

	QueryClosestRecursive(0, Point, BestDistSq, BestTriangle);

	if (BestTriangle != INDEX_NONE)
	{
		const FSkinnedTriangle& Tri = SkinnedTriangles[TriangleIndicesSorted[BestTriangle]];
		OutResult.ClosestPoint = ClosestPointOnTriangle(Point, Tri.V0, Tri.V1, Tri.V2);
		OutResult.Normal = Tri.Normal;
		OutResult.Distance = FMath::Sqrt(BestDistSq);
		OutResult.TriangleIndex = BestTriangle;
		OutResult.bValid = true;
		return true;
	}

	return false;
}

void FSkeletalMeshBVH::QueryClosestRecursive(int32 NodeIndex, const FVector& Point, float& BestDistSq, int32& BestTriangle) const
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	const FBVHNode& Node = Nodes[NodeIndex];

	// Early rejection: check if node bounds can contain a closer point
	const float NodeDistSq = Node.Bounds.ComputeSquaredDistanceToPoint(Point);
	if (NodeDistSq > BestDistSq)
	{
		return;
	}

	if (Node.IsLeaf())
	{
		// Check all triangles in leaf
		const int32 End = Node.TriangleStartIndex + Node.TriangleCount;
		for (int32 i = Node.TriangleStartIndex; i < End; ++i)
		{
			if (!TriangleIndicesSorted.IsValidIndex(i))
			{
				continue;
			}

			const FSkinnedTriangle& Tri = SkinnedTriangles[TriangleIndicesSorted[i]];
			const FVector ClosestPt = ClosestPointOnTriangle(Point, Tri.V0, Tri.V1, Tri.V2);
			const float DistSq = FVector::DistSquared(Point, ClosestPt);

			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				BestTriangle = i;
			}
		}
	}
	else
	{
		// Recurse into children
		// Visit closer child first for better pruning
		const float LeftDistSq = Nodes.IsValidIndex(Node.LeftChild) ?
			Nodes[Node.LeftChild].Bounds.ComputeSquaredDistanceToPoint(Point) : FLT_MAX;
		const float RightDistSq = Nodes.IsValidIndex(Node.RightChild) ?
			Nodes[Node.RightChild].Bounds.ComputeSquaredDistanceToPoint(Point) : FLT_MAX;

		if (LeftDistSq < RightDistSq)
		{
			QueryClosestRecursive(Node.LeftChild, Point, BestDistSq, BestTriangle);
			QueryClosestRecursive(Node.RightChild, Point, BestDistSq, BestTriangle);
		}
		else
		{
			QueryClosestRecursive(Node.RightChild, Point, BestDistSq, BestTriangle);
			QueryClosestRecursive(Node.LeftChild, Point, BestDistSq, BestTriangle);
		}
	}
}

void FSkeletalMeshBVH::QuerySphere(const FVector& Center, float Radius, TArray<int32>& OutTriangleIndices) const
{
	OutTriangleIndices.Reset();

	if (!IsValid())
	{
		return;
	}

	const float RadiusSq = Radius * Radius;
	QuerySphereRecursive(0, Center, RadiusSq, OutTriangleIndices);
}

void FSkeletalMeshBVH::QuerySphereRecursive(int32 NodeIndex, const FVector& Center, float RadiusSq, TArray<int32>& OutTriangleIndices) const
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	const FBVHNode& Node = Nodes[NodeIndex];

	// Check if sphere intersects node bounds
	const float DistSq = Node.Bounds.ComputeSquaredDistanceToPoint(Center);
	if (DistSq > RadiusSq)
	{
		return;
	}

	if (Node.IsLeaf())
	{
		// Add all triangles in leaf (actual intersection test can be done by caller)
		const int32 End = Node.TriangleStartIndex + Node.TriangleCount;
		for (int32 i = Node.TriangleStartIndex; i < End; ++i)
		{
			if (TriangleIndicesSorted.IsValidIndex(i))
			{
				OutTriangleIndices.Add(TriangleIndicesSorted[i]);
			}
		}
	}
	else
	{
		QuerySphereRecursive(Node.LeftChild, Center, RadiusSq, OutTriangleIndices);
		QuerySphereRecursive(Node.RightChild, Center, RadiusSq, OutTriangleIndices);
	}
}

void FSkeletalMeshBVH::QueryAABB(const FBox& AABB, TArray<int32>& OutTriangleIndices) const
{
	OutTriangleIndices.Reset();

	if (!IsValid())
	{
		return;
	}

	QueryAABBRecursive(0, AABB, OutTriangleIndices);
}

void FSkeletalMeshBVH::QueryAABBRecursive(int32 NodeIndex, const FBox& AABB, TArray<int32>& OutTriangleIndices) const
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	const FBVHNode& Node = Nodes[NodeIndex];

	// Check if AABB intersects node bounds
	if (!Node.Bounds.Intersect(AABB))
	{
		return;
	}

	if (Node.IsLeaf())
	{
		// Add all triangles in leaf
		const int32 End = Node.TriangleStartIndex + Node.TriangleCount;
		for (int32 i = Node.TriangleStartIndex; i < End; ++i)
		{
			if (TriangleIndicesSorted.IsValidIndex(i))
			{
				OutTriangleIndices.Add(TriangleIndicesSorted[i]);
			}
		}
	}
	else
	{
		QueryAABBRecursive(Node.LeftChild, AABB, OutTriangleIndices);
		QueryAABBRecursive(Node.RightChild, AABB, OutTriangleIndices);
	}
}

FVector FSkeletalMeshBVH::ClosestPointOnTriangle(const FVector& Point, const FVector& V0, const FVector& V1, const FVector& V2)
{
	// Compute vectors
	const FVector Edge0 = V1 - V0;
	const FVector Edge1 = V2 - V0;
	const FVector V0ToPoint = V0 - Point;

	// Compute dot products
	const float A = FVector::DotProduct(Edge0, Edge0);
	const float B = FVector::DotProduct(Edge0, Edge1);
	const float C = FVector::DotProduct(Edge1, Edge1);
	const float D = FVector::DotProduct(Edge0, V0ToPoint);
	const float E = FVector::DotProduct(Edge1, V0ToPoint);

	// Compute determinant
	const float Det = A * C - B * B;
	float S = B * E - C * D;
	float T = B * D - A * E;

	if (S + T <= Det)
	{
		if (S < 0.0f)
		{
			if (T < 0.0f)
			{
				// Region 4
				if (D < 0.0f)
				{
					S = FMath::Clamp(-D / A, 0.0f, 1.0f);
					T = 0.0f;
				}
				else
				{
					S = 0.0f;
					T = FMath::Clamp(-E / C, 0.0f, 1.0f);
				}
			}
			else
			{
				// Region 3
				S = 0.0f;
				T = FMath::Clamp(-E / C, 0.0f, 1.0f);
			}
		}
		else if (T < 0.0f)
		{
			// Region 5
			S = FMath::Clamp(-D / A, 0.0f, 1.0f);
			T = 0.0f;
		}
		else
		{
			// Region 0 (inside triangle)
			const float InvDet = 1.0f / Det;
			S *= InvDet;
			T *= InvDet;
		}
	}
	else
	{
		if (S < 0.0f)
		{
			// Region 2
			const float Tmp0 = B + D;
			const float Tmp1 = C + E;
			if (Tmp1 > Tmp0)
			{
				const float Numer = Tmp1 - Tmp0;
				const float Denom = A - 2.0f * B + C;
				S = FMath::Clamp(Numer / Denom, 0.0f, 1.0f);
				T = 1.0f - S;
			}
			else
			{
				S = 0.0f;
				T = FMath::Clamp(-E / C, 0.0f, 1.0f);
			}
		}
		else if (T < 0.0f)
		{
			// Region 6
			const float Tmp0 = B + E;
			const float Tmp1 = A + D;
			if (Tmp1 > Tmp0)
			{
				const float Numer = Tmp1 - Tmp0;
				const float Denom = A - 2.0f * B + C;
				T = FMath::Clamp(Numer / Denom, 0.0f, 1.0f);
				S = 1.0f - T;
			}
			else
			{
				T = 0.0f;
				S = FMath::Clamp(-D / A, 0.0f, 1.0f);
			}
		}
		else
		{
			// Region 1
			const float Numer = (C + E) - (B + D);
			if (Numer <= 0.0f)
			{
				S = 0.0f;
			}
			else
			{
				const float Denom = A - 2.0f * B + C;
				S = FMath::Clamp(Numer / Denom, 0.0f, 1.0f);
			}
			T = 1.0f - S;
		}
	}

	return V0 + S * Edge0 + T * Edge1;
}

bool FSkeletalMeshBVH::GetSkinnedVertexPosition(int32 VertexIndex, FVector& OutPosition) const
{
	USkeletalMeshComponent* SkelMesh = SkelMeshComponent.Get();
	if (!SkelMesh || VertexIndex < 0 || VertexIndex >= VertexCount)
	{
		return false;
	}

	USkeletalMesh* MeshAsset = SkelMesh->GetSkeletalMeshAsset();
	if (!MeshAsset)
	{
		return false;
	}

	FSkeletalMeshRenderData* RenderData = MeshAsset->GetResourceForRendering();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return false;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
	FSkinWeightVertexBuffer& SkinWeightBuffer = *const_cast<FSkinWeightVertexBuffer*>(&LODData.SkinWeightVertexBuffer);

	// Get position in component space and transform to world space
	FVector LocalPos = FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(SkelMesh, VertexIndex, LODData, SkinWeightBuffer));
	OutPosition = SkelMesh->GetComponentTransform().TransformPosition(LocalPos);
	return true;
}
