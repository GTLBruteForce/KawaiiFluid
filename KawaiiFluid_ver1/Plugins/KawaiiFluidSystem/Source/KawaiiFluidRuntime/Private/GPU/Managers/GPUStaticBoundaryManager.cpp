// Copyright KawaiiFluid Team. All Rights Reserved.
// FGPUStaticBoundaryManager Implementation

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
	bIsInitialized = false;

	UE_LOG(LogGPUStaticBoundary, Log, TEXT("FGPUStaticBoundaryManager released"));
}

//=============================================================================
// Boundary Particle Generation
//=============================================================================

void FGPUStaticBoundaryManager::GenerateBoundaryParticles(
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
		return;
	}

	// Check if parameters changed
	const bool bParamsChanged = !FMath::IsNearlyEqual(CachedSmoothingRadius, SmoothingRadius) ||
		!FMath::IsNearlyEqual(CachedRestDensity, RestDensity);

	if (bParamsChanged || bCacheDirty)
	{
		CachedSmoothingRadius = SmoothingRadius;
		CachedRestDensity = RestDensity;
		bCacheDirty = false;
	}

	// Clear previous particles
	BoundaryParticles.Reset();

	// Calculate spacing and Psi
	const float Spacing = SmoothingRadius * SpacingMultiplier;
	const float Psi = CalculatePsi(Spacing, RestDensity);

	// Reserve estimated capacity
	const int32 EstimatedCount =
		Spheres.Num() * 100 +      // ~100 particles per sphere (estimate)
		Capsules.Num() * 150 +     // ~150 particles per capsule
		Boxes.Num() * 200 +        // ~200 particles per box
		Convexes.Num() * 150;      // ~150 particles per convex
	BoundaryParticles.Reserve(EstimatedCount);

	// Generate boundary particles for each collider type (only static colliders, BoneIndex < 0)

	// Spheres
	for (const FGPUCollisionSphere& Sphere : Spheres)
	{
		if (Sphere.BoneIndex < 0)  // Only static colliders
		{
			GenerateSphereBoundaryParticles(
				Sphere.Center,
				Sphere.Radius,
				Spacing,
				Psi,
				Sphere.OwnerID);
		}
	}

	// Capsules
	for (const FGPUCollisionCapsule& Capsule : Capsules)
	{
		if (Capsule.BoneIndex < 0)  // Only static colliders
		{
			GenerateCapsuleBoundaryParticles(
				Capsule.Start,
				Capsule.End,
				Capsule.Radius,
				Spacing,
				Psi,
				Capsule.OwnerID);
		}
	}

	// Boxes
	for (const FGPUCollisionBox& Box : Boxes)
	{
		if (Box.BoneIndex < 0)  // Only static colliders
		{
			FQuat4f Rotation(Box.Rotation.X, Box.Rotation.Y, Box.Rotation.Z, Box.Rotation.W);
			GenerateBoxBoundaryParticles(
				Box.Center,
				Box.Extent,
				Rotation,
				Spacing,
				Psi,
				Box.OwnerID);
		}
	}

	// Convex hulls
	for (const FGPUCollisionConvex& Convex : Convexes)
	{
		if (Convex.BoneIndex < 0)  // Only static colliders
		{
			GenerateConvexBoundaryParticles(
				Convex,
				ConvexPlanes,
				Spacing,
				Psi,
				Convex.OwnerID);
		}
	}

	// Log generation results (every 60 frames)
	static int32 LogCounter = 0;
	if (++LogCounter % 60 == 1)
	{
		UE_LOG(LogGPUStaticBoundary, Log, TEXT("Generated %d static boundary particles (Spacing=%.1f, Psi=%.4f)"),
			BoundaryParticles.Num(), Spacing, Psi);
	}
}

void FGPUStaticBoundaryManager::ClearBoundaryParticles()
{
	BoundaryParticles.Reset();
	bCacheDirty = true;
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
	// The factor 0.1 is empirically tuned to:
	// 1. Provide enough density contribution to prevent penetration
	// 2. Not create artificial suction/pressure at boundaries

	const float ParticleRadius = Spacing * 0.5f;  // Approximate particle radius
	const float SurfaceArea = Spacing * Spacing;
	const float EffectiveVolume = SurfaceArea * ParticleRadius;

	// Scaling factor tuned for proper density contribution at boundaries
	// - Too high (0.5): causes "wall climbing" due to artificial suction
	// - Too low (0.05): insufficient density contribution, doesn't fix deficit
	// - 0.2~0.3: balanced - fills density deficit without over-contribution
	return RestDensity * EffectiveVolume * 0.3f;
}

void FGPUStaticBoundaryManager::GenerateSphereBoundaryParticles(
	const FVector3f& Center,
	float Radius,
	float Spacing,
	float Psi,
	int32 OwnerID)
{
	// Use Fibonacci spiral for uniform distribution on sphere
	const float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
	const float AngleIncrement = PI * 2.0f * GoldenRatio;

	// Calculate number of points based on surface area and spacing
	const float SurfaceArea = 4.0f * PI * Radius * Radius;
	const int32 NumPoints = FMath::Max(4, FMath::CeilToInt(SurfaceArea / (Spacing * Spacing)));

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

		BoundaryParticles.Add(Particle);
	}
}

void FGPUStaticBoundaryManager::GenerateCapsuleBoundaryParticles(
	const FVector3f& Start,
	const FVector3f& End,
	float Radius,
	float Spacing,
	float Psi,
	int32 OwnerID)
{
	const FVector3f Axis = End - Start;
	const float Height = Axis.Size();

	if (Height < SMALL_NUMBER)
	{
		// Degenerate capsule = sphere
		GenerateSphereBoundaryParticles((Start + End) * 0.5f, Radius, Spacing, Psi, OwnerID);
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

			BoundaryParticles.Add(Particle);
		}
	}

	// Hemisphere caps (simplified as Fibonacci spiral)
	const float HemisphereSurfaceArea = 2.0f * PI * Radius * Radius;
	const int32 NumCapPoints = FMath::Max(4, FMath::CeilToInt(HemisphereSurfaceArea / (Spacing * Spacing)));
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

		BoundaryParticles.Add(Particle);
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

		BoundaryParticles.Add(Particle);
	}
}

void FGPUStaticBoundaryManager::GenerateBoxBoundaryParticles(
	const FVector3f& Center,
	const FVector3f& Extent,
	const FQuat4f& Rotation,
	float Spacing,
	float Psi,
	int32 OwnerID)
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

				BoundaryParticles.Add(Particle);
			}
		}
	}
}

void FGPUStaticBoundaryManager::GenerateConvexBoundaryParticles(
	const FGPUCollisionConvex& Convex,
	const TArray<FGPUConvexPlane>& AllPlanes,
	float Spacing,
	float Psi,
	int32 OwnerID)
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

					BoundaryParticles.Add(Particle);
				}
			}
		}
	}
}
