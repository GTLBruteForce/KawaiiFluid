// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIResources.h"
#include "RenderResource.h"
#include "GPU/GPUFluidParticle.h"
#include "GPU/Managers/GPUSpawnManager.h"
#include "GPU/Managers/GPUStreamCompactionManager.h"
#include "GPU/Managers/GPUCollisionManager.h"
#include "GPU/Managers/GPUSpatialHashManager.h"
#include "GPU/Managers/GPUBoundarySkinningManager.h"
#include "GPU/Managers/GPUAdhesionManager.h"
#include "Core/FluidAnisotropy.h"
#include <atomic>

// Log category
DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);

// Forward declarations
struct FFluidParticle;
class FRDGBuilder;
class FRHIGPUBufferReadback;

/**
 * GPU Fluid Simulator
 * Manages GPU-based SPH fluid physics simulation
 *
 * This class handles:
 * - GPU buffer management for particle data
 * - CPU ↔ GPU data transfers
 * - Orchestration of compute shader passes
 * - Integration with spatial hash system
 */
class KAWAIIFLUIDRUNTIME_API FGPUFluidSimulator : public FRenderResource
{
public:
	FGPUFluidSimulator();
	virtual ~FGPUFluidSimulator();

	//=============================================================================
	// Initialization
	//=============================================================================

	/** Initialize with maximum particle capacity */
	void Initialize(int32 InMaxParticleCount);

	/** Release all GPU resources */
	void Release();

	/** Check if simulator is initialized and ready */
	bool IsReady() const { return bIsInitialized && MaxParticleCount > 0; }

	//=============================================================================
	// FRenderResource Interface
	//=============================================================================

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	//=============================================================================
	// Data Transfer (CPU ↔ GPU)
	//=============================================================================

	/**
	 * Upload CPU particle data to GPU
	 * Call this before simulation each frame
	 * @param CPUParticles - Source particles (will be converted to GPU format)
	 */
	void UploadParticles(const TArray<FFluidParticle>& CPUParticles);

	/**
	 * Download GPU particle data back to CPU
	 * Call this after simulation to get results
	 * @param OutCPUParticles - Destination particles (will be updated from GPU data)
	 */
	void DownloadParticles(TArray<FFluidParticle>& OutCPUParticles);

	/**
	 * Get all GPU particles directly from readback buffer
	 * Unlike DownloadParticles, this creates new FFluidParticle instances
	 * Useful for stats collection when no existing CPU particles are available
	 * @param OutParticles - Output array (will be populated with all GPU particles)
	 * @return true if valid GPU data was retrieved
	 */
	bool GetAllGPUParticles(TArray<FFluidParticle>& OutParticles);

	/**
	 * Get current particle count on GPU
	 */
	int32 GetParticleCount() const { return CurrentParticleCount; }

	/**
	 * Get maximum particle capacity
	 */
	int32 GetMaxParticleCount() const { return MaxParticleCount; }

	/**
	 * Clear all particles on GPU (resets CurrentParticleCount and PersistentParticleCount)
	 */
	void ClearAllParticles()
	{
		CurrentParticleCount = 0;
		PersistentParticleCount.store(0);
		bNeedsFullUpload = true;
		InvalidatePreviousPositions();
	}

	//=============================================================================
	// Simulation Execution
	//=============================================================================

	/**
	 * Execute full GPU simulation for one substep
	 * This runs all compute passes: Predict → Hash → Density → Pressure → Viscosity → Collision → Finalize
	 *
	 * @param Params - Simulation parameters for this substep
	 */
	void SimulateSubstep(const FGPUFluidSimulationParams& Params);

	/**
	 * Execute GPU simulation using RDG (Render Dependency Graph)
	 * This is the preferred method when integrating with render pipeline
	 *
	 * @param GraphBuilder - RDG builder to add passes to
	 * @param Params - Simulation parameters
	 */
	void SimulateSubstep_RDG(FRDGBuilder& GraphBuilder, const FGPUFluidSimulationParams& Params);

	//=============================================================================
	// Buffer Access
	//=============================================================================

	/** Get particle buffer RHI reference (for external use) */
	FBufferRHIRef GetParticleBufferRHI() const { return ParticleBufferRHI; }

