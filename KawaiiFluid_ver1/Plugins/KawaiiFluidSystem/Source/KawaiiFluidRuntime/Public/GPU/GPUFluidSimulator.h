// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIResources.h"
#include "RenderResource.h"
#include "GPU/GPUFluidParticle.h"
#include <atomic>

// Forward declarations
struct FFluidParticle;
class FRDGBuilder;

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
	 * Get current particle count on GPU
	 */
	int32 GetParticleCount() const { return CurrentParticleCount; }

	/**
	 * Get maximum particle capacity
	 */
	int32 GetMaxParticleCount() const { return MaxParticleCount; }

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

	//=============================================================================
	// Configuration
	//=============================================================================

	/** Set external force to apply (e.g., wind, explosions) */
	void SetExternalForce(const FVector3f& Force) { ExternalForce = Force; }

	/** Set velocity damping factor (0-1) */
	void SetVelocityDamping(float Damping) { VelocityDamping = FMath::Clamp(Damping, 0.0f, 1.0f); }

	/** Set maximum velocity cap */
	void SetMaxVelocity(float MaxVel) { MaxVelocity = FMath::Max(MaxVel, 0.0f); }

	//=============================================================================
	// Distance Field Collision
	//=============================================================================

	/**
	 * Enable or disable Distance Field collision
	 * @param bEnabled - Whether to use DF collision
	 */
	void SetDistanceFieldCollisionEnabled(bool bEnabled) { DFCollisionParams.bEnabled = bEnabled ? 1 : 0; }

	/**
	 * Set Distance Field collision parameters
	 * @param Params - DF collision configuration
	 */
	void SetDistanceFieldCollisionParams(const FGPUDistanceFieldCollisionParams& Params) { DFCollisionParams = Params; }

	/**
	 * Get Distance Field collision parameters
	 */
	const FGPUDistanceFieldCollisionParams& GetDistanceFieldCollisionParams() const { return DFCollisionParams; }

	/**
	 * Set the Global Distance Field texture SRV
	 * This should be set from the scene renderer before simulation
	 * @param SRV - The GDF texture SRV
	 */
	void SetGlobalDistanceFieldSRV(FRDGTextureSRVRef SRV) { CachedGDFTextureSRV = SRV; }

	/** Check if Distance Field collision is enabled */
	bool IsDistanceFieldCollisionEnabled() const { return DFCollisionParams.bEnabled != 0; }

	//=============================================================================
	// Collision Primitives (from FluidCollider system)
	//=============================================================================

	/**
	 * Upload collision primitives to GPU
	 * Call this before simulation each frame with collision data from FluidCollider
	 * @param Primitives - Collection of collision primitives
	 */
	void UploadCollisionPrimitives(const FGPUCollisionPrimitives& Primitives);

	/**
	 * Set primitive collision threshold
	 * @param Threshold - Distance threshold for collision detection
	 */
	void SetPrimitiveCollisionThreshold(float Threshold) { PrimitiveCollisionThreshold = Threshold; }

	/**
	 * Check if collision primitives are available
	 */
	bool HasCollisionPrimitives() const { return bCollisionPrimitivesValid; }

	/**
	 * Get total number of collision primitives
	 */
	int32 GetCollisionPrimitiveCount() const
	{
		return CachedSpheres.Num() + CachedCapsules.Num() + CachedBoxes.Num() + CachedConvexHeaders.Num();
	}

	//=============================================================================
	// GPU Adhesion System (Bone-based attachment tracking)
	//=============================================================================

	/**
	 * Set adhesion parameters for GPU-based attachment
	 * @param Params - Adhesion configuration
	 */
	void SetAdhesionParams(const FGPUAdhesionParams& Params) { CachedAdhesionParams = Params; }

	/**
	 * Get adhesion parameters
	 */
	const FGPUAdhesionParams& GetAdhesionParams() const { return CachedAdhesionParams; }

	/**
	 * Check if adhesion is enabled
	 */
	bool IsAdhesionEnabled() const { return CachedAdhesionParams.bEnableAdhesion != 0; }

	/**
	 * Get bone transform count
	 */
	int32 GetBoneTransformCount() const { return CachedBoneTransforms.Num(); }

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
	int32 GetFilteredCandidateCount() const { return FilteredCandidateCount; }

	/**
	 * Check if filtered candidates are available
	 */
	bool HasFilteredCandidates() const { return bHasFilteredCandidates; }

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
	void SetDefaultSpawnRadius(float Radius) { DefaultSpawnRadius = Radius; }

	/**
	 * Get next particle ID to assign
	 */
	int32 GetNextParticleID() const { return NextParticleID.load(); }

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
		FRDGBufferUAVRef ParticleIndicesUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add compute density pass */
	void AddComputeDensityPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add solve pressure pass */
	void AddSolvePressurePass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add apply viscosity pass */
	void AddApplyViscosityPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add apply cohesion pass (surface tension / cohesion forces) */
	void AddApplyCohesionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
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

	/** Add spawn particles pass (GPU-based particle creation from spawn requests) */
	void AddSpawnParticlesPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferUAVRef ParticleCounterUAV,
		const TArray<FGPUSpawnRequest>& SpawnRequests);

	/** Add adhesion pass (bone-based attachment tracking) */
	void AddAdhesionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferUAVRef AttachmentUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add update attached positions pass (move attached particles with bones) */
	void AddUpdateAttachedPositionsPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef AttachmentSRV,
		FRDGBufferSRVRef InBoneTransformsSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add update attached positions pass - internal version using RW attachment buffer */
	void AddUpdateAttachedPositionsPassInternal(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferUAVRef AttachmentUAV,
		const FGPUFluidSimulationParams& Params);

	/** Clear just-detached flag at end of frame */
	void AddClearDetachedFlagPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV);

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
	float VelocityDamping;
	float MaxVelocity;

	// Distance Field Collision
	FGPUDistanceFieldCollisionParams DFCollisionParams;
	FRDGTextureSRVRef CachedGDFTextureSRV;

	//=============================================================================
	// Collision Primitives (from FluidCollider system)
	//=============================================================================

	// Cached collision primitive data (CPU side, for RDG buffer creation each frame)
	TArray<FGPUCollisionSphere> CachedSpheres;
	TArray<FGPUCollisionCapsule> CachedCapsules;
	TArray<FGPUCollisionBox> CachedBoxes;
	TArray<FGPUCollisionConvex> CachedConvexHeaders;
	TArray<FGPUConvexPlane> CachedConvexPlanes;

	// Collision threshold for primitives
	float PrimitiveCollisionThreshold = 1.0f;

	// Flag: primitives need upload
	bool bCollisionPrimitivesValid = false;

	// Critical section for thread-safe buffer access
	FCriticalSection BufferLock;

	//=============================================================================
	// Bone Transform and Adhesion Buffers
	//=============================================================================

	// Cached bone transforms (CPU side, for RDG buffer creation each frame)
	TArray<FGPUBoneTransform> CachedBoneTransforms;

	// Cached adhesion parameters
	FGPUAdhesionParams CachedAdhesionParams;

	// Attachment data buffer (one per particle)
	TRefCountPtr<FRDGPooledBuffer> PersistentAttachmentBuffer;
	int32 AttachmentBufferSize = 0;  // Track buffer size for resize detection

	// Bone transforms buffer
	FBufferRHIRef BoneTransformsBufferRHI;
	FShaderResourceViewRHIRef BoneTransformsSRV;

	// Flag: bone transforms valid
	bool bBoneTransformsValid = false;

	//=============================================================================
	// GPU Particle Spawn System
	// Thread-safe spawn request queue processed on render thread
	//=============================================================================

	// Double-buffered spawn requests for thread safety
	// GameThread writes to PendingSpawnRequests, RenderThread consumes from ActiveSpawnRequests
	TArray<FGPUSpawnRequest> PendingSpawnRequests;
	TArray<FGPUSpawnRequest> ActiveSpawnRequests;

	// Lock for spawn request queue access (mutable for const methods)
	mutable FCriticalSection SpawnRequestLock;

	// GPU Counter buffer for atomic particle count
	TRefCountPtr<FRDGPooledBuffer> ParticleCounterBuffer;

	// Next particle ID to assign (atomic for thread safety)
	std::atomic<int32> NextParticleID{0};

	// Default spawn radius
	float DefaultSpawnRadius = 5.0f;

	// Flag: spawn requests are pending
	std::atomic<bool> bHasPendingSpawnRequests{false};

	//=============================================================================
	// Stream Compaction Buffers (Phase 2 - Per-Polygon Collision)
	//=============================================================================

	// Marked flags buffer (0 or 1 per particle)
	FBufferRHIRef MarkedFlagsBufferRHI;
	FShaderResourceViewRHIRef MarkedFlagsSRV;
	FUnorderedAccessViewRHIRef MarkedFlagsUAV;

	// Marked AABB index buffer (-1 or interaction index per particle)
	FBufferRHIRef MarkedAABBIndexBufferRHI;
	FShaderResourceViewRHIRef MarkedAABBIndexSRV;
	FUnorderedAccessViewRHIRef MarkedAABBIndexUAV;

	// Prefix sum buffer
	FBufferRHIRef PrefixSumsBufferRHI;
	FShaderResourceViewRHIRef PrefixSumsSRV;
	FUnorderedAccessViewRHIRef PrefixSumsUAV;

	// Block sums buffer for multi-block prefix sum
	FBufferRHIRef BlockSumsBufferRHI;
	FShaderResourceViewRHIRef BlockSumsSRV;
	FUnorderedAccessViewRHIRef BlockSumsUAV;

	// Compacted candidates buffer
	FBufferRHIRef CompactedCandidatesBufferRHI;
	FUnorderedAccessViewRHIRef CompactedCandidatesUAV;

	// Total count buffer (single uint)
	FBufferRHIRef TotalCountBufferRHI;
	FUnorderedAccessViewRHIRef TotalCountUAV;

	// Filter AABBs buffer (uploaded from CPU each frame)
	FBufferRHIRef FilterAABBsBufferRHI;
	FShaderResourceViewRHIRef FilterAABBsSRV;

	// Staging buffers for GPU→CPU readback
	FBufferRHIRef TotalCountStagingBufferRHI;
	FBufferRHIRef CandidatesStagingBufferRHI;

	// Stream compaction state
	int32 FilteredCandidateCount = 0;
	bool bHasFilteredCandidates = false;
	int32 CurrentFilterAABBCount = 0;
	bool bStreamCompactionBuffersAllocated = false;

	// Helper methods for stream compaction
	void AllocateStreamCompactionBuffers(FRHICommandListImmediate& RHICmdList);
	void ReleaseStreamCompactionBuffers();
	void DispatchStreamCompactionShaders(FRHICommandListImmediate& RHICmdList, int32 InParticleCount, int32 InNumAABBs, FShaderResourceViewRHIRef InParticleSRV);
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
