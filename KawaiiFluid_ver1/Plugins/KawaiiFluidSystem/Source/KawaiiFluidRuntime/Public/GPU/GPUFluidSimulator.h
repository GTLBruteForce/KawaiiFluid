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
#include "GPU/Managers/GPUZOrderSortManager.h"
#include "GPU/Managers/GPUBoundarySkinningManager.h"
#include "GPU/Managers/GPUAdhesionManager.h"
#include "GPU/Managers/GPUStaticBoundaryManager.h"
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
	 * Synchronous version of GetAllGPUParticles
	 * Blocks until GPU readback completes - use for PIE/Save when async readback isn't ready
	 * @param OutParticles - Output array (will be populated with all GPU particles)
	 * @return true if valid GPU data was retrieved
	 */
	bool GetAllGPUParticlesSync(TArray<FFluidParticle>& OutParticles);

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
		FScopeLock Lock(&BufferLock);

		CurrentParticleCount = 0;
		NewParticleCount = 0;
		NewParticlesToAppend.Empty();
		CachedGPUParticles.Empty();
		bNeedsFullUpload = true;
		InvalidatePreviousPositions();

		// SpawnManager 상태도 리셋 (NextParticleID, AlreadyRequestedIDs 등)
		if (SpawnManager)
		{
			SpawnManager->Reset();
		}
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

	/** Get persistent Z-Order CellStart buffer (for Ray Marching volume building) */
	TRefCountPtr<FRDGPooledBuffer> GetPersistentCellStartBuffer() const { return PersistentCellStartBuffer; }

	/** Get persistent Z-Order CellEnd buffer (for Ray Marching volume building) */
	TRefCountPtr<FRDGPooledBuffer> GetPersistentCellEndBuffer() const { return PersistentCellEndBuffer; }

	/** Check if Z-Order buffers are valid for Ray Marching */
	bool HasValidZOrderBuffers() const
	{
		return PersistentCellStartBuffer.IsValid() && PersistentCellEndBuffer.IsValid();
	}

	/**
	 * Enable/disable Z-Order buffer extraction for Ray Marching
	 * Only Ray Marching pipeline should enable this to avoid unnecessary GPU memory copies
	 * @param bEnable true to extract CellStart/CellEnd buffers after simulation
	 */
	void SetExtractZOrderBuffersForRayMarching(bool bEnable) { bExtractZOrderBuffersForRayMarching = bEnable; }

	/** Check if Z-Order buffer extraction is enabled */
	bool IsExtractZOrderBuffersEnabled() const { return bExtractZOrderBuffersForRayMarching; }

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
		// Also update ZOrderSortManager
		if (ZOrderSortManager.IsValid())
		{
			ZOrderSortManager->SetSimulationBounds(BoundsMin, BoundsMax);
		}
	}

	/** Get current simulation bounds */
	void GetSimulationBounds(FVector3f& OutMin, FVector3f& OutMax) const
	{
		OutMin = SimulationBoundsMin;
		OutMax = SimulationBoundsMax;
	}

	/** Get cached CellSize from last simulation (for Ray Marching volume building) */
	float GetCellSize() const { return CachedCellSize; }

	/**
	 * Set grid resolution preset for Z-Order sorting shader permutation
	 * @param Preset - Grid resolution preset (Small/Medium/Large)
	 */
	void SetGridResolutionPreset(EGridResolutionPreset Preset)
	{
		if (ZOrderSortManager.IsValid())
		{
			ZOrderSortManager->SetGridResolutionPreset(Preset);
		}
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

	/** Set boundary adhesion parameters */
	void SetBoundaryAdhesionParams(const FGPUBoundaryAdhesionParams& Params)
	{
		if (BoundarySkinningManager.IsValid()) { BoundarySkinningManager->SetBoundaryAdhesionParams(Params); }
	}

	/** Get boundary adhesion parameters */
	const FGPUBoundaryAdhesionParams& GetBoundaryAdhesionParams() const;

	/** Check if boundary adhesion is enabled */
	bool IsBoundaryAdhesionEnabled() const { return BoundarySkinningManager.IsValid() && BoundarySkinningManager->IsBoundaryAdhesionEnabled(); }

	/** Get total boundary particle count (Skinned + Static) */
	int32 GetBoundaryParticleCount() const
	{
		if (!BoundarySkinningManager.IsValid()) return 0;
		return BoundarySkinningManager->GetTotalLocalBoundaryParticleCount() + BoundarySkinningManager->GetStaticBoundaryParticleCount();
	}

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
	 * Update AABB for a boundary owner (call each frame)
	 * Used for early-out optimization in boundary adhesion pass
	 * @param OwnerID - Unique ID for this interaction component
	 * @param AABB - World-space AABB of the boundary owner
	 */
	void UpdateBoundaryOwnerAABB(int32 OwnerID, const FGPUBoundaryOwnerAABB& AABB);

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
	// Static Boundary Particles (Delegated to FGPUStaticBoundaryManager)
	// Generates boundary particles on static mesh colliders for density contribution
	//=============================================================================

	/**
	 * Generate boundary particles from static collision primitives
	 * Call this after uploading collision primitives to generate boundary particles
	 * for proper density calculation near walls/floors (Akinci 2012)
	 * @param SmoothingRadius - Fluid smoothing radius (for spacing calculation)
	 * @param RestDensity - Fluid rest density (for Psi calculation)
	 */
	void GenerateStaticBoundaryParticles(float SmoothingRadius, float RestDensity);

	/**
	 * Clear all static boundary particles
	 */
	void ClearStaticBoundaryParticles();

	/**
	 * Check if static boundary particles are available
	 */
	bool HasStaticBoundaryParticles() const { return StaticBoundaryManager.IsValid() && StaticBoundaryManager->HasBoundaryParticles(); }

	/**
	 * Get static boundary particle count
	 */
	int32 GetStaticBoundaryParticleCount() const { return StaticBoundaryManager.IsValid() ? StaticBoundaryManager->GetBoundaryParticleCount() : 0; }

	/**
	 * Enable/disable static boundary generation
	 */
	void SetStaticBoundaryEnabled(bool bEnabled) { if (StaticBoundaryManager.IsValid()) StaticBoundaryManager->SetEnabled(bEnabled); }

	/**
	 * Check if static boundary generation is enabled
	 */
	bool IsStaticBoundaryEnabled() const { return StaticBoundaryManager.IsValid() && StaticBoundaryManager->IsEnabled(); }

	/**
	 * Set static boundary particle spacing in cm
	 */
	void SetStaticBoundaryParticleSpacing(float Spacing) { if (StaticBoundaryManager.IsValid()) StaticBoundaryManager->SetParticleSpacing(Spacing); }

	/**
	 * Check if static boundary GPU processing is enabled (BoundarySkinningManager flag)
	 */
	bool IsGPUStaticBoundaryEnabled() const { return BoundarySkinningManager.IsValid() && BoundarySkinningManager->IsStaticBoundaryEnabled(); }

	/**
	 * Get static boundary particles (for debug visualization)
	 */
	const TArray<FGPUBoundaryParticle>& GetStaticBoundaryParticles() const
	{
		static TArray<FGPUBoundaryParticle> Empty;
		return StaticBoundaryManager.IsValid() ? StaticBoundaryManager->GetBoundaryParticles() : Empty;
	}

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
	 * Add despawn requests by particle IDs (thread-safe)
	 * Uses binary search on GPU for O(log n) matching per particle
	 * @param ParticleIDs - Array of particle IDs to despawn
	 * @param AllCurrentReadbackIDs - All particle IDs in current readback (for cleanup)
	 */
	void AddDespawnByIDRequests(const TArray<int32>& ParticleIDs, const TArray<int32>& AllCurrentReadbackIDs);

	/**
	 * Get readback GPU particle data (thread-safe)
	 * Returns false if no valid GPU results available
	 * @param OutParticles - Output array of GPU particles with ParticleID
	 * @return true if valid data was copied
	 */
	bool GetReadbackGPUParticles(TArray<FGPUFluidParticle>& OutParticles);

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

	/**
	 * Set next particle ID (for PIE transition - sync with uploaded particles)
	 */
	void SetNextParticleID(int32 ID) { if (SpawnManager.IsValid()) SpawnManager->SetNextParticleID(ID); }

	/**
	 * Get the spawn manager (for per-source particle count tracking)
	 */
	FGPUSpawnManager* GetSpawnManager() const { return SpawnManager.Get(); }

private:
	//=============================================================================
	// Internal Methods
	//=============================================================================

	/** Struct to hold shared spatial data for simulation passes */
	struct FSimulationSpatialData
	{
		// Hash Table buffers (Legacy / Compatibility)
		FRDGBufferRef CellCountsBuffer = nullptr;
		FRDGBufferRef ParticleIndicesBuffer = nullptr;
		FRDGBufferSRVRef CellCountsSRV = nullptr;
		FRDGBufferSRVRef ParticleIndicesSRV = nullptr;

		// Z-Order buffers (Sorted)
		FRDGBufferRef CellStartBuffer = nullptr;
		FRDGBufferRef CellEndBuffer = nullptr;
		FRDGBufferSRVRef CellStartSRV = nullptr;
		FRDGBufferSRVRef CellEndSRV = nullptr;

		// Neighbor Cache buffers
		FRDGBufferRef NeighborListBuffer = nullptr;
		FRDGBufferRef NeighborCountsBuffer = nullptr;
		FRDGBufferSRVRef NeighborListSRV = nullptr;
		FRDGBufferSRVRef NeighborCountsSRV = nullptr;

		// Skinned Boundary Particle buffers (SkeletalMesh - same-frame)
		// Created in AddBoundarySkinningPass, used in AddSolveDensityPressurePass
		FRDGBufferRef SkinnedBoundaryBuffer = nullptr;
		FRDGBufferSRVRef SkinnedBoundarySRV = nullptr;
		int32 SkinnedBoundaryParticleCount = 0;
		bool bSkinnedBoundaryPerformed = false;

		// Skinned Boundary Z-Order buffers (same-frame)
		FRDGBufferRef SkinnedZOrderSortedBuffer = nullptr;
		FRDGBufferRef SkinnedZOrderCellStartBuffer = nullptr;
		FRDGBufferRef SkinnedZOrderCellEndBuffer = nullptr;
		FRDGBufferSRVRef SkinnedZOrderSortedSRV = nullptr;
		FRDGBufferSRVRef SkinnedZOrderCellStartSRV = nullptr;
		FRDGBufferSRVRef SkinnedZOrderCellEndSRV = nullptr;
		int32 SkinnedZOrderParticleCount = 0;
		bool bSkinnedZOrderPerformed = false;

		// Static Boundary Particle buffers (StaticMesh - persistent GPU)
		// Cached on GPU, only re-sorted when dirty
		FRDGBufferSRVRef StaticBoundarySRV = nullptr;
		FRDGBufferSRVRef StaticZOrderSortedSRV = nullptr;
		FRDGBufferSRVRef StaticZOrderCellStartSRV = nullptr;
		FRDGBufferSRVRef StaticZOrderCellEndSRV = nullptr;
		int32 StaticBoundaryParticleCount = 0;
		bool bStaticBoundaryAvailable = false;

		// Legacy aliases (for backward compatibility during transition)
		FRDGBufferRef& WorldBoundaryBuffer = SkinnedBoundaryBuffer;
		FRDGBufferSRVRef& WorldBoundarySRV = SkinnedBoundarySRV;
		int32& WorldBoundaryParticleCount = SkinnedBoundaryParticleCount;
		bool& bBoundarySkinningPerformed = bSkinnedBoundaryPerformed;

		FRDGBufferRef& BoundaryZOrderSortedBuffer = SkinnedZOrderSortedBuffer;
		FRDGBufferRef& BoundaryZOrderCellStartBuffer = SkinnedZOrderCellStartBuffer;
		FRDGBufferRef& BoundaryZOrderCellEndBuffer = SkinnedZOrderCellEndBuffer;
		FRDGBufferSRVRef& BoundaryZOrderSortedSRV = SkinnedZOrderSortedSRV;
		FRDGBufferSRVRef& BoundaryZOrderCellStartSRV = SkinnedZOrderCellStartSRV;
		FRDGBufferSRVRef& BoundaryZOrderCellEndSRV = SkinnedZOrderCellEndSRV;
		int32& BoundaryZOrderParticleCount = SkinnedZOrderParticleCount;
		bool& bBoundaryZOrderPerformed = bSkinnedZOrderPerformed;
	};

	/** Phase 1: Prepare particle buffer (Spawn, Upload, Reuse, Append) */
	FRDGBufferRef PrepareParticleBuffer(
		FRDGBuilder& GraphBuilder,
		const FGPUFluidSimulationParams& Params,
		int32 SpawnCount);

	/** Phase 2: Build spatial structures (Z-Order Sort or Hash Table) */
	FSimulationSpatialData BuildSpatialStructures(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef& InOutParticleBuffer,
		FRDGBufferSRVRef& OutParticlesSRV,
		FRDGBufferUAVRef& OutParticlesUAV,
		FRDGBufferSRVRef& OutPositionsSRV,
		FRDGBufferUAVRef& OutPositionsUAV,
		const FGPUFluidSimulationParams& Params);

	/** Phase 3: Execute physics solver (Predict, Density/Pressure Loop) */
	void ExecutePhysicsSolver(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	/** Phase 4: Execute collision and adhesion passes */
	void ExecuteCollisionAndAdhesion(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		const FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	/** Phase 5: Execute post-simulation passes (Viscosity, Finalize, Anisotropy) */
	void ExecutePostSimulation(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef ParticleBuffer,
		FRDGBufferUAVRef ParticlesUAV,
		const FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	/** Phase 6: Extract persistent buffers for next frame */
	void ExtractPersistentBuffers(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef ParticleBuffer,
		const FSimulationSpatialData& SpatialData);

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
		const FGPUFluidSimulationParams& Params,
		const FSimulationSpatialData& SpatialData);

	/** Add apply viscosity pass */
	void AddApplyViscosityPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		FRDGBufferSRVRef NeighborListSRV,
		FRDGBufferSRVRef NeighborCountsSRV,
		const FGPUFluidSimulationParams& Params,
		const FSimulationSpatialData& SpatialData);

	/** Add particle sleeping pass (NVIDIA Flex stabilization technique) */
	void AddParticleSleepingPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferUAVRef SleepCountersUAV,
		FRDGBufferSRVRef NeighborListSRV,
		FRDGBufferSRVRef NeighborCountsSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add combined viscosity and cohesion pass (optimized single-loop version) */
	void AddApplyViscosityAndCohesionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		FRDGBufferSRVRef NeighborListSRV,
		FRDGBufferSRVRef NeighborCountsSRV,
		const FGPUFluidSimulationParams& Params,
		const FSimulationSpatialData& SpatialData);

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

	/**
	 * 즉시 PersistentParticleBuffer 생성 (시뮬레이션 없이 렌더링 가능하도록)
	 * UploadParticles 후 Simulate() 호출 전에도 렌더링할 수 있게 함
	 */
	void CreateImmediatePersistentBuffer();

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
		const FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	/** Add boundary skinning pass (GPU transform of bone-local particles to world space) */
	void AddBoundarySkinningPass(
		FRDGBuilder& GraphBuilder,
		FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	//=============================================================================
	// Z-Order (Morton Code) Sorting Pipeline (Delegated to FGPUZOrderSortManager)
	//=============================================================================

	/** Execute Z-Order sorting pipeline (delegates to ZOrderSortManager) */
	FRDGBufferRef ExecuteZOrderSortingPipeline(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef InParticleBuffer,
		FRDGBufferUAVRef& OutCellStartUAV,
		FRDGBufferSRVRef& OutCellStartSRV,
		FRDGBufferUAVRef& OutCellEndUAV,
		FRDGBufferSRVRef& OutCellEndSRV,
		FRDGBufferRef& OutCellStartBuffer,
		FRDGBufferRef& OutCellEndBuffer,
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

	// Persistent Spatial Hash buffers - reused across frames (GPU clear instead of CPU upload)
	TRefCountPtr<FRDGPooledBuffer> PersistentCellCountsBuffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentParticleIndicesBuffer;

	// Persistent Z-Order buffers - for Ray Marching volume building
	// Extracted after simulation when Z-Order sorting is enabled AND bExtractZOrderBuffersForRayMarching is true
	TRefCountPtr<FRDGPooledBuffer> PersistentCellStartBuffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentCellEndBuffer;

	// Flag to control Z-Order buffer extraction (only Ray Marching pipeline should enable this)
	// Default false to avoid unnecessary GPU memory copies for SSFR pipeline
	bool bExtractZOrderBuffersForRayMarching = false;

	// Neighbor caching buffers - reuse neighbor list across solver iterations
	// NeighborList: [ParticleCount * MAX_NEIGHBORS_PER_PARTICLE] - cached neighbor indices
	// NeighborCounts: [ParticleCount] - number of neighbors per particle
	TRefCountPtr<FRDGPooledBuffer> NeighborListBuffer;
	TRefCountPtr<FRDGPooledBuffer> NeighborCountsBuffer;
	int32 NeighborBufferParticleCapacity = 0;

	// Particle Sleeping (NVIDIA Flex stabilization)
	TRefCountPtr<FRDGPooledBuffer> SleepCountersBuffer;
	int32 SleepCountersCapacity = 0;

	// Simulation bounds (local copy for GetSimulationBounds API)
	FVector3f SimulationBoundsMin = FVector3f(-1280.0f, -1280.0f, -1280.0f);
	FVector3f SimulationBoundsMax = FVector3f(1280.0f, 1280.0f, 1280.0f);

	// Cached CellSize from last simulation (for Ray Marching volume building)
	float CachedCellSize = 0.0f;

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
	// Z-Order Sorting (Delegated to FGPUZOrderSortManager)
	// Z-Order Morton code sorting for cache-coherent neighbor search
	//=============================================================================

	TUniquePtr<FGPUZOrderSortManager> ZOrderSortManager;

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

	//=============================================================================
	// Static Boundary Particles (Delegated to FGPUStaticBoundaryManager)
	// Generates boundary particles on static colliders for density contribution
	//=============================================================================

	TUniquePtr<FGPUStaticBoundaryManager> StaticBoundaryManager;

	//=============================================================================
	// Shadow Position Readback (Async GPU→CPU for HISM Shadow Instances)
	// Uses FRHIGPUBufferReadback for non-blocking readback (2-3 frame latency)
	//=============================================================================

	static constexpr int32 NUM_SHADOW_READBACK_BUFFERS = 3;  // Triple buffering

	/** Async readback objects for position data */
	FRHIGPUBufferReadback* ShadowPositionReadbacks[NUM_SHADOW_READBACK_BUFFERS] = { nullptr };

	/** Current write index for ring buffer (round-robin: 0 → 1 → 2 → 0 ...) */
	int32 ShadowReadbackWriteIndex = 0;

	/**
	 * Buffer index from the most recent ProcessShadowReadback() call.
	 * ProcessAnisotropyReadback() uses this to read from the same buffer,
	 * ensuring position and anisotropy data are from the same frame.
	 * Reset to -1 after anisotropy is processed to prevent duplicate reads.
	 */
	int32 LastProcessedShadowReadbackIndex = -1;

	/** Frame number tracking for each readback buffer */
	uint64 ShadowReadbackFrameNumbers[NUM_SHADOW_READBACK_BUFFERS] = { 0 };

	/** Particle count for each readback buffer */
	int32 ShadowReadbackParticleCounts[NUM_SHADOW_READBACK_BUFFERS] = { 0 };

	/** Ready positions (copied from completed readback) */
	TArray<FVector3f> ReadyShadowPositions;

	/** Ready velocities (copied from completed readback, for prediction) */
	TArray<FVector3f> ReadyShadowVelocities;

	/** Ready anisotropy data (copied from completed readback, for ellipsoid shadows) */
	TArray<FVector4f> ReadyShadowAnisotropyAxis1;
	TArray<FVector4f> ReadyShadowAnisotropyAxis2;
	TArray<FVector4f> ReadyShadowAnisotropyAxis3;

	/** Ready neighbor counts (copied from shadow readback, for isolation detection) */
	TArray<uint32> ReadyShadowNeighborCounts;

	/** Async readback objects for anisotropy data (separate from position readback) */
	FRHIGPUBufferReadback* ShadowAnisotropyReadbacks[NUM_SHADOW_READBACK_BUFFERS][3] = { {nullptr} };

	/** Particle count at the time anisotropy readback was enqueued (prevents buffer overrun) */
	int32 ShadowAnisotropyReadbackParticleCounts[NUM_SHADOW_READBACK_BUFFERS] = { 0 };

	/** Frame number of ready positions */
	std::atomic<uint64> ReadyShadowPositionsFrame{0};

	/** Frame number of ready anisotropy (must match ReadyShadowPositionsFrame for valid ellipsoid) */
	std::atomic<uint64> ReadyShadowAnisotropyFrame{0};

	/** Enable flag for shadow readback */
	std::atomic<bool> bShadowReadbackEnabled{false};

	//=============================================================================
	// Stats/Recycle Readback (Async GPU→CPU for ParticleID-based operations)
	// Uses FRHIGPUBufferReadback for non-blocking readback (2-3 frame latency)
	//=============================================================================

	static constexpr int32 NUM_STATS_READBACK_BUFFERS = 3;  // Triple buffering

	/** Async readback objects for full particle data (for ID-based operations) */
	FRHIGPUBufferReadback* StatsReadbacks[NUM_STATS_READBACK_BUFFERS] = { nullptr };

	/** Current write index for stats readback ring buffer */
	int32 StatsReadbackWriteIndex = 0;

	/** Frame number tracking for each stats readback buffer */
	uint64 StatsReadbackFrameNumbers[NUM_STATS_READBACK_BUFFERS] = { 0 };

	/** Particle count for each stats readback buffer */
	int32 StatsReadbackParticleCounts[NUM_STATS_READBACK_BUFFERS] = { 0 };

	/** Enable flag for anisotropy readback (requires bShadowReadbackEnabled) */
	std::atomic<bool> bAnisotropyReadbackEnabled{false};

public:
	//=============================================================================
	// Shadow Position Readback API
	//=============================================================================

	/**
	 * Enable or disable shadow position readback
	 * When enabled, particle positions are asynchronously read back for HISM shadows
	 * @param bEnabled - true to enable async position readback
	 */
	void SetShadowReadbackEnabled(bool bEnabled) { bShadowReadbackEnabled.store(bEnabled); }

	/**
	 * Check if shadow readback is enabled
	 */
	bool IsShadowReadbackEnabled() const { return bShadowReadbackEnabled.load(); }

	/**
	 * Check if shadow positions are ready for use
	 * @return true if async readback has completed and positions are available
	 */
	bool HasReadyShadowPositions() const { return ReadyShadowPositionsFrame.load() > 0 && ReadyShadowPositions.Num() > 0; }

	/**
	 * Get shadow positions (non-blocking, returns previously completed readback)
	 * @param OutPositions - Output array of particle positions
	 * @return true if valid positions were retrieved
	 */
	bool GetShadowPositions(TArray<FVector>& OutPositions) const;

	/**
	 * Get shadow positions and velocities (non-blocking, for prediction)
	 * @param OutPositions - Output array of particle positions
	 * @param OutVelocities - Output array of particle velocities
	 * @return true if valid data was retrieved
	 */
	bool GetShadowPositionsAndVelocities(TArray<FVector>& OutPositions, TArray<FVector>& OutVelocities) const;

	/**
	 * Get shadow position count
	 */
	int32 GetShadowPositionCount() const { return ReadyShadowPositions.Num(); }

	/**
	 * Enable or disable anisotropy readback for ellipsoid shadows
	 * Requires shadow readback to be enabled first
	 * @param bEnabled - true to enable anisotropy readback
	 */
	void SetAnisotropyReadbackEnabled(bool bEnabled) { bAnisotropyReadbackEnabled.store(bEnabled); }

	/**
	 * Check if anisotropy readback is enabled
	 */
	bool IsAnisotropyReadbackEnabled() const { return bAnisotropyReadbackEnabled.load(); }

	/**
	 * Check if anisotropy data is ready for use
	 * @return true if anisotropy readback has completed
	 */
	bool HasReadyAnisotropyData() const { return ReadyShadowAnisotropyAxis1.Num() > 0; }

	/**
	 * Get shadow data with anisotropy (non-blocking, for ellipsoid HISM shadows)
	 * @param OutPositions - Output array of particle positions
	 * @param OutVelocities - Output array of particle velocities
	 * @param OutAnisotropyAxis1 - Output array of first ellipsoid axis (xyz=dir, w=scale)
	 * @param OutAnisotropyAxis2 - Output array of second ellipsoid axis
	 * @param OutAnisotropyAxis3 - Output array of third ellipsoid axis
	 * @return true if valid data was retrieved
	 */
	bool GetShadowDataWithAnisotropy(
		TArray<FVector>& OutPositions,
		TArray<FVector>& OutVelocities,
		TArray<FVector4>& OutAnisotropyAxis1,
		TArray<FVector4>& OutAnisotropyAxis2,
		TArray<FVector4>& OutAnisotropyAxis3) const;

	//=============================================================================
	// Neighbor Count Readback API (for isolation detection)
	// Note: Neighbor counts are automatically included in shadow position readback
	// since FGPUFluidParticle already contains NeighborCount field.
	//=============================================================================

	/**
	 * Check if neighbor count data is ready for use
	 * @return true if shadow readback has completed (neighbor counts are included)
	 */
	bool HasReadyNeighborCountData() const { return ReadyShadowNeighborCounts.Num() > 0; }

	/**
	 * Get neighbor counts (non-blocking, for isolation detection)
	 * @param OutNeighborCounts - Output array of neighbor counts per particle
	 * @return true if valid data was retrieved
	 */
	bool GetShadowNeighborCounts(TArray<int32>& OutNeighborCounts) const;

private:
	/** Allocate shadow readback objects */
	void AllocateShadowReadbackObjects(FRHICommandListImmediate& RHICmdList);

	/** Release shadow readback objects */
	void ReleaseShadowReadbackObjects();

	/** Enqueue shadow position copy to readback buffer */
	void EnqueueShadowPositionReadback(FRHICommandListImmediate& RHICmdList, FRHIBuffer* SourceBuffer, int32 ParticleCount);

	/** Enqueue anisotropy data copy to readback buffer */
	void EnqueueAnisotropyReadback(FRHICommandListImmediate& RHICmdList, int32 ParticleCount);

	/** Process shadow readback (check for completion, copy to ready buffer) */
	void ProcessShadowReadback();

	/** Process anisotropy readback */
	void ProcessAnisotropyReadback();

	//=============================================================================
	// Stats/Recycle Readback Internal Functions
	//=============================================================================

	/** Allocate stats readback objects */
	void AllocateStatsReadbackObjects(FRHICommandListImmediate& RHICmdList);

	/** Release stats readback objects */
	void ReleaseStatsReadbackObjects();

	/** Enqueue stats readback (full particle data for ID-based operations) */
	void EnqueueStatsReadback(FRHICommandListImmediate& RHICmdList, FRHIBuffer* SourceBuffer, int32 ParticleCount);

	/** Process stats readback (check for completion, copy to ReadbackGPUParticles) */
	void ProcessStatsReadback();
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