	/** Get particle SRV for shader binding */
	FShaderResourceViewRHIRef GetParticleSRV() const { return ParticleSRV; }

	/** Get particle UAV for shader binding */
	FUnorderedAccessViewRHIRef GetParticleUAV() const { return ParticleUAV; }

	/** Get persistent pooled buffer for RDG registration (Phase 2: GPU→GPU copy) */
	TRefCountPtr<FRDGPooledBuffer> GetPersistentParticleBuffer() const { return PersistentParticleBuffer; }

	/** Get particle count that matches PersistentParticleBuffer (synchronized in render thread) */
	int32 GetPersistentParticleCount() const { return PersistentParticleCount.load(); }

	/** Access previous frame position buffer (for history trails) */
	TRefCountPtr<FRDGPooledBuffer>& AccessPreviousPositionsBuffer() { return PreviousPositionsBuffer; }

	/** Get previous position count */
	int32 GetPreviousPositionsCount() const { return PreviousPositionsCount; }

	/** Returns true if previous positions buffer is valid and matches required particle count */
	bool HasPreviousPositions(int32 RequiredCount) const
	{
		return bPreviousPositionsValid && PreviousPositionsBuffer.IsValid() && PreviousPositionsCount == RequiredCount;
	}

	/** Mark previous positions as valid/invalid */
	void MarkPreviousPositionsValid(bool bIsValid) { bPreviousPositionsValid = bIsValid; }

	/** Set previous positions count */
	void SetPreviousPositionsCount(int32 Count) { PreviousPositionsCount = Count; }

	/** Invalidate previous positions buffer (call when particle order changes drastically) */
	void InvalidatePreviousPositions()
	{
		bPreviousPositionsValid = false;
		PreviousPositionsCount = 0;
		PreviousPositionsBuffer.SafeRelease();
	}

	//=============================================================================
	// Configuration
	//=============================================================================

	/** Set external force to apply (e.g., wind, explosions) */
	void SetExternalForce(const FVector3f& Force) { ExternalForce = Force; }

	/** Set maximum velocity safety clamp (to prevent divergence) */
	void SetMaxVelocity(float MaxVel) { MaxVelocity = FMath::Max(MaxVel, 0.0f); }

	/** Set anisotropy parameters for ellipsoid rendering */
	void SetAnisotropyParams(const FFluidAnisotropyParams& InParams) { CachedAnisotropyParams = InParams; }

	/**
	 * Set simulation bounds for Morton code computation
	 * IMPORTANT: Bounds must fit within Morton code capacity!
	 * Max extent per axis = 1024 * CellSize (typically ~2048 units)
	 * Bounds should be centered around particle spawn location
	 * @param BoundsMin - Minimum corner of simulation bounds
	 * @param BoundsMax - Maximum corner of simulation bounds
	 */
	void SetSimulationBounds(const FVector3f& BoundsMin, const FVector3f& BoundsMax)
	{
		SimulationBoundsMin = BoundsMin;
		SimulationBoundsMax = BoundsMax;
		// Also update SpatialHashManager
		if (SpatialHashManager.IsValid())
		{
			SpatialHashManager->SetSimulationBounds(BoundsMin, BoundsMax);
		}
	}

	/** Get current simulation bounds */
	void GetSimulationBounds(FVector3f& OutMin, FVector3f& OutMax) const
	{
		OutMin = SimulationBoundsMin;
		OutMax = SimulationBoundsMax;
	}


	/** Get anisotropy parameters */
	const FFluidAnisotropyParams& GetAnisotropyParams() const { return CachedAnisotropyParams; }

	/** Check if anisotropy is enabled */
	bool IsAnisotropyEnabled() const { return CachedAnisotropyParams.bEnabled; }

	/** Get persistent anisotropy axis buffers (for rendering) */
	TRefCountPtr<FRDGPooledBuffer> GetPersistentAnisotropyAxis1Buffer() const { return PersistentAnisotropyAxis1Buffer; }
	TRefCountPtr<FRDGPooledBuffer> GetPersistentAnisotropyAxis2Buffer() const { return PersistentAnisotropyAxis2Buffer; }
	TRefCountPtr<FRDGPooledBuffer> GetPersistentAnisotropyAxis3Buffer() const { return PersistentAnisotropyAxis3Buffer; }

