// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUStaticBoundaryManager Implementation
// 
// Performance Optimization (v2):
// - Primitive ID-based caching to avoid regenerating unchanged boundary particles
// - Only new primitives trigger generation; existing primitives reuse cached data
// - GPU upload only when active primitive set changes

#include "GPU/Managers/GPUStaticBoundaryManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUStaticBoundary, Log, All);
DEFINE_LOG_CATEGORY(LogGPUStaticBoundary);

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUStaticBoundaryManager::FGPUStaticBoundaryManager()
{
}

FGPUStaticBoundaryManager::~FGPUStaticBoundaryManager()
{
	Release();
}

//=============================================================================
// Lifecycle
//=============================================================================

void FGPUStaticBoundaryManager::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	bIsInitialized = true;
	UE_LOG(LogGPUStaticBoundary, Log, TEXT("FGPUStaticBoundaryManager initialized"));
}

void FGPUStaticBoundaryManager::Release()
{
	if (!bIsInitialized)
	{
		return;
	}

	BoundaryParticles.Empty();
	PrimitiveCache.Empty();
	ActivePrimitiveKeys.Empty();
	PreviousActivePrimitiveKeys.Empty();
	bIsInitialized = false;

	UE_LOG(LogGPUStaticBoundary, Log, TEXT("FGPUStaticBoundaryManager released"));
}

//=============================================================================
// Primitive Key Generation
//=============================================================================

uint64 FGPUStaticBoundaryManager::MakePrimitiveKey(EPrimitiveType Type, int32 OwnerID, uint32 GeometryHash)
{
	// Key format: (Type:8 | OwnerID:32 | GeometryHash:24) = 64-bit
	uint64 Key = 0;
	Key |= (static_cast<uint64>(Type) & 0xFF) << 56;          // 8 bits for type
	Key |= (static_cast<uint64>(OwnerID) & 0xFFFFFFFF) << 24; // 32 bits for OwnerID
	Key |= (static_cast<uint64>(GeometryHash) & 0xFFFFFF);    // 24 bits for geometry hash
	return Key;
}

uint32 FGPUStaticBoundaryManager::ComputeGeometryHash(const FGPUCollisionSphere& Sphere)
{
	// Hash: Center position + Radius
	uint32 Hash = GetTypeHash(Sphere.Center.X);
	Hash = HashCombine(Hash, GetTypeHash(Sphere.Center.Y));
	Hash = HashCombine(Hash, GetTypeHash(Sphere.Center.Z));
	Hash = HashCombine(Hash, GetTypeHash(Sphere.Radius));
	return Hash & 0xFFFFFF;  // Truncate to 24 bits
}

uint32 FGPUStaticBoundaryManager::ComputeGeometryHash(const FGPUCollisionCapsule& Capsule)
{
	// Hash: Start + End + Radius
	uint32 Hash = GetTypeHash(Capsule.Start.X);
	Hash = HashCombine(Hash, GetTypeHash(Capsule.Start.Y));
	Hash = HashCombine(Hash, GetTypeHash(Capsule.Start.Z));
	Hash = HashCombine(Hash, GetTypeHash(Capsule.End.X));
	Hash = HashCombine(Hash, GetTypeHash(Capsule.End.Y));
	Hash = HashCombine(Hash, GetTypeHash(Capsule.End.Z));
	Hash = HashCombine(Hash, GetTypeHash(Capsule.Radius));
	return Hash & 0xFFFFFF;
}

uint32 FGPUStaticBoundaryManager::ComputeGeometryHash(const FGPUCollisionBox& Box)
{
	// Hash: Center + Extent + Rotation
	uint32 Hash = GetTypeHash(Box.Center.X);
	Hash = HashCombine(Hash, GetTypeHash(Box.Center.Y));
	Hash = HashCombine(Hash, GetTypeHash(Box.Center.Z));
	Hash = HashCombine(Hash, GetTypeHash(Box.Extent.X));
	Hash = HashCombine(Hash, GetTypeHash(Box.Extent.Y));
	Hash = HashCombine(Hash, GetTypeHash(Box.Extent.Z));
	Hash = HashCombine(Hash, GetTypeHash(Box.Rotation.X));
	Hash = HashCombine(Hash, GetTypeHash(Box.Rotation.Y));
	Hash = HashCombine(Hash, GetTypeHash(Box.Rotation.Z));
	Hash = HashCombine(Hash, GetTypeHash(Box.Rotation.W));
	return Hash & 0xFFFFFF;
}

