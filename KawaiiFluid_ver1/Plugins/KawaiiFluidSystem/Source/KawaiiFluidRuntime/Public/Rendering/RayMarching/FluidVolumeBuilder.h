// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"

class FRDGBuilder;
struct FGPUFluidSimulationParams;

/**
 * Volume textures used by the Ray Marching pipeline
 */
struct FFluidVolumeTextures
{
	/** Main density volume (Resolution³) - used for volumetric rendering */
	FRDGTextureRef DensityVolume = nullptr;

	/** SDF volume (Resolution³) - used for Sphere Tracing */
	FRDGTextureRef SDFVolume = nullptr;

	/** MinMax mipmap chain (for hierarchical empty space skipping) */
	FRDGTextureRef MinMaxMipmap = nullptr;

	/** Occupancy bitmask buffer (32³ bits = 1024 uint32) */
	FRDGBufferRef OccupancyMask = nullptr;

	/** Tight AABB buffer (6 uints: min.xyz, max.xyz as sortable uints) - RDG ref */
	FRDGBufferRef AABBBuffer = nullptr;

	/** Tight AABB buffer (Pooled - persistent across GraphBuilder instances) */
	TRefCountPtr<FRDGPooledBuffer> AABBBufferPooled;

	/** Volume bounds in world space (fallback, used when Tight AABB is disabled) */
	FVector3f VolumeBoundsMin = FVector3f::ZeroVector;
	FVector3f VolumeBoundsMax = FVector3f::ZeroVector;

	/** Volume resolution */
	int32 VolumeResolution = 256;

	/** Check if textures are valid (SDF mode or Density mode) */
	bool IsValid() const
	{
		return DensityVolume != nullptr || SDFVolume != nullptr;
	}

	/** Check if using SDF mode */
	bool HasSDF() const
	{
		return SDFVolume != nullptr;
	}

	/** Check if using Tight AABB (checks both RDG ref and Pooled buffer) */
	bool HasTightAABB() const
	{
		return AABBBuffer != nullptr || AABBBufferPooled.IsValid();
	}
};

/**
 * Input data for building fluid volumes
 */
struct FFluidVolumeInput
{
	/** Z-Order sorted particle buffer (RDG - valid only within same GraphBuilder) */
	FRDGBufferRef SortedParticles = nullptr;

	/** Cell start indices from Z-Order sort (RDG - valid only within same GraphBuilder) */
	FRDGBufferRef CellStart = nullptr;

	/** Cell end indices from Z-Order sort (RDG - valid only within same GraphBuilder) */
	FRDGBufferRef CellEnd = nullptr;

	//========================================
	// Persistent Pooled Buffers (for cross-GraphBuilder usage)
	// These are valid across multiple GraphBuilder instances
	//========================================

	/** Pooled particle buffer (persistent across frames) */
	TRefCountPtr<FRDGPooledBuffer> SortedParticlesPooled;

	/** Pooled cell start buffer (persistent across frames) */
	TRefCountPtr<FRDGPooledBuffer> CellStartPooled;

	/** Pooled cell end buffer (persistent across frames) */
	TRefCountPtr<FRDGPooledBuffer> CellEndPooled;

	/** Number of active particles */
	int32 ParticleCount = 0;

	/** Smoothing radius (for kernel calculations) */
	float SmoothingRadius = 20.0f;

	/** Cell size (synchronized with Z-Order grid) */
	float CellSize = 20.0f;

	/** Pre-computed Poly6 kernel coefficient */
	float Poly6Coeff = 0.0f;

	/** Particle radius (for AABB expansion) */
	float ParticleRadius = 5.0f;

	/** Volume bounds (from simulation or AABB compute) */
	FVector3f BoundsMin = FVector3f::ZeroVector;
	FVector3f BoundsMax = FVector3f::ZeroVector;

	/** Morton/Z-Order bounds minimum (for cell ID calculation) */
	FVector3f MortonBoundsMin = FVector3f::ZeroVector;
};

/**
 * Configuration for volume building
 */
struct FFluidVolumeConfig
{
	/** Volume resolution (64, 128, 256, or 512) */
	int32 VolumeResolution = 256;

	/** Density threshold for surface detection */
	float DensityThreshold = 0.5f;

	/** Build occupancy bitmask */
	bool bBuildOccupancyMask = true;

	/** Build MinMax mipmap chain */
	bool bBuildMinMaxMipmap = true;

	/** Number of MinMax mip levels to build */
	int32 MinMaxMipLevels = 4;  // L0=128, L1=64, L2=32, L3=16

	//========================================
	// SDF Options
	//========================================

	/** Build SDF volume instead of density volume */
	bool bBuildSDF = true;

