// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Collision/MeshFluidCollider.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/StaticMesh.h"
#include "GPU/GPUFluidParticle.h"

UMeshFluidCollider::UMeshFluidCollider()
{
	TargetMeshComponent = nullptr;
	bAutoFindMesh = true;
	bUseSimplifiedCollision = true;
	CollisionMargin = 1.0f;
	bCacheValid = false;
	CachedBounds = FBox(ForceInit);
}

void UMeshFluidCollider::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoFindMesh)
	{
		AutoFindMeshComponent();
	}
}

void UMeshFluidCollider::AutoFindMeshComponent()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Priority 1: SkeletalMeshComponent (precise collision via PhysicsAsset)
	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (SkelMesh)
	{
		TargetMeshComponent = SkelMesh;

		// Debug: log PhysicsAsset info
		UPhysicsAsset* PhysAsset = SkelMesh->GetPhysicsAsset();
		if (PhysAsset)
		{
			int32 TotalCapsules = 0;
			int32 TotalSpheres = 0;
			int32 TotalBoxes = 0;
			for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
			{
				if (BodySetup)
				{
					TotalCapsules += BodySetup->AggGeom.SphylElems.Num();
					TotalSpheres += BodySetup->AggGeom.SphereElems.Num();
					TotalBoxes += BodySetup->AggGeom.BoxElems.Num();
				}
			}
			UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: PhysicsAsset '%s' - Capsules: %d, Spheres: %d, Boxes: %d"),
				*PhysAsset->GetName(), TotalCapsules, TotalSpheres, TotalBoxes);
		}
		else
		{
			//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: Found SkeletalMesh but NO PhysicsAsset!"));
		}
		return;
	}

	// Priority 2: CapsuleComponent (simple capsule collision)
	UCapsuleComponent* Capsule = Owner->FindComponentByClass<UCapsuleComponent>();
	if (Capsule)
	{
		TargetMeshComponent = Capsule;
		//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: Using CapsuleComponent"));
		return;
	}

	// Priority 3: StaticMeshComponent
	UStaticMeshComponent* StaticMesh = Owner->FindComponentByClass<UStaticMeshComponent>();
	if (StaticMesh)
	{
		TargetMeshComponent = StaticMesh;
		//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: Using StaticMeshComponent"));
		return;
	}

	//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: No mesh component found!"));
}

void UMeshFluidCollider::CacheCollisionShapes()
{
	CachedCapsules.Reset();
	CachedSpheres.Reset();
	CachedBoxes.Reset();
	CachedConvexes.Reset();
	CachedBounds = FBox(ForceInit);
	bCacheValid = false;

	if (!TargetMeshComponent)
	{
		return;
	}

	// Handle CapsuleComponent
	UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(TargetMeshComponent);
	if (Capsule)
	{
		FVector CapsuleCenter = Capsule->GetComponentLocation();
		float CapsuleRadius = Capsule->GetScaledCapsuleRadius() + CollisionMargin;
		float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		FVector CapsuleUp = Capsule->GetUpVector();

		float HalfLength = CapsuleHalfHeight - CapsuleRadius;
		FCachedCapsule CachedCap;
		CachedCap.Start = CapsuleCenter - CapsuleUp * HalfLength;
		CachedCap.End = CapsuleCenter + CapsuleUp * HalfLength;
		CachedCap.Radius = CapsuleRadius;
		CachedCapsules.Add(CachedCap);

		CachedBounds += CachedCap.Start;
		CachedBounds += CachedCap.End;
		CachedBounds = CachedBounds.ExpandBy(CapsuleRadius);
		bCacheValid = true;
		return;
	}

	// Handle SkeletalMeshComponent - use PhysicsAsset
	USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(TargetMeshComponent);
	if (SkelMesh)
	{
		CacheSkeletalMeshCollision(SkelMesh);
		if (bCacheValid)
		{
			return;
		}
	}

	// Handle StaticMeshComponent - use Simple Collision
	UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(TargetMeshComponent);
	if (StaticMesh)
	{
		CacheStaticMeshCollision(StaticMesh);
		if (bCacheValid)
		{
			return;
		}
	}

	// Fallback: use bounding box
	FBoxSphereBounds Bounds = TargetMeshComponent->Bounds;
	CachedBounds = Bounds.GetBox();
	bCacheValid = true;
}

