// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUBoundarySkinningManager - GPU Boundary Skinning and Adhesion System

#pragma once

#include <GPU/GPUFluidSpatialData.h>

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIResources.h"
#include "GPU/GPUFluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"  // For EGridResolutionPreset

class USkeletalMeshComponent;

class FRDGBuilder;

/**
 * FGPUBoundarySkinningManager
 *
 * Manages GPU-based boundary skinning for Flex-style adhesion.
 * Transforms bone-local boundary particles to world space using GPU compute,
 * enabling efficient interaction between fluid and skinned meshes.
 *
 * Features:
 * - Persistent local boundary particles (uploaded once per mesh)
 * - Per-frame bone transform updates
 * - GPU-based skinning transform
 * - Z-Order (Morton code) sorting for O(K) neighbor search
 * - BoundaryCellStart/End for cache-coherent access
 */
class KAWAIIFLUIDRUNTIME_API FGPUBoundarySkinningManager
{
public:
	FGPUBoundarySkinningManager();
	~FGPUBoundarySkinningManager();

	//=========================================================================
	// Lifecycle
	//=========================================================================

	void Initialize();
	void Release();
	bool IsReady() const { return bIsInitialized; }

	//=========================================================================
	// Static Boundary Particles (StaticMesh colliders - Persistent GPU)
	// Uploaded once, cached on GPU, re-sorted only when dirty
	//=========================================================================

	/**
	 * Upload static boundary particles to persistent GPU buffer
	 * Called once when static colliders are generated, cached on GPU
	 * @param Particles - World-space static boundary particles
	 */
	void UploadStaticBoundaryParticles(const TArray<FGPUBoundaryParticle>& Particles);

	/**
	 * Clear static boundary particles
	 */
	void ClearStaticBoundaryParticles();

	/**
	 * Mark static boundary as dirty (needs re-upload and re-sort)
	 * Call when colliders change (add/remove/move)
	 */
	void MarkStaticBoundaryDirty() { bStaticBoundaryDirty = true; }

	/** Check if static boundary is enabled and has data */
	bool HasStaticBoundaryData() const { return bStaticBoundaryEnabled && StaticBoundaryParticleCount > 0; }

	/** Get static boundary particle count */
	int32 GetStaticBoundaryParticleCount() const { return StaticBoundaryParticleCount; }

	/** Enable/disable static boundary processing */
	void SetStaticBoundaryEnabled(bool bEnabled) { bStaticBoundaryEnabled = bEnabled; }
	bool IsStaticBoundaryEnabled() const { return bStaticBoundaryEnabled; }

	/** Get static boundary buffers (for density/adhesion passes) */
	TRefCountPtr<FRDGPooledBuffer>& GetStaticBoundaryBuffer() { return PersistentStaticBoundaryBuffer; }
	TRefCountPtr<FRDGPooledBuffer>& GetStaticZOrderSortedBuffer() { return PersistentStaticZOrderSorted; }
	TRefCountPtr<FRDGPooledBuffer>& GetStaticCellStartBuffer() { return PersistentStaticCellStart; }
	TRefCountPtr<FRDGPooledBuffer>& GetStaticCellEndBuffer() { return PersistentStaticCellEnd; }

	/** Check if static Z-Order data is valid */
	bool HasStaticZOrderData() const
	{
		return bStaticZOrderValid
			&& PersistentStaticZOrderSorted.IsValid()
			&& PersistentStaticCellStart.IsValid()
			&& PersistentStaticCellEnd.IsValid();
	}

	/**
	 * Execute Z-Order sorting for static boundary particles (if dirty)
	 * Called once when static boundary is uploaded, cached until dirty
	 * @param GraphBuilder - RDG builder
	 * @param Params - Simulation parameters
	 */
	void ExecuteStaticBoundaryZOrderSort(FRDGBuilder& GraphBuilder, const FGPUFluidSimulationParams& Params);

	//=========================================================================
	// GPU Boundary Skinning (Persistent Local + GPU Transform)
	//=========================================================================

	/**
	 * Upload local boundary particles for GPU skinning
	 * @param OwnerID - Unique ID for the mesh owner
	 * @param LocalParticles - Bone-local boundary particles
	 */
	void UploadLocalBoundaryParticles(int32 OwnerID, const TArray<FGPUBoundaryParticleLocal>& LocalParticles);

	/**
	 * Upload bone transforms for boundary skinning
	 * @param OwnerID - Unique ID for the mesh owner
	 * @param BoneTransforms - Current bone transforms
	 * @param ComponentTransform - Component world transform
	 */
	void UploadBoneTransformsForBoundary(int32 OwnerID, const TArray<FMatrix44f>& BoneTransforms, const FMatrix44f& ComponentTransform);