	/** Access anisotropy buffers for writing (compute shader output) */
	TRefCountPtr<FRDGPooledBuffer>& AccessPersistentAnisotropyAxis1Buffer() { return PersistentAnisotropyAxis1Buffer; }
	TRefCountPtr<FRDGPooledBuffer>& AccessPersistentAnisotropyAxis2Buffer() { return PersistentAnisotropyAxis2Buffer; }
	TRefCountPtr<FRDGPooledBuffer>& AccessPersistentAnisotropyAxis3Buffer() { return PersistentAnisotropyAxis3Buffer; }

	//=============================================================================
	// Distance Field Collision (Delegated to FGPUCollisionManager)
	//=============================================================================

	/** Enable or disable Distance Field collision */
	void SetDistanceFieldCollisionEnabled(bool bEnabled) { if (CollisionManager.IsValid()) CollisionManager->SetDistanceFieldCollisionEnabled(bEnabled); }

	/** Set Distance Field collision parameters */
	void SetDistanceFieldCollisionParams(const FGPUDistanceFieldCollisionParams& Params) { if (CollisionManager.IsValid()) CollisionManager->SetDistanceFieldCollisionParams(Params); }

	/** Get Distance Field collision parameters */
	const FGPUDistanceFieldCollisionParams& GetDistanceFieldCollisionParams() const { static FGPUDistanceFieldCollisionParams Default; return CollisionManager.IsValid() ? CollisionManager->GetDistanceFieldCollisionParams() : Default; }

	/** Set the Global Distance Field texture */
	void SetGlobalDistanceFieldTexture(FRHITexture* Texture) { if (CollisionManager.IsValid()) CollisionManager->SetGDFTexture(Texture); }

	/** Check if Distance Field collision is enabled */
	bool IsDistanceFieldCollisionEnabled() const { return CollisionManager.IsValid() && CollisionManager->IsDistanceFieldCollisionEnabled(); }

	// Collision Primitives (Delegated to FGPUCollisionManager)
	//=============================================================================
	//=============================================================================

	/** Upload collision primitives to GPU */
	void UploadCollisionPrimitives(const FGPUCollisionPrimitives& Primitives) { if (CollisionManager.IsValid()) CollisionManager->UploadCollisionPrimitives(Primitives); }

	/** Set primitive collision threshold */
	void SetPrimitiveCollisionThreshold(float Threshold) { if (CollisionManager.IsValid()) CollisionManager->SetPrimitiveCollisionThreshold(Threshold); }

	/** Check if collision primitives are available */
	bool HasCollisionPrimitives() const { return CollisionManager.IsValid() && CollisionManager->HasCollisionPrimitives(); }

	/** Get total number of collision primitives */
	int32 GetCollisionPrimitiveCount() const { return CollisionManager.IsValid() ? CollisionManager->GetCollisionPrimitiveCount() : 0; }

	/** Access cached collision data (for SimPasses) */
	const TArray<FGPUCollisionSphere>& GetCachedSpheres() const { static TArray<FGPUCollisionSphere> Empty; return CollisionManager.IsValid() ? CollisionManager->GetCachedSpheres() : Empty; }
	const TArray<FGPUCollisionCapsule>& GetCachedCapsules() const { static TArray<FGPUCollisionCapsule> Empty; return CollisionManager.IsValid() ? CollisionManager->GetCachedCapsules() : Empty; }
	const TArray<FGPUCollisionBox>& GetCachedBoxes() const { static TArray<FGPUCollisionBox> Empty; return CollisionManager.IsValid() ? CollisionManager->GetCachedBoxes() : Empty; }
	const TArray<FGPUCollisionConvex>& GetCachedConvexHeaders() const { static TArray<FGPUCollisionConvex> Empty; return CollisionManager.IsValid() ? CollisionManager->GetCachedConvexHeaders() : Empty; }
	const TArray<FGPUConvexPlane>& GetCachedConvexPlanes() const { static TArray<FGPUConvexPlane> Empty; return CollisionManager.IsValid() ? CollisionManager->GetCachedConvexPlanes() : Empty; }