void UMeshFluidCollider::CacheSkeletalMeshCollision(USkeletalMeshComponent* SkelMesh)
{
	UPhysicsAsset* PhysAsset = SkelMesh->GetPhysicsAsset();
	if (!PhysAsset)
	{
		return;
	}

	for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
	{
		if (!BodySetup)
		{
			continue;
		}

		int32 BoneIndex = SkelMesh->GetBoneIndex(BodySetup->BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		FTransform BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);

		// Cache capsules
		for (const FKSphylElem& SphylElem : BodySetup->AggGeom.SphylElems)
		{
			FTransform CapsuleLocalTransform = SphylElem.GetTransform();
			FTransform CapsuleWorldTransform = CapsuleLocalTransform * BoneTransform;

			FVector CapsuleCenter = CapsuleWorldTransform.GetLocation();
			float CapsuleRadius = SphylElem.Radius + CollisionMargin;
			float CapsuleLength = SphylElem.Length;
			FVector CapsuleUp = CapsuleWorldTransform.GetRotation().GetUpVector();

			float HalfLength = CapsuleLength * 0.5f;

			FCachedCapsule CachedCap;
			CachedCap.Start = CapsuleCenter - CapsuleUp * HalfLength;
			CachedCap.End = CapsuleCenter + CapsuleUp * HalfLength;
			CachedCap.Radius = CapsuleRadius;
			CachedCap.BoneName = BodySetup->BoneName;
			CachedCap.BoneTransform = BoneTransform;
			CachedCap.BoneIndex = BoneIndex;  // Set bone index

			// [Debug] Verify BoneIndex at cache time
			int32 CachedIndex = CachedCapsules.Num();
			//UE_LOG(LogTemp, Warning, TEXT("[CapsuleCache] Idx=%d, BoneName='%s', BoneIndex=%d"),CachedIndex, *BodySetup->BoneName.ToString(), BoneIndex);

			CachedCapsules.Add(CachedCap);

			CachedBounds += CachedCap.Start;
			CachedBounds += CachedCap.End;
		}

		// Cache spheres
		for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
		{
			FTransform SphereLocalTransform = SphereElem.GetTransform();
			FTransform SphereWorldTransform = SphereLocalTransform * BoneTransform;

			FCachedSphere CachedSph;
			CachedSph.Center = SphereWorldTransform.GetLocation();
			CachedSph.Radius = SphereElem.Radius + CollisionMargin;
			CachedSph.BoneName = BodySetup->BoneName;
			CachedSph.BoneTransform = BoneTransform;
			CachedSph.BoneIndex = BoneIndex;  // Set bone index
			CachedSpheres.Add(CachedSph);

			CachedBounds += CachedSph.Center;
		}

		// Cache boxes
		for (const FKBoxElem& BoxElem : BodySetup->AggGeom.BoxElems)
		{
			FTransform BoxLocalTransform = BoxElem.GetTransform();
			FTransform BoxWorldTransform = BoxLocalTransform * BoneTransform;

			FCachedBox CachedBx;
			CachedBx.Center = BoxWorldTransform.GetLocation();
			CachedBx.Extent = FVector(BoxElem.X * 0.5f + CollisionMargin,
			                          BoxElem.Y * 0.5f + CollisionMargin,
			                          BoxElem.Z * 0.5f + CollisionMargin);
			CachedBx.Rotation = BoxWorldTransform.GetRotation();
			CachedBx.BoneName = BodySetup->BoneName;
			CachedBx.BoneTransform = BoneTransform;
			CachedBx.BoneIndex = BoneIndex;  // Set bone index
			CachedBoxes.Add(CachedBx);

			// Add 8 box corners to expand bounding box
			for (int32 CornerIdx = 0; CornerIdx < 8; ++CornerIdx)
			{
				FVector LocalCorner(
					(CornerIdx & 1) ? CachedBx.Extent.X : -CachedBx.Extent.X,
					(CornerIdx & 2) ? CachedBx.Extent.Y : -CachedBx.Extent.Y,
					(CornerIdx & 4) ? CachedBx.Extent.Z : -CachedBx.Extent.Z
				);
				FVector WorldCorner = CachedBx.Center + CachedBx.Rotation.RotateVector(LocalCorner);
				CachedBounds += WorldCorner;
			}
		}

		// Cache convex hulls
		for (const FKConvexElem& ConvexElem : BodySetup->AggGeom.ConvexElems)
		{
			FCachedConvex CachedCvx;
			CachedCvx.BoneName = BodySetup->BoneName;
			CachedCvx.BoneTransform = BoneTransform;
			CachedCvx.BoneIndex = BoneIndex;  // Set bone index

			// Extract planes from vertices (Convex Hull)
			const TArray<FVector>& VertexData = ConvexElem.VertexData;
			if (VertexData.Num() < 4)
			{
				continue;  // Need at least 4 vertices
			}

			// Transform vertices to world space
			TArray<FVector> WorldVerts;
			WorldVerts.Reserve(VertexData.Num());
			FVector CenterSum = FVector::ZeroVector;
			for (const FVector& V : VertexData)
			{
				FVector WorldV = BoneTransform.TransformPosition(V);
				WorldVerts.Add(WorldV);
				CenterSum += WorldV;
			}

			CachedCvx.Center = CenterSum / static_cast<float>(WorldVerts.Num());

			// Bounding radius 계산
			float MaxDistSq = 0.0f;
			for (const FVector& V : WorldVerts)
			{
				float DistSq = FVector::DistSquared(V, CachedCvx.Center);
				MaxDistSq = FMath::Max(MaxDistSq, DistSq);
				CachedBounds += V;
			}
			CachedCvx.BoundingRadius = FMath::Sqrt(MaxDistSq) + CollisionMargin;

			// Generate planes from index data
			const TArray<int32>& IndexData = ConvexElem.IndexData;
			if (IndexData.Num() >= 3)
			{
				// Create a plane for each triangle (with duplicate removal)
				TSet<uint32> PlaneHashes;
				for (int32 i = 0; i + 2 < IndexData.Num(); i += 3)
				{
					int32 I0 = IndexData[i];
					int32 I1 = IndexData[i + 1];
					int32 I2 = IndexData[i + 2];

					if (I0 < WorldVerts.Num() && I1 < WorldVerts.Num() && I2 < WorldVerts.Num())
					{
						FVector V0 = WorldVerts[I0];
						FVector V1 = WorldVerts[I1];
						FVector V2 = WorldVerts[I2];

						FVector Edge1 = V1 - V0;
						FVector Edge2 = V2 - V0;
						FVector Normal = FVector::CrossProduct(Edge1, Edge2);
						float NormalLen = Normal.Size();

						if (NormalLen > KINDA_SMALL_NUMBER)
						{
							Normal /= NormalLen;

							// Adjust normal to face outward
							FVector ToCenter = CachedCvx.Center - V0;
							if (FVector::DotProduct(Normal, ToCenter) > 0)
							{
								Normal = -Normal;
							}

							// Remove duplicates using hash (quantized normal)
							int32 Nx = FMath::RoundToInt(Normal.X * 1000.0f);
							int32 Ny = FMath::RoundToInt(Normal.Y * 1000.0f);
							int32 Nz = FMath::RoundToInt(Normal.Z * 1000.0f);
							uint32 Hash = HashCombine(HashCombine(GetTypeHash(Nx), GetTypeHash(Ny)), GetTypeHash(Nz));

							if (!PlaneHashes.Contains(Hash))
							{
								PlaneHashes.Add(Hash);

								FCachedConvexPlane Plane;
								Plane.Normal = Normal;
								Plane.Distance = FVector::DotProduct(V0, Normal);
								CachedCvx.Planes.Add(Plane);
							}
						}
					}
				}
			}

			if (CachedCvx.Planes.Num() >= 4)
			{
				CachedConvexes.Add(MoveTemp(CachedCvx));
			}
		}
	}

	if (CachedCapsules.Num() > 0 || CachedSpheres.Num() > 0 || CachedBoxes.Num() > 0 || CachedConvexes.Num() > 0)
	{
		// Expand bounding box by largest radius
		float MaxRadius = CollisionMargin;
		for (const FCachedCapsule& Cap : CachedCapsules)
		{
			MaxRadius = FMath::Max(MaxRadius, Cap.Radius);
		}
		for (const FCachedSphere& Sph : CachedSpheres)
		{
			MaxRadius = FMath::Max(MaxRadius, Sph.Radius);
		}
		for (const FCachedBox& Box : CachedBoxes)
		{
			MaxRadius = FMath::Max(MaxRadius, Box.Extent.GetMax());
		}
		for (const FCachedConvex& Cvx : CachedConvexes)
		{
			MaxRadius = FMath::Max(MaxRadius, Cvx.BoundingRadius);
		}
		CachedBounds = CachedBounds.ExpandBy(MaxRadius);
		bCacheValid = true;
	}
}