uint32 FGPUStaticBoundaryManager::ComputeGeometryHash(const FGPUCollisionConvex& Convex)
{
	// Hash: Center + BoundingRadius + PlaneStartIndex + PlaneCount
	uint32 Hash = GetTypeHash(Convex.Center.X);
	Hash = HashCombine(Hash, GetTypeHash(Convex.Center.Y));
	Hash = HashCombine(Hash, GetTypeHash(Convex.Center.Z));
	Hash = HashCombine(Hash, GetTypeHash(Convex.BoundingRadius));
	Hash = HashCombine(Hash, GetTypeHash(Convex.PlaneStartIndex));
	Hash = HashCombine(Hash, GetTypeHash(Convex.PlaneCount));
	return Hash & 0xFFFFFF;
}

//=============================================================================
// Boundary Particle Generation (with Primitive-based Caching)
//=============================================================================

bool FGPUStaticBoundaryManager::GenerateBoundaryParticles(
	const TArray<FGPUCollisionSphere>& Spheres,
	const TArray<FGPUCollisionCapsule>& Capsules,
	const TArray<FGPUCollisionBox>& Boxes,
	const TArray<FGPUCollisionConvex>& Convexes,
	const TArray<FGPUConvexPlane>& ConvexPlanes,
	float SmoothingRadius,
	float RestDensity)
{
	if (!bIsInitialized || !bIsEnabled)
	{
		return false;
	}

	// Check if generation parameters changed (requires cache invalidation)
	const bool bParamsChanged = 
		!FMath::IsNearlyEqual(CachedSmoothingRadius, SmoothingRadius) ||
		!FMath::IsNearlyEqual(CachedRestDensity, RestDensity) ||
		!FMath::IsNearlyEqual(CachedParticleSpacing, ParticleSpacing);

	if (bParamsChanged || bCacheInvalidated)
	{
		// Parameters changed - invalidate entire cache
		PrimitiveCache.Empty();
		CachedSmoothingRadius = SmoothingRadius;
		CachedRestDensity = RestDensity;
		CachedParticleSpacing = ParticleSpacing;
		bCacheInvalidated = false;
		
		UE_LOG(LogGPUStaticBoundary, Log, TEXT("Cache invalidated due to parameter change (Spacing=%.1f, Density=%.1f)"), 
			ParticleSpacing, RestDensity);
	}

	// Calculate Psi once
	const float Spacing = ParticleSpacing;
	const float Psi = CalculatePsi(Spacing, RestDensity);

	// Track active primitives for this frame
	PreviousActivePrimitiveKeys = MoveTemp(ActivePrimitiveKeys);
	ActivePrimitiveKeys.Reset();

	int32 NewPrimitivesGenerated = 0;
	int32 CachedPrimitivesReused = 0;

	// Process Spheres
	for (const FGPUCollisionSphere& Sphere : Spheres)
	{
		if (Sphere.BoneIndex >= 0)  // Skip skinned colliders
		{
			continue;
		}

		const uint64 Key = MakePrimitiveKey(EPrimitiveType::Sphere, Sphere.OwnerID, ComputeGeometryHash(Sphere));
		ActivePrimitiveKeys.Add(Key);

		if (!PrimitiveCache.Contains(Key))
		{
			// New primitive - generate boundary particles
			TArray<FGPUBoundaryParticle>& CachedParticles = PrimitiveCache.Add(Key);
			GenerateSphereBoundaryParticles(Sphere.Center, Sphere.Radius, Spacing, Psi, Sphere.OwnerID, CachedParticles);
			NewPrimitivesGenerated++;
		}
		else
		{
			CachedPrimitivesReused++;
		}
	}

	// Process Capsules
	for (const FGPUCollisionCapsule& Capsule : Capsules)
	{
		if (Capsule.BoneIndex >= 0)
		{
			continue;
		}

		const uint64 Key = MakePrimitiveKey(EPrimitiveType::Capsule, Capsule.OwnerID, ComputeGeometryHash(Capsule));
		ActivePrimitiveKeys.Add(Key);

		if (!PrimitiveCache.Contains(Key))
		{
			TArray<FGPUBoundaryParticle>& CachedParticles = PrimitiveCache.Add(Key);
			GenerateCapsuleBoundaryParticles(Capsule.Start, Capsule.End, Capsule.Radius, Spacing, Psi, Capsule.OwnerID, CachedParticles);
			NewPrimitivesGenerated++;
		}
		else
		{
			CachedPrimitivesReused++;
		}
	}

	// Process Boxes
	for (const FGPUCollisionBox& Box : Boxes)
	{
		if (Box.BoneIndex >= 0)
		{
			continue;
		}

		const uint64 Key = MakePrimitiveKey(EPrimitiveType::Box, Box.OwnerID, ComputeGeometryHash(Box));
		ActivePrimitiveKeys.Add(Key);

		if (!PrimitiveCache.Contains(Key))
		{
			FQuat4f Rotation(Box.Rotation.X, Box.Rotation.Y, Box.Rotation.Z, Box.Rotation.W);
			TArray<FGPUBoundaryParticle>& CachedParticles = PrimitiveCache.Add(Key);
			GenerateBoxBoundaryParticles(Box.Center, Box.Extent, Rotation, Spacing, Psi, Box.OwnerID, CachedParticles);
			NewPrimitivesGenerated++;
		}
		else
		{
			CachedPrimitivesReused++;
		}
	}

	// Process Convex hulls
	for (const FGPUCollisionConvex& Convex : Convexes)
	{
		if (Convex.BoneIndex >= 0)
		{
			continue;
		}

		const uint64 Key = MakePrimitiveKey(EPrimitiveType::Convex, Convex.OwnerID, ComputeGeometryHash(Convex));
		ActivePrimitiveKeys.Add(Key);

		if (!PrimitiveCache.Contains(Key))
		{
			TArray<FGPUBoundaryParticle>& CachedParticles = PrimitiveCache.Add(Key);
			GenerateConvexBoundaryParticles(Convex, ConvexPlanes, Spacing, Psi, Convex.OwnerID, CachedParticles);
			NewPrimitivesGenerated++;
		}
		else
		{
			CachedPrimitivesReused++;
		}
	}

	// Check if active primitive set changed (requires GPU re-upload)
	// TSet doesn't have != operator, so we compare manually
	bool bActivePrimitivesChanged = (ActivePrimitiveKeys.Num() != PreviousActivePrimitiveKeys.Num());
	if (!bActivePrimitivesChanged)
	{
		// Same size - check if all keys match
		for (const uint64& Key : ActivePrimitiveKeys)
		{
			if (!PreviousActivePrimitiveKeys.Contains(Key))
			{
				bActivePrimitivesChanged = true;
				break;
			}
		}
	}

	if (bActivePrimitivesChanged || NewPrimitivesGenerated > 0)
	{
		// Rebuild BoundaryParticles array from active cached primitives
		BoundaryParticles.Reset();

		// Estimate capacity
		int32 EstimatedTotal = 0;
		for (const uint64& Key : ActivePrimitiveKeys)
		{
			if (const TArray<FGPUBoundaryParticle>* CachedParticles = PrimitiveCache.Find(Key))
			{
				EstimatedTotal += CachedParticles->Num();
			}
		}
		BoundaryParticles.Reserve(EstimatedTotal);

		// Append all active primitive particles
		for (const uint64& Key : ActivePrimitiveKeys)
		{
			if (const TArray<FGPUBoundaryParticle>* CachedParticles = PrimitiveCache.Find(Key))
			{
				BoundaryParticles.Append(*CachedParticles);
			}
		}

		UE_LOG(LogGPUStaticBoundary, Log, 
			TEXT("Boundary particles updated: Total=%d, NewPrimitives=%d, CachedReused=%d, ActivePrimitives=%d"),
			BoundaryParticles.Num(), NewPrimitivesGenerated, CachedPrimitivesReused, ActivePrimitiveKeys.Num());

		return true;  // GPU upload required
	}

	// No changes - skip GPU upload
	return false;
}