	/**
	 * Register a skeletal mesh component for late bone transform refresh
	 * Call this instead of/in addition to UploadBoneTransformsForBoundary
	 * @param OwnerID - Unique ID for the mesh owner
	 * @param SkelMesh - Skeletal mesh component to read bones from
	 */
	void RegisterSkeletalMeshReference(int32 OwnerID, USkeletalMeshComponent* SkelMesh);

	/**
	 * Refresh bone transforms from all registered skeletal meshes
	 * MUST be called on Game Thread, right before render thread starts (BeginRenderViewFamily)
	 * This ensures bone transforms match what the skeletal mesh will render with
	 */
	void RefreshAllBoneTransforms();

	/** Remove skinning data for an owner */
	void RemoveBoundarySkinningData(int32 OwnerID);

	/** Clear all boundary skinning data */
	void ClearAllBoundarySkinningData();

	/** Check if GPU boundary skinning is enabled */
	bool IsGPUBoundarySkinningEnabled() const { return TotalLocalBoundaryParticleCount > 0; }

	/** Get total local boundary particle count */
	int32 GetTotalLocalBoundaryParticleCount() const { return TotalLocalBoundaryParticleCount; }

	//=========================================================================
	// Bone Transform Access (for BoneDeltaAttachment system)
	//=========================================================================

	/**
	 * Get bone transforms for a specific owner
	 * Used by the BoneDeltaAttachment system to transform attached particles
	 * @param OwnerID - Unique ID for the mesh owner
	 * @return Pointer to bone transforms array, nullptr if owner not found
	 */
	const TArray<FMatrix44f>* GetBoneTransforms(int32 OwnerID) const;

	/**
	 * Get the first available bone transforms (for single-owner scenarios)
	 * Returns the bone transforms of the first registered skeletal mesh owner
	 * @param OutOwnerID - Output: Owner ID of the returned transforms
	 * @return Pointer to bone transforms array, nullptr if no owners
	 */
	const TArray<FMatrix44f>* GetFirstAvailableBoneTransforms(int32* OutOwnerID = nullptr) const;

	/**
	 * Get bone count for a specific owner
	 * @param OwnerID - Unique ID for the mesh owner
	 * @return Number of bones, 0 if owner not found
	 */
	int32 GetBoneCount(int32 OwnerID) const;

	//=========================================================================
	// Boundary Adhesion Parameters
	//=========================================================================

	void SetBoundaryAdhesionParams(const FGPUBoundaryAdhesionParams& Params) { CachedBoundaryAdhesionParams = Params; }
	const FGPUBoundaryAdhesionParams& GetBoundaryAdhesionParams() const { return CachedBoundaryAdhesionParams; }
	bool IsBoundaryAdhesionEnabled() const;

	//=========================================================================
	// Boundary Owner AABB (for early-out optimization)
	// Skip boundary adhesion pass entirely when AABB doesn't overlap volume
	//=========================================================================

	/**
	 * Update AABB for a boundary owner
	 * @param OwnerID - Unique ID for the mesh owner
	 * @param AABB - World-space AABB of the boundary owner
	 */
	void UpdateBoundaryOwnerAABB(int32 OwnerID, const FGPUBoundaryOwnerAABB& AABB);

	/**
	 * Get combined AABB of all boundary owners
	 * @return Combined AABB encompassing all boundary owners
	 */
	const FGPUBoundaryOwnerAABB& GetCombinedBoundaryAABB() const { return CombinedBoundaryAABB; }

	/**
	 * Check if any boundary owner AABB overlaps with the simulation volume
	 * @param VolumeMin - Minimum corner of simulation volume (expanded by AdhesionRadius)
	 * @param VolumeMax - Maximum corner of simulation volume (expanded by AdhesionRadius)
	 * @param AdhesionRadius - Adhesion search radius
	 * @return true if any boundary AABB overlaps with the volume
	 */
	bool DoesBoundaryOverlapVolume(const FVector3f& VolumeMin, const FVector3f& VolumeMax, float AdhesionRadius) const;

	/**
	 * Check if boundary adhesion pass should be skipped due to no overlap
	 * @param Params - Simulation parameters containing volume bounds
	 * @return true if the pass should be skipped (no overlap)
	 */
	bool ShouldSkipBoundaryAdhesionPass(const FGPUFluidSimulationParams& Params) const;

	//=========================================================================
	// RDG Pass (called from simulator)
	//=========================================================================