void UMeshFluidCollider::CacheStaticMeshCollision(UStaticMeshComponent* StaticMesh)
{
	UStaticMesh* Mesh = StaticMesh->GetStaticMesh();
	if (!Mesh)
	{
		return;
	}

	UBodySetup* BodySetup = Mesh->GetBodySetup();
	if (!BodySetup)
	{
		return;
	}

	FTransform ComponentTransform = StaticMesh->GetComponentTransform();

	// Cache spheres
	for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
	{
		FTransform SphereLocalTransform = SphereElem.GetTransform();
		FTransform SphereWorldTransform = SphereLocalTransform * ComponentTransform;

		FCachedSphere CachedSph;
		CachedSph.Center = SphereWorldTransform.GetLocation();
		CachedSph.Radius = SphereElem.Radius * ComponentTransform.GetScale3D().GetMax() + CollisionMargin;
		CachedSph.BoneName = NAME_None;
		CachedSph.BoneTransform = ComponentTransform;
		CachedSpheres.Add(CachedSph);

		CachedBounds += CachedSph.Center;
	}

	// Cache capsules
	for (const FKSphylElem& SphylElem : BodySetup->AggGeom.SphylElems)
	{
		FTransform CapsuleLocalTransform = SphylElem.GetTransform();
		FTransform CapsuleWorldTransform = CapsuleLocalTransform * ComponentTransform;

		FVector CapsuleCenter = CapsuleWorldTransform.GetLocation();
		FVector3d Scale = ComponentTransform.GetScale3D();
		float ScaledRadius = SphylElem.Radius * FMath::Max(Scale.X, Scale.Y) + CollisionMargin;
		float ScaledLength = SphylElem.Length * Scale.Z;
		FVector CapsuleUp = CapsuleWorldTransform.GetRotation().GetUpVector();

		float HalfLength = ScaledLength * 0.5f;

		FCachedCapsule CachedCap;
		CachedCap.Start = CapsuleCenter - CapsuleUp * HalfLength;
		CachedCap.End = CapsuleCenter + CapsuleUp * HalfLength;
		CachedCap.Radius = ScaledRadius;
		CachedCap.BoneName = NAME_None;
		CachedCap.BoneTransform = ComponentTransform;
		CachedCapsules.Add(CachedCap);

		CachedBounds += CachedCap.Start;
		CachedBounds += CachedCap.End;
	}

	// Cache boxes
	for (const FKBoxElem& BoxElem : BodySetup->AggGeom.BoxElems)
	{
		FTransform BoxLocalTransform = BoxElem.GetTransform();
		FTransform BoxWorldTransform = BoxLocalTransform * ComponentTransform;

		FVector3d Scale = ComponentTransform.GetScale3D();

		FCachedBox CachedBx;
		CachedBx.Center = BoxWorldTransform.GetLocation();
		CachedBx.Extent = FVector(BoxElem.X * 0.5f * Scale.X + CollisionMargin,
		                          BoxElem.Y * 0.5f * Scale.Y + CollisionMargin,
		                          BoxElem.Z * 0.5f * Scale.Z + CollisionMargin);
		CachedBx.Rotation = BoxWorldTransform.GetRotation();
		CachedBx.BoneName = NAME_None;
		CachedBx.BoneTransform = ComponentTransform;
		CachedBoxes.Add(CachedBx);

		// Add 8 box corners
		for (int32 CornerIdx = 0; CornerIdx < 8; ++CornerIdx)
		{
			FVector LocalCorner(
				(CornerIdx & 1) ? CachedBx.Extent.X : -CachedBx.Extent.X,
				(CornerIdx & 2) ? CachedBx.Extent.Y : -CachedBx.Extent.Y,
				(CornerIdx & 4) ? CachedBx.Extent.Z : -CachedBx.Extent.Z
			);
			FVector WorldCorner = CachedBx.Center + CachedBx.Rotation.RotateVector(LocalCorner);
			CachedBounds += WorldCorner;
		}
	}

	// Cache convex hulls
	for (const FKConvexElem& ConvexElem : BodySetup->AggGeom.ConvexElems)
	{
		FCachedConvex CachedCvx;
		CachedCvx.BoneName = NAME_None;
		CachedCvx.BoneTransform = ComponentTransform;

		const TArray<FVector>& VertexData = ConvexElem.VertexData;
		if (VertexData.Num() < 4)
		{
			continue;
		}

		// Transform to world space
		TArray<FVector> WorldVerts;
		WorldVerts.Reserve(VertexData.Num());
		FVector CenterSum = FVector::ZeroVector;
		for (const FVector& V : VertexData)
		{
			FVector WorldV = ComponentTransform.TransformPosition(V);
			WorldVerts.Add(WorldV);
			CenterSum += WorldV;
		}

		CachedCvx.Center = CenterSum / static_cast<float>(WorldVerts.Num());

		// Bounding radius
		float MaxDistSq = 0.0f;
		for (const FVector& V : WorldVerts)
		{
			float DistSq = FVector::DistSquared(V, CachedCvx.Center);
			MaxDistSq = FMath::Max(MaxDistSq, DistSq);
			CachedBounds += V;
		}
		CachedCvx.BoundingRadius = FMath::Sqrt(MaxDistSq) + CollisionMargin;

		// Generate planes from index data
		const TArray<int32>& IndexData = ConvexElem.IndexData;
		if (IndexData.Num() >= 3)
		{
			TSet<uint32> PlaneHashes;
			for (int32 i = 0; i + 2 < IndexData.Num(); i += 3)
			{
				int32 I0 = IndexData[i];
				int32 I1 = IndexData[i + 1];
				int32 I2 = IndexData[i + 2];

				if (I0 < WorldVerts.Num() && I1 < WorldVerts.Num() && I2 < WorldVerts.Num())
				{
					FVector V0 = WorldVerts[I0];
					FVector V1 = WorldVerts[I1];
					FVector V2 = WorldVerts[I2];

					FVector Edge1 = V1 - V0;
					FVector Edge2 = V2 - V0;
					FVector Normal = FVector::CrossProduct(Edge1, Edge2);
					float NormalLen = Normal.Size();

					if (NormalLen > KINDA_SMALL_NUMBER)
					{
						Normal /= NormalLen;

						// Adjust to face outward
						FVector ToCenter = CachedCvx.Center - V0;
						if (FVector::DotProduct(Normal, ToCenter) > 0)
						{
							Normal = -Normal;
						}

						// 해시로 중복 제거
						int32 Nx = FMath::RoundToInt(Normal.X * 1000.0f);
						int32 Ny = FMath::RoundToInt(Normal.Y * 1000.0f);
						int32 Nz = FMath::RoundToInt(Normal.Z * 1000.0f);
						uint32 Hash = HashCombine(HashCombine(GetTypeHash(Nx), GetTypeHash(Ny)), GetTypeHash(Nz));

						if (!PlaneHashes.Contains(Hash))
						{
							PlaneHashes.Add(Hash);

							FCachedConvexPlane Plane;
							Plane.Normal = Normal;
							Plane.Distance = FVector::DotProduct(V0, Normal);
							CachedCvx.Planes.Add(Plane);
						}
					}
				}
			}
		}

		if (CachedCvx.Planes.Num() >= 4)
		{
			CachedConvexes.Add(MoveTemp(CachedCvx));
		}
	}

	if (CachedCapsules.Num() > 0 || CachedSpheres.Num() > 0 || CachedBoxes.Num() > 0 || CachedConvexes.Num() > 0)
	{
		float MaxRadius = CollisionMargin;
		for (const FCachedCapsule& Cap : CachedCapsules)
		{
			MaxRadius = FMath::Max(MaxRadius, Cap.Radius);
		}
		for (const FCachedSphere& Sph : CachedSpheres)
		{
			MaxRadius = FMath::Max(MaxRadius, Sph.Radius);
		}
		for (const FCachedBox& Box : CachedBoxes)
		{
			MaxRadius = FMath::Max(MaxRadius, Box.Extent.GetMax());
		}
		for (const FCachedConvex& Cvx : CachedConvexes)
		{
			MaxRadius = FMath::Max(MaxRadius, Cvx.BoundingRadius);
		}
		CachedBounds = CachedBounds.ExpandBy(MaxRadius);
		bCacheValid = true;

		//UE_LOG(LogTemp, Log, TEXT("MeshFluidCollider: StaticMesh cached - Spheres: %d, Capsules: %d, Boxes: %d, Convexes: %d"),CachedSpheres.Num(), CachedCapsules.Num(), CachedBoxes.Num(), CachedConvexes.Num());
	}
}