	//=============================================================================
	// GPU Adhesion System (Bone-based attachment tracking)
	//=============================================================================

	/**
	 * Set adhesion parameters for GPU-based attachment
	 * @param Params - Adhesion configuration
	 */
	void SetAdhesionParams(const FGPUAdhesionParams& Params) { if (AdhesionManager.IsValid()) AdhesionManager->SetAdhesionParams(Params); }

	/**
	 * Get bone transform count
	 */
	int32 GetBoneTransformCount() const { return CollisionManager.IsValid() ? CollisionManager->GetCachedBoneTransforms().Num() : 0; }

	//=============================================================================
	// GPU Boundary Particles (Flex-style Adhesion from FluidInteractionComponent)
	//=============================================================================

	/**
	 * Upload boundary particles to GPU
	 * Call this before simulation each frame with boundary particle data from FluidInteractionComponents
	 * @param BoundaryParticles - Collection of boundary particles
	 */
	void UploadBoundaryParticles(const FGPUBoundaryParticles& BoundaryParticles);

	/** Set boundary adhesion parameters */
	void SetBoundaryAdhesionParams(const FGPUBoundaryAdhesionParams& Params)
	{
		if (BoundarySkinningManager.IsValid()) { BoundarySkinningManager->SetBoundaryAdhesionParams(Params); }
	}

	/** Get boundary adhesion parameters */
	const FGPUBoundaryAdhesionParams& GetBoundaryAdhesionParams() const;

	/** Check if boundary adhesion is enabled */
	bool IsBoundaryAdhesionEnabled() const { return BoundarySkinningManager.IsValid() && BoundarySkinningManager->IsBoundaryAdhesionEnabled(); }

	/** Get boundary particle count */
	int32 GetBoundaryParticleCount() const { return BoundarySkinningManager.IsValid() ? BoundarySkinningManager->GetBoundaryParticleCount() : 0; }

	//=============================================================================
	// GPU Boundary Skinning (Delegated to FGPUBoundarySkinningManager)
	// Stores bone-local boundary particles once, transforms to world space on GPU each frame
	//=============================================================================

	/**
	 * Upload local boundary particles (persistent, upload once)
	 * Call this when FluidInteractionComponent generates boundary particles
	 * @param OwnerID - Unique ID for this interaction component
	 * @param LocalParticles - Bone-local boundary particles
	 */
	void UploadLocalBoundaryParticles(int32 OwnerID, const TArray<FGPUBoundaryParticleLocal>& LocalParticles);

	/**
	 * Upload bone transforms for boundary skinning (call each frame)
	 * @param OwnerID - Unique ID for this interaction component
	 * @param BoneTransforms - Bone transforms in world space
	 * @param ComponentTransform - Fallback transform for static meshes
	 */
	void UploadBoneTransformsForBoundary(int32 OwnerID, const TArray<FMatrix44f>& BoneTransforms, const FMatrix44f& ComponentTransform);

	/**
	 * Remove boundary skinning data for an owner
	 * Call when FluidInteractionComponent is destroyed
	 * @param OwnerID - Unique ID of the interaction component to remove
	 */
	void RemoveBoundarySkinningData(int32 OwnerID);

	/**
	 * Clear all boundary skinning data
	 */
	void ClearAllBoundarySkinningData();

	/**
	 * Check if GPU boundary skinning is enabled (has any local boundary particles)
	 */
	bool IsGPUBoundarySkinningEnabled() const { return BoundarySkinningManager.IsValid() && BoundarySkinningManager->IsGPUBoundarySkinningEnabled(); }

	/**
	 * Get total local boundary particle count across all owners
	 */
	int32 GetTotalLocalBoundaryParticleCount() const { return BoundarySkinningManager.IsValid() ? BoundarySkinningManager->GetTotalLocalBoundaryParticleCount() : 0; }

	//=============================================================================
	// Stream Compaction (Phase 2 - Per-Polygon Collision)
	// GPU AABB filtering using parallel prefix sum
	//=============================================================================

