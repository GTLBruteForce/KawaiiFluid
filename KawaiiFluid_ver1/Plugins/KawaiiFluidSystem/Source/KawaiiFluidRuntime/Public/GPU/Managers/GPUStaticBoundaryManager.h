// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUStaticBoundaryManager - Static Mesh Boundary Particle Generator
// Generates boundary particles on static colliders for density contribution (Akinci 2012)
//
// Performance Optimization (v2):
// - Primitive ID-based caching: boundary particles are cached per-primitive
// - Only new primitives trigger generation; existing primitives reuse cached data
// - GPU upload only when active primitive set changes

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
 * - **Primitive ID-based caching** for performance (avoids regeneration)
 *
 * Usage:
 * 1. Call GenerateBoundaryParticles() with collision primitives
 * 2. Boundary particles are automatically added to density solver
 * 3. Enable debug visualization to verify particle placement
 *
 * Caching Strategy:
 * - Each primitive is identified by a unique key (type + OwnerID + geometry hash)
 * - First encounter: generate boundary particles and cache them
 * - Subsequent encounters: retrieve from cache (O(1) lookup)
 * - Cache invalidation: only on World change or explicit clear
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
	 * Generate boundary particles from collision primitives (with caching)
	 * 
	 * This function uses primitive-based caching to avoid regenerating boundary
	 * particles for primitives that haven't changed. Only new primitives will
	 * have their boundary particles generated.
	 * 
	 * @param Spheres - Sphere colliders
	 * @param Capsules - Capsule colliders
	 * @param Boxes - Box colliders
	 * @param Convexes - Convex hull headers
	 * @param ConvexPlanes - Convex hull planes
	 * @param SmoothingRadius - Fluid smoothing radius (for spacing calculation)
	 * @param RestDensity - Fluid rest density (for Psi calculation)
	 * @return true if boundary particles changed (requires GPU re-upload)
	 */
	bool GenerateBoundaryParticles(
		const TArray<FGPUCollisionSphere>& Spheres,
		const TArray<FGPUCollisionCapsule>& Capsules,
		const TArray<FGPUCollisionBox>& Boxes,
		const TArray<FGPUCollisionConvex>& Convexes,
		const TArray<FGPUConvexPlane>& ConvexPlanes,
		float SmoothingRadius,
		float RestDensity);

	/**
	 * Clear all generated boundary particles and cache
	 */
	void ClearBoundaryParticles();

	/**
	 * Invalidate cache (call when World changes)
	 * This forces full regeneration on next GenerateBoundaryParticles call
	 */
	void InvalidateCache();

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
	// Primitive Key Generation (for cache lookup)
	//=========================================================================

	/** Primitive type enum for cache key */
	enum class EPrimitiveType : uint8
	{
		Sphere = 0,
		Capsule = 1,
		Box = 2,
		Convex = 3
	};

	/** 
	 * Generate unique cache key for a primitive
	 * Key format: (Type:8 | OwnerID:32 | GeometryHash:24) = 64-bit
	 */
	static uint64 MakePrimitiveKey(EPrimitiveType Type, int32 OwnerID, uint32 GeometryHash);
	
	/** Compute geometry hash for each primitive type */
	static uint32 ComputeGeometryHash(const FGPUCollisionSphere& Sphere);
	static uint32 ComputeGeometryHash(const FGPUCollisionCapsule& Capsule);
	static uint32 ComputeGeometryHash(const FGPUCollisionBox& Box);
	static uint32 ComputeGeometryHash(const FGPUCollisionConvex& Convex);

	//=========================================================================
	// Generation Helpers (output to provided array)
	//=========================================================================

	/** Generate boundary particles on a sphere surface */
	void GenerateSphereBoundaryParticles(
		const FVector3f& Center,
		float Radius,
		float Spacing,
		float Psi,
		int32 OwnerID,
		TArray<FGPUBoundaryParticle>& OutParticles);

	/** Generate boundary particles on a capsule surface */
	void GenerateCapsuleBoundaryParticles(
		const FVector3f& Start,
		const FVector3f& End,
		float Radius,
		float Spacing,
		float Psi,
		int32 OwnerID,
		TArray<FGPUBoundaryParticle>& OutParticles);

	/** Generate boundary particles on a box surface */
	void GenerateBoxBoundaryParticles(
		const FVector3f& Center,
		const FVector3f& Extent,
		const FQuat4f& Rotation,
		float Spacing,
		float Psi,
		int32 OwnerID,
		TArray<FGPUBoundaryParticle>& OutParticles);

	/** Generate boundary particles on a convex hull surface */
	void GenerateConvexBoundaryParticles(
		const FGPUCollisionConvex& Convex,
		const TArray<FGPUConvexPlane>& AllPlanes,
		float Spacing,
		float Psi,
		int32 OwnerID,
		TArray<FGPUBoundaryParticle>& OutParticles);

	/** Calculate Psi value based on spacing */
	float CalculatePsi(float Spacing, float RestDensity) const;

	//=========================================================================
	// State
	//=========================================================================

	bool bIsInitialized = false;
	bool bIsEnabled = true;
	float ParticleSpacing = 5.0f;  // Default: 5.0 cm (same as FluidInteractionComponent)

	//=========================================================================
	// Generated Data (Active boundary particles for current frame)
	//=========================================================================

	TArray<FGPUBoundaryParticle> BoundaryParticles;

	//=========================================================================
	// Primitive Cache (Persistent across frames)
	// Key: PrimitiveKey (Type + OwnerID + GeometryHash)
	// Value: Cached boundary particles for that primitive
	//=========================================================================

	TMap<uint64, TArray<FGPUBoundaryParticle>> PrimitiveCache;
	
	/** Set of active primitive keys in current frame (for change detection) */
	TSet<uint64> ActivePrimitiveKeys;
	TSet<uint64> PreviousActivePrimitiveKeys;

	//=========================================================================
	// Cache Parameters (detect parameter changes requiring recalculation)
	//=========================================================================

	float CachedSmoothingRadius = 0.0f;
	float CachedRestDensity = 0.0f;
	float CachedParticleSpacing = 0.0f;
	bool bCacheInvalidated = false;
};
