// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RHIResources.h"
#include "RenderGraphResources.h"
#include "Core/KawaiiRenderParticle.h"
#include <atomic>

// Forward declaration
class FGPUFluidSimulator;
class FRDGBuilder;

/**
 * Render resource that manages fluid particle data as GPU buffers
 * Uploads CPU simulation data to GPU for use in Niagara/SSFR
 */
class KAWAIIFLUIDRUNTIME_API FKawaiiFluidRenderResource : public FRenderResource
{
public:
	FKawaiiFluidRenderResource();
	virtual ~FKawaiiFluidRenderResource();

	//========================================
	// FRenderResource Interface
	//========================================

	/** Initialize GPU resources (render thread) */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;

	/** Release GPU resources (render thread) */
	virtual void ReleaseRHI() override;

	//========================================
	// GPU buffer access (render thread)
	//========================================

	/** Get SRV (Shader Resource View) of Structured Buffer */
	FRHIShaderResourceView* GetParticleSRV() const { return ParticleSRV; }

	/** Get RHI buffer directly (for future integration) */
	FRHIBuffer* GetParticleBufferRHI() const 
	{ 
		return ParticleBuffer.GetReference(); 
	}

	/** Current particle count */
	int32 GetParticleCount() const { return ParticleCount; }

	/** Get buffer maximum capacity */
	int32 GetBufferCapacity() const { return BufferCapacity; }

	/** Check if buffer is valid */
	bool IsValid() const { return ParticleBuffer.IsValid() && ParticleSRV.IsValid(); }

	//========================================
	// GPU buffer access (Phase 2: GPU → GPU rendering)
	//========================================

	/** Get GPU Pooled Buffer (for RDG registration) - Legacy AoS */
	TRefCountPtr<FRDGPooledBuffer> GetPooledParticleBuffer() const
	{
		return PooledParticleBuffer;
	}

	/** Check if GPU buffer is valid (for Phase 2 path) */
	bool HasValidGPUBuffer() const
	{
		return PooledParticleBuffer.IsValid() && ParticleCount > 0 && bBufferReadyForRendering.load();
	}

	/** Mark buffer as ready for rendering (called after ExtractRenderDataPass completes) */
	void SetBufferReadyForRendering(bool bReady) { bBufferReadyForRendering = bReady; }

	//========================================
	// SoA (Structure of Arrays) buffer access
	// - Memory bandwidth optimization: 32B/particle → 12B/particle (for SDF)
	//========================================

	/** Position-only SoA buffer (float3 * N, 12B each) - SDF hotpath */
	TRefCountPtr<FRDGPooledBuffer> GetPooledPositionBuffer() const
	{
		return PooledPositionBuffer;
	}

	/** Velocity-only SoA buffer (float3 * N, 12B each) - for motion blur */
	TRefCountPtr<FRDGPooledBuffer> GetPooledVelocityBuffer() const
	{
		return PooledVelocityBuffer;
	}

	/** Check if SoA buffer is valid */
	bool HasValidSoABuffers() const
	{
		return PooledPositionBuffer.IsValid() && ParticleCount > 0 && bBufferReadyForRendering.load();
	}

	//========================================
	// GPU simulator interface
	//========================================

	/**
	 * Set GPU simulator reference (called from game thread)
	 * Allows render thread to directly access simulator buffers in GPU mode
	 * @param InSimulator GPU simulator reference (nullptr for CPU mode)
	 * @param InParticleCount GPU particle count
	 * @param InParticleRadius Particle radius
	 */
	void SetGPUSimulatorReference(FGPUFluidSimulator* InSimulator, int32 InParticleCount, float InParticleRadius);

	/** Clear GPU simulator reference (switch to CPU mode) */
	void ClearGPUSimulatorReference();

	/** Get current GPU simulator reference */
	FGPUFluidSimulator* GetGPUSimulator() const { return CachedGPUSimulator; }

	/** Check if in GPU simulator mode */
	bool HasGPUSimulator() const { return CachedGPUSimulator != nullptr; }

	/**
	 * Get unified particle count (called from render thread)
	 * GPU mode: GPU simulator's particle count
	 * CPU mode: Cached particle count
	 */
	int32 GetUnifiedParticleCount() const;

	/**
	 * Get unified particle radius
	 */
	float GetUnifiedParticleRadius() const { return CachedParticleRadius; }

	/**
	 * Get Physics buffer SRV (called from render thread)
	 * GPU mode: GPUSimulator's PersistentParticleBuffer (FGPUFluidParticle format)
	 * CPU mode: nullptr (no Physics buffer)
	 * @param GraphBuilder RDG builder
	 * @return Physics buffer SRV or nullptr
	 */
	FRDGBufferSRVRef GetPhysicsBufferSRV(FRDGBuilder& GraphBuilder) const;

	/**
	 * Get Anisotropy Axis buffers (valid only in GPU mode)
	 * @param GraphBuilder RDG builder
	 * @param OutAxis1SRV Axis1 buffer SRV
	 * @param OutAxis2SRV Axis2 buffer SRV
	 * @param OutAxis3SRV Axis3 buffer SRV
	 * @return true if Anisotropy buffers are valid
	 */
	bool GetAnisotropyBufferSRVs(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef& OutAxis1SRV,
		FRDGBufferSRVRef& OutAxis2SRV,
		FRDGBufferSRVRef& OutAxis3SRV) const;