void FGPUStaticBoundaryManager::ClearBoundaryParticles()
{
	BoundaryParticles.Reset();
	PrimitiveCache.Empty();
	ActivePrimitiveKeys.Empty();
	PreviousActivePrimitiveKeys.Empty();
	bCacheInvalidated = true;
}

void FGPUStaticBoundaryManager::InvalidateCache()
{
	PrimitiveCache.Empty();
	ActivePrimitiveKeys.Empty();
	PreviousActivePrimitiveKeys.Empty();
	bCacheInvalidated = true;
	
	UE_LOG(LogGPUStaticBoundary, Log, TEXT("Cache explicitly invalidated"));
}

//=============================================================================
// Generation Helpers
//=============================================================================

float FGPUStaticBoundaryManager::CalculatePsi(float Spacing, float RestDensity) const
{
	// Psi (ψ) - Boundary particle density contribution (Akinci 2012)
	//
	// For SURFACE sampling (2D), Psi should be:
	// Psi = RestDensity * EffectiveVolume
	// where EffectiveVolume = Spacing^2 * thickness (not Spacing^3!)
	//
	// Using particle radius as thickness gives reasonable density contribution
	// without the over-estimation that causes "wall climbing" artifacts.
	//
	// Convert cm to m for proper unit consistency with RestDensity (kg/m³)

	const float Spacing_m = Spacing * 0.01f;  // cm → m
	const float ParticleRadius_m = Spacing_m * 0.5f;
	const float SurfaceArea_m = Spacing_m * Spacing_m;
	const float EffectiveVolume_m = SurfaceArea_m * ParticleRadius_m;

	// Scaling factor tuned for proper density contribution at boundaries
	// - Too high (0.5): causes "wall climbing" due to artificial suction
	// - Too low (0.05): insufficient density contribution, doesn't fix deficit
	// - 0.2~0.3: balanced - fills density deficit without over-contribution
	return RestDensity * EffectiveVolume_m * 0.3f;
}