	/**
	 * Execute AABB filtering on GPU
	 * Filters particles inside any of the provided AABBs using Stream Compaction
	 * @param FilterAABBs - Array of AABBs to filter against
	 */
	void ExecuteAABBFiltering(const TArray<FGPUFilterAABB>& FilterAABBs);

	/**
	 * Get filtered candidate particles (GPU→CPU readback)
	 * Call after ExecuteAABBFiltering to get results
	 * @param OutCandidates - Output array of candidate particles
	 * @return true if candidates are available
	 */
	bool GetFilteredCandidates(TArray<FGPUCandidateParticle>& OutCandidates);

	/**
	 * Get count of filtered candidates
	 */
	int32 GetFilteredCandidateCount() const { return StreamCompactionManager.IsValid() ? StreamCompactionManager->GetFilteredCandidateCount() : 0; }

	/**
	 * Check if filtered candidates are available
	 */
	bool HasFilteredCandidates() const { return StreamCompactionManager.IsValid() && StreamCompactionManager->HasFilteredCandidates(); }

	/**
	 * Apply particle corrections from CPU Per-Polygon collision processing
	 * Uploads corrections to GPU and applies them via compute shader
	 * @param Corrections - Array of position corrections from Per-Polygon collision
	 */
	void ApplyCorrections(const TArray<FParticleCorrection>& Corrections);

	/**
	 * Apply attached particle position updates
	 * Updates positions for particles attached to skeletal mesh surfaces
	 * Also handles detachment with velocity setting
	 * @param Updates - Array of position updates from attachment system
	 */
	void ApplyAttachmentUpdates(const TArray<FAttachedParticleUpdate>& Updates);

	//=============================================================================
	// Collision Feedback (Delegated to FGPUCollisionManager)
	//=============================================================================

	/** Enable or disable collision feedback recording */
	void SetCollisionFeedbackEnabled(bool bEnabled) { if (CollisionManager.IsValid()) CollisionManager->SetCollisionFeedbackEnabled(bEnabled); }

	/** Check if collision feedback is enabled */
	bool IsCollisionFeedbackEnabled() const { return CollisionManager.IsValid() && CollisionManager->IsCollisionFeedbackEnabled(); }

