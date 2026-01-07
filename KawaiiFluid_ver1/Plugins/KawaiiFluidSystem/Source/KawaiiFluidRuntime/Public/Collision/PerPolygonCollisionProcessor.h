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
 * Handles particle attachment to surfaces with detachment based on surface acceleration.
 *
 * Usage:
 * 1. UpdateBVHCache() - Update/create BVH for interaction components
 * 2. ProcessCollisions() - Process all candidate particles (ParallelFor)
 * 3. UpdateAttachedParticles() - Update attached particle positions & check detachment
 * 4. Apply corrections to GPU via GPUFluidSimulator::ApplyCorrections()
 */
class KAWAIIFLUIDRUNTIME_API FPerPolygonCollisionProcessor
{
public:
	FPerPolygonCollisionProcessor();
	~FPerPolygonCollisionProcessor();

	/**
	 * Process collisions for all candidate particles
	 * Also handles new attachments when adhesion is strong enough
	 * @param Candidates - Particles from GPU AABB filtering
	 * @param InteractionComponents - Per-Polygon enabled interaction components
	 * @param ParticleRadius - Particle collision radius
	 * @param AdhesionStrength - Fluid adhesion strength (0-1) from Preset
	 * @param ContactOffset - Extra contact offset applied to collider distance checks
	 * @param OutCorrections - Output correction data for GPU
	 */
	void ProcessCollisions(
		const TArray<FGPUCandidateParticle>& Candidates,
		const TArray<UFluidInteractionComponent*>& InteractionComponents,
		float ParticleRadius,
		float AdhesionStrength,
		float ContactOffset,
		TArray<FParticleCorrection>& OutCorrections
	);