void FGPUStaticBoundaryManager::GenerateSphereBoundaryParticles(
	const FVector3f& Center,
	float Radius,
	float Spacing,
	float Psi,
	int32 OwnerID,
	TArray<FGPUBoundaryParticle>& OutParticles)
{
	// Use Fibonacci spiral for uniform distribution on sphere
	const float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
	const float AngleIncrement = PI * 2.0f * GoldenRatio;

	// Calculate number of points based on surface area and spacing
	const float SurfaceArea = 4.0f * PI * Radius * Radius;
	const int32 NumPoints = FMath::Max(4, FMath::CeilToInt(SurfaceArea / (Spacing * Spacing)));

	OutParticles.Reserve(OutParticles.Num() + NumPoints);

	for (int32 i = 0; i < NumPoints; ++i)
	{
		// Fibonacci spiral latitude
		const float T = static_cast<float>(i) / static_cast<float>(NumPoints - 1);
		const float Phi = FMath::Acos(1.0f - 2.0f * T);  // [0, PI]
		const float Theta = AngleIncrement * i;          // Longitude

		// Convert to Cartesian
		const float SinPhi = FMath::Sin(Phi);
		const float CosPhi = FMath::Cos(Phi);
		const float SinTheta = FMath::Sin(Theta);
		const float CosTheta = FMath::Cos(Theta);

		FVector3f Normal(SinPhi * CosTheta, SinPhi * SinTheta, CosPhi);
		FVector3f Position = Center + Normal * Radius;

		FGPUBoundaryParticle Particle;
		Particle.Position = Position;
		Particle.Normal = Normal;
		Particle.Psi = Psi;
		Particle.OwnerID = OwnerID;

		OutParticles.Add(Particle);
	}
}