bool UMeshFluidCollider::GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const
{
	if (!bCacheValid)
	{
		return false;
	}

	// AABB culling: skip if outside bounding box
	const float CullingMargin = 50.0f;  // Collision margin + safety
	if (!CachedBounds.ExpandBy(CullingMargin).IsInside(Point))
	{
		return false;
	}

	float MinDistance = TNumericLimits<float>::Max();
	bool bFoundAny = false;

	// Iterate cached capsules
	for (const FCachedCapsule& Cap : CachedCapsules)
	{
		// Find closest point on capsule axis
		FVector SegmentDir = Cap.End - Cap.Start;
		float SegmentLengthSq = SegmentDir.SizeSquared();

		FVector ClosestOnAxis;
		if (SegmentLengthSq < KINDA_SMALL_NUMBER)
		{
			ClosestOnAxis = Cap.Start;
		}
		else
		{
			float t = FVector::DotProduct(Point - Cap.Start, SegmentDir) / SegmentLengthSq;
			t = FMath::Clamp(t, 0.0f, 1.0f);
			ClosestOnAxis = Cap.Start + SegmentDir * t;
		}

		FVector ToPointVec = Point - ClosestOnAxis;
		float DistToAxis = ToPointVec.Size();

		FVector TempNormal;
		FVector TempClosestPoint;
		float TempDistance;

		if (DistToAxis < KINDA_SMALL_NUMBER)
		{
			TempNormal = FVector::ForwardVector;
			TempClosestPoint = ClosestOnAxis + TempNormal * Cap.Radius;
			TempDistance = -Cap.Radius;
		}
		else
		{
			TempNormal = ToPointVec / DistToAxis;
			TempClosestPoint = ClosestOnAxis + TempNormal * Cap.Radius;
			TempDistance = DistToAxis - Cap.Radius;
		}

		if (TempDistance < MinDistance)
		{
			MinDistance = TempDistance;
			OutClosestPoint = TempClosestPoint;
			OutNormal = TempNormal;
			OutDistance = TempDistance;
			bFoundAny = true;
		}
	}

	// Iterate cached spheres
	for (const FCachedSphere& Sph : CachedSpheres)
	{
		FVector ToPointVec = Point - Sph.Center;
		float DistToCenter = ToPointVec.Size();

		FVector TempNormal;
		FVector TempClosestPoint;
		float TempDistance;

		if (DistToCenter < KINDA_SMALL_NUMBER)
		{
			TempNormal = FVector::UpVector;
			TempClosestPoint = Sph.Center + TempNormal * Sph.Radius;
			TempDistance = -Sph.Radius;
		}
		else
		{
			TempNormal = ToPointVec / DistToCenter;
			TempClosestPoint = Sph.Center + TempNormal * Sph.Radius;
			TempDistance = DistToCenter - Sph.Radius;
		}

		if (TempDistance < MinDistance)
		{
			MinDistance = TempDistance;
			OutClosestPoint = TempClosestPoint;
			OutNormal = TempNormal;
			OutDistance = TempDistance;
			bFoundAny = true;
		}
	}

	// Iterate cached boxes
	for (const FCachedBox& Box : CachedBoxes)
	{
		// Transform world point to box local space
		FVector LocalPoint = Box.Rotation.UnrotateVector(Point - Box.Center);

		// Find closest point in local space (clamping)
		FVector ClampedLocal;
		ClampedLocal.X = FMath::Clamp(LocalPoint.X, -Box.Extent.X, Box.Extent.X);
		ClampedLocal.Y = FMath::Clamp(LocalPoint.Y, -Box.Extent.Y, Box.Extent.Y);
		ClampedLocal.Z = FMath::Clamp(LocalPoint.Z, -Box.Extent.Z, Box.Extent.Z);

		// Transform back to world space
		FVector TempClosestPoint = Box.Center + Box.Rotation.RotateVector(ClampedLocal);

		FVector ToPointVec = Point - TempClosestPoint;
		float TempDistance = ToPointVec.Size();

		FVector TempNormal;
		if (TempDistance < KINDA_SMALL_NUMBER)
		{
			// Point is inside box - push to nearest face
			FVector AbsLocal = LocalPoint.GetAbs();
			FVector DistToFace = Box.Extent - AbsLocal;

			if (DistToFace.X <= DistToFace.Y && DistToFace.X <= DistToFace.Z)
			{
				TempNormal = FVector(FMath::Sign(LocalPoint.X), 0.0f, 0.0f);
				TempDistance = -DistToFace.X;
			}
			else if (DistToFace.Y <= DistToFace.Z)
			{
				TempNormal = FVector(0.0f, FMath::Sign(LocalPoint.Y), 0.0f);
				TempDistance = -DistToFace.Y;
			}
			else
			{
				TempNormal = FVector(0.0f, 0.0f, FMath::Sign(LocalPoint.Z));
				TempDistance = -DistToFace.Z;
			}
			TempNormal = Box.Rotation.RotateVector(TempNormal);
			TempClosestPoint = Point - TempNormal * TempDistance;
		}
		else
		{
			TempNormal = ToPointVec / TempDistance;
		}

		if (TempDistance < MinDistance)
		{
			MinDistance = TempDistance;
			OutClosestPoint = TempClosestPoint;
			OutNormal = TempNormal;
			OutDistance = TempDistance;
			bFoundAny = true;
		}
	}

	// Iterate cached convex hulls
	for (const FCachedConvex& Cvx : CachedConvexes)
	{
		// Bounding sphere culling
		float BoundDist = FVector::Dist(Point, Cvx.Center) - Cvx.BoundingRadius;
		if (BoundDist > MinDistance)
		{
			continue;
		}

		// Convex SDF: maximum signed distance among all planes
		float MaxPlaneDist = -TNumericLimits<float>::Max();
		FVector BestNormal = FVector::UpVector;

		for (const FCachedConvexPlane& Plane : Cvx.Planes)
		{
			float PlaneDist = FVector::DotProduct(Point, Plane.Normal) - Plane.Distance;
			if (PlaneDist > MaxPlaneDist)
			{
				MaxPlaneDist = PlaneDist;
				BestNormal = Plane.Normal;
			}
		}

		float TempDistance = MaxPlaneDist;

		if (TempDistance < MinDistance)
		{
			MinDistance = TempDistance;
			OutNormal = BestNormal;
			OutDistance = TempDistance;
			// Closest point is offset from query point by distance along normal
			OutClosestPoint = Point - BestNormal * TempDistance;
			bFoundAny = true;
		}
	}

	// Fallback to bounding box if no cached data
	if (!bFoundAny && TargetMeshComponent)
	{
		FVector BoxCenter = CachedBounds.GetCenter();
		FVector BoxExtent = CachedBounds.GetExtent();

		FVector LocalPoint = Point - BoxCenter;
		FVector ClampedPoint;
		ClampedPoint.X = FMath::Clamp(LocalPoint.X, -BoxExtent.X, BoxExtent.X);
		ClampedPoint.Y = FMath::Clamp(LocalPoint.Y, -BoxExtent.Y, BoxExtent.Y);
		ClampedPoint.Z = FMath::Clamp(LocalPoint.Z, -BoxExtent.Z, BoxExtent.Z);

		OutClosestPoint = BoxCenter + ClampedPoint;
		FVector ToPointVec = Point - OutClosestPoint;
		OutDistance = ToPointVec.Size();
		OutNormal = OutDistance > KINDA_SMALL_NUMBER ? ToPointVec / OutDistance : FVector::UpVector;
		bFoundAny = true;
	}

	return bFoundAny;
}