	/** Data needed for ApplyBoneTransformPass (returned by AddBoundarySkinningPass) */
	struct FBoundarySkinningOutputs
	{
		FRDGBufferRef LocalBoundaryParticlesBuffer = nullptr;
		FRDGBufferRef BoneTransformsBuffer = nullptr;
		int32 BoneCount = 0;
		FMatrix44f ComponentTransform = FMatrix44f::Identity;
	};

	/**
	 * Add boundary skinning pass to transform local particles to world space
	 * @param GraphBuilder - RDG builder
	 * @param OutWorldBoundaryBuffer - Output: World-space boundary particles buffer
	 * @param OutBoundaryParticleCount - Output: Number of boundary particles
	 * @param DeltaTime - Frame delta time for velocity calculation
	 * @param OutSkinningOutputs - Optional: Additional outputs for ApplyBoneTransformPass
	 */
	void AddBoundarySkinningPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef& OutWorldBoundaryBuffer,
		int32& OutBoundaryParticleCount,
		float DeltaTime,
		FBoundarySkinningOutputs* OutSkinningOutputs = nullptr);

	/**
	 * Add boundary adhesion pass
	 * @param GraphBuilder - RDG builder
	 * @param ParticlesUAV - Fluid particles buffer
	 * @param CurrentParticleCount - Number of fluid particles
	 * @param Params - Simulation parameters
	 * @param InSameFrameBoundaryBuffer - Optional: Same-frame boundary buffer (for first frame support)
	 * @param InSameFrameBoundaryCount - Optional: Same-frame boundary particle count
	 * @param InZOrderSortedSRV - Optional: Same-frame Z-Order sorted boundary buffer
	 * @param InZOrderCellStartSRV - Optional: Same-frame Z-Order cell start buffer
	 * @param InZOrderCellEndSRV - Optional: Same-frame Z-Order cell end buffer
	 */
	void AddBoundaryAdhesionPass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
		int32 CurrentParticleCount,
		const FGPUFluidSimulationParams& Params,
		FRDGBufferRef InSameFrameBoundaryBuffer = nullptr,
		int32 InSameFrameBoundaryCount = 0,
		FRDGBufferSRVRef InZOrderSortedSRV = nullptr,
		FRDGBufferSRVRef InZOrderCellStartSRV = nullptr,
		FRDGBufferSRVRef InZOrderCellEndSRV = nullptr);

	/** Get world boundary buffer for other passes */
	TRefCountPtr<FRDGPooledBuffer>& GetWorldBoundaryBuffer() { return PersistentWorldBoundaryBuffer; }

	/** Check if world boundary buffer is valid */
	bool HasWorldBoundaryBuffer() const { return PersistentWorldBoundaryBuffer.IsValid(); }

	//=========================================================================
	// Z-Order Sorting for Boundary Particles
	// Enables O(K) neighbor search instead of O(M) full traversal
	//=========================================================================

	/**
	 * Execute Z-Order sorting pipeline for boundary particles
	 * Called after BoundarySkinningPass to sort world-space boundary particles
	 * Works independently of FluidInteraction - supports static boundary particles
	 * @param GraphBuilder - RDG builder
	 * @param Params - Simulation parameters (for CellSize, bounds)
	 * @param InSameFrameBoundaryBuffer - Optional: Same-frame boundary buffer (for first frame support)
	 * @param InSameFrameBoundaryCount - Optional: Same-frame boundary particle count
	 * @param OutSortedBuffer - Output: Sorted boundary particles buffer (same-frame access)
	 * @param OutCellStartBuffer - Output: Cell start indices buffer
	 * @param OutCellEndBuffer - Output: Cell end indices buffer
	 * @param OutParticleCount - Output: Number of boundary particles sorted
	 * @return true if Z-Order sorting was performed
	 */
	bool ExecuteBoundaryZOrderSort(
		FRDGBuilder& GraphBuilder,
		const FGPUFluidSimulationParams& Params,
		FRDGBufferRef InSameFrameBoundaryBuffer,
		int32 InSameFrameBoundaryCount,
		FRDGBufferRef& OutSortedBuffer,
		FRDGBufferRef& OutCellStartBuffer,
		FRDGBufferRef& OutCellEndBuffer,
		int32& OutParticleCount);

	/** Set Z-Order configuration */
	void SetBoundaryZOrderConfig(EGridResolutionPreset Preset, const FVector3f& BoundsMin, const FVector3f& BoundsMax)
	{
		GridResolutionPreset = Preset;
		ZOrderBoundsMin = BoundsMin;
		ZOrderBoundsMax = BoundsMax;
	}

	/** Enable/disable Z-Order sorting for boundary */
	void SetBoundaryZOrderEnabled(bool bEnabled) { bUseBoundaryZOrder = bEnabled; }
	bool IsBoundaryZOrderEnabled() const { return bUseBoundaryZOrder; }

	/** Enable/disable Hybrid Tiled Z-Order mode for unlimited simulation range */
	void SetHybridTiledZOrderEnabled(bool bEnabled) { bUseHybridTiledZOrder = bEnabled; }
	bool IsHybridTiledZOrderEnabled() const { return bUseHybridTiledZOrder; }

	/** Check if Z-Order data is valid */
	bool HasBoundaryZOrderData() const
	{
		return bBoundaryZOrderValid
			&& PersistentSortedBoundaryBuffer.IsValid()
			&& PersistentBoundaryCellStart.IsValid()
			&& PersistentBoundaryCellEnd.IsValid();
	}

	/** Get sorted boundary buffer (Z-Order sorted) */
	TRefCountPtr<FRDGPooledBuffer>& GetSortedBoundaryBuffer() { return PersistentSortedBoundaryBuffer; }

	/** Get boundary cell start/end buffers */
	TRefCountPtr<FRDGPooledBuffer>& GetBoundaryCellStartBuffer() { return PersistentBoundaryCellStart; }
	TRefCountPtr<FRDGPooledBuffer>& GetBoundaryCellEndBuffer() { return PersistentBoundaryCellEnd; }

	/** Mark boundary Z-Order as dirty (needs re-sort) */
	void MarkBoundaryZOrderDirty() { bBoundaryZOrderDirty = true; }