void FGPUStaticBoundaryManager::GenerateCapsuleBoundaryParticles(
	const FVector3f& Start,
	const FVector3f& End,
	float Radius,
	float Spacing,
	float Psi,
	int32 OwnerID,
	TArray<FGPUBoundaryParticle>& OutParticles)
{
	const FVector3f Axis = End - Start;
	const float Height = Axis.Size();

	if (Height < SMALL_NUMBER)
	{
		// Degenerate capsule = sphere
		GenerateSphereBoundaryParticles((Start + End) * 0.5f, Radius, Spacing, Psi, OwnerID, OutParticles);
		return;
	}

	const FVector3f AxisDir = Axis / Height;

	// Build orthonormal basis
	FVector3f Tangent, Bitangent;
	if (FMath::Abs(AxisDir.Z) < 0.999f)
	{
		Tangent = FVector3f::CrossProduct(FVector3f(0, 0, 1), AxisDir).GetSafeNormal();
	}
	else
	{
		Tangent = FVector3f::CrossProduct(FVector3f(1, 0, 0), AxisDir).GetSafeNormal();
	}
	Bitangent = FVector3f::CrossProduct(AxisDir, Tangent);

	// Cylinder body
	const int32 NumRings = FMath::Max(2, FMath::CeilToInt(Height / Spacing));
	const float Circumference = 2.0f * PI * Radius;
	const int32 NumPointsPerRing = FMath::Max(6, FMath::CeilToInt(Circumference / Spacing));

	// Estimate total particles
	const int32 CylinderParticles = (NumRings + 1) * NumPointsPerRing;
	const float HemisphereSurfaceArea = 2.0f * PI * Radius * Radius;
	const int32 NumCapPoints = FMath::Max(4, FMath::CeilToInt(HemisphereSurfaceArea / (Spacing * Spacing)));
	OutParticles.Reserve(OutParticles.Num() + CylinderParticles + NumCapPoints * 2);

	for (int32 Ring = 0; Ring <= NumRings; ++Ring)
	{
		const float T = static_cast<float>(Ring) / static_cast<float>(NumRings);
		const FVector3f RingCenter = Start + AxisDir * (Height * T);

		for (int32 i = 0; i < NumPointsPerRing; ++i)
		{
			const float Angle = 2.0f * PI * static_cast<float>(i) / static_cast<float>(NumPointsPerRing);
			const FVector3f RadialDir = Tangent * FMath::Cos(Angle) + Bitangent * FMath::Sin(Angle);
			const FVector3f Position = RingCenter + RadialDir * Radius;

			FGPUBoundaryParticle Particle;
			Particle.Position = Position;
			Particle.Normal = RadialDir;
			Particle.Psi = Psi;
			Particle.OwnerID = OwnerID;

			OutParticles.Add(Particle);
		}
	}

	// Hemisphere caps (simplified as Fibonacci spiral)
	const float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
	const float AngleIncrement = PI * 2.0f * GoldenRatio;

	// Start cap (hemisphere pointing in -AxisDir)
	for (int32 i = 0; i < NumCapPoints; ++i)
	{
		const float T = static_cast<float>(i) / static_cast<float>(NumCapPoints - 1);
		const float Phi = FMath::Acos(1.0f - T);  // [0, PI/2] for hemisphere
		const float Theta = AngleIncrement * i;

		const float SinPhi = FMath::Sin(Phi);
		const float CosPhi = FMath::Cos(Phi);

		// Local hemisphere direction (pointing in -Z locally)
		FVector3f LocalDir(SinPhi * FMath::Cos(Theta), SinPhi * FMath::Sin(Theta), -CosPhi);

		// Transform to world
		FVector3f WorldDir = Tangent * LocalDir.X + Bitangent * LocalDir.Y + AxisDir * LocalDir.Z;
		FVector3f Position = Start + WorldDir * Radius;

		FGPUBoundaryParticle Particle;
		Particle.Position = Position;
		Particle.Normal = WorldDir;
		Particle.Psi = Psi;
		Particle.OwnerID = OwnerID;

		OutParticles.Add(Particle);
	}

	// End cap (hemisphere pointing in +AxisDir)
	for (int32 i = 0; i < NumCapPoints; ++i)
	{
		const float T = static_cast<float>(i) / static_cast<float>(NumCapPoints - 1);
		const float Phi = FMath::Acos(1.0f - T);
		const float Theta = AngleIncrement * i;

		const float SinPhi = FMath::Sin(Phi);
		const float CosPhi = FMath::Cos(Phi);

		FVector3f LocalDir(SinPhi * FMath::Cos(Theta), SinPhi * FMath::Sin(Theta), CosPhi);
		FVector3f WorldDir = Tangent * LocalDir.X + Bitangent * LocalDir.Y + AxisDir * LocalDir.Z;
		FVector3f Position = End + WorldDir * Radius;

		FGPUBoundaryParticle Particle;
		Particle.Position = Position;
		Particle.Normal = WorldDir;
		Particle.Psi = Psi;
		Particle.OwnerID = OwnerID;

		OutParticles.Add(Particle);
	}
}

