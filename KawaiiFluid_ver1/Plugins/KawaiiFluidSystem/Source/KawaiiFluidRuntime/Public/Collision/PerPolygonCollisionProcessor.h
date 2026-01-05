// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GPU/GPUFluidParticle.h"
#include "Collision/SkeletalMeshBVH.h"

// Forward declarations
class UFluidInteractionComponent;

/**
 * Per-Polygon Collision Processor
 *
 * Processes collisions between fluid particles and skeletal mesh triangles.
 * Uses BVH (Bounding Volume Hierarchy) for efficient triangle queries.
 *
 * Usage:
 * 1. UpdateBVHCache() - Update/create BVH for interaction components
 * 2. ProcessCollisions() - Process all candidate particles (ParallelFor)
 * 3. Apply corrections to GPU via GPUFluidSimulator::ApplyCorrections()
 */
class KAWAIIFLUIDRUNTIME_API FPerPolygonCollisionProcessor
{
public:
	FPerPolygonCollisionProcessor();
	~FPerPolygonCollisionProcessor();

	/**
	 * Process collisions for all candidate particles
	 * @param Candidates - Particles from GPU AABB filtering
	 * @param InteractionComponents - Per-Polygon enabled interaction components
	 * @param ParticleRadius - Particle collision radius
	 * @param OutCorrections - Output correction data for GPU
	 */
	void ProcessCollisions(
		const TArray<FGPUCandidateParticle>& Candidates,
		const TArray<UFluidInteractionComponent*>& InteractionComponents,
		float ParticleRadius,
		TArray<FParticleCorrection>& OutCorrections
	);

	/**
	 * Update BVH cache for interaction components
	 * Creates new BVH for components without one, updates skinned positions for existing ones
	 * @param InteractionComponents - Components to update
	 */
	void UpdateBVHCache(const TArray<UFluidInteractionComponent*>& InteractionComponents);

	/**
	 * Clear all cached BVH data
	 */
	void ClearBVHCache();

	/**
	 * Get BVH for a specific interaction component
	 * @param Component - The interaction component
	 * @return Pointer to BVH or nullptr if not cached
	 */
	FSkeletalMeshBVH* GetBVH(UFluidInteractionComponent* Component);

	/** Get statistics from last ProcessCollisions call */
	int32 GetLastProcessedCount() const { return LastProcessedCount; }
	int32 GetLastCollisionCount() const { return LastCollisionCount; }
	float GetLastProcessingTimeMs() const { return LastProcessingTimeMs; }
	float GetLastBVHUpdateTimeMs() const { return LastBVHUpdateTimeMs; }

	/** Configuration */
	void SetCollisionMargin(float Margin) { CollisionMargin = Margin; }
	float GetCollisionMargin() const { return CollisionMargin; }

	void SetFriction(float InFriction) { Friction = InFriction; }
	float GetFriction() const { return Friction; }

	void SetRestitution(float InRestitution) { Restitution = InRestitution; }
	float GetRestitution() const { return Restitution; }

private:
	/**
	 * Process collision for a single particle
	 * @param Candidate - The candidate particle data
	 * @param BVH - The BVH to query against
	 * @param ParticleRadius - Particle collision radius
	 * @param InCollisionMargin - Collision detection margin from InteractionComponent
	 * @param InFriction - Surface friction from InteractionComponent
	 * @param InRestitution - Bounce coefficient from InteractionComponent
	 * @param OutCorrection - Output correction data
	 * @return True if collision occurred
	 */
	bool ProcessSingleParticle(
		const FGPUCandidateParticle& Candidate,
		FSkeletalMeshBVH* BVH,
		float ParticleRadius,
		float InCollisionMargin,
		float InFriction,
		float InRestitution,
		FParticleCorrection& OutCorrection
	);

	/**
	 * Create or get BVH for a skeletal mesh component
	 * @param SkelMesh - The skeletal mesh component
	 * @return Pointer to BVH or nullptr on failure
	 */
	TSharedPtr<FSkeletalMeshBVH> CreateOrGetBVH(USkeletalMeshComponent* SkelMesh);

private:
	// BVH cache: Component -> BVH
	// Using TWeakObjectPtr as key to handle component destruction
	TMap<TWeakObjectPtr<UFluidInteractionComponent>, TSharedPtr<FSkeletalMeshBVH>> BVHCache;

	// Collision parameters
	float CollisionMargin;    // Extra margin for collision detection (cm)
	float Friction;           // Surface friction coefficient
	float Restitution;        // Bounce coefficient

	// Statistics
	int32 LastProcessedCount;
	int32 LastCollisionCount;
	float LastProcessingTimeMs;
	float LastBVHUpdateTimeMs;
};
