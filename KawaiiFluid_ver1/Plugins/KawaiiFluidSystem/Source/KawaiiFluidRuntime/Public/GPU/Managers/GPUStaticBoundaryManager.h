// Copyright KawaiiFluid Team. All Rights Reserved.
// FGPUStaticBoundaryManager - Static Mesh Boundary Particle Generator
// Generates boundary particles on static colliders for density contribution (Akinci 2012)

#pragma once

#include "CoreMinimal.h"
#include "GPU/GPUFluidParticle.h"

/**
 * FGPUStaticBoundaryManager
 *
 * Generates boundary particles on static mesh colliders (floor, walls, obstacles)
 * for proper density calculation near boundaries (Akinci 2012).
 *
 * Features:
 * - Generates boundary particles from Sphere, Capsule, Box, Convex colliders
 * - Uses SmoothingRadius/2 spacing for proper coverage
 * - Calculates surface normals for each particle
 * - Computes Psi (density contribution) based on particle spacing
 *
 * Usage:
 * 1. Call GenerateBoundaryParticles() with collision primitives
 * 2. Boundary particles are automatically added to density solver
 * 3. Enable debug visualization to verify particle placement
 */
class KAWAIIFLUIDRUNTIME_API FGPUStaticBoundaryManager
{
public:
	FGPUStaticBoundaryManager();
	~FGPUStaticBoundaryManager();

	//=========================================================================
	// Lifecycle
	//=========================================================================

	void Initialize();
	void Release();
	bool IsReady() const { return bIsInitialized; }

	//=========================================================================
	// Boundary Particle Generation
	//=========================================================================

	/**
	 * Generate boundary particles from collision primitives
	 * @param Spheres - Sphere colliders
	 * @param Capsules - Capsule colliders
	 * @param Boxes - Box colliders
	 * @param Convexes - Convex hull headers
	 * @param ConvexPlanes - Convex hull planes
	 * @param SmoothingRadius - Fluid smoothing radius (for spacing calculation)
	 * @param RestDensity - Fluid rest density (for Psi calculation)
	 */
	void GenerateBoundaryParticles(
		const TArray<FGPUCollisionSphere>& Spheres,
		const TArray<FGPUCollisionCapsule>& Capsules,
		const TArray<FGPUCollisionBox>& Boxes,
		const TArray<FGPUCollisionConvex>& Convexes,
		const TArray<FGPUConvexPlane>& ConvexPlanes,
		float SmoothingRadius,
		float RestDensity);

	/**
	 * Clear all generated boundary particles
	 */
	void ClearBoundaryParticles();

	//=========================================================================
	// Accessors
	//=========================================================================

	/** Get generated boundary particles */
	const TArray<FGPUBoundaryParticle>& GetBoundaryParticles() const { return BoundaryParticles; }

	/** Get boundary particle count */
	int32 GetBoundaryParticleCount() const { return BoundaryParticles.Num(); }

	/** Check if boundary particles are available */
	bool HasBoundaryParticles() const { return BoundaryParticles.Num() > 0; }

	//=========================================================================
	// Configuration
	//=========================================================================

	/** Enable/disable static boundary generation */
	void SetEnabled(bool bEnabled) { bIsEnabled = bEnabled; }
	bool IsEnabled() const { return bIsEnabled; }

	/** Set particle spacing in cm (default 5.0 cm, same as FluidInteractionComponent) */
	void SetParticleSpacing(float Spacing) { ParticleSpacing = FMath::Max(Spacing, 1.0f); }
	float GetParticleSpacing() const { return ParticleSpacing; }

private:
	//=========================================================================
	// Generation Helpers
	//=========================================================================

	/** Generate boundary particles on a sphere surface */
	void GenerateSphereBoundaryParticles(
		const FVector3f& Center,
		float Radius,
		float Spacing,
		float Psi,
		int32 OwnerID);

	/** Generate boundary particles on a capsule surface */
	void GenerateCapsuleBoundaryParticles(
		const FVector3f& Start,
		const FVector3f& End,
		float Radius,
		float Spacing,
		float Psi,
		int32 OwnerID);

	/** Generate boundary particles on a box surface */
	void GenerateBoxBoundaryParticles(
		const FVector3f& Center,
		const FVector3f& Extent,
		const FQuat4f& Rotation,
		float Spacing,
		float Psi,
		int32 OwnerID);

	/** Generate boundary particles on a convex hull surface */
	void GenerateConvexBoundaryParticles(
		const FGPUCollisionConvex& Convex,
		const TArray<FGPUConvexPlane>& AllPlanes,
		float Spacing,
		float Psi,
		int32 OwnerID);

	/** Calculate Psi value based on spacing */
	float CalculatePsi(float Spacing, float RestDensity) const;

	//=========================================================================
	// State
	//=========================================================================

	bool bIsInitialized = false;
	bool bIsEnabled = true;
	float ParticleSpacing = 5.0f;  // Default: 5.0 cm (same as FluidInteractionComponent)

	//=========================================================================
	// Generated Data
	//=========================================================================

	TArray<FGPUBoundaryParticle> BoundaryParticles;

	//=========================================================================
	// Cache
	//=========================================================================

	float CachedSmoothingRadius = 0.0f;
	float CachedRestDensity = 0.0f;
	bool bCacheDirty = true;
};