	/** SmoothMin parameter for smooth fluid surface blending */
	float SmoothK = 30.0f;

	/** Surface offset (negative = larger fluid, positive = smaller) */
	float SurfaceOffset = 0.0f;

	//========================================
	// Optimization Options
	//========================================

	/** Use Tight AABB (computed from particles) instead of simulation bounds
	 *  GPU-only: AABB is computed and used within the same frame, no CPU readback needed */
	bool bUseTightAABB = false;

	/** AABB padding multiplier (multiplied by particle radius) */
	float AABBPaddingMultiplier = 2.0f;

	/** Use Sparse Voxel (only compute SDF where particles exist) */
	bool bUseSparseVoxel = false;

	/** Use Temporal Coherence (reuse previous frame's SDF) */
	bool bUseTemporalCoherence = false;

	/** Temporal dirty threshold (cm/frame) - particles moving faster than this are recomputed */
	float TemporalDirtyThreshold = 5.0f;
};

/**
 * FluidVolumeBuilder
 *
 * Builds 3D density volume and optimization structures from Z-Order sorted particles.
 * Used by the Ray Marching rendering pipeline.
 *
 * Features:
 * - Converts Z-Order sorted particles to 3D density volume
 * - Builds 32³ occupancy bitmask for O(1) empty block detection
 * - Builds MinMax mipmap chain for hierarchical empty space skipping
 * - Synchronized with Z-Order cell size for optimal cache efficiency
 */
class KAWAIIFLUIDRUNTIME_API FFluidVolumeBuilder
{
public:
	FFluidVolumeBuilder();
	~FFluidVolumeBuilder();

	/**
	 * Build all volume textures from Z-Order sorted particles
	 * @param GraphBuilder RDG builder
	 * @param Input Input data (particles, cell indices, bounds)
	 * @param Config Volume configuration
	 * @return Built volume textures
	 */
	FFluidVolumeTextures BuildVolumes(
		FRDGBuilder& GraphBuilder,
		const FFluidVolumeInput& Input,
		const FFluidVolumeConfig& Config);

	/**
	 * Compute tight AABB around fluid particles
	 * Uses GPU parallel reduction for efficiency
	 * @param GraphBuilder RDG builder
	 * @param ParticleBuffer Particle buffer (SRV)
	 * @param ParticleCount Number of active particles
	 * @param ParticleRadius Particle radius for AABB expansion
	 * @return Buffer containing AABB (6 floats: min.xyz, max.xyz)
	 */
	FRDGBufferRef ComputeFluidAABB(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef ParticleBuffer,
		int32 ParticleCount,
		float ParticleRadius);

	/**
	 * Get the last computed volume textures
	 * Valid only within the same frame after BuildVolumes
	 */
	const FFluidVolumeTextures& GetCachedVolumes() const { return CachedVolumes; }

private:
	//========================================
	// Internal Volume Building Methods
	//========================================

	/** Build density volume from particles */
	FRDGTextureRef BuildDensityVolume(
		FRDGBuilder& GraphBuilder,
		const FFluidVolumeInput& Input,
		const FFluidVolumeConfig& Config);

	/** Build SDF volume from particles using Z-Order neighbor search
	 *  @param AABBBuffer Optional Tight AABB buffer (GPU-only, computed in same frame) */
	FRDGTextureRef BuildSDFVolume(
		FRDGBuilder& GraphBuilder,
		const FFluidVolumeInput& Input,
		const FFluidVolumeConfig& Config,
		FRDGBufferRef AABBBuffer = nullptr);

	/**
	 * Build SDF volume using Sparse Voxel optimization
	 * Pass 1: Mark voxels within particle influence range
	 * Pass 2: Compute SDF only for active voxels
	 */
	FRDGTextureRef BuildSDFVolumeSparse(
		FRDGBuilder& GraphBuilder,
		const FFluidVolumeInput& Input,
		const FFluidVolumeConfig& Config);

	/** Build occupancy bitmask from density volume */
	FRDGBufferRef BuildOccupancyMask(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef DensityVolume,
		const FFluidVolumeConfig& Config);

	/** Build MinMax mipmap chain from density volume */
	FRDGTextureRef BuildMinMaxMipmap(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef DensityVolume,
		const FFluidVolumeConfig& Config);

	/** Clear a volume texture to zero */
	void ClearVolume(
		FRDGBuilder& GraphBuilder,
		FRDGTextureUAVRef VolumeUAV,
		int32 Resolution,
		float ClearValue = 0.0f);

	//========================================
	// Cached Data
	//========================================

	/** Cached volume textures from last build */
	FFluidVolumeTextures CachedVolumes;

	/** Cached global shader map */
	FGlobalShaderMap* GlobalShaderMap = nullptr;
};