	/** Get collision feedback for a specific collider */
	bool GetCollisionFeedbackForCollider(int32 ColliderIndex, TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
	{
		if (!CollisionManager.IsValid()) { OutFeedback.Reset(); OutCount = 0; return false; }
		return CollisionManager->GetCollisionFeedbackForCollider(ColliderIndex, OutFeedback, OutCount);
	}

	/** Get all collision feedback (unfiltered) */
	bool GetAllCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
	{
		if (!CollisionManager.IsValid()) { OutFeedback.Reset(); OutCount = 0; return false; }
		return CollisionManager->GetAllCollisionFeedback(OutFeedback, OutCount);
	}

	/** Get current collision feedback count */
	int32 GetCollisionFeedbackCount() const { return CollisionManager.IsValid() ? CollisionManager->GetCollisionFeedbackCount() : 0; }

	//=============================================================================
	// Collider Contact Count API (Delegated to FGPUCollisionManager)
	//=============================================================================

	/** Get contact count for a specific collider index */
	int32 GetColliderContactCount(int32 ColliderIndex) const { return CollisionManager.IsValid() ? CollisionManager->GetColliderContactCount(ColliderIndex) : 0; }

	/** Get all collider contact counts */
	void GetAllColliderContactCounts(TArray<int32>& OutCounts) const { if (CollisionManager.IsValid()) CollisionManager->GetAllColliderContactCounts(OutCounts); else OutCounts.Empty(); }

	/** Get total collider count */
	int32 GetTotalColliderCount() const { return CollisionManager.IsValid() ? CollisionManager->GetTotalColliderCount() : 0; }

	/** Get total contact count for all colliders with a specific OwnerID */
	int32 GetContactCountForOwner(int32 OwnerID) const { return CollisionManager.IsValid() ? CollisionManager->GetContactCountForOwner(OwnerID) : 0; }

	//=============================================================================
	// GPU Particle Spawning (Thread-Safe)
	// CPU sends spawn requests, GPU creates particles via atomic counter
	// This eliminates race conditions between game thread and render thread
	//=============================================================================

	/**
	 * Add a spawn request (thread-safe, call from any thread)
	 * The request is queued and processed on the next simulation frame
	 * @param Position - World position to spawn at
	 * @param Velocity - Initial velocity
	 * @param Mass - Particle mass (default 1.0f)
	 */
	void AddSpawnRequest(const FVector3f& Position, const FVector3f& Velocity, float Mass = 1.0f);

	/**
	 * Add multiple spawn requests at once (thread-safe, more efficient than individual calls)
	 * @param Requests - Array of spawn requests to add
	 */
	void AddSpawnRequests(const TArray<FGPUSpawnRequest>& Requests);

	/**
	 * Clear all pending spawn requests
	 */
	void ClearSpawnRequests();

	/**
	 * Get number of pending spawn requests
	 */
	int32 GetPendingSpawnCount() const;

	/**
	 * Set default particle radius for spawning
	 */
	void SetDefaultSpawnRadius(float Radius) { if (SpawnManager.IsValid()) SpawnManager->SetDefaultRadius(Radius); }

	/**
	 * Set default particle mass for spawning (used when spawn request Mass = 0)
	 */
	void SetDefaultSpawnMass(float Mass) { if (SpawnManager.IsValid()) SpawnManager->SetDefaultMass(Mass); }

	/**
	 * Get next particle ID to assign
	 */
	int32 GetNextParticleID() const { return SpawnManager.IsValid() ? SpawnManager->GetNextParticleID() : 0; }

private:
	//=============================================================================
	// Internal Methods
	//=============================================================================

	/** Resize GPU buffers to new capacity */
	void ResizeBuffers(FRHICommandListBase& RHICmdList, int32 NewCapacity);

	/** Convert CPU particle to GPU format */
	static FGPUFluidParticle ConvertToGPU(const FFluidParticle& CPUParticle);

	/** Update CPU particle from GPU data */
	static void ConvertFromGPU(FFluidParticle& OutCPUParticle, const FGPUFluidParticle& GPUParticle);

	//=============================================================================
	// RDG Pass Helpers
	//=============================================================================

	/** Create RDG buffer from RHI buffer */
	FRDGBufferRef RegisterExternalBuffer(FRDGBuilder& GraphBuilder, const TCHAR* Name);

	/** Add predict positions pass */
	void AddPredictPositionsPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add spatial hash build pass (uses existing FluidSpatialHashBuild) */
	void AddSpatialHashBuildPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef PositionsSRV,
		FRDGBufferUAVRef CellCountsUAV,
		FRDGBufferUAVRef InParticleIndicesUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add combined density + pressure pass (single neighbor traversal + neighbor caching) */
	void AddSolveDensityPressurePass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		FRDGBufferSRVRef CellStartSRV,
		FRDGBufferSRVRef CellEndSRV,
		FRDGBufferUAVRef NeighborListUAV,
		FRDGBufferUAVRef NeighborCountsUAV,
		int32 IterationIndex,
		const FGPUFluidSimulationParams& Params);