bool UMeshFluidCollider::GetClosestPointWithBone(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance, FName& OutBoneName, FTransform& OutBoneTransform) const
{
	if (!bCacheValid)
	{
		OutBoneName = NAME_None;
		OutBoneTransform = FTransform::Identity;
		return false;
	}

	// AABB culling: skip if outside bounding box
	const float CullingMargin = 50.0f;
	if (!CachedBounds.ExpandBy(CullingMargin).IsInside(Point))
	{
		OutBoneName = NAME_None;
		OutBoneTransform = FTransform::Identity;
		return false;
	}

	float MinDistance = TNumericLimits<float>::Max();
	bool bFoundAny = false;

	// Iterate cached capsules
	for (const FCachedCapsule& Cap : CachedCapsules)
	{
		FVector SegmentDir = Cap.End - Cap.Start;
		float SegmentLengthSq = SegmentDir.SizeSquared();

		FVector ClosestOnAxis;
		if (SegmentLengthSq < KINDA_SMALL_NUMBER)
		{
			ClosestOnAxis = Cap.Start;
		}
		else
		{
			float t = FVector::DotProduct(Point - Cap.Start, SegmentDir) / SegmentLengthSq;
			t = FMath::Clamp(t, 0.0f, 1.0f);
			ClosestOnAxis = Cap.Start + SegmentDir * t;
		}

		FVector ToPointVec = Point - ClosestOnAxis;
		float DistToAxis = ToPointVec.Size();

		FVector TempNormal;
		FVector TempClosestPoint;
		float TempDistance;

		if (DistToAxis < KINDA_SMALL_NUMBER)
		{
			TempNormal = FVector::ForwardVector;
			TempClosestPoint = ClosestOnAxis + TempNormal * Cap.Radius;
			TempDistance = -Cap.Radius;
		}
		else
		{
			TempNormal = ToPointVec / DistToAxis;
			TempClosestPoint = ClosestOnAxis + TempNormal * Cap.Radius;
			TempDistance = DistToAxis - Cap.Radius;
		}

		if (TempDistance < MinDistance)
		{
			MinDistance = TempDistance;
			OutClosestPoint = TempClosestPoint;
			OutNormal = TempNormal;
			OutDistance = TempDistance;
			OutBoneName = Cap.BoneName;
			OutBoneTransform = Cap.BoneTransform;
			bFoundAny = true;
		}
	}

	// Iterate cached spheres
	for (const FCachedSphere& Sph : CachedSpheres)
	{
		FVector ToPointVec = Point - Sph.Center;
		float DistToCenter = ToPointVec.Size();

		FVector TempNormal;
		FVector TempClosestPoint;
		float TempDistance;

		if (DistToCenter < KINDA_SMALL_NUMBER)
		{
			TempNormal = FVector::UpVector;
			TempClosestPoint = Sph.Center + TempNormal * Sph.Radius;
			TempDistance = -Sph.Radius;
		}
		else
		{
			TempNormal = ToPointVec / DistToCenter;
			TempClosestPoint = Sph.Center + TempNormal * Sph.Radius;
			TempDistance = DistToCenter - Sph.Radius;
		}

		if (TempDistance < MinDistance)
		{
			MinDistance = TempDistance;
			OutClosestPoint = TempClosestPoint;
			OutNormal = TempNormal;
			OutDistance = TempDistance;
			OutBoneName = Sph.BoneName;
			OutBoneTransform = Sph.BoneTransform;
			bFoundAny = true;
		}
	}

	// Iterate cached boxes
	for (const FCachedBox& Box : CachedBoxes)
	{
		// Transform world point to box local space
		FVector LocalPoint = Box.Rotation.UnrotateVector(Point - Box.Center);

		// Find closest point in local space (clamping)
		FVector ClampedLocal;
		ClampedLocal.X = FMath::Clamp(LocalPoint.X, -Box.Extent.X, Box.Extent.X);
		ClampedLocal.Y = FMath::Clamp(LocalPoint.Y, -Box.Extent.Y, Box.Extent.Y);
		ClampedLocal.Z = FMath::Clamp(LocalPoint.Z, -Box.Extent.Z, Box.Extent.Z);

		// Transform back to world space
		FVector TempClosestPoint = Box.Center + Box.Rotation.RotateVector(ClampedLocal);

		FVector ToPointVec = Point - TempClosestPoint;
		float TempDistance = ToPointVec.Size();

		FVector TempNormal;
		if (TempDistance < KINDA_SMALL_NUMBER)
		{
			// Point is inside box - push to nearest face
			FVector AbsLocal = LocalPoint.GetAbs();
			FVector DistToFace = Box.Extent - AbsLocal;

			if (DistToFace.X <= DistToFace.Y && DistToFace.X <= DistToFace.Z)
			{
				TempNormal = FVector(FMath::Sign(LocalPoint.X), 0.0f, 0.0f);
				TempDistance = -DistToFace.X;
			}
			else if (DistToFace.Y <= DistToFace.Z)
			{
				TempNormal = FVector(0.0f, FMath::Sign(LocalPoint.Y), 0.0f);
				TempDistance = -DistToFace.Y;
			}
			else
			{
				TempNormal = FVector(0.0f, 0.0f, FMath::Sign(LocalPoint.Z));
				TempDistance = -DistToFace.Z;
			}
			TempNormal = Box.Rotation.RotateVector(TempNormal);
			TempClosestPoint = Point - TempNormal * TempDistance;
		}
		else
		{
			TempNormal = ToPointVec / TempDistance;
		}

		if (TempDistance < MinDistance)
		{
			MinDistance = TempDistance;
			OutClosestPoint = TempClosestPoint;
			OutNormal = TempNormal;
			OutDistance = TempDistance;
			OutBoneName = Box.BoneName;
			OutBoneTransform = Box.BoneTransform;
			bFoundAny = true;
		}
	}

	// Iterate cached convex hulls
	for (const FCachedConvex& Cvx : CachedConvexes)
	{
		// Bounding sphere culling
		float BoundDist = FVector::Dist(Point, Cvx.Center) - Cvx.BoundingRadius;
		if (BoundDist > MinDistance)
		{
			continue;
		}

		// Convex SDF: maximum signed distance among all planes
		float MaxPlaneDist = -TNumericLimits<float>::Max();
		FVector BestNormal = FVector::UpVector;

		for (const FCachedConvexPlane& Plane : Cvx.Planes)
		{
			float PlaneDist = FVector::DotProduct(Point, Plane.Normal) - Plane.Distance;
			if (PlaneDist > MaxPlaneDist)
			{
				MaxPlaneDist = PlaneDist;
				BestNormal = Plane.Normal;
			}
		}

		float TempDistance = MaxPlaneDist;

		if (TempDistance < MinDistance)
		{
			MinDistance = TempDistance;
			OutNormal = BestNormal;
			OutDistance = TempDistance;
			OutClosestPoint = Point - BestNormal * TempDistance;
			OutBoneName = Cvx.BoneName;
			OutBoneTransform = Cvx.BoneTransform;
			bFoundAny = true;
		}
	}

	// Fallback to bounding box if no cached data (no bone info)
	if (!bFoundAny && TargetMeshComponent)
	{
		FVector BoxCenter = CachedBounds.GetCenter();
		FVector BoxExtent = CachedBounds.GetExtent();

		FVector LocalPoint = Point - BoxCenter;
		FVector ClampedPoint;
		ClampedPoint.X = FMath::Clamp(LocalPoint.X, -BoxExtent.X, BoxExtent.X);
		ClampedPoint.Y = FMath::Clamp(LocalPoint.Y, -BoxExtent.Y, BoxExtent.Y);
		ClampedPoint.Z = FMath::Clamp(LocalPoint.Z, -BoxExtent.Z, BoxExtent.Z);

		OutClosestPoint = BoxCenter + ClampedPoint;
		FVector ToPointVec = Point - OutClosestPoint;
		OutDistance = ToPointVec.Size();
		OutNormal = OutDistance > KINDA_SMALL_NUMBER ? ToPointVec / OutDistance : FVector::UpVector;
		OutBoneName = NAME_None;
		OutBoneTransform = FTransform::Identity;
		bFoundAny = true;
	}

	return bFoundAny;
}

