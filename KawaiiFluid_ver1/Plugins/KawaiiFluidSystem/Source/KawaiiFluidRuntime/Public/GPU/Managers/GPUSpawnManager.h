// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUSpawnManager - Thread-safe particle spawn queue manager

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "GPU/GPUFluidParticle.h"
#include <atomic>

class FRHIGPUBufferReadback;
class FRDGBuilder;

/**
 * FGPUSpawnManager
 *
 * Manages thread-safe particle spawn requests for GPU fluid simulation.
 * Game thread queues spawn requests, render thread processes them.
 * Uses double-buffering to avoid lock contention during simulation.
 */
class KAWAIIFLUIDRUNTIME_API FGPUSpawnManager
{
public:
	FGPUSpawnManager();
	~FGPUSpawnManager();

	//=========================================================================
	// Lifecycle
	//=========================================================================

	/** Initialize the spawn manager with max particle capacity */
	void Initialize(int32 InMaxParticleCount);

	/** Release all resources */
	void Release();

	/** Check if initialized and ready */
	bool IsReady() const { return bIsInitialized; }

	/** Reset all state (clear all requests and reset NextParticleID) */
	void Reset()
	{
		FScopeLock SpawnGuard(&SpawnLock);
		FScopeLock DespawnGuard(&DespawnByIDLock);

		PendingSpawnRequests.Empty();
		ActiveSpawnRequests.Empty();
		PendingDespawnByIDs.Empty();
		ActiveDespawnByIDs.Empty();
		AlreadyRequestedIDs.Empty();

		bHasPendingSpawnRequests.store(false);
		bHasPendingDespawnByIDRequests.store(false);
		NextParticleID.store(0);
	}

	/** Clear only despawn tracking state (keeps NextParticleID intact)
	 * Use this when uploading particles with new IDs - clears stale despawn requests
	 * without disrupting the atomic ID allocation from AllocateParticleIDs()
	 */
	void ClearDespawnTracking()
	{
		FScopeLock Lock(&DespawnByIDLock);
		PendingDespawnByIDs.Empty();
		ActiveDespawnByIDs.Empty();
		AlreadyRequestedIDs.Empty();
		bHasPendingDespawnByIDRequests.store(false);
	}

	//=========================================================================
	// Thread-Safe Public API (callable from any thread)
	//=========================================================================

	/**
	 * Add a spawn request (thread-safe)
	 * @param Position - World position to spawn at
	 * @param Velocity - Initial velocity
	 * @param Mass - Particle mass (0 = use default)
	 */
	void AddSpawnRequest(const FVector3f& Position, const FVector3f& Velocity, float Mass = 1.0f);

	/**
	 * Add multiple spawn requests at once (thread-safe, more efficient)
	 * @param Requests - Array of spawn requests
	 */
	void AddSpawnRequests(const TArray<FGPUSpawnRequest>& Requests);

	/** Clear all pending spawn requests */
	void ClearSpawnRequests();

	/** Get number of pending spawn requests (thread-safe) */
	int32 GetPendingSpawnCount() const;

	/** Check if there are pending spawn requests (lock-free) */
	bool HasPendingSpawnRequests() const { return bHasPendingSpawnRequests.load(); }

	//=========================================================================
	// ID-Based Despawn API (Thread-Safe)
	//=========================================================================

	/**
	 * Add a despawn request by particle ID (thread-safe)
	 * @param ParticleID - ID of particle to despawn
	 */
	void AddDespawnByIDRequest(int32 ParticleID);

	/**
	 * Cleanup AlreadyRequestedIDs based on currently alive particles.
	 * Both arrays are sorted by ParticleID, so std::set_intersection is used O(n+m).
	 * @param AliveParticleIDs - Sorted array of IDs currently alive on GPU
	 */
	void CleanupCompletedRequests(const TArray<int32>& AliveParticleIDs);