	/**
	 * Update attached particle positions using barycentric interpolation
	 * Also checks for detachment conditions (surface acceleration, gravity, etc.)
	 * @param InteractionComponents - Per-Polygon enabled interaction components
	 * @param DeltaTime - Frame delta time for acceleration calculation
	 * @param OutUpdates - Output position updates for GPU
	 */
	void UpdateAttachedParticles(
		const TArray<UFluidInteractionComponent*>& InteractionComponents,
		float DeltaTime,
		TArray<FAttachedParticleUpdate>& OutUpdates
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

	//========================================
	// Attachment Configuration
	//========================================

	/**
	 * Set detachment acceleration threshold
	 * Higher value = particles stay attached through faster movements
	 * @param Threshold - Acceleration threshold in cm/s² (default: 5000)
	 */
	void SetDetachAccelerationThreshold(float Threshold) { DetachAccelerationThreshold = Threshold; }
	float GetDetachAccelerationThreshold() const { return DetachAccelerationThreshold; }

	/**
	 * Set minimum adhesion strength for attachment
	 * Particles only attach when AdhesionStrength >= this value
	 * @param MinAdhesion - Minimum adhesion (0-1, default: 0.3)
	 */
	void SetMinAdhesionForAttachment(float MinAdhesion) { MinAdhesionForAttachment = MinAdhesion; }
	float GetMinAdhesionForAttachment() const { return MinAdhesionForAttachment; }

	/**
	 * Set gravity influence on detachment (for upside-down surfaces)
	 * @param Influence - 0 = gravity ignored, 1 = full gravity influence (default: 0.5)
	 */
	void SetGravityDetachInfluence(float Influence) { GravityDetachInfluence = FMath::Clamp(Influence, 0.0f, 1.0f); }
	float GetGravityDetachInfluence() const { return GravityDetachInfluence; }

	/** Get number of currently attached particles */
	int32 GetAttachedParticleCount() const { return AttachedParticles.Num(); }

	/** Clear all attachments */
	void ClearAttachments() { AttachedParticles.Empty(); }

	/** Remove attachment for specific particle */
	void RemoveAttachment(uint32 ParticleIndex) { AttachedParticles.Remove(ParticleIndex); }

	/** Check if a particle is attached */
	bool IsParticleAttached(uint32 ParticleIndex) const { return AttachedParticles.Contains(ParticleIndex); }

	/** Get attachment info for a particle (returns nullptr if not attached) */
	const FParticleAttachmentInfo* GetAttachmentInfo(uint32 ParticleIndex) const { return AttachedParticles.Find(ParticleIndex); }

private:
	/**
	 * Process collision for a single particle
	 * @param Candidate - The candidate particle data
	 * @param BVH - The BVH to query against
	 * @param ParticleRadius - Particle collision radius
	 * @param InCollisionMargin - Collision detection margin from InteractionComponent
	 * @param InFriction - Surface friction from InteractionComponent
	 * @param InRestitution - Bounce coefficient from InteractionComponent
	 * @param InAdhesionStrength - Fluid adhesion strength (0-1) from Preset
	 * @param ContactOffset - Extra distance allowed to overlap the collider
	 * @param OutCorrection - Output correction data
	 * @param OutTriangleIndex - Output triangle index for attachment (INDEX_NONE if no collision)
	 * @param OutClosestPoint - Output closest point on triangle for attachment
	 * @return True if collision occurred
	 */
	bool ProcessSingleParticle(
		const FGPUCandidateParticle& Candidate,
		FSkeletalMeshBVH* BVH,
		float ParticleRadius,
		float InCollisionMargin,
		float InFriction,
		float InRestitution,
		float InAdhesionStrength,
		float ContactOffset,
		FParticleCorrection& OutCorrection,
		int32& OutTriangleIndex,
		FVector& OutClosestPoint
	);

	/**
	 * Create or get BVH for a skeletal mesh component
	 * @param SkelMesh - The skeletal mesh component
	 * @return Pointer to BVH or nullptr on failure
	 */
	TSharedPtr<FSkeletalMeshBVH> CreateOrGetBVH(USkeletalMeshComponent* SkelMesh);

	/**
	 * Compute barycentric coordinates for a point on a triangle
	 * @param Point - The point (should be on or near triangle surface)
	 * @param V0, V1, V2 - Triangle vertices
	 * @param OutU, OutV - Output barycentric coordinates (W = 1 - U - V)
	 */
	static void ComputeBarycentricCoordinates(
		const FVector& Point, const FVector& V0, const FVector& V1, const FVector& V2,
		float& OutU, float& OutV);

	/**
	 * Check if a particle should detach based on surface acceleration and gravity
	 * @param Info - Attachment info with previous position
	 * @param CurrentPosition - Current surface position
	 * @param CurrentNormal - Current surface normal
	 * @param DeltaTime - Frame delta time
	 * @param OutDetachVelocity - Output velocity to give the particle when detaching
	 * @return True if particle should detach
	 */
	bool ShouldDetach(
		const FParticleAttachmentInfo& Info,
		const FVector& CurrentPosition,
		const FVector& CurrentNormal,
		float DeltaTime,
		FVector& OutDetachVelocity) const;

	/**
	 * Try to attach a particle to a triangle
	 * @param ParticleIndex - GPU particle index
	 * @param InteractionIndex - Interaction component index
	 * @param TriangleIndex - Triangle index in BVH
	 * @param ClosestPoint - Point on triangle surface
	 * @param Triangle - The triangle data
	 * @param AdhesionStrength - Current adhesion strength
	 * @param WorldTime - Current world time
	 */
	void TryAttachParticle(
		uint32 ParticleIndex,
		int32 InteractionIndex,
		int32 TriangleIndex,
		const FVector& ClosestPoint,
		const FSkinnedTriangle& Triangle,
		float AdhesionStrength,
		float WorldTime);

private:
	// BVH cache: Component -> BVH
	// Using TWeakObjectPtr as key to handle component destruction
	TMap<TWeakObjectPtr<UFluidInteractionComponent>, TSharedPtr<FSkeletalMeshBVH>> BVHCache;

	// Attached particles: ParticleIndex -> AttachmentInfo
	TMap<uint32, FParticleAttachmentInfo> AttachedParticles;

	// Lock for thread-safe attachment modification
	FCriticalSection AttachmentLock;

	// Collision parameters
	float CollisionMargin;    // Extra margin for collision detection (cm)
	float Friction;           // Surface friction coefficient
	float Restitution;        // Bounce coefficient

	// Attachment parameters
	float DetachAccelerationThreshold;  // Acceleration needed to detach (cm/s², default: 5000)
	float MinAdhesionForAttachment;     // Minimum adhesion to attach (0-1, default: 0.3)
	float GravityDetachInfluence;       // How much gravity affects detachment (0-1, default: 0.5)

	// Gravity vector (cached from simulation params)
	FVector GravityVector;

	// Statistics
	int32 LastProcessedCount;
	int32 LastCollisionCount;
	int32 LastAttachmentCount;
	int32 LastDetachmentCount;
	float LastProcessingTimeMs;
	float LastBVHUpdateTimeMs;
};
