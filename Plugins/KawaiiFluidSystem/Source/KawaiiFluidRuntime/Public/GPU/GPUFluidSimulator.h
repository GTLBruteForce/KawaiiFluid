// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIResources.h"
#include "RenderResource.h"
#include "GPU/GPUFluidParticle.h"
#include "GPU/GPUFluidSpatialData.h"
#include "GPU/Managers/GPUSpawnManager.h"
#include "GPU/Managers/GPUCollisionManager.h"
#include "GPU/Managers/GPUZOrderSortManager.h"
#include "GPU/Managers/GPUBoundarySkinningManager.h"
#include "GPU/Managers/GPUAdhesionManager.h"
#include "GPU/Managers/GPUStaticBoundaryManager.h"
#include "GPU/GPUBoundaryAttachment.h"
#include "Core/FluidAnisotropy.h"
#include <atomic>

// Log category
DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);

// Forward declarations
struct FFluidParticle;
class FRDGBuilder;
class FRHIGPUBufferReadback;
class USkeletalMeshComponent;

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
	 * @param bAppend - If true, append to existing particles instead of replacing (for batched multi-component uploads)
	 */
	void UploadParticles(const TArray<FFluidParticle>& CPUParticles, bool bAppend = false);

	/**
	 * Finalize batch upload after multiple UploadParticles(bAppend=true) calls
	 * Creates the GPU persistent buffer from all accumulated CachedGPUParticles
	 * Call this once after all components have finished uploading
	 */
	void FinalizeUpload();

	/**
	 * Clear accumulated particles (call before batch upload begins)
	 * Resets CachedGPUParticles for fresh batch upload
	 */
	void ClearCachedParticles();

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
	 * Get particles filtered by SourceID (for batched simulation)
	 * Only returns particles belonging to the specified source component
	 * @param SourceID - Source component ID to filter by
	 * @param OutParticles - Output array (will be populated with filtered particles)
	 * @return true if valid GPU data was retrieved
	 */
	bool GetParticlesBySourceID(int32 SourceID, TArray<FFluidParticle>& OutParticles);

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

		// Reset double buffered neighbor cache state
		CurrentNeighborBufferIndex = 0;
		bPrevNeighborCacheValid = false;

		// Reset indirect dispatch state
		bEverHadParticles = false;
		PersistentParticleCountBuffer = nullptr;

		// Also reset SpawnManager state (NextParticleID, despawn queues, etc.)
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

	/**
	 * Run single initialization simulation step after uploading particles
	 * Stabilizes particle positions without full frame lifecycle
	 * @param Params - Simulation parameters
	 */
	void RunInitializationSimulation(const FGPUFluidSimulationParams& Params);

	//=============================================================================
	// Frame Lifecycle (called once per frame, used by Subsystem)
	//=============================================================================

	/**
	 * Begin frame - Process readbacks, spawn, despawn (call once at frame start)
	 * Must be called from render thread before any SimulateSubstep calls
	 * @param Params - Simulation parameters
	 */
	void BeginFrame();

	/**
	 * End frame - Extract persistent buffers, enqueue readbacks (call once at frame end)
	 * Must be called from render thread after all SimulateSubstep calls
	 */
	void EndFrame();

	/**
	 * Check if currently in a frame (between BeginFrame and EndFrame)
	 */
	bool IsFrameActive() const { return bFrameActive; }

	//=============================================================================
	// Deferred Simulation Execution (for PreRenderViewFamily synchronization)
	// This eliminates 1-frame delay by executing simulation in the same RDG
	// as the ExtractRenderData pass, ensuring particles use current-frame data.
	//=============================================================================

	/**
	 * Store simulation parameters for deferred execution.
	 * Called from game thread, actual simulation runs in PreRenderViewFamily_RenderThread.
	 * @param Params - Simulation parameters to store
	 */
	void EnqueueSimulation(const FGPUFluidSimulationParams& Params);

	/**
	 * Execute all pending simulations in render thread (called from ViewExtension).
	 * This must be called BEFORE ExtractRenderDataPass to ensure same-frame data.
	 * @param GraphBuilder - RDG builder for simulation passes
	 */
	void ExecutePendingSimulations_RenderThread(FRDGBuilder& GraphBuilder);

	/**
	 * Check if there are pending simulations to execute.
	 * @return true if simulations are queued
	 */
	bool HasPendingSimulations() const;

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

	/** Get persistent Z-Order CellStart buffer */
	TRefCountPtr<FRDGPooledBuffer> GetPersistentCellStartBuffer() const { return PersistentCellStartBuffer; }

	/** Get persistent Z-Order CellEnd buffer */
	TRefCountPtr<FRDGPooledBuffer> GetPersistentCellEndBuffer() const { return PersistentCellEndBuffer; }

	/** Check if Z-Order buffers are valid */
	bool HasValidZOrderBuffers() const
	{
		return PersistentCellStartBuffer.IsValid() && PersistentCellEndBuffer.IsValid();
	}

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

	/**
	 * Enable/disable Hybrid Tiled Z-Order mode for unlimited simulation range
	 * When enabled:
	 *   - Uses 32-bit sort keys (TileHash 14 bits + LocalMorton 18 bits)
	 *   - Simulation range is UNLIMITED (no bounds clipping)
	 *   - Radix sort uses 4 passes instead of 3
	 * When disabled:
	 *   - Uses 21-bit Morton codes (classic mode)
	 *   - Simulation range limited to bounds (±1280cm default)
	 * @param bEnabled - true to enable Hybrid mode
	 */
	void SetHybridTiledZOrderEnabled(bool bEnabled)
	{
		if (ZOrderSortManager.IsValid())
		{
			ZOrderSortManager->SetHybridTiledZOrderEnabled(bEnabled);
		}
		if (BoundarySkinningManager.IsValid())
		{
			BoundarySkinningManager->SetHybridTiledZOrderEnabled(bEnabled);
		}
	}

	/** Check if Hybrid Tiled Z-Order mode is enabled */
	bool IsHybridTiledZOrderEnabled() const
	{
		return ZOrderSortManager.IsValid() && ZOrderSortManager->IsHybridTiledZOrderEnabled();
	}

	/** Get anisotropy parameters */
	const FFluidAnisotropyParams& GetAnisotropyParams() const { return CachedAnisotropyParams; }

	/** Check if anisotropy is enabled */
	bool IsAnisotropyEnabled() const { return CachedAnisotropyParams.bEnabled; }

	/** Get persistent anisotropy axis buffers (for rendering) */
	TRefCountPtr<FRDGPooledBuffer> GetPersistentAnisotropyAxis1Buffer() const { return PersistentAnisotropyAxis1Buffer; }
	TRefCountPtr<FRDGPooledBuffer> GetPersistentAnisotropyAxis2Buffer() const { return PersistentAnisotropyAxis2Buffer; }
	TRefCountPtr<FRDGPooledBuffer> GetPersistentAnisotropyAxis3Buffer() const { return PersistentAnisotropyAxis3Buffer; }
	TRefCountPtr<FRDGPooledBuffer> GetPersistentRenderOffsetBuffer() const { return PersistentRenderOffsetBuffer; }

	/** Access anisotropy buffers for writing (compute shader output) */
	TRefCountPtr<FRDGPooledBuffer>& AccessPersistentAnisotropyAxis1Buffer() { return PersistentAnisotropyAxis1Buffer; }
	TRefCountPtr<FRDGPooledBuffer>& AccessPersistentAnisotropyAxis2Buffer() { return PersistentAnisotropyAxis2Buffer; }
	TRefCountPtr<FRDGPooledBuffer>& AccessPersistentAnisotropyAxis3Buffer() { return PersistentAnisotropyAxis3Buffer; }
	TRefCountPtr<FRDGPooledBuffer>& AccessPersistentRenderOffsetBuffer() { return PersistentRenderOffsetBuffer; }

	//=============================================================================
	// Heightmap Collision (Delegated to FGPUCollisionManager)
	// For Landscape terrain collision
	//=============================================================================

	/** Enable or disable Heightmap collision */
	void SetHeightmapCollisionEnabled(bool bEnabled) { if (CollisionManager.IsValid()) CollisionManager->SetHeightmapCollisionEnabled(bEnabled); }

	/** Set Heightmap collision parameters */
	void SetHeightmapCollisionParams(const FGPUHeightmapCollisionParams& Params) { if (CollisionManager.IsValid()) CollisionManager->SetHeightmapCollisionParams(Params); }

	/** Get Heightmap collision parameters */
	const FGPUHeightmapCollisionParams& GetHeightmapCollisionParams() const { static FGPUHeightmapCollisionParams Default; return CollisionManager.IsValid() ? CollisionManager->GetHeightmapCollisionParams() : Default; }

	/** Upload heightmap texture data */
	void UploadHeightmapData(const TArray<float>& HeightData, int32 Width, int32 Height) { if (CollisionManager.IsValid()) CollisionManager->UploadHeightmapTexture(HeightData, Width, Height); }

	/** Check if Heightmap collision is enabled */
	bool IsHeightmapCollisionEnabled() const { return CollisionManager.IsValid() && CollisionManager->IsHeightmapCollisionEnabled(); }

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
	 * Register a skeletal mesh for late bone transform refresh
	 * Bone transforms will be read in BeginRenderViewFamily for frame sync
	 * @param OwnerID - Unique ID for this interaction component
	 * @param SkelMesh - Skeletal mesh component to read bones from
	 */
	void RegisterSkeletalMeshForBoundary(int32 OwnerID, USkeletalMeshComponent* SkelMesh);

	/**
	 * Refresh all bone transforms from registered skeletal meshes
	 * MUST be called on Game Thread, right before render thread starts
	 */
	void RefreshAllBoneTransforms();

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
	 * Invalidate static boundary particle cache (call when World changes)
	 * This forces full regeneration on next GenerateStaticBoundaryParticles call
	 */
	void InvalidateStaticBoundaryCache() { if (StaticBoundaryManager.IsValid()) StaticBoundaryManager->InvalidateCache(); }

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
	// Bone Delta Attachment System (NEW simplified bone-following)
	// Per-particle BoneIndex, LocalOffset, PreviousPosition
	// Detach when distance from PreviousPosition > 300cm
	//=============================================================================

	/**
	 * Get the persistent BoneDeltaAttachment buffer for RDG passes
	 * Contains per-particle attachment data (BoneIndex, LocalOffset, PreviousPosition)
	 */
	TRefCountPtr<FRDGPooledBuffer> GetBoneDeltaAttachmentBuffer() const { return BoneDeltaAttachmentBuffer; }

	/**
	 * Check if BoneDeltaAttachment buffer is valid and has sufficient capacity
	 * @param RequiredCapacity - Minimum required particle capacity
	 */
	bool HasValidBoneDeltaAttachmentBuffer(int32 RequiredCapacity) const
	{
		return BoneDeltaAttachmentBuffer.IsValid() && BoneDeltaAttachmentCapacity >= RequiredCapacity;
	}

	/**
	 * Get current BoneDeltaAttachment buffer capacity
	 */
	int32 GetBoneDeltaAttachmentCapacity() const { return BoneDeltaAttachmentCapacity; }

	//=============================================================================
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

	/** Get all collision feedback (unfiltered, bone colliders only) */
	bool GetAllCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
	{
		if (!CollisionManager.IsValid()) { OutFeedback.Reset(); OutCount = 0; return false; }
		return CollisionManager->GetAllCollisionFeedback(OutFeedback, OutCount);
	}

	/** Get current collision feedback count (bone colliders) */
	int32 GetCollisionFeedbackCount() const { return CollisionManager.IsValid() ? CollisionManager->GetCollisionFeedbackCount() : 0; }

	/** Get all StaticMesh collision feedback (BoneIndex < 0, for buoyancy center calculation) */
	bool GetAllStaticMeshCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
	{
		if (!CollisionManager.IsValid()) { OutFeedback.Reset(); OutCount = 0; return false; }
		return CollisionManager->GetAllStaticMeshCollisionFeedback(OutFeedback, OutCount);
	}

	/** Get current StaticMesh collision feedback count */
	int32 GetStaticMeshCollisionFeedbackCount() const { return CollisionManager.IsValid() ? CollisionManager->GetStaticMeshCollisionFeedbackCount() : 0; }

	/** Get all FluidInteraction StaticMesh collision feedback (BoneIndex < 0, bHasFluidInteraction = 1) */
	bool GetAllFluidInteractionSMCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
	{
		if (!CollisionManager.IsValid()) { OutFeedback.Reset(); OutCount = 0; return false; }
		return CollisionManager->GetAllFluidInteractionSMCollisionFeedback(OutFeedback, OutCount);
	}

	/** Get current FluidInteraction StaticMesh collision feedback count */
	int32 GetFluidInteractionSMCollisionFeedbackCount() const { return CollisionManager.IsValid() ? CollisionManager->GetFluidInteractionSMCollisionFeedbackCount() : 0; }

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
	 * Add GPU brush despawn request - removes particles within radius (thread-safe)
	 * @param Center - World position of brush center
	 * @param Radius - Brush radius
	 */
	void AddGPUDespawnBrushRequest(const FVector3f& Center, float Radius);

	/**
	 * Add GPU source despawn request - removes all particles with SourceID (thread-safe)
	 * @param SourceID - Source component ID to despawn
	 */
	void AddGPUDespawnSourceRequest(int32 SourceID);

	/**
	 * Add GPU oldest despawn request for recycling (thread-safe, cumulative)
	 * GPU computes actual removal: max(0, GPU_Count + IncomingSpawnCount - MaxParticleCount)
	 * @param IncomingSpawnCount - Number of particles about to be spawned
	 */
	void AddGPUDespawnOldestRequest(int32 IncomingSpawnCount);

	/**
	 * Add explicit oldest removal - removes exactly N oldest particles (thread-safe, cumulative)
	 * @param RemoveCount - Exact number to remove (CPU specifies count)
	 */
	void AddGPUExplicitRemoveOldestRequest(int32 RemoveCount);

	/**
	 * Lightweight API for despawn operations - returns positions, IDs and source IDs
	 * Uses cached data from ProcessStatsReadback (no sync GPU readback needed)
	 * @param OutPositions - Output array of particle positions
	 * @param OutParticleIDs - Output array of particle IDs (same index as positions)
	 * @param OutSourceIDs - Output array of source IDs (same index as positions)
	 * @return true if valid data was copied
	 */
	bool GetParticlePositionsAndIDs(TArray<FVector3f>& OutPositions, TArray<int32>& OutParticleIDs, TArray<int32>& OutSourceIDs);

	/**
	 * Lightweight API for ISM rendering - returns positions and velocities only
	 * Much faster than GetAllGPUParticles (24 bytes vs 64 bytes per particle)
	 * @param OutPositions - Output array of positions
	 * @param OutVelocities - Output array of velocities
	 * @return true if valid data was copied
	 */
	bool GetParticlePositionsAndVelocities(TArray<FVector3f>& OutPositions, TArray<FVector3f>& OutVelocities);

	/**
	 * Enable/disable velocity readback for ISM rendering
	 * When enabled, CachedParticleVelocities is populated during ProcessStatsReadback
	 */
	void SetFullReadbackEnabled(bool bEnabled) { bFullReadbackEnabled.store(bEnabled); }

	/**
	 * Get particle IDs for a specific SourceID from cached readback data
	 * Returns nullptr if no cached data or SourceID not found
	 * Uses CachedSourceIDToParticleIDs built during readback processing
	 * @param SourceID - Source component ID to query
	 * @return Pointer to array of particle IDs, or nullptr if not available
	 */
	const TArray<int32>* GetParticleIDsBySourceID(int32 SourceID) const;

	/**
	 * Get all particle IDs from cached readback data
	 * Returns nullptr if no cached data available
	 * @return Pointer to array of all particle IDs, or nullptr if not available
	 */
	const TArray<int32>* GetAllParticleIDs() const;

	/**
	 * Get particle flags from cached readback data
	 * Returns nullptr if no cached data available
	 * @return Pointer to array of particle flags, or nullptr if not available
	 */
	const TArray<uint32>* GetParticleFlags() const;

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
	 * Atomically allocate a range of particle IDs (thread-safe for multi-module upload)
	 * @param Count - Number of IDs to allocate
	 * @return Starting ID of allocated range, or 0 if SpawnManager invalid
	 */
	int32 AllocateParticleIDs(int32 Count) { return SpawnManager.IsValid() ? SpawnManager->AllocateParticleIDs(Count) : 0; }

	/**
	 * Get the spawn manager (for per-source particle count tracking)
	 */
	FGPUSpawnManager* GetSpawnManager() const { return SpawnManager.Get(); }