	/**
	 * Add multiple despawn requests by particle IDs (thread-safe, more efficient)
	 * CleanupCompletedRequests is called from ProcessStatsReadback when readback completes
	 * @param ParticleIDs - Array of particle IDs to despawn
	 */
	void AddDespawnByIDRequests(const TArray<int32>& ParticleIDs);

	/** Swap pending ID despawn requests to active buffer (call at start of simulation frame)
	 * @return Number of IDs to despawn this frame
	 */
	int32 SwapDespawnByIDBuffers();

	/** Get number of pending ID despawn requests (thread-safe) */
	int32 GetPendingDespawnByIDCount() const;

	/** Check if there are pending ID despawn requests (lock-free) */
	bool HasPendingDespawnByIDRequests() const { return bHasPendingDespawnByIDRequests.load(); }

	/**
	 * Add ID-based despawn particles RDG pass
	 * Uses binary search on sorted ID list for O(log n) lookup per particle
	 * @param GraphBuilder - RDG builder
	 * @param InOutParticleBuffer - Particle Buffer After Remove
	 * @param InOutParticleCount - Particle Count After Remove
	 */
	void AddDespawnByIDPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef& InOutParticleBuffer,
		int32& InOutParticleCount);

	//=========================================================================
	// Source Counter API (Per-Component Particle Count Tracking)
	//=========================================================================

	/**
	 * Get persistent source counter buffer for RDG pass
	 * Buffer contains [MaxSourceCount] uint32 counters, one per SourceID
	 */
	TRefCountPtr<FRDGPooledBuffer> GetSourceCounterBuffer() const { return SourceCounterBuffer; }

	/**
	 * Register source counter buffer for RDG and get UAV
	 * Call this in simulation passes that need to update source counters
	 */
	FRDGBufferUAVRef RegisterSourceCounterUAV(FRDGBuilder& GraphBuilder);

	/**
	 * Get particle count for a specific source (component)
	 * Returns cached value from last readback (2-3 frame latency)
	 * @param SourceID - Source component ID (0 to MaxSourceCount-1)
	 */
	int32 GetParticleCountForSource(int32 SourceID) const;

	/**
	 * Get all source counts (returns copy of cached array)
	 */
	TArray<int32> GetAllSourceCounts() const;

	/**
	 * Enqueue source counter readback (call from render thread after simulation)
	 */
	void EnqueueSourceCounterReadback(FRHICommandListImmediate& RHICmdList);

	/**
	 * Process source counter readback (check completion, copy to cache)
	 * Call from game thread or render thread before querying counts
	 */
	void ProcessSourceCounterReadback();

	/**
	 * Clear all source counters (call when clearing all particles)
	 */
	void ClearSourceCounters(FRDGBuilder& GraphBuilder);

	/**
	 * Initialize source counters from uploaded particles (for level load)
	 * Counts particles by SourceID and uploads to GPU buffer + CPU cache
	 * @param Particles - Array of particles to count by SourceID
	 */
	void InitializeSourceCountersFromParticles(const TArray<FGPUFluidParticle>& Particles);

	//=========================================================================
	// Configuration
	//=========================================================================

	/** Set default particle radius for spawning */
	void SetDefaultRadius(float Radius) { DefaultSpawnRadius = Radius; }

	/** Get default particle radius */
	float GetDefaultRadius() const { return DefaultSpawnRadius; }

	/** Set default particle mass (used when spawn request Mass = 0) */
	void SetDefaultMass(float Mass) { DefaultSpawnMass = Mass; }

	/** Get default particle mass */
	float GetDefaultMass() const { return DefaultSpawnMass; }

	/** Get next particle ID that will be assigned */
	int32 GetNextParticleID() const { return NextParticleID.load(); }

	/** Set next particle ID (for PIE transition - sync with uploaded particles) */
	void SetNextParticleID(int32 ID) { NextParticleID.store(ID); }

	/** Atomically allocate a range of particle IDs (thread-safe for multi-module upload)
	 * @param Count - Number of IDs to allocate
	 * @return Starting ID of allocated range [StartID, StartID + Count)
	 */
	int32 AllocateParticleIDs(int32 Count) { return NextParticleID.fetch_add(Count); }

	/** Reset NextParticleID to 0 when particle count is 0 (prevents overflow) */
	void TryResetParticleID(int32 CurrentParticleCount)
	{
		if (CurrentParticleCount == 0)
		{
			NextParticleID.store(0);
		}
	}

	//=========================================================================
	// Render Thread API
	//=========================================================================

	/**
	 * Swap pending requests to active buffer (call at start of simulation frame)
	 * This moves pending requests from game thread buffer to render thread buffer.
	 * Must be called from render thread.
	 */
	void SwapBuffers();

	/** Check if there are active requests to process */
	bool HasActiveRequests() const { return ActiveSpawnRequests.Num() > 0; }

	/** Get number of active spawn requests for this frame */
	int32 GetActiveRequestCount() const { return ActiveSpawnRequests.Num(); }

	/** Get the active spawn requests array (render thread only) */
	const TArray<FGPUSpawnRequest>& GetActiveRequests() const { return ActiveSpawnRequests; }

	/** Clear active requests after processing */
	void ClearActiveRequests() { ActiveSpawnRequests.Empty(); }

	/**
	 * Add spawn particles RDG pass
	 * @param GraphBuilder - RDG builder
	 * @param ParticlesUAV - Particle buffer UAV
	 * @param ParticleCounterUAV - Atomic counter for particle allocation
	 * @param MaxParticleCount - Maximum particle capacity (buffer size)
	 */
	void AddSpawnParticlesPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferUAVRef ParticleCounterUAV,
		int32 MaxParticleCount);

	/**
	 * Update next particle ID after spawning
	 * @param SpawnedCount - Number of particles successfully spawned
	 */
	void OnSpawnComplete(int32 SpawnedCount);