private:
	//=========================================================================
	// State
	//=========================================================================

	bool bIsInitialized = false;

	//=========================================================================
	// Static Boundary Particles (StaticMesh colliders - Persistent GPU)
	//=========================================================================

	TRefCountPtr<FRDGPooledBuffer> PersistentStaticBoundaryBuffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentStaticZOrderSorted;
	TRefCountPtr<FRDGPooledBuffer> PersistentStaticCellStart;
	TRefCountPtr<FRDGPooledBuffer> PersistentStaticCellEnd;
	int32 StaticBoundaryParticleCount = 0;
	int32 StaticBoundaryBufferCapacity = 0;
	bool bStaticBoundaryEnabled = false;
	bool bStaticBoundaryDirty = true;
	bool bStaticZOrderValid = false;

	// Temporary CPU storage for upload (cleared after GPU upload)
	TArray<FGPUBoundaryParticle> PendingStaticBoundaryParticles;

	FGPUBoundaryAdhesionParams CachedBoundaryAdhesionParams;

	//=========================================================================
	// GPU Boundary Skinning Data
	//=========================================================================

	/** Skinning data per owner */
	struct FGPUBoundarySkinningData
	{
		int32 OwnerID = -1;
		TArray<FGPUBoundaryParticleLocal> LocalParticles;

		// =====================================================================
		// ATOMIC SWAP DOUBLE BUFFER for thread-safe bone transform transfer
		// Game Thread writes to Buffer[WriteIndex], then swaps WriteIndex
		// Render Thread reads from Buffer[1 - WriteIndex] (the completed buffer)
		// This prevents race conditions without expensive locks
		// =====================================================================
		TArray<FMatrix44f> BoneTransformsBuffer[2];       // Double buffer
		FMatrix44f ComponentTransformBuffer[2];           // Double buffer
		int32 WriteBufferIndex = 0;                       // Swap index (atomic not needed - see below)
		// NOTE: We don't need TAtomic because:
		// 1. Game Thread writes complete BEFORE Render Thread reads (Unreal's frame sync)
		// 2. The index is only modified by Game Thread
		// 3. int32 read/write is atomic on x64

		// Legacy single buffer (kept for compatibility, will be removed)
		TArray<FMatrix44f> BoneTransforms;
		TArray<FMatrix44f> RenderBoneTransforms;
		FMatrix44f ComponentTransform = FMatrix44f::Identity;
		FMatrix44f RenderComponentTransform = FMatrix44f::Identity;

		TWeakObjectPtr<USkeletalMeshComponent> SkeletalMeshRef;  // For late bone refresh
		bool bLocalParticlesUploaded = false;

		FGPUBoundarySkinningData()
		{
			ComponentTransformBuffer[0] = FMatrix44f::Identity;
			ComponentTransformBuffer[1] = FMatrix44f::Identity;
		}
	};

	TMap<int32, FGPUBoundarySkinningData> BoundarySkinningDataMap;
	int32 TotalLocalBoundaryParticleCount = 0;

	//=========================================================================
	// Persistent Buffers
	//=========================================================================

	TMap<int32, TRefCountPtr<FRDGPooledBuffer>> PersistentLocalBoundaryBuffers;
	TRefCountPtr<FRDGPooledBuffer> PersistentWorldBoundaryBuffer;
	TRefCountPtr<FRDGPooledBuffer> PreviousWorldBoundaryBuffer;  // For velocity calculation
	int32 WorldBoundaryBufferCapacity = 0;
	bool bHasPreviousFrame = false;  // True after first frame

	//=========================================================================
	// Dirty Tracking
	//=========================================================================

	bool bBoundarySkinningDataDirty = false;

	//=========================================================================
	// Z-Order Sorting Data
	//=========================================================================

	bool bUseBoundaryZOrder = true;
	bool bBoundaryZOrderDirty = true;
	bool bBoundaryZOrderValid = false;

	// Grid configuration (must match fluid simulation)
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;
	FVector3f ZOrderBoundsMin = FVector3f(-1280.0f, -1280.0f, -1280.0f);
	FVector3f ZOrderBoundsMax = FVector3f(1280.0f, 1280.0f, 1280.0f);

	// Hybrid Tiled Z-Order mode for unlimited simulation range
	bool bUseHybridTiledZOrder = false;

	// Persistent Z-Order buffers
	TRefCountPtr<FRDGPooledBuffer> PersistentSortedBoundaryBuffer;
	TRefCountPtr<FRDGPooledBuffer> PersistentBoundaryCellStart;
	TRefCountPtr<FRDGPooledBuffer> PersistentBoundaryCellEnd;
	int32 BoundaryZOrderBufferCapacity = 0;

	//=========================================================================
	// Boundary Owner AABBs (for early-out optimization)
	//=========================================================================

	TMap<int32, FGPUBoundaryOwnerAABB> BoundaryOwnerAABBs;
	FGPUBoundaryOwnerAABB CombinedBoundaryAABB;  // Combined AABB of all owners
	bool bBoundaryAABBDirty = true;

	/** Recalculate combined AABB from all owner AABBs */
	void RecalculateCombinedAABB();

	//=========================================================================
	// Bone Transform Snapshot Queue (for deferred simulation execution)
	// Prevents race condition: Game thread overwrites bone transforms before
	// Render thread uses them. Each simulation enqueue captures a snapshot.
	//=========================================================================

	/** Lightweight snapshot data (copyable, no atomics) */
	struct FBoneTransformSnapshotData
	{
		int32 OwnerID = -1;
		TArray<FMatrix44f> BoneTransforms;
		FMatrix44f ComponentTransform = FMatrix44f::Identity;
		bool bLocalParticlesUploaded = false;
	};

	/** Snapshotted bone transforms for a single pending simulation */
	struct FBoneTransformSnapshot
	{
		TMap<int32, FBoneTransformSnapshotData> SkinningDataSnapshot;
	};

	/** Queue of bone transform snapshots (one per pending simulation) */
	TArray<FBoneTransformSnapshot> PendingBoneTransformSnapshots;

	/** Currently active snapshot during AddBoundarySkinningPass execution */
	TOptional<FBoneTransformSnapshot> ActiveSnapshot;

public:
	/**
	 * Snapshot current bone transforms for deferred simulation execution.
	 * Call this from EnqueueSimulation() to capture bone transforms at enqueue time.
	 * Thread-safe: uses BoundarySkinningLock.
	 */
	void SnapshotBoneTransformsForPendingSimulation();

	/**
	 * Pop and activate a snapshotted bone transform for execution.
	 * Call before SimulateSubstep_RDG() in ExecutePendingSimulations_RenderThread().
	 * @return true if a snapshot was available and is now active
	 */
	bool PopAndActivateSnapshot();

	/**
	 * Clear the active snapshot after simulation execution completes.
	 * Call after SimulateSubstep_RDG() completes.
	 */
	void ClearActiveSnapshot();

	/**
	 * Check if there's an active snapshot being used.
	 */
	bool HasActiveSnapshot() const { return ActiveSnapshot.IsSet(); }

	/**
	 * Get the active snapshot's skinning data for a specific owner.
	 * Returns nullptr if no active snapshot or owner not found.
	 */
	const FBoneTransformSnapshotData* GetActiveSnapshotData(int32 OwnerID) const;

private:
	//=========================================================================
	// Thread Safety
	//=========================================================================

	mutable FCriticalSection BoundarySkinningLock;
};