private:
	//=============================================================================
	// Internal Methods
	//=============================================================================

	// Note: FSimulationSpatialData moved to GPU/GPUFluidSpatialData.h to avoid circular dependency

	/** Phase 1: Prepare particle buffer (CPU Upload or Reuse PersistentBuffer) */
	FRDGBufferRef PrepareParticleBuffer(
		FRDGBuilder& GraphBuilder,
		const FGPUFluidSimulationParams& Params);

	/** Phase 2: Build spatial structures (Z-Order Sort or Hash Table) */
	FSimulationSpatialData BuildSpatialStructures(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef& InOutParticleBuffer,
		FRDGBufferSRVRef& OutParticlesSRV,
		FRDGBufferUAVRef& OutParticlesUAV,
		FRDGBufferSRVRef& OutPositionsSRV,
		FRDGBufferUAVRef& OutPositionsUAV,
		const FGPUFluidSimulationParams& Params,
		// Optional: BoneDeltaAttachment buffer to reorder along with particles during Z-Order sorting
		FRDGBufferRef* InOutAttachmentBuffer = nullptr);

	/** Phase 3: Execute constraint solver loop (Density/Pressure + Collision per iteration)
	 *  XPBD Principle: Collision constraints are solved inside the solver loop
	 *  to ensure proper convergence between density and collision constraints.
	 *  This prevents jittering caused by density pushing particles through walls. */
	void ExecuteConstraintSolverLoop(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	/** Phase 4: Execute adhesion passes (Bone attachment only)
	 *  Collision passes have been moved into ExecuteConstraintSolverLoop.
	 *  This phase now only handles bone-based adhesion which runs after constraint solving. */
	void ExecuteAdhesion(
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

	/** 
	 * Swap neighbor cache buffers for Cohesion Force double buffering.
	 * Called in EndFrame after RDG execution completes.
	 * Moves current frame's NeighborListBuffer/NeighborCountsBuffer to PrevNeighbor buffers.
	 * This allows PredictPositions (Phase 2) to use previous frame's neighbors for Cohesion Force.
	 */
	void SwapNeighborCacheBuffers();

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

	/** Add viscosity pass (XSPH + Laplacian + Boundary viscosity) */
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

	/** Add bounds collision pass */
	void AddBoundsCollisionPass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	/** Add primitive collision pass (spheres, capsules, boxes, convexes) */
	void AddPrimitiveCollisionPass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	/** Add heightmap collision pass (Landscape terrain) */
	void AddHeightmapCollisionPass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	//-------------------------------------------------------------------------
	// Collision Feedback Buffer Management (delegated to CollisionFeedbackManager)
	//-------------------------------------------------------------------------

	/** Allocate collision feedback readback buffers */
	void AllocateCollisionFeedbackBuffers(FRHICommandListImmediate& RHICmdList);

	/** Release collision feedback buffers */
	void ReleaseCollisionFeedbackBuffers();

	/**
	 * Create PersistentParticleBuffer immediately (enable rendering without simulation)
	 * Allows rendering even before Simulate() is called after UploadParticles
	 */
	void CreateImmediatePersistentBuffer();

	/**
	 * Create PersistentParticleBuffer immediately from a copy
	 * Deadlock prevention: Can be called without BufferLock (uses already copied data)
	 * Called outside Lock scope in FinalizeUpload()
	 * @param InParticles - Copy of particle data (const ref, copied internally if needed)
	 * @param InParticleCount - Number of particles
	 */
	void CreateImmediatePersistentBufferFromCopy(const TArray<FGPUFluidParticle>& InParticles, int32 InParticleCount);

	/** Process collision feedback readback (non-blocking) */
	void ProcessCollisionFeedbackReadback(FRHICommandListImmediate& RHICmdList);

	/** Process collider contact count readback (non-blocking) */
	void ProcessColliderContactCountReadback(FRHICommandListImmediate& RHICmdList);

	/** Add finalize positions pass */
	void AddFinalizePositionsPass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
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
		const FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	/** Add boundary skinning pass (GPU transform of bone-local particles to world space) */
	void AddBoundarySkinningPass(
		FRDGBuilder& GraphBuilder,
		FSimulationSpatialData& SpatialData,
		const FGPUFluidSimulationParams& Params);

	//=============================================================================
	// Bone Delta Attachment System (NEW simplified bone-following)
	//=============================================================================

	/**
	 * Ensure BoneDeltaAttachment buffer exists with sufficient capacity.
	 * Creates or re-registers the buffer for use in RDG passes.
	 * @param GraphBuilder - RDG builder
	 * @param RequiredCapacity - Minimum particle capacity required
	 * @return RDG buffer reference for the attachment data
	 */
	FRDGBufferRef EnsureBoneDeltaAttachmentBuffer(
		FRDGBuilder& GraphBuilder,
		int32 RequiredCapacity);

	/**
	 * Add apply bone transform pass (runs at SIMULATION START)
	 * Sets velocity for attached particles to follow bone movement naturally.
	 * Uses velocity-only correction (no position teleport) to prevent pressure explosion.
	 * Uses the SAME bone transform buffer as BoundarySkinningCS for PERFECT sync.
	 * @param GraphBuilder - RDG builder
	 * @param ParticlesUAV - Particles buffer (read/write)
	 * @param BoneDeltaAttachmentSRV - Attachment data (read only)
	 * @param LocalBoundaryParticlesSRV - Local boundary particles (persistent)
	 * @param BoundaryParticleCount - Number of boundary particles
	 * @param BoneTransformsSRV - Bone transforms (same as BoundarySkinningCS uses)
	 * @param BoneCount - Number of bones
	 * @param ComponentTransform - Fallback transform for static meshes
	 * @param DeltaTime - Frame delta time for velocity calculation
	 */
	void AddApplyBoneTransformPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef BoneDeltaAttachmentSRV,
		FRDGBufferSRVRef LocalBoundaryParticlesSRV,
		int32 BoundaryParticleCount,
		FRDGBufferSRVRef BoneTransformsSRV,
		int32 BoneCount,
		const FMatrix44f& ComponentTransform,
		float DeltaTime);

	/**
	 * Add update bone delta attachment pass (runs at SIMULATION END)
	 * Updates attachment data after physics simulation: detach check, new attachment, LocalOffset update.
	 * Stores OriginalIndex for stable attachment across Z-Order sorting.
	 * @param GraphBuilder - RDG builder
	 * @param ParticlesUAV - Particles buffer (read/write)
	 * @param BoneDeltaAttachmentUAV - Attachment data (read/write)
	 * @param SortedBoundaryParticlesSRV - Z-Order sorted boundary particles (with OriginalIndex)
	 * @param BoundaryCellStartSRV - Cell start indices for boundary search
	 * @param BoundaryCellEndSRV - Cell end indices for boundary search
	 * @param BoundaryParticleCount - Number of sorted boundary particles
	 * @param WorldBoundaryParticlesSRV - Unsorted world boundary particles (for LocalOffset calculation)
	 * @param WorldBoundaryParticleCount - Number of world boundary particles
	 * @param Params - Simulation parameters
	 */
	void AddUpdateBoneDeltaAttachmentPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferUAVRef BoneDeltaAttachmentUAV,
		FRDGBufferSRVRef SortedBoundaryParticlesSRV,
		FRDGBufferSRVRef BoundaryCellStartSRV,
		FRDGBufferSRVRef BoundaryCellEndSRV,
		int32 BoundaryParticleCount,
		FRDGBufferSRVRef WorldBoundaryParticlesSRV,
		int32 WorldBoundaryParticleCount,
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
		const FGPUFluidSimulationParams& Params,
		// Optional: BoneDeltaAttachment buffer to reorder along with particles
		FRDGBufferRef InAttachmentBuffer = nullptr,
		FRDGBufferRef* OutSortedAttachmentBuffer = nullptr);

	//=============================================================================
	// ParticleID Sorting for Readback Optimization
	//=============================================================================

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

	// Frame lifecycle state
	bool bFrameActive = false;

	// Deferred simulation execution (for PreRenderViewFamily synchronization)
	// Stores simulation parameters from game thread, executed in render thread
	TArray<FGPUFluidSimulationParams> PendingSimulationParams;
	mutable FCriticalSection PendingSimulationLock;

	// Cached GPU particle data for upload/readback
	// Cached GPU particle data for upload
	TArray<FGPUFluidParticle> CachedGPUParticles;

	// Cached SourceID → ParticleIDs mapping (built during readback processing)
	// Fixed-size array indexed by SourceID (0~63), no hash lookup needed
	TArray<TArray<int32>> CachedSourceIDToParticleIDs;

	// Cached all particle IDs (built during readback processing)
	TArray<int32> CachedAllParticleIDs;

	// Cached particle positions (always built during readback for lightweight despawn API)
	TArray<FVector3f> CachedParticlePositions;

	// Cached particle source IDs (always built during readback for lightweight despawn API)
	TArray<int32> CachedParticleSourceIDs;

	// Cached particle velocities (always built during readback for lightweight ISM rendering)
	TArray<FVector3f> CachedParticleVelocities;

	// Cached particle flags (always built during readback for debug visualization)
	TArray<uint32> CachedParticleFlags;

	// Flag indicating valid GPU results are available for download
	std::atomic<bool> bHasValidGPUResults{false};

	// Flag to enable velocity readback (for ISM rendering)
	// When true, CachedParticleVelocities is populated during ProcessStatsReadback
	std::atomic<bool> bFullReadbackEnabled{false};

	// Persistent GPU buffer - reused across frames (Phase 2)
	// After simulation, this contains the results to be used next frame
	TRefCountPtr<FRDGPooledBuffer> PersistentParticleBuffer;

	// Persistent Spatial Hash buffers - reused across frames (GPU clear instead of CPU upload)
	TRefCountPtr<FRDGPooledBuffer> PersistentCellCountsBuffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentParticleIndicesBuffer;

	// Persistent Z-Order buffers
	TRefCountPtr<FRDGPooledBuffer> PersistentCellStartBuffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentCellEndBuffer;

	// Double Buffered Neighbor Cache (RAW Hazard Prevention)
	// Buffer[0] and Buffer[1] are used alternately each frame to prevent GPU resource hazards.
	// Problem: If we reuse the same buffer, PredictPositions (SRV read) and ConstraintSolverLoop
	// (UAV write) access the same physical buffer, causing GPU pipeline stalls.
	// Solution: True double buffering with physically separate buffers.
	// CurrentNeighborBufferIndex: index used for WRITING this frame
	// Read index = 1 - CurrentNeighborBufferIndex (previous frame's buffer for Cohesion Force)
	TRefCountPtr<FRDGPooledBuffer> NeighborListBuffers[2];
	TRefCountPtr<FRDGPooledBuffer> NeighborCountsBuffers[2];
	int32 NeighborBufferAllocCapacities[2] = {0, 0};    // Allocated buffer size (for reallocation check)
	int32 NeighborBufferParticleCapacities[2] = {0, 0}; // Actual particle count written (for PrevParticleCount)
	int32 CurrentNeighborBufferIndex = 0;
	bool bPrevNeighborCacheValid = false;        // False on first frame (skip Cohesion)

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
	TRefCountPtr<FRDGPooledBuffer> PersistentRenderOffsetBuffer;  // Surface particle render offset
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
	// Indirect Dispatch Infrastructure
	// PersistentParticleCountBuffer: 28 bytes (7 x uint32)
	//   [0-2]: IndirectArgs for TG=256 (GroupX, 1, 1)
	//   [3-5]: IndirectArgs for TG=512 (GroupX, 1, 1)
	//   [6]:   Raw particle count
	//=============================================================================

	/** GPU-authoritative particle count buffer for DispatchIndirect */
	TRefCountPtr<FRDGPooledBuffer> PersistentParticleCountBuffer;

	/** Async readback for GPU particle count → CPU CurrentParticleCount */
	FRHIGPUBufferReadback* ParticleCountReadback = nullptr;

	/** True once particles have ever existed (replaces CurrentParticleCount==0 early-out) */
	bool bEverHadParticles = false;

	/** Transient: IndirectArgs buffer valid only during SimulateSubstep_RDG scope */
	FRDGBufferRef CurrentIndirectArgsBuffer = nullptr;

	/** Register PersistentParticleCountBuffer as RDG external buffer, creating if needed */
	FRDGBufferRef RegisterParticleCountBuffer(FRDGBuilder& GraphBuilder);

	/** Enqueue async readback of PersistentParticleCountBuffer */
	void EnqueueParticleCountReadback(FRHICommandListImmediate& RHICmdList);

	/** Process completed readback → update CurrentParticleCount */
	void ProcessParticleCountReadback();

	//=============================================================================
	// Collision System (Delegated to FGPUCollisionManager)
	// Bounds, Primitive, Heightmap collision + Feedback
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
	// Bone Delta Attachment (NEW simplified bone-following system)
	// Per-particle attachment data: BoneIndex, LocalOffset, PreviousPosition
	// Used by ApplyBoneTransform (start) and UpdateBoneDeltaAttachment (end)
	//=============================================================================

	/** Persistent buffer for per-particle bone delta attachment data */
	TRefCountPtr<FRDGPooledBuffer> BoneDeltaAttachmentBuffer;

	/** Current capacity of BoneDeltaAttachment buffer */
	int32 BoneDeltaAttachmentCapacity = 0;

	//=============================================================================
	// Shadow Data (extracted from StatsReadback in ProcessStatsReadback)
	//=============================================================================

	/** Ready positions (extracted from StatsReadback) */
	TArray<FVector3f> ReadyShadowPositions;

	/** Ready velocities (extracted from StatsReadback, for prediction) */
	TArray<FVector3f> ReadyShadowVelocities;

	/** Ready neighbor counts (extracted from StatsReadback, for isolation detection) */
	TArray<uint32> ReadyShadowNeighborCounts;

	/** Frame number of ready shadow data */
	std::atomic<uint64> ReadyShadowPositionsFrame{0};

	/** Enable flag for shadow data extraction */
	std::atomic<bool> bShadowReadbackEnabled{false};

	//=============================================================================
	// Anisotropy Readback (Async GPU→CPU for Ellipsoid ISM Shadows)
	// Uses FRHIGPUBufferReadback for non-blocking readback (2-3 frame latency)
	//=============================================================================

	static constexpr int32 NUM_ANISOTROPY_READBACK_BUFFERS = 3;  // Triple buffering

	/** Async readback objects for anisotropy data (3 axes) */
	FRHIGPUBufferReadback* AnisotropyReadbacks[NUM_ANISOTROPY_READBACK_BUFFERS][3] = { {nullptr} };

	/** Current write index for anisotropy ring buffer */
	int32 AnisotropyReadbackWriteIndex = 0;

	/** Frame number tracking for each anisotropy readback buffer */
	uint64 AnisotropyReadbackFrameNumbers[NUM_ANISOTROPY_READBACK_BUFFERS] = { 0 };

	/** Particle count at the time anisotropy readback was enqueued */
	int32 AnisotropyReadbackParticleCounts[NUM_ANISOTROPY_READBACK_BUFFERS] = { 0 };

	/** Ready anisotropy data (copied from completed readback, for ellipsoid shadows) */
	TArray<FVector4f> ReadyShadowAnisotropyAxis1;
	TArray<FVector4f> ReadyShadowAnisotropyAxis2;
	TArray<FVector4f> ReadyShadowAnisotropyAxis3;

	/** Frame number of ready anisotropy */
	std::atomic<uint64> ReadyShadowAnisotropyFrame{0};

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

	/** Compact stats readback mode tracking (true = compact 32-byte, false = full 64-byte) */
	bool bStatsReadbackCompactMode[NUM_STATS_READBACK_BUFFERS] = { false };

	/** Persistent compact stats buffer for GPU extraction */
	TRefCountPtr<FRDGPooledBuffer> PersistentCompactStatsBuffer;

	/** Enable flag for anisotropy readback (requires bShadowReadbackEnabled) */
	std::atomic<bool> bAnisotropyReadbackEnabled{false};

	//=============================================================================
	// Debug Z-Order Array Index Readback (Async GPU→CPU for debug visualization)
	// Records each particle's array index after Z-Order sort (before ParticleID resort)
	// Uses FRHIGPUBufferReadback for non-blocking readback (2-3 frame latency)
	//=============================================================================

	static constexpr int32 NUM_DEBUG_INDEX_READBACK_BUFFERS = 3;  // Triple buffering

	/** Async readback objects for debug Z-Order array indices */
	FRHIGPUBufferReadback* DebugIndexReadbacks[NUM_DEBUG_INDEX_READBACK_BUFFERS] = { nullptr };

	/** Current write index for debug index readback ring buffer */
	int32 DebugIndexReadbackWriteIndex = 0;

	/** Frame number tracking for each debug index readback buffer */
	uint64 DebugIndexReadbackFrameNumbers[NUM_DEBUG_INDEX_READBACK_BUFFERS] = { 0 };

	/** Particle count for each debug index readback buffer */
	int32 DebugIndexReadbackParticleCounts[NUM_DEBUG_INDEX_READBACK_BUFFERS] = { 0 };

	/** Persistent GPU buffer for debug Z-Order array indices (int32 per particle) */
	TRefCountPtr<FRDGPooledBuffer> PersistentDebugZOrderIndexBuffer;

	/** Cached debug Z-Order array indices [ParticleID] → ZOrderArrayIndex */
	TArray<int32> CachedZOrderArrayIndices;

	/** Frame number of cached Z-Order indices */
	std::atomic<uint64> ReadyZOrderIndicesFrame{0};

	/** Enable flag for debug Z-Order index readback and recording */
	std::atomic<bool> bDebugZOrderIndexEnabled{false};

	//=============================================================================
	// Particle Bounds Readback (Async GPU→CPU for World Collision Query)
	// Reads particle AABB from GPU to expand world collision query bounds
	// Uses FRHIGPUBufferReadback for non-blocking readback (2-3 frame latency)
	//=============================================================================

	static constexpr int32 NUM_PARTICLE_BOUNDS_READBACK_BUFFERS = 3;  // Triple buffering

	/** Async readback objects for particle bounds (2 × FVector3f: Min, Max) */
	FRHIGPUBufferReadback* ParticleBoundsReadbacks[NUM_PARTICLE_BOUNDS_READBACK_BUFFERS] = { nullptr };

	/** Current write index for particle bounds readback ring buffer */
	int32 ParticleBoundsReadbackWriteIndex = 0;

	/** Frame number tracking for each particle bounds readback buffer */
	uint64 ParticleBoundsReadbackFrameNumbers[NUM_PARTICLE_BOUNDS_READBACK_BUFFERS] = { 0 };

	/** Cached particle bounds (AABB from GPU readback) */
	FBox CachedParticleBounds;

	/** Frame number of cached particle bounds */
	std::atomic<uint64> ReadyParticleBoundsFrame{0};

	/** Enable flag for particle bounds readback (for unlimited simulation range) */
	std::atomic<bool> bParticleBoundsReadbackEnabled{false};