	/** Add apply viscosity pass */
	void AddApplyViscosityPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		FRDGBufferSRVRef NeighborListSRV,
		FRDGBufferSRVRef NeighborCountsSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add apply cohesion pass (surface tension / cohesion forces) */
	void AddApplyCohesionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		FRDGBufferSRVRef NeighborListSRV,
		FRDGBufferSRVRef NeighborCountsSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add bounds collision pass */
	void AddBoundsCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add distance field collision pass */
	void AddDistanceFieldCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add primitive collision pass (spheres, capsules, boxes, convexes) */
	void AddPrimitiveCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		const FGPUFluidSimulationParams& Params);

	//-------------------------------------------------------------------------
	// Collision Feedback Buffer Management (delegated to CollisionFeedbackManager)
	//-------------------------------------------------------------------------

	/** Allocate collision feedback readback buffers */
	void AllocateCollisionFeedbackBuffers(FRHICommandListImmediate& RHICmdList);

	/** Release collision feedback buffers */
	void ReleaseCollisionFeedbackBuffers();

	/** Process collision feedback readback (non-blocking) */
	void ProcessCollisionFeedbackReadback(FRHICommandListImmediate& RHICmdList);

	/** Process collider contact count readback (non-blocking) */
	void ProcessColliderContactCountReadback(FRHICommandListImmediate& RHICmdList);

	/** Add finalize positions pass */
	void AddFinalizePositionsPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add extract positions pass (for spatial hash) */
	void AddExtractPositionsPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef ParticlesSRV,
		FRDGBufferUAVRef PositionsUAV,
		int32 ParticleCount,
		bool bUsePredictedPosition);

	/** Add boundary adhesion pass (Flex-style adhesion to surface particles) */
	void AddBoundaryAdhesionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add boundary skinning pass (GPU transform of bone-local particles to world space) */
	void AddBoundarySkinningPass(
		FRDGBuilder& GraphBuilder,
		const FGPUFluidSimulationParams& Params);

	//=============================================================================
	// Z-Order (Morton Code) Sorting Pipeline (Delegated to FGPUSpatialHashManager)
	//=============================================================================

	/** Execute Z-Order sorting pipeline (delegates to SpatialHashManager) */
	FRDGBufferRef ExecuteZOrderSortingPipeline(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef InParticleBuffer,
		FRDGBufferUAVRef& OutCellStartUAV,
		FRDGBufferSRVRef& OutCellStartSRV,
		FRDGBufferUAVRef& OutCellEndUAV,
		FRDGBufferSRVRef& OutCellEndSRV,
		const FGPUFluidSimulationParams& Params);