bool UMeshFluidCollider::IsPointInside(const FVector& Point) const
{
	if (!TargetMeshComponent)
	{
		return false;
	}

	// Handle CapsuleComponent
	UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(TargetMeshComponent);
	if (Capsule)
	{
		FVector CapsuleCenter = Capsule->GetComponentLocation();
		float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		FVector CapsuleUp = Capsule->GetUpVector();

		FVector ToPoint = Point - CapsuleCenter;
		float AxisProjection = FVector::DotProduct(ToPoint, CapsuleUp);

		if (FMath::Abs(AxisProjection) > CapsuleHalfHeight)
		{
			FVector SphereCenter = CapsuleCenter + CapsuleUp * FMath::Sign(AxisProjection) * (CapsuleHalfHeight - CapsuleRadius);
			return FVector::DistSquared(Point, SphereCenter) <= CapsuleRadius * CapsuleRadius;
		}
		else
		{
			FVector ClosestOnAxis = CapsuleCenter + CapsuleUp * AxisProjection;
			return FVector::DistSquared(Point, ClosestOnAxis) <= CapsuleRadius * CapsuleRadius;
		}
	}

	// Handle SkeletalMeshComponent - use PhysicsAsset
	USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(TargetMeshComponent);
	if (SkelMesh)
	{
		UPhysicsAsset* PhysAsset = SkelMesh->GetPhysicsAsset();
		if (PhysAsset)
		{
			for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
			{
				if (!BodySetup)
				{
					continue;
				}

				int32 BoneIndex = SkelMesh->GetBoneIndex(BodySetup->BoneName);
				if (BoneIndex == INDEX_NONE)
				{
					continue;
				}

				FTransform BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);

				// Process PhysicsAsset Sphyl (capsule) elements
				for (const FKSphylElem& SphylElem : BodySetup->AggGeom.SphylElems)
				{
					FTransform CapsuleLocalTransform = SphylElem.GetTransform();
					FTransform CapsuleWorldTransform = CapsuleLocalTransform * BoneTransform;

					FVector CapsuleCenter = CapsuleWorldTransform.GetLocation();
					float CapsuleRadius = SphylElem.Radius + CollisionMargin;
					float CapsuleLength = SphylElem.Length;
					FVector CapsuleUp = CapsuleWorldTransform.GetRotation().GetUpVector();

					float HalfLength = CapsuleLength * 0.5f;
					FVector CapsuleStart = CapsuleCenter - CapsuleUp * HalfLength;
					FVector CapsuleEnd = CapsuleCenter + CapsuleUp * HalfLength;

					// Find closest point on capsule axis
					FVector SegmentDir = CapsuleEnd - CapsuleStart;
					float SegmentLengthSq = SegmentDir.SizeSquared();

					FVector ClosestOnAxis;
					if (SegmentLengthSq < KINDA_SMALL_NUMBER)
					{
						ClosestOnAxis = CapsuleStart;
					}
					else
					{
						float t = FVector::DotProduct(Point - CapsuleStart, SegmentDir) / SegmentLengthSq;
						t = FMath::Clamp(t, 0.0f, 1.0f);
						ClosestOnAxis = CapsuleStart + SegmentDir * t;
					}

					float DistSq = FVector::DistSquared(Point, ClosestOnAxis);
					if (DistSq <= CapsuleRadius * CapsuleRadius)
					{
						return true;
					}
				}

				// Process PhysicsAsset Sphere elements
				for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
				{
					FTransform SphereLocalTransform = SphereElem.GetTransform();
					FTransform SphereWorldTransform = SphereLocalTransform * BoneTransform;

					FVector SphereCenter = SphereWorldTransform.GetLocation();
					float SphereRadius = SphereElem.Radius + CollisionMargin;

					float DistSq = FVector::DistSquared(Point, SphereCenter);
					if (DistSq <= SphereRadius * SphereRadius)
					{
						return true;
					}
				}

				// Process PhysicsAsset Box elements
				for (const FKBoxElem& BoxElem : BodySetup->AggGeom.BoxElems)
				{
					FTransform BoxLocalTransform = BoxElem.GetTransform();
					FTransform BoxWorldTransform = BoxLocalTransform * BoneTransform;

					FVector BoxCenter = BoxWorldTransform.GetLocation();
					FVector BoxExtent(BoxElem.X * 0.5f + CollisionMargin,
					                  BoxElem.Y * 0.5f + CollisionMargin,
					                  BoxElem.Z * 0.5f + CollisionMargin);
					FQuat BoxRotation = BoxWorldTransform.GetRotation();

					// Transform world point to box local space
					FVector LocalPoint = BoxRotation.UnrotateVector(Point - BoxCenter);

					// Check if local point is within box bounds
					if (FMath::Abs(LocalPoint.X) <= BoxExtent.X &&
					    FMath::Abs(LocalPoint.Y) <= BoxExtent.Y &&
					    FMath::Abs(LocalPoint.Z) <= BoxExtent.Z)
					{
						return true;
					}
				}
			}

			// Check cached convex hulls (use cache instead of reading from PhysicsAsset)
			for (const FCachedConvex& Cvx : CachedConvexes)
			{
				// Bounding sphere culling
				float BoundDist = FVector::Dist(Point, Cvx.Center) - Cvx.BoundingRadius;
				if (BoundDist > 0.0f)
				{
					continue;
				}

				// Inside convex = signed distance is negative for all planes
				bool bInside = true;
				for (const FCachedConvexPlane& Plane : Cvx.Planes)
				{
					float PlaneDist = FVector::DotProduct(Point, Plane.Normal) - Plane.Distance;
					if (PlaneDist > 0.0f)
					{
						bInside = false;
						break;
					}
				}

				if (bInside)
				{
					return true;
				}
			}

			return false;
		}
	}

	// Check cached convex hulls (StaticMesh, etc.)
	for (const FCachedConvex& Cvx : CachedConvexes)
	{
		// Bounding sphere culling
		float BoundDist = FVector::Dist(Point, Cvx.Center) - Cvx.BoundingRadius;
		if (BoundDist > 0.0f)
		{
			continue;
		}

		// Inside convex = signed distance is negative for all planes
		bool bInside = true;
		for (const FCachedConvexPlane& Plane : Cvx.Planes)
		{
			float PlaneDist = FVector::DotProduct(Point, Plane.Normal) - Plane.Distance;
			if (PlaneDist > 0.0f)
			{
				bInside = false;
				break;
			}
		}

		if (bInside)
		{
			return true;
		}
	}

	// Fallback: use bounding box
	FBoxSphereBounds Bounds = TargetMeshComponent->Bounds;
	return Bounds.GetBox().IsInside(Point);
}