public:
	//=============================================================================
	// Particle Bounds Readback API (for Unlimited Simulation Range)
	//=============================================================================

	/**
	 * Enable or disable particle bounds readback
	 * When enabled, particle AABB is asynchronously read back for world collision query
	 * @param bEnabled - true to enable async bounds readback
	 */
	void SetParticleBoundsReadbackEnabled(bool bEnabled) { bParticleBoundsReadbackEnabled.store(bEnabled); }

	/**
	 * Check if particle bounds readback is enabled
	 */
	bool IsParticleBoundsReadbackEnabled() const { return bParticleBoundsReadbackEnabled.load(); }

	/**
	 * Check if particle bounds are ready for use
	 * @return true if async readback has completed and bounds are available
	 */
	bool HasReadyParticleBounds() const { return ReadyParticleBoundsFrame.load() > 0 && CachedParticleBounds.IsValid; }

	/**
	 * Get cached particle bounds (non-blocking, returns previously completed readback)
	 * @return Particle AABB from GPU (may be 2-3 frames old)
	 */
	FBox GetCachedParticleBounds() const { return CachedParticleBounds; }

	/**
	 * Get the frame number of cached particle bounds
	 * @return Frame number when bounds were captured
	 */
	uint64 GetCachedParticleBoundsFrame() const { return ReadyParticleBoundsFrame.load(); }

	/**
	 * Enqueue particle bounds readback (call from render thread after ExtractRenderDataWithBounds)
	 * @param RHICmdList - RHI command list
	 * @param SourceBuffer - Bounds buffer from GPU (2 × FVector3f: Min, Max)
	 */
	void EnqueueParticleBoundsReadback(FRHICommandListImmediate& RHICmdList, FRHIBuffer* SourceBuffer);

	//=============================================================================
	// Shadow Position Readback API
	//=============================================================================

	/**
	 * Enable or disable shadow position readback
	 * When enabled, particle positions are asynchronously read back for ISM shadows
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
	 * Get shadow data with anisotropy (non-blocking, for ellipsoid ISM shadows)
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

	//=============================================================================
	// Debug Z-Order Array Index API (for visualization)
	// Returns array indices after Z-Order sort, before ParticleID re-sort
	//=============================================================================

	/**
	 * Enable or disable debug Z-Order array index recording and readback
	 * When enabled, records each particle's array index after Z-Order sort
	 * @param bEnabled - true to enable debug index recording
	 */
	void SetDebugZOrderIndexEnabled(bool bEnabled) { bDebugZOrderIndexEnabled.store(bEnabled); }

	/**
	 * Check if debug Z-Order index readback is enabled
	 */
	bool IsDebugZOrderIndexEnabled() const { return bDebugZOrderIndexEnabled.load(); }

	/**
	 * Check if debug Z-Order index data is ready for use
	 * @return true if async readback has completed and indices are available
	 */
	bool HasReadyZOrderIndices() const { return ReadyZOrderIndicesFrame.load() > 0 && CachedZOrderArrayIndices.Num() > 0; }

	/**
	 * Get debug Z-Order array indices (non-blocking, returns previously completed readback)
	 * Returns mapping: [ParticleID] → ZOrderArrayIndex
	 * @param OutIndices - Output array of Z-Order array indices (indexed by ParticleID)
	 * @return true if valid indices were retrieved
	 */
	bool GetZOrderArrayIndices(TArray<int32>& OutIndices) const;