	/** Check if Anisotropy is enabled */
	bool IsAnisotropyEnabled() const;

	//========================================
	// Bounds data (for SDF volume)
	//========================================

	/**
	 * Set GPU Bounds buffer (called from render thread)
	 * Set after ExtractRenderDataWithBoundsPass in ViewExtension
	 */
	void SetBoundsBuffer(TRefCountPtr<FRDGPooledBuffer> InBoundsBuffer);

	/** Get GPU Bounds buffer */
	TRefCountPtr<FRDGPooledBuffer> GetPooledBoundsBuffer() const { return PooledBoundsBuffer; }

	/** Check if Bounds buffer is valid */
	bool HasValidBoundsBuffer() const { return PooledBoundsBuffer.IsValid(); }

	/**
	 * Set FKawaiiRenderParticle buffer (called from render thread)
	 * Set after ExtractRenderDataPass in ViewExtension
	 */
	void SetRenderParticleBuffer(TRefCountPtr<FRDGPooledBuffer> InBuffer);

	/** Get FKawaiiRenderParticle buffer (for SDF iteration) */
	TRefCountPtr<FRDGPooledBuffer> GetPooledRenderParticleBuffer() const { return PooledRenderParticleBuffer; }

	/** Get Bounds buffer pointer (for QueueBufferExtraction) */
	TRefCountPtr<FRDGPooledBuffer>* GetPooledBoundsBufferPtr() { return &PooledBoundsBuffer; }

	/** Get RenderParticle buffer pointer (for QueueBufferExtraction) */
	TRefCountPtr<FRDGPooledBuffer>* GetPooledRenderParticleBufferPtr() { return &PooledRenderParticleBuffer; }

	/** Get Position buffer pointer (for QueueBufferExtraction) */
	TRefCountPtr<FRDGPooledBuffer>* GetPooledPositionBufferPtr() { return &PooledPositionBuffer; }

	/** Get Velocity buffer pointer (for QueueBufferExtraction) */
	TRefCountPtr<FRDGPooledBuffer>* GetPooledVelocityBufferPtr() { return &PooledVelocityBuffer; }

	//========================================
	// Z-Order buffer access (for Ray Marching volume building)
	//========================================

	/**
	 * Get Z-Order CellStart buffer (fetched directly from GPU simulator)
	 * Used as cell start index in Ray Marching volume building
	 */
	TRefCountPtr<FRDGPooledBuffer> GetPooledCellStartBuffer() const;

	/**
	 * Get Z-Order CellEnd buffer (fetched directly from GPU simulator)
	 * Used as cell end index in Ray Marching volume building
	 */
	TRefCountPtr<FRDGPooledBuffer> GetPooledCellEndBuffer() const;

	/**
	 * Check if Z-Order buffer is valid
	 * Returns true if GPU simulator exists and Z-Order buffer is valid
	 */
	bool HasValidZOrderBuffers() const;

private:
	//========================================
	// GPU resources
	//========================================

	/** Structured Buffer (GPU memory) */
	FBufferRHIRef ParticleBuffer;

	/** Shader Resource View (for shader read) */
	FShaderResourceViewRHIRef ParticleSRV;

	/** Unordered Access View (for shader write - Phase 2 GPU→GPU copy) */
	FUnorderedAccessViewRHIRef ParticleUAV;

	/** Pooled Buffer for RDG registration (Phase 2 GPU→GPU copy) - Legacy AoS */
	TRefCountPtr<FRDGPooledBuffer> PooledParticleBuffer;

	//========================================
	// SoA (Structure of Arrays) buffers
	//========================================

	/** Position-only buffer (float3 * N, 12B each) - SDF hotpath */
	TRefCountPtr<FRDGPooledBuffer> PooledPositionBuffer;

	/** Velocity-only buffer (float3 * N, 12B each) - for motion blur */
	TRefCountPtr<FRDGPooledBuffer> PooledVelocityBuffer;

	/** Bounds buffer (float3 * 2: Min, Max) - SDF volume range */
	TRefCountPtr<FRDGPooledBuffer> PooledBoundsBuffer;

	/** FKawaiiRenderParticle buffer (for SDF iteration) */
	TRefCountPtr<FRDGPooledBuffer> PooledRenderParticleBuffer;

	/** Current particle count stored in buffer */
	int32 ParticleCount;

	/** Buffer maximum capacity (minimize reallocation) */
	int32 BufferCapacity;

	/** Buffer is ready for rendering (ExtractRenderDataPass has completed) */
	std::atomic<bool> bBufferReadyForRendering{false};

	//========================================
	// GPU simulator reference
	//========================================

	/** GPU simulator reference (for direct buffer access from render thread) */
	std::atomic<FGPUFluidSimulator*> CachedGPUSimulator{nullptr};

	/** Cached particle count */
	std::atomic<int32> CachedGPUParticleCount{0};

	/** Cached particle radius */
	std::atomic<float> CachedParticleRadius{10.0f};

	//========================================
	// Internal helpers
	//========================================

	/** Check if buffer resize is needed */
	bool NeedsResize(int32 NewCount) const;

	/** Recreate buffer (when size changes) */
	void ResizeBuffer(FRHICommandListBase& RHICmdList, int32 NewCapacity);
};
