// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUCollisionFeedbackManager - Collision feedback system with async GPU readback

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIResources.h"
#include "GPU/GPUFluidParticle.h"

class FRHICommandListImmediate;
class FRHIGPUBufferReadback;
class FRDGBuilder;

/**
 * FGPUCollisionFeedbackManager
 *
 * Manages GPU collision feedback system for particle-collider interaction.
 * Uses triple-buffered async GPU readback (FRHIGPUBufferReadback) for
 * efficient GPU->CPU data transfer without pipeline stalls.
 *
 * Features:
 * - Collision feedback entries (position, velocity, force for each collision)
 * - Per-collider contact counts (simple collision detection)
 * - Async readback with no FlushRenderingCommands
 *
 * Unified Buffer Layout:
 * - All feedback types merged into single buffer with embedded counters
 * - Buffer layout:
 *   [Header: 16 bytes] BoneCount, SMCount, FISMCount, Reserved
 *   [BoneFeedback: 4096 entries] offset 16
 *   [StaticMeshFeedback: 1024 entries] offset 393,232
 *   [FluidInteractionFeedback: 1024 entries] offset 491,536
 */
class KAWAIIFLUIDRUNTIME_API FGPUCollisionFeedbackManager
{
public:
	FGPUCollisionFeedbackManager();
	~FGPUCollisionFeedbackManager();

	//=========================================================================
	// Constants
	//=========================================================================

	/** Max feedback entries for bone colliders (BoneIndex >= 0) */
	static constexpr int32 MAX_COLLISION_FEEDBACK = 4096;

	/** Max feedback entries for WorldCollision StaticMesh (BoneIndex < 0, bHasFluidInteraction = 0) */
	static constexpr int32 MAX_STATICMESH_COLLISION_FEEDBACK = 1024;

	/** Max feedback entries for FluidInteraction StaticMesh (BoneIndex < 0, bHasFluidInteraction = 1) */
	static constexpr int32 MAX_FLUIDINTERACTION_SM_FEEDBACK = 1024;

	static constexpr int32 NUM_FEEDBACK_BUFFERS = 3;
	static constexpr int32 MAX_COLLIDER_COUNT = 256;

	//=========================================================================
	// Unified Buffer Layout Constants
	//=========================================================================

	/** Header size: 4 uint32 counters (BoneCount, SMCount, FISMCount, Reserved) */
	static constexpr int32 UNIFIED_HEADER_SIZE = 16;

	/** Offset to bone feedback section (after header) */
	static constexpr int32 BONE_FEEDBACK_OFFSET = UNIFIED_HEADER_SIZE;

	/** Offset to StaticMesh feedback section */
	static constexpr int32 SM_FEEDBACK_OFFSET = BONE_FEEDBACK_OFFSET + MAX_COLLISION_FEEDBACK * 96;  // 96 = sizeof(FGPUCollisionFeedback)

	/** Offset to FluidInteraction feedback section */
	static constexpr int32 FISM_FEEDBACK_OFFSET = SM_FEEDBACK_OFFSET + MAX_STATICMESH_COLLISION_FEEDBACK * 96;

	/** Total unified buffer size in bytes */
	static constexpr int32 UNIFIED_BUFFER_SIZE = FISM_FEEDBACK_OFFSET + MAX_FLUIDINTERACTION_SM_FEEDBACK * 96;

	//=========================================================================
	// Lifecycle
	//=========================================================================

	/** Initialize the feedback manager */
	void Initialize();

	/** Release all resources */
	void Release();

	/** Check if ready */
	bool IsReady() const { return bIsInitialized; }

	//=========================================================================
	// Enable/Disable
	//=========================================================================

	/** Enable or disable collision feedback recording */
	void SetEnabled(bool bEnabled) { bFeedbackEnabled = bEnabled; }

	/** Check if collision feedback is enabled */
	bool IsEnabled() const { return bFeedbackEnabled; }

	//=========================================================================
	// Buffer Management (called from render thread)
	//=========================================================================

	/** Allocate readback objects (call from render thread) */
	void AllocateReadbackObjects(FRHICommandListImmediate& RHICmdList);

	/** Release readback objects */
	void ReleaseReadbackObjects();

	/** Get or create unified feedback buffer for RDG pass (all feedback types merged) */
	TRefCountPtr<FRDGPooledBuffer>& GetUnifiedFeedbackBuffer() { return UnifiedFeedbackBuffer; }

	/** Get or create contact count buffer for RDG pass */
	TRefCountPtr<FRDGPooledBuffer>& GetContactCountBuffer() { return ColliderContactCountBuffer; }

	//=========================================================================
	// Legacy API (deprecated, kept for compatibility - will be removed)
	//=========================================================================

	/** @deprecated Use GetUnifiedFeedbackBuffer() instead */
	TRefCountPtr<FRDGPooledBuffer>& GetFeedbackBuffer() { return UnifiedFeedbackBuffer; }
	/** @deprecated Counters are now embedded in unified buffer */
	TRefCountPtr<FRDGPooledBuffer>& GetCounterBuffer() { static TRefCountPtr<FRDGPooledBuffer> Dummy; return Dummy; }
	/** @deprecated Use GetUnifiedFeedbackBuffer() instead */
	TRefCountPtr<FRDGPooledBuffer>& GetStaticMeshFeedbackBuffer() { return UnifiedFeedbackBuffer; }
	/** @deprecated Counters are now embedded in unified buffer */
	TRefCountPtr<FRDGPooledBuffer>& GetStaticMeshCounterBuffer() { static TRefCountPtr<FRDGPooledBuffer> Dummy; return Dummy; }
	/** @deprecated Use GetUnifiedFeedbackBuffer() instead */
	TRefCountPtr<FRDGPooledBuffer>& GetFluidInteractionSMFeedbackBuffer() { return UnifiedFeedbackBuffer; }
	/** @deprecated Counters are now embedded in unified buffer */
	TRefCountPtr<FRDGPooledBuffer>& GetFluidInteractionSMCounterBuffer() { static TRefCountPtr<FRDGPooledBuffer> Dummy; return Dummy; }