private:
	/** Allocate anisotropy readback objects */
	void AllocateAnisotropyReadbackObjects(FRHICommandListImmediate& RHICmdList);

	/** Release anisotropy readback objects */
void ReleaseAnisotropyReadbackObjects();	

	/** Enqueue anisotropy data copy to readback buffer */
	void EnqueueAnisotropyReadback(FRHICommandListImmediate& RHICmdList, int32 ParticleCount);

	/** Process anisotropy readback */
	void ProcessAnisotropyReadback();

	//=============================================================================
	// Stats/Recycle Readback Internal Functions
	//=============================================================================

	/** Allocate stats readback objects */
	void AllocateStatsReadbackObjects(FRHICommandListImmediate& RHICmdList);

	/** Release stats readback objects */
	void ReleaseStatsReadbackObjects();

	/** Enqueue stats readback (full 64-byte or compact 32-byte particle data for ID-based operations) */
	void EnqueueStatsReadback(FRHICommandListImmediate& RHICmdList, FRHIBuffer* SourceBuffer, int32 ParticleCount, bool bCompactMode = false);

	/** Process stats readback (check for completion, populate cached lightweight data) */
	void ProcessStatsReadback(FRHICommandListImmediate& RHICmdList);

	//=============================================================================
	// Debug Z-Order Index Readback Internal Functions
	//=============================================================================

	/** Allocate debug Z-Order index readback objects */
	void AllocateDebugIndexReadbackObjects(FRHICommandListImmediate& RHICmdList);

	/** Release debug Z-Order index readback objects */
	void ReleaseDebugIndexReadbackObjects();

	/** Enqueue debug Z-Order index readback (int32 array: [ParticleID] → ZOrderArrayIndex) */
	void EnqueueDebugIndexReadback(FRHICommandListImmediate& RHICmdList, FRHIBuffer* SourceBuffer, int32 ParticleCount);

	/** Process debug Z-Order index readback (check for completion, populate CachedZOrderArrayIndices) */
	void ProcessDebugIndexReadback();

	/** Add RDG pass to record Z-Order array indices (call BEFORE ParticleID re-sort) */
	void AddRecordZOrderIndicesPass(FRDGBuilder& GraphBuilder, FRDGBufferRef ParticleBuffer, int32 ParticleCount);

	//=============================================================================
	// Particle Bounds Readback Internal Functions
	//=============================================================================

	/** Allocate particle bounds readback objects */
	void AllocateParticleBoundsReadbackObjects(FRHICommandListImmediate& RHICmdList);

	/** Release particle bounds readback objects */
	void ReleaseParticleBoundsReadbackObjects();

	/** Process particle bounds readback (check for completion, populate CachedParticleBounds) */
	void ProcessParticleBoundsReadback();
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
