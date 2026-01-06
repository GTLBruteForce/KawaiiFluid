// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphDefinitions.h"

/**
 * @brief Manages SDF volume texture for optimized ray marching
 *
 * Creates and updates a 3D texture containing baked SDF values.
 * This allows O(1) SDF lookup during ray marching instead of O(N) particle iteration.
 *
 * Usage:
 * 1. Call CalculateGPUBounds() to get accurate particle bounds from GPU buffer
 * 2. Call BakeSDFVolume() each frame with particle data
 * 3. Use GetSDFVolumeTexture() in ray marching shader
 * 4. Use GetVolumeMin/Max() for world-space to UV conversion
 */
class KAWAIIFLUIDRUNTIME_API FSDFVolumeManager
{
public:
	FSDFVolumeManager();
	~FSDFVolumeManager();

	/**
	 * @brief Calculate particle bounds on GPU using parallel reduction
	 *
	 * Dispatches compute shader to calculate min/max bounds of all particles.
	 * Must be called before BakeSDFVolume() when using GPU particle buffer.
	 *
	 * @param GraphBuilder - RDG graph builder
	 * @param ParticleBufferSRV - Structured buffer containing particle positions
	 * @param ParticleCount - Number of particles
	 * @param ParticleRadius - Radius of each particle (for bounds expansion)
	 * @param Margin - Additional margin around bounds
	 * @return Buffer containing [Min, Max] as FVector3f[2]
	 */
	FRDGBufferRef CalculateGPUBounds(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef ParticleBufferSRV,
		int32 ParticleCount,
		float ParticleRadius,
		float Margin);

	/**
	 * @brief Bake SDF volume using pre-calculated GPU bounds (legacy - uses cached bounds)
	 *
	 * Uses bounds from CalculateGPUBounds() to create accurate SDF volume.
	 * NOTE: This method still uses 1-frame latency cached bounds.
	 *
	 * @param GraphBuilder - RDG graph builder
	 * @param ParticleBufferSRV - Structured buffer containing particle positions
	 * @param ParticleCount - Number of particles
	 * @param ParticleRadius - Radius of each particle
	 * @param SDFSmoothness - Smooth minimum factor for metaball blending
	 * @param BoundsBuffer - GPU buffer containing [Min, Max] from CalculateGPUBounds()
	 * @return Created SDF volume texture SRV for use in ray marching
	 */
	FRDGTextureSRVRef BakeSDFVolumeWithGPUBounds(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef ParticleBufferSRV,
		int32 ParticleCount,
		float ParticleRadius,
		float SDFSmoothness,
		FRDGBufferRef BoundsBuffer);

	/**
	 * @brief Bake SDF volume reading bounds directly from GPU buffer (Optimized - no readback)
	 *
	 * Reads bounds from GPU buffer in same frame, eliminating 1-frame latency.
	 * This is the preferred method for GPU simulation mode.
	 *
	 * @param GraphBuilder - RDG graph builder
	 * @param ParticleBufferSRV - Structured buffer containing particle positions
	 * @param ParticleCount - Number of particles
	 * @param ParticleRadius - Radius of each particle
	 * @param SDFSmoothness - Smooth minimum factor for metaball blending
	 * @param BoundsBufferSRV - SRV for GPU buffer containing [Min, Max]
	 * @return Created SDF volume texture SRV for use in ray marching
	 */
	FRDGTextureSRVRef BakeSDFVolumeWithGPUBoundsDirect(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef ParticleBufferSRV,
		int32 ParticleCount,
		float ParticleRadius,
		float SDFSmoothness,
		FRDGBufferSRVRef BoundsBufferSRV);

	/**
	 * @brief Bake SDF volume from particle positions (legacy - CPU bounds)
	 *
	 * Dispatches compute shader to calculate SDF at each voxel position.
	 * Should be called each frame before ray marching.
	 *
	 * @param GraphBuilder - RDG graph builder
	 * @param ParticleBufferSRV - Structured buffer containing particle positions
	 * @param ParticleCount - Number of particles
	 * @param ParticleRadius - Radius of each particle
	 * @param SDFSmoothness - Smooth minimum factor for metaball blending
	 * @param VolumeMin - World-space minimum corner (from bounding box)
	 * @param VolumeMax - World-space maximum corner (from bounding box)
	 * @return Created SDF volume texture SRV for use in ray marching
	 */
	FRDGTextureSRVRef BakeSDFVolume(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef ParticleBufferSRV,
		int32 ParticleCount,
		float ParticleRadius,
		float SDFSmoothness,
		const FVector3f& VolumeMin,
		const FVector3f& VolumeMax);

	/** Get volume resolution (default: 64x64x64) */
	FIntVector GetVolumeResolution() const { return VolumeResolution; }

	/** Set volume resolution (call before BakeSDFVolume) */
	void SetVolumeResolution(const FIntVector& NewResolution) { VolumeResolution = NewResolution; }

	/** Get cached volume bounds (valid after BakeSDFVolume) */
	FVector3f GetVolumeMin() const { return CachedVolumeMin; }
	FVector3f GetVolumeMax() const { return CachedVolumeMax; }

	/** Check if GPU bounds are valid (set after CalculateGPUBounds readback) */
	bool HasValidGPUBounds() const { return bHasValidGPUBounds; }

	/** Get last calculated GPU bounds (requires readback to be complete) */
	void GetLastGPUBounds(FVector3f& OutMin, FVector3f& OutMax) const
	{
		OutMin = LastGPUBoundsMin;
		OutMax = LastGPUBoundsMax;
	}

	/** Update cached bounds from readback data */
	void UpdateCachedBoundsFromReadback(const FVector3f& Min, const FVector3f& Max)
	{
		LastGPUBoundsMin = Min;
		LastGPUBoundsMax = Max;
		bHasValidGPUBounds = true;
	}

private:
	/** Volume texture resolution */
	FIntVector VolumeResolution = FIntVector(64, 64, 64);

	/** Cached volume bounds for shader parameters */
	FVector3f CachedVolumeMin = FVector3f::ZeroVector;
	FVector3f CachedVolumeMax = FVector3f::ZeroVector;

	/** GPU bounds from parallel reduction (updated via readback) */
	FVector3f LastGPUBoundsMin = FVector3f::ZeroVector;
	FVector3f LastGPUBoundsMax = FVector3f::ZeroVector;
	bool bHasValidGPUBounds = false;
};

/**
 * @brief Helper to calculate bounding box from particle positions
 *
 * @param Particles - Array of particle positions
 * @param ParticleRadius - Radius of particles
 * @param Margin - Additional margin around bounding box
 * @param OutMin - Output minimum corner
 * @param OutMax - Output maximum corner
 */
KAWAIIFLUIDRUNTIME_API void CalculateParticleBoundingBox(
	const TArray<FVector3f>& Particles,
	float ParticleRadius,
	float Margin,
	FVector3f& OutMin,
	FVector3f& OutMax);
