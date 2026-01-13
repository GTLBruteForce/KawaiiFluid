// Copyright KawaiiFluid Team. All Rights Reserved.
// FGPUCollisionManager - Unified collision system manager

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIResources.h"
#include "GPU/GPUFluidParticle.h"
#include "GPU/Managers/GPUCollisionFeedbackManager.h"

class FRHICommandListImmediate;
class FRDGBuilder;

/**
 * FGPUCollisionManager
 *
 * Manages all collision-related systems for GPU fluid simulation:
 * - Bounds collision (AABB/OBB)
 * - Distance Field collision
 * - Primitive collision (Spheres, Capsules, Boxes, Convexes)
 * - Collision feedback (GPU -> CPU readback)
 *
 * This consolidates collision logic that was previously scattered across
 * GPUFluidSimulator and GPUFluidSimulator_Collision.cpp
 */
class KAWAIIFLUIDRUNTIME_API FGPUCollisionManager
{
public:
	FGPUCollisionManager();
	~FGPUCollisionManager();

	//=========================================================================
	// Lifecycle
	//=========================================================================

	/** Initialize the collision manager */
	void Initialize();

	/** Release all resources */
	void Release();

	/** Check if manager is ready */
	bool IsReady() const { return bIsInitialized; }

	//=========================================================================
	// Distance Field Collision Configuration
	//=========================================================================

	/** Enable or disable Distance Field collision */
	void SetDistanceFieldCollisionEnabled(bool bEnabled) { DFCollisionParams.bEnabled = bEnabled ? 1 : 0; }

	/** Set Distance Field collision parameters */
	void SetDistanceFieldCollisionParams(const FGPUDistanceFieldCollisionParams& Params) { DFCollisionParams = Params; }

	/** Get Distance Field collision parameters */
	const FGPUDistanceFieldCollisionParams& GetDistanceFieldCollisionParams() const { return DFCollisionParams; }

	/** Set GDF Texture for collision */
	void SetGDFTexture(FRHITexture* InTexture) { CachedGDFTexture = InTexture; }

	/** Check if Distance Field collision is enabled */
	bool IsDistanceFieldCollisionEnabled() const { return DFCollisionParams.bEnabled != 0; }

	//=========================================================================
	// Collision Primitives
	//=========================================================================

	/**
	 * Upload collision primitives to GPU
	 * @param Primitives - Collection of collision primitives
	 */
	void UploadCollisionPrimitives(const FGPUCollisionPrimitives& Primitives);

	/** Set primitive collision threshold */
	void SetPrimitiveCollisionThreshold(float Threshold) { PrimitiveCollisionThreshold = Threshold; }

	/** Get primitive collision threshold */
	float GetPrimitiveCollisionThreshold() const { return PrimitiveCollisionThreshold; }

	/** Check if collision primitives are available */
	bool HasCollisionPrimitives() const { return bCollisionPrimitivesValid; }

	/** Get total number of collision primitives */
	int32 GetCollisionPrimitiveCount() const
	{
		return CachedSpheres.Num() + CachedCapsules.Num() + CachedBoxes.Num() + CachedConvexHeaders.Num();
	}

	/** Get total collider count */
	int32 GetTotalColliderCount() const { return GetCollisionPrimitiveCount(); }

	//=========================================================================
	// Collision Passes (called from render thread)
	//=========================================================================

	/** Add bounds collision pass (AABB/OBB) */
	void AddBoundsCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		int32 ParticleCount,
		const FGPUFluidSimulationParams& Params);

	/** Add distance field collision pass */
	void AddDistanceFieldCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		int32 ParticleCount,
		const FGPUFluidSimulationParams& Params);

	/** Add primitive collision pass (spheres, capsules, boxes, convexes) */
	void AddPrimitiveCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		int32 ParticleCount,
		const FGPUFluidSimulationParams& Params);

	//=========================================================================
	// Collision Feedback
	//=========================================================================

	/** Enable or disable collision feedback recording */
	void SetCollisionFeedbackEnabled(bool bEnabled);

	/** Check if collision feedback is enabled */
	bool IsCollisionFeedbackEnabled() const;

	/** Allocate collision feedback readback buffers */
	void AllocateCollisionFeedbackBuffers(FRHICommandListImmediate& RHICmdList);

	/** Release collision feedback buffers */
	void ReleaseCollisionFeedbackBuffers();

	/** Process collision feedback readback (non-blocking) */
	void ProcessCollisionFeedbackReadback(FRHICommandListImmediate& RHICmdList);

	/** Process collider contact count readback (non-blocking) */
	void ProcessColliderContactCountReadback(FRHICommandListImmediate& RHICmdList);

	/** Get collision feedback for a specific collider */
	bool GetCollisionFeedbackForCollider(int32 ColliderIndex, TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount);

	/** Get all collision feedback (unfiltered) */
	bool GetAllCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount);

	/** Get current collision feedback count */
	int32 GetCollisionFeedbackCount() const;

	/** Get collider contact count */
	int32 GetColliderContactCount(int32 ColliderIndex) const;

	/** Get all collider contact counts */
	void GetAllColliderContactCounts(TArray<int32>& OutCounts) const;

	/** Get contact count for a specific owner ID */
	int32 GetContactCountForOwner(int32 OwnerID) const;

	/** Get internal feedback manager (for advanced use) */
	FGPUCollisionFeedbackManager* GetFeedbackManager() const { return FeedbackManager.Get(); }

	//=========================================================================
	// Cached Data Accessors (for SimPasses)
	//=========================================================================

	const TArray<FGPUCollisionSphere>& GetCachedSpheres() const { return CachedSpheres; }
	const TArray<FGPUCollisionCapsule>& GetCachedCapsules() const { return CachedCapsules; }
	const TArray<FGPUCollisionBox>& GetCachedBoxes() const { return CachedBoxes; }
	const TArray<FGPUCollisionConvex>& GetCachedConvexHeaders() const { return CachedConvexHeaders; }
	const TArray<FGPUConvexPlane>& GetCachedConvexPlanes() const { return CachedConvexPlanes; }
	const TArray<FGPUBoneTransform>& GetCachedBoneTransforms() const { return CachedBoneTransforms; }

	/** Check if bone transforms are valid */
	bool AreBoneTransformsValid() const { return bBoneTransformsValid; }

private:
	//=========================================================================
	// State
	//=========================================================================

	bool bIsInitialized = false;
	mutable FCriticalSection CollisionLock;

	//=========================================================================
	// Distance Field Collision
	//=========================================================================

	FGPUDistanceFieldCollisionParams DFCollisionParams;
	FTextureRHIRef CachedGDFTexture;

	//=========================================================================
	// Collision Primitives
	//=========================================================================

	TArray<FGPUCollisionSphere> CachedSpheres;
	TArray<FGPUCollisionCapsule> CachedCapsules;
	TArray<FGPUCollisionBox> CachedBoxes;
	TArray<FGPUCollisionConvex> CachedConvexHeaders;
	TArray<FGPUConvexPlane> CachedConvexPlanes;
	TArray<FGPUBoneTransform> CachedBoneTransforms;

	float PrimitiveCollisionThreshold = 1.0f;
	bool bCollisionPrimitivesValid = false;
	bool bBoneTransformsValid = false;

	//=========================================================================
	// Collision Feedback
	//=========================================================================

	TUniquePtr<FGPUCollisionFeedbackManager> FeedbackManager;
};