private:
	//=============================================================================
	// GPU Buffers
	//=============================================================================

	// Main particle buffer
	FBufferRHIRef ParticleBufferRHI;
	FShaderResourceViewRHIRef ParticleSRV;
	FUnorderedAccessViewRHIRef ParticleUAV;

	// Position-only buffer for spatial hash
	FBufferRHIRef PositionBufferRHI;
	FShaderResourceViewRHIRef PositionSRV;
	FUnorderedAccessViewRHIRef PositionUAV;

	// Staging buffer for CPU readback
	FBufferRHIRef StagingBufferRHI;

	// Spatial hash buffers
	FBufferRHIRef CellCountsBufferRHI;
	FShaderResourceViewRHIRef CellCountsSRV;
	FUnorderedAccessViewRHIRef CellCountsUAV;

	FBufferRHIRef ParticleIndicesBufferRHI;
	FShaderResourceViewRHIRef ParticleIndicesSRV;
	FUnorderedAccessViewRHIRef ParticleIndicesUAV;

	//=============================================================================
	// State
	//=============================================================================

	bool bIsInitialized;
	int32 MaxParticleCount;
	int32 CurrentParticleCount;

	// Cached GPU particle data for upload/readback
	TArray<FGPUFluidParticle> CachedGPUParticles;

	// Separate buffer for GPU readback results (to avoid race with upload)
	TArray<FGPUFluidParticle> ReadbackGPUParticles;

	// Flag indicating valid GPU results are available for download
	std::atomic<bool> bHasValidGPUResults{false};

	// Persistent GPU buffer - reused across frames (Phase 2)
	// After simulation, this contains the results to be used next frame
	TRefCountPtr<FRDGPooledBuffer> PersistentParticleBuffer;

	// Particle count that matches PersistentParticleBuffer (updated in render thread)
	// This ensures buffer and count are always synchronized
	std::atomic<int32> PersistentParticleCount{0};

	// Persistent Spatial Hash buffers - reused across frames (GPU clear instead of CPU upload)
	TRefCountPtr<FRDGPooledBuffer> PersistentCellCountsBuffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentParticleIndicesBuffer;

	// Neighbor caching buffers - reuse neighbor list across solver iterations
	// NeighborList: [ParticleCount * MAX_NEIGHBORS_PER_PARTICLE] - cached neighbor indices
	// NeighborCounts: [ParticleCount] - number of neighbors per particle
	TRefCountPtr<FRDGPooledBuffer> NeighborListBuffer;
	TRefCountPtr<FRDGPooledBuffer> NeighborCountsBuffer;
	int32 NeighborBufferParticleCapacity = 0;

	// Simulation bounds (local copy for GetSimulationBounds API)
	FVector3f SimulationBoundsMin = FVector3f(-1280.0f, -1280.0f, -1280.0f);
	FVector3f SimulationBoundsMax = FVector3f(1280.0f, 1280.0f, 1280.0f);

	// Flag: need to upload all particles from CPU (initial or after resize)
	bool bNeedsFullUpload = true;

	// Previous frame particle count (to detect new particles)
	int32 PreviousParticleCount = 0;

	// New particles to append (only the new ones, not all particles)
	TArray<FGPUFluidParticle> NewParticlesToAppend;

	// Count of new particles to append this frame
	int32 NewParticleCount = 0;

	//=============================================================================
	// Configuration
	//=============================================================================

	FVector3f ExternalForce;
	float MaxVelocity;       // Safety clamp to prevent divergence (default: 50000 cm/s = 500 m/s)

	// Anisotropy parameters
	FFluidAnisotropyParams CachedAnisotropyParams;

	// Anisotropy buffers (computed by FluidAnisotropyCS, used by FluidDepthPass)
	TRefCountPtr<FRDGPooledBuffer> PersistentAnisotropyAxis1Buffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentAnisotropyAxis2Buffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentAnisotropyAxis3Buffer;
	int32 AnisotropyFrameCounter = 0;  // Frame counter for UpdateInterval optimization

	// Critical section for thread-safe buffer access
	FCriticalSection BufferLock;

	//=============================================================================
	// Rendering Buffers
	//=============================================================================

	// Previous positions buffer for rendering history trails
	TRefCountPtr<FRDGPooledBuffer> PreviousPositionsBuffer;
	int32 PreviousPositionsCount = 0;
	bool bPreviousPositionsValid = false;

	//=============================================================================
	// GPU Particle Spawn System (Delegated to FGPUSpawnManager)
	//=============================================================================

	// SpawnManager handles all spawn-related functionality
	// Thread-safe spawn request queue processed on render thread
	TUniquePtr<FGPUSpawnManager> SpawnManager;

	// GPU Counter buffer for atomic particle count (used during spawn pass)
	TRefCountPtr<FRDGPooledBuffer> ParticleCounterBuffer;

	//=============================================================================
	// Stream Compaction (Delegated to FGPUStreamCompactionManager)
	// GPU AABB filtering using parallel prefix sum for per-polygon collision
	//=============================================================================

	// StreamCompactionManager handles all AABB filtering and correction functionality
	TUniquePtr<FGPUStreamCompactionManager> StreamCompactionManager;

	//=============================================================================
	// Collision System (Delegated to FGPUCollisionManager)
	// Bounds, DistanceField, Primitive collision + Feedback
	//=============================================================================

	// CollisionManager handles all collision passes and feedback
	TUniquePtr<FGPUCollisionManager> CollisionManager;

	//=============================================================================
	// Spatial Hash (Delegated to FGPUSpatialHashManager)
	// Z-Order Morton code sorting for cache-coherent neighbor search
	//=============================================================================

	TUniquePtr<FGPUSpatialHashManager> SpatialHashManager;

	//=============================================================================
	// Boundary Skinning (Delegated to FGPUBoundarySkinningManager)
	// GPU-based boundary skinning and adhesion
	//=============================================================================

	TUniquePtr<FGPUBoundarySkinningManager> BoundarySkinningManager;

	//=============================================================================
	// Adhesion System (Delegated to FGPUAdhesionManager)
	// GPU-based particle adhesion to bone colliders
	//=============================================================================

	TUniquePtr<FGPUAdhesionManager> AdhesionManager;
};

/**
 * Utility class for performing GPU simulation from render thread
 */
class KAWAIIFLUIDRUNTIME_API FGPUFluidSimulationTask
{
public:
	/**
	 * Execute simulation on render thread
	 * @param Simulator - GPU simulator instance
	 * @param Params - Simulation parameters
	 * @param NumSubsteps - Number of substeps to run
	 */
	static void Execute(
		FGPUFluidSimulator* Simulator,
		const FGPUFluidSimulationParams& Params,
		int32 NumSubsteps = 1);
};