private:
	//=========================================================================
	// State
	//=========================================================================

	bool bIsInitialized = false;
	int32 MaxParticleCapacity = 0;

	//=========================================================================
	// Double-Buffered Spawn Requests
	//=========================================================================
	TArray<FGPUSpawnRequest> PendingSpawnRequests;
	TArray<FGPUSpawnRequest> ActiveSpawnRequests;
	mutable FCriticalSection SpawnLock;

	// Lock-free flag for quick pending check
	std::atomic<bool> bHasPendingSpawnRequests{false};

	//=========================================================================
	// Double-Buffered ID-Based Despawn Requests
	//=========================================================================
	TArray<int32> PendingDespawnByIDs;
	TArray<int32> ActiveDespawnByIDs;
	TArray<int32> AlreadyRequestedIDs;  // Maintained in sorted order, uses std::set_* operations
	mutable FCriticalSection DespawnByIDLock;

	// Lock-free flag for quick pending check
	std::atomic<bool> bHasPendingDespawnByIDRequests{false};

	//=========================================================================
	// Particle ID Tracking
	//=========================================================================

	// Next particle ID to assign (atomic for thread safety)
	std::atomic<int32> NextParticleID{0};

	//=========================================================================
	// Configuration
	//=========================================================================

	float DefaultSpawnRadius = 5.0f;
	float DefaultSpawnMass = 1.0f;

	//=========================================================================
	// Source Counter (Per-Component Particle Count)
	//=========================================================================

	// GPU buffer storing per-source particle counts [MaxSourceCount]
	TRefCountPtr<FRDGPooledBuffer> SourceCounterBuffer;

	// Ring buffer for async GPU→CPU readback (handles GPU latency)
	static constexpr int32 SourceCounterRingBufferSize = 4;
	TArray<FRHIGPUBufferReadback*> SourceCounterReadbacks;
	int32 SourceCounterWriteIndex = 0;  // Next slot to write (enqueue)
	int32 SourceCounterReadIndex = 0;   // Next slot to read (process)
	int32 SourceCounterPendingCount = 0; // Number of pending readbacks

	// CPU-cached source counts (updated from readback)
	TArray<int32> CachedSourceCounts;
	mutable FCriticalSection SourceCountLock;
};