void UMeshFluidCollider::ExportToGPUPrimitives(
	TArray<FGPUCollisionSphere>& OutSpheres,
	TArray<FGPUCollisionCapsule>& OutCapsules,
	TArray<FGPUCollisionBox>& OutBoxes,
	TArray<FGPUCollisionConvex>& OutConvexes,
	TArray<FGPUConvexPlane>& OutPlanes,
	float InFriction,
	float InRestitution,
	int32 InOwnerID
) const
{
	if (!bCacheValid)
	{
		return;
	}

	int32 CachedPlaneCount = 0;
	for (const FCachedConvex& Cvx : CachedConvexes)
	{
		CachedPlaneCount += Cvx.Planes.Num();
	}

	// Debug: log exported primitive counts once per OwnerID to avoid spam
	static TSet<int32> LoggedOwnerIDs;
	if (!LoggedOwnerIDs.Contains(InOwnerID) && (CachedSpheres.Num() > 0 || CachedCapsules.Num() > 0 || CachedBoxes.Num() > 0 || CachedConvexes.Num() > 0))
	{
		UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider Export (OwnerID=%d): Spheres=%d, Capsules=%d, Boxes=%d, Convexes=%d, Planes=%d"),
			InOwnerID, CachedSpheres.Num(), CachedCapsules.Num(), CachedBoxes.Num(), CachedConvexes.Num(), CachedPlaneCount);
		LoggedOwnerIDs.Add(InOwnerID);
	}

	// Export spheres
	static bool bLoggedSpheres = false;
	for (const FCachedSphere& Sph : CachedSpheres)
	{
		FGPUCollisionSphere GPUSphere;
		GPUSphere.Center = FVector3f(Sph.Center);
		GPUSphere.Radius = Sph.Radius;
		GPUSphere.Friction = InFriction;
		GPUSphere.Restitution = InRestitution;
		GPUSphere.BoneIndex = Sph.BoneIndex;  // Use cached bone index
		GPUSphere.OwnerID = InOwnerID;

		// [Debug] GPU Collider log (first call only)
		if (!bLoggedSpheres)
		{
			int32 ColliderArrayIndex = OutSpheres.Num();
			UE_LOG(LogTemp, Warning, TEXT("[GPUCollider] Sphere[%d]: BoneName='%s', BoneIndex=%d, OwnerID=%d"),
				ColliderArrayIndex, *Sph.BoneName.ToString(), Sph.BoneIndex, InOwnerID);
		}

		OutSpheres.Add(GPUSphere);
	}
	if (!bLoggedSpheres) bLoggedSpheres = true;

	// Export capsules
	static int32 ExportCallCount = 0;
	ExportCallCount++;
	bool bShouldLog = (ExportCallCount <= 1);  // Log first call only

	for (const FCachedCapsule& Cap : CachedCapsules)
	{
		int32 ColliderArrayIndex = OutCapsules.Num();

		FGPUCollisionCapsule GPUCapsule;
		GPUCapsule.Start = FVector3f(Cap.Start);
		GPUCapsule.End = FVector3f(Cap.End);
		GPUCapsule.Radius = Cap.Radius;
		GPUCapsule.Friction = InFriction;
		GPUCapsule.Restitution = InRestitution;
		GPUCapsule.BoneIndex = Cap.BoneIndex;  // Use cached bone index
		GPUCapsule.OwnerID = InOwnerID;

		// [Debug] Verify BoneIndex during GPU export
		if (bShouldLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[GPUExport] Capsule[%d]: BoneName='%s', BoneIndex=%d, OwnerID=%d"),
				ColliderArrayIndex, *Cap.BoneName.ToString(), Cap.BoneIndex, InOwnerID);
		}

		OutCapsules.Add(GPUCapsule);
	}

	// Export boxes
	for (const FCachedBox& Box : CachedBoxes)
	{
		FGPUCollisionBox GPUBox;
		GPUBox.Center = FVector3f(Box.Center);
		GPUBox.Extent = FVector3f(Box.Extent);
		GPUBox.Rotation = FVector4f(
			static_cast<float>(Box.Rotation.X),
			static_cast<float>(Box.Rotation.Y),
			static_cast<float>(Box.Rotation.Z),
			static_cast<float>(Box.Rotation.W)
		);
		GPUBox.Friction = InFriction;
		GPUBox.Restitution = InRestitution;
		GPUBox.BoneIndex = Box.BoneIndex;  // Use cached bone index
		GPUBox.OwnerID = InOwnerID;
		OutBoxes.Add(GPUBox);
	}

	// Export convexes (with plane references)
	for (const FCachedConvex& Cvx : CachedConvexes)
	{
		FGPUCollisionConvex GPUConvex;
		GPUConvex.Center = FVector3f(Cvx.Center);
		GPUConvex.BoundingRadius = Cvx.BoundingRadius;
		GPUConvex.PlaneStartIndex = OutPlanes.Num();  // Start index in plane buffer
		GPUConvex.PlaneCount = Cvx.Planes.Num();
		GPUConvex.Friction = InFriction;
		GPUConvex.Restitution = InRestitution;
		GPUConvex.BoneIndex = Cvx.BoneIndex;  // Use cached bone index
		GPUConvex.OwnerID = InOwnerID;
		OutConvexes.Add(GPUConvex);

		// Add planes to the plane buffer
		for (const FCachedConvexPlane& Plane : Cvx.Planes)
		{
			FGPUConvexPlane GPUPlane;
			GPUPlane.Normal = FVector3f(Plane.Normal);
			GPUPlane.Distance = Plane.Distance;
			OutPlanes.Add(GPUPlane);
		}
	}
}