void FGPUStaticBoundaryManager::GenerateBoxBoundaryParticles(
	const FVector3f& Center,
	const FVector3f& Extent,
	const FQuat4f& Rotation,
	float Spacing,
	float Psi,
	int32 OwnerID,
	TArray<FGPUBoundaryParticle>& OutParticles)
{
	// Local axes
	const FVector3f LocalX = Rotation.RotateVector(FVector3f(1, 0, 0));
	const FVector3f LocalY = Rotation.RotateVector(FVector3f(0, 1, 0));
	const FVector3f LocalZ = Rotation.RotateVector(FVector3f(0, 0, 1));

	// Generate particles on each face
	// Face normals and positions (in local space, then rotated)
	struct FFaceInfo
	{
		FVector3f Normal;
		FVector3f Center;
		FVector3f UAxis;
		FVector3f VAxis;
		float UExtent;
		float VExtent;
	};

	TArray<FFaceInfo> Faces;

	// +X face
	Faces.Add({ LocalX, Center + LocalX * Extent.X, LocalY, LocalZ, Extent.Y, Extent.Z });
	// -X face
	Faces.Add({ -LocalX, Center - LocalX * Extent.X, LocalY, LocalZ, Extent.Y, Extent.Z });
	// +Y face
	Faces.Add({ LocalY, Center + LocalY * Extent.Y, LocalX, LocalZ, Extent.X, Extent.Z });
	// -Y face
	Faces.Add({ -LocalY, Center - LocalY * Extent.Y, LocalX, LocalZ, Extent.X, Extent.Z });
	// +Z face
	Faces.Add({ LocalZ, Center + LocalZ * Extent.Z, LocalX, LocalY, Extent.X, Extent.Y });
	// -Z face
	Faces.Add({ -LocalZ, Center - LocalZ * Extent.Z, LocalX, LocalY, Extent.X, Extent.Y });

	// Estimate total particles
	int32 EstimatedTotal = 0;
	for (const FFaceInfo& Face : Faces)
	{
		const int32 NumU = FMath::Max(1, FMath::CeilToInt(Face.UExtent * 2.0f / Spacing));
		const int32 NumV = FMath::Max(1, FMath::CeilToInt(Face.VExtent * 2.0f / Spacing));
		EstimatedTotal += (NumU + 1) * (NumV + 1);
	}
	OutParticles.Reserve(OutParticles.Num() + EstimatedTotal);

	for (const FFaceInfo& Face : Faces)
	{
		const int32 NumU = FMath::Max(1, FMath::CeilToInt(Face.UExtent * 2.0f / Spacing));
		const int32 NumV = FMath::Max(1, FMath::CeilToInt(Face.VExtent * 2.0f / Spacing));

		for (int32 iu = 0; iu <= NumU; ++iu)
		{
			for (int32 iv = 0; iv <= NumV; ++iv)
			{
				const float U = -Face.UExtent + (2.0f * Face.UExtent * iu / NumU);
				const float V = -Face.VExtent + (2.0f * Face.VExtent * iv / NumV);

				FVector3f Position = Face.Center + Face.UAxis * U + Face.VAxis * V;

				FGPUBoundaryParticle Particle;
				Particle.Position = Position;
				Particle.Normal = Face.Normal;
				Particle.Psi = Psi;
				Particle.OwnerID = OwnerID;

				OutParticles.Add(Particle);
			}
		}
	}
}