	//=========================================================================
	// Readback Processing (called from render thread after simulation)
	//=========================================================================

	/** Process collision feedback readback (non-blocking) */
	void ProcessFeedbackReadback(FRHICommandListImmediate& RHICmdList);

	/** Process contact count readback (non-blocking) */
	void ProcessContactCountReadback(FRHICommandListImmediate& RHICmdList);

	/** Enqueue copy for next frame's readback */
	void EnqueueReadbackCopy(FRHICommandListImmediate& RHICmdList);

	/** Increment frame counter (call after EnqueueCopy) */
	void IncrementFrameCounter();

	/** Get current write index for triple buffering */
	int32 GetCurrentWriteIndex() const { return CurrentWriteIndex; }

	//=========================================================================
	// Query API (thread-safe, callable from game thread)
	//=========================================================================

	/**
	 * Get collision feedback for a specific collider
	 * @param ColliderIndex - Index of the collider
	 * @param OutFeedback - Output array of feedback entries
	 * @param OutCount - Number of feedback entries
	 * @return true if feedback is available
	 */
	bool GetFeedbackForCollider(int32 ColliderIndex, TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount);

	/**
	 * Get all collision feedback (unfiltered, bone colliders only)
	 * @param OutFeedback - Output array of all feedback entries
	 * @param OutCount - Number of feedback entries
	 * @return true if feedback is available
	 */
	bool GetAllFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount);

	/** Get current feedback count (bone colliders) */
	int32 GetFeedbackCount() const { return ReadyFeedbackCount; }

	/**
	 * Get all StaticMesh collision feedback (BoneIndex < 0, bHasFluidInteraction = 0)
	 * WorldCollision StaticMesh without FluidInteraction component
	 * @param OutFeedback - Output array of StaticMesh feedback entries
	 * @param OutCount - Number of feedback entries
	 * @return true if feedback is available
	 */
	bool GetAllStaticMeshFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount);

	/** Get current StaticMesh feedback count (WorldCollision) */
	int32 GetStaticMeshFeedbackCount() const { return ReadyStaticMeshFeedbackCount; }

	/**
	 * Get all FluidInteraction StaticMesh collision feedback (BoneIndex < 0, bHasFluidInteraction = 1)
	 * StaticMesh with FluidInteraction component - used for buoyancy center calculation
	 * @param OutFeedback - Output array of FluidInteraction SM feedback entries
	 * @param OutCount - Number of feedback entries
	 * @return true if feedback is available
	 */
	bool GetAllFluidInteractionSMFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount);

	/** Get current FluidInteraction StaticMesh feedback count */
	int32 GetFluidInteractionSMFeedbackCount() const { return ReadyFluidInteractionSMFeedbackCount; }

	/**
	 * Get contact count for a specific collider index
	 * @param ColliderIndex - Index of the collider
	 * @return Number of particles colliding with this collider
	 */
	int32 GetContactCount(int32 ColliderIndex) const;

	/**
	 * Get all collider contact counts
	 * @param OutCounts - Output array of contact counts per collider
	 */
	void GetAllContactCounts(TArray<int32>& OutCounts) const;

private:
	//=========================================================================
	// State
	//=========================================================================

	bool bIsInitialized = false;
	bool bFeedbackEnabled = false;

	//=========================================================================
	// GPU Buffers (managed via RDG extraction)
	//=========================================================================

	// Unified feedback buffer containing all feedback types with embedded counters
	// Layout: [Header:16B][BoneFeedback][SMFeedback][FISMFeedback]
	TRefCountPtr<FRDGPooledBuffer> UnifiedFeedbackBuffer;

	// Contact count buffer (separate, needed even when feedback disabled)
	TRefCountPtr<FRDGPooledBuffer> ColliderContactCountBuffer;

	//=========================================================================
	// Async Readback Objects (triple buffered)
	//=========================================================================

	// Unified feedback readback (replaces 6 separate readbacks)
	FRHIGPUBufferReadback* UnifiedFeedbackReadbacks[NUM_FEEDBACK_BUFFERS] = {nullptr};

	// Contact count readback (unchanged)
	FRHIGPUBufferReadback* ContactCountReadbacks[NUM_FEEDBACK_BUFFERS] = {nullptr};

	//=========================================================================
	// Ready Data (thread-safe access via lock)
	//=========================================================================

	mutable FCriticalSection FeedbackLock;

	// Bone collider ready data (BoneIndex >= 0)
	TArray<FGPUCollisionFeedback> ReadyFeedback;
	int32 ReadyFeedbackCount = 0;

	// StaticMesh collider ready data (BoneIndex < 0, bHasFluidInteraction = 0, WorldCollision)
	TArray<FGPUCollisionFeedback> ReadyStaticMeshFeedback;
	int32 ReadyStaticMeshFeedbackCount = 0;

	// FluidInteraction StaticMesh ready data (BoneIndex < 0, bHasFluidInteraction = 1)
	TArray<FGPUCollisionFeedback> ReadyFluidInteractionSMFeedback;
	int32 ReadyFluidInteractionSMFeedbackCount = 0;

	TArray<int32> ReadyContactCounts;

	//=========================================================================
	// Triple Buffer State
	//=========================================================================

	int32 CurrentWriteIndex = 0;
	int32 FeedbackFrameNumber = 0;
	int32 ContactCountFrameNumber = 0;
	std::atomic<int32> CompletedFeedbackFrame{-1};
};
