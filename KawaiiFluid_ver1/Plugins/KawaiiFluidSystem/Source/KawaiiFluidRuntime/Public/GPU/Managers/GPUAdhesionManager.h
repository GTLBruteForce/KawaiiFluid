// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUAdhesionManager - GPU-based particle adhesion system manager

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIResources.h"
#include "GPU/GPUFluidParticle.h"
#include "GPU/GPUFluidSpatialData.h"

class FRHICommandListImmediate;
class FRDGBuilder;
class FGPUCollisionManager;

/**
 * FGPUAdhesionManager
 *
 * Manages GPU-based particle adhesion system:
 * - Particle attachment to bone-based collision primitives
 * - Attachment tracking buffer (per particle)
 * - Update attached particle positions
 * - Detachment handling
 * - Stack pressure calculation for attached particles
 *
 * This consolidates adhesion logic that was previously in GPUFluidSimulator
 */
class KAWAIIFLUIDRUNTIME_API FGPUAdhesionManager
{
public:
	FGPUAdhesionManager();
	~FGPUAdhesionManager();

	//=========================================================================
	// Lifecycle
	//=========================================================================

	/** Initialize the adhesion manager */
	void Initialize();

	/** Release all resources */
	void Release();

	/** Check if manager is ready */
	bool IsReady() const { return bIsInitialized; }

	//=========================================================================
	// Configuration
	//=========================================================================

	/** Set adhesion parameters */
	void SetAdhesionParams(const FGPUAdhesionParams& Params) { AdhesionParams = Params; }

	/** Get adhesion parameters */
	const FGPUAdhesionParams& GetAdhesionParams() const { return AdhesionParams; }

	/** Check if adhesion is enabled */
	bool IsAdhesionEnabled() const { return AdhesionParams.bEnableAdhesion != 0; }

	//=========================================================================
	// Attachment Buffer Management
	//=========================================================================

	/** Get persistent attachment buffer */
	TRefCountPtr<FRDGPooledBuffer> GetPersistentAttachmentBuffer() const { return PersistentAttachmentBuffer; }

	/** Access persistent attachment buffer for writing */
	TRefCountPtr<FRDGPooledBuffer>& AccessPersistentAttachmentBuffer() { return PersistentAttachmentBuffer; }

	/** Get attachment buffer size */
	int32 GetAttachmentBufferSize() const { return AttachmentBufferSize; }

	/** Set attachment buffer size */
	void SetAttachmentBufferSize(int32 Size) { AttachmentBufferSize = Size; }

	//=========================================================================
	// Adhesion Passes (called from render thread)
	//=========================================================================

	/**
	 * Add adhesion pass (create attachments to bone colliders)
	 * @param GraphBuilder - RDG builder
	 * @param ParticlesUAV - Particle buffer UAV
	 * @param AttachmentUAV - Attachment buffer UAV
	 * @param CollisionManager - Collision manager for bone transforms and primitives
	 * @param CurrentParticleCount - Current particle count
	 * @param Params - Simulation parameters
	 */
	void AddAdhesionPass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
		FRDGBufferUAVRef AttachmentUAV,
		FGPUCollisionManager* CollisionManager,
		int32 CurrentParticleCount,
		const FGPUFluidSimulationParams& Params);

	/**
	 * Add update attached positions pass (move attached particles with bones)
	 * @param GraphBuilder - RDG builder
	 * @param SpatialData - Spatial hash data with SOA buffers
	 * @param AttachmentUAV - Attachment buffer UAV
	 * @param CollisionManager - Collision manager for bone transforms and primitives
	 * @param CurrentParticleCount - Current particle count
	 * @param Params - Simulation parameters
	 */
	void AddUpdateAttachedPositionsPass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
		FRDGBufferUAVRef AttachmentUAV,
		FGPUCollisionManager* CollisionManager,
		int32 CurrentParticleCount,
		const FGPUFluidSimulationParams& Params);

	/**
	 * Add clear detached flag pass (clear just-detached flag at end of frame)
	 * @param GraphBuilder - RDG builder
	 * @param SpatialData - Spatial hash data with SOA buffers
	 * @param CurrentParticleCount - Current particle count
	 */
	void AddClearDetachedFlagPass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
		int32 CurrentParticleCount);

	/**
	 * Add stack pressure pass (weight transfer from stacked attached particles)
	 * @param GraphBuilder - RDG builder
	 * @param ParticlesUAV - Particle buffer UAV
	 * @param AttachmentSRV - Attachment buffer SRV
	 * @param CellCountsSRV - Spatial hash cell counts SRV
	 * @param ParticleIndicesSRV - Spatial hash particle indices SRV
	 * @param CollisionManager - Collision manager for primitives
	 * @param CurrentParticleCount - Current particle count
	 * @param Params - Simulation parameters
	 */
	void AddStackPressurePass(
		FRDGBuilder& GraphBuilder,
		const FSimulationSpatialData& SpatialData,
		FRDGBufferSRVRef AttachmentSRV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		FGPUCollisionManager* CollisionManager,
		int32 CurrentParticleCount,
		const FGPUFluidSimulationParams& Params);

private:
	//=========================================================================
	// State
	//=========================================================================

	bool bIsInitialized = false;
	mutable FCriticalSection AdhesionLock;

	//=========================================================================
	// Adhesion Parameters
	//=========================================================================

	FGPUAdhesionParams AdhesionParams;

	//=========================================================================
	// Attachment Buffer
	//=========================================================================

	// Persistent attachment buffer (one entry per particle)
	TRefCountPtr<FRDGPooledBuffer> PersistentAttachmentBuffer;
	int32 AttachmentBufferSize = 0;  // Track buffer size for resize detection
};