void FGPUStaticBoundaryManager::GenerateConvexBoundaryParticles(
	const FGPUCollisionConvex& Convex,
	const TArray<FGPUConvexPlane>& AllPlanes,
	float Spacing,
	float Psi,
	int32 OwnerID,
	TArray<FGPUBoundaryParticle>& OutParticles)
{
	// For convex hulls, we sample points on each face
	// Each face is defined by a plane, and we need to find the face vertices
	// This is simplified: we sample within the bounding sphere, projecting onto each plane

	const FVector3f Center = Convex.Center;
	const float BoundingRadius = Convex.BoundingRadius;

	// For each plane, generate a grid of points that lie on the plane within the convex hull
	for (int32 PlaneIdx = 0; PlaneIdx < Convex.PlaneCount; ++PlaneIdx)
	{
		const int32 GlobalPlaneIdx = Convex.PlaneStartIndex + PlaneIdx;
		if (GlobalPlaneIdx >= AllPlanes.Num())
		{
			continue;
		}

		const FGPUConvexPlane& Plane = AllPlanes[GlobalPlaneIdx];
		const FVector3f PlaneNormal = Plane.Normal;
		const float PlaneDistance = Plane.Distance;

		// Find a point on the plane closest to center
		const float DistToPlane = FVector3f::DotProduct(Center, PlaneNormal) - PlaneDistance;
		const FVector3f PlaneCenter = Center - PlaneNormal * DistToPlane;

		// Build tangent basis on plane
		FVector3f Tangent, Bitangent;
		if (FMath::Abs(PlaneNormal.Z) < 0.999f)
		{
			Tangent = FVector3f::CrossProduct(FVector3f(0, 0, 1), PlaneNormal).GetSafeNormal();
		}
		else
		{
			Tangent = FVector3f::CrossProduct(FVector3f(1, 0, 0), PlaneNormal).GetSafeNormal();
		}
		Bitangent = FVector3f::CrossProduct(PlaneNormal, Tangent);

		// Sample grid on plane within bounding radius
		const int32 NumSamples = FMath::Max(3, FMath::CeilToInt(BoundingRadius * 2.0f / Spacing));
		const float SampleExtent = BoundingRadius;

		for (int32 iu = 0; iu <= NumSamples; ++iu)
		{
			for (int32 iv = 0; iv <= NumSamples; ++iv)
			{
				const float U = -SampleExtent + (2.0f * SampleExtent * iu / NumSamples);
				const float V = -SampleExtent + (2.0f * SampleExtent * iv / NumSamples);

				FVector3f TestPoint = PlaneCenter + Tangent * U + Bitangent * V;

				// Check if point is inside all planes (inside convex hull)
				bool bInside = true;
				for (int32 CheckPlaneIdx = 0; CheckPlaneIdx < Convex.PlaneCount; ++CheckPlaneIdx)
				{
					const int32 CheckGlobalIdx = Convex.PlaneStartIndex + CheckPlaneIdx;
					if (CheckGlobalIdx >= AllPlanes.Num())
					{
						continue;
					}

					const FGPUConvexPlane& CheckPlane = AllPlanes[CheckGlobalIdx];
					const float Dist = FVector3f::DotProduct(TestPoint, CheckPlane.Normal) - CheckPlane.Distance;

					// Small tolerance for points on face
					if (Dist > 0.1f)
					{
						bInside = false;
						break;
					}
				}

				if (bInside)
				{
					FGPUBoundaryParticle Particle;
					Particle.Position = TestPoint;
					Particle.Normal = PlaneNormal;
					Particle.Psi = Psi;
					Particle.OwnerID = OwnerID;

					OutParticles.Add(Particle);
				}
			}
		}
	}
}