void UMeshFluidCollider::ExportToGPUPrimitivesWithBones(
	TArray<FGPUCollisionSphere>& OutSpheres,
	TArray<FGPUCollisionCapsule>& OutCapsules,
	TArray<FGPUCollisionBox>& OutBoxes,
	TArray<FGPUCollisionConvex>& OutConvexes,
	TArray<FGPUConvexPlane>& OutPlanes,
	TArray<FGPUBoneTransform>& OutBoneTransforms,
	TMap<FName, int32>& BoneNameToIndex,
	float InFriction,
	float InRestitution,
	int32 InOwnerID
) const
{
	if (!bCacheValid)
	{
		return;
	}

	int32 CachedPlaneCount = 0;
	for (const FCachedConvex& Cvx : CachedConvexes)
	{
		CachedPlaneCount += Cvx.Planes.Num();
	}

	// Debug: log exported primitive counts once per OwnerID to avoid spam
	static TSet<int32> LoggedOwnerIDs;
	if (!LoggedOwnerIDs.Contains(InOwnerID) && (CachedSpheres.Num() > 0 || CachedCapsules.Num() > 0 || CachedBoxes.Num() > 0 || CachedConvexes.Num() > 0))
	{
		UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider Export (OwnerID=%d): Spheres=%d, Capsules=%d, Boxes=%d, Convexes=%d, Planes=%d"),
			InOwnerID, CachedSpheres.Num(), CachedCapsules.Num(), CachedBoxes.Num(), CachedConvexes.Num(), CachedPlaneCount);
		LoggedOwnerIDs.Add(InOwnerID);
	}

	// Helper lambda: get or create bone index
	auto GetOrCreateBoneIndex = [&](FName BoneName, const FTransform& BoneTransform) -> int32
	{
		if (BoneName == NAME_None)
		{
			return -1;
		}

		if (int32* ExistingIndex = BoneNameToIndex.Find(BoneName))
		{
			// Save current as previous before updating (for velocity calculation)
			OutBoneTransforms[*ExistingIndex].UpdatePrevious();
			// Update existing bone transform (in case it moved)
			OutBoneTransforms[*ExistingIndex].SetFromTransform(BoneTransform);
			return *ExistingIndex;
		}

		// Create new bone entry
		int32 NewIndex = OutBoneTransforms.Num();
		BoneNameToIndex.Add(BoneName, NewIndex);

		FGPUBoneTransform GPUBone;
		GPUBone.SetFromTransform(BoneTransform);
		GPUBone.UpdatePrevious();  // Initialize previous to current
		OutBoneTransforms.Add(GPUBone);

		return NewIndex;
	};

	// Export spheres with bone indices
	for (const FCachedSphere& Sph : CachedSpheres)
	{
		FGPUCollisionSphere GPUSphere;
		GPUSphere.Center = FVector3f(Sph.Center);
		GPUSphere.Radius = Sph.Radius;
		GPUSphere.Friction = InFriction;
		GPUSphere.Restitution = InRestitution;
		GPUSphere.BoneIndex = Sph.BoneIndex;  // Use Skeleton BoneIndex directly (BUG FIX)
		GPUSphere.OwnerID = InOwnerID;

		// Update BoneTransforms array (for velocity calculation)
		GetOrCreateBoneIndex(Sph.BoneName, Sph.BoneTransform);

		OutSpheres.Add(GPUSphere);
	}

	// Export capsules with bone indices
	for (const FCachedCapsule& Cap : CachedCapsules)
	{
		FGPUCollisionCapsule GPUCapsule;
		GPUCapsule.Start = FVector3f(Cap.Start);
		GPUCapsule.End = FVector3f(Cap.End);
		GPUCapsule.Radius = Cap.Radius;
		GPUCapsule.Friction = InFriction;
		GPUCapsule.Restitution = InRestitution;
		GPUCapsule.BoneIndex = Cap.BoneIndex;  // Use Skeleton BoneIndex directly (BUG FIX)
		GPUCapsule.OwnerID = InOwnerID;

		// Update BoneTransforms array (for velocity calculation)
		GetOrCreateBoneIndex(Cap.BoneName, Cap.BoneTransform);

		OutCapsules.Add(GPUCapsule);
	}

	// Export boxes with bone indices
	for (const FCachedBox& Box : CachedBoxes)
	{
		FGPUCollisionBox GPUBox;
		GPUBox.Center = FVector3f(Box.Center);
		GPUBox.Extent = FVector3f(Box.Extent);
		GPUBox.Rotation = FVector4f(
			static_cast<float>(Box.Rotation.X),
			static_cast<float>(Box.Rotation.Y),
			static_cast<float>(Box.Rotation.Z),
			static_cast<float>(Box.Rotation.W)
		);
		GPUBox.Friction = InFriction;
		GPUBox.Restitution = InRestitution;
		GPUBox.BoneIndex = Box.BoneIndex;  // Use Skeleton BoneIndex directly (BUG FIX)
		GPUBox.OwnerID = InOwnerID;

		// Update BoneTransforms array (for velocity calculation)
		GetOrCreateBoneIndex(Box.BoneName, Box.BoneTransform);

		OutBoxes.Add(GPUBox);
	}

	// Export convexes with bone indices
	for (const FCachedConvex& Cvx : CachedConvexes)
	{
		FGPUCollisionConvex GPUConvex;
		GPUConvex.Center = FVector3f(Cvx.Center);
		GPUConvex.BoundingRadius = Cvx.BoundingRadius;
		GPUConvex.PlaneStartIndex = OutPlanes.Num();
		GPUConvex.PlaneCount = Cvx.Planes.Num();
		GPUConvex.Friction = InFriction;
		GPUConvex.Restitution = InRestitution;
		GPUConvex.BoneIndex = Cvx.BoneIndex;  // Use Skeleton BoneIndex directly (BUG FIX)
		GPUConvex.OwnerID = InOwnerID;

		// Update BoneTransforms array (for velocity calculation)
		GetOrCreateBoneIndex(Cvx.BoneName, Cvx.BoneTransform);

		OutConvexes.Add(GPUConvex);

		// Add planes to the plane buffer
		for (const FCachedConvexPlane& Plane : Cvx.Planes)
		{
			FGPUConvexPlane GPUPlane;
			GPUPlane.Normal = FVector3f(Plane.Normal);
			GPUPlane.Distance = Plane.Distance;
			OutPlanes.Add(GPUPlane);
		}
	}
}
