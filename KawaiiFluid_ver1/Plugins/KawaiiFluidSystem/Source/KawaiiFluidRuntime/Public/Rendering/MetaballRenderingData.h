// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "ScreenPass.h"

/**
 * Intermediate textures produced by ScreenSpace pipeline
 * These are cached between PostBasePass and Tonemap passes
 */
struct FMetaballIntermediateTextures
{
	/** Smoothed depth texture (bilateral filtered) */
	FRDGTextureRef SmoothedDepthTexture = nullptr;

	/** View-space normal texture */
	FRDGTextureRef NormalTexture = nullptr;

	/** Accumulated thickness texture */
	FRDGTextureRef ThicknessTexture = nullptr;

	/** GBuffer textures (optional, for GBuffer shading mode) */
	FRDGTextureRef GBufferATexture = nullptr;
	FRDGTextureRef GBufferBTexture = nullptr;
	FRDGTextureRef GBufferCTexture = nullptr;
	FRDGTextureRef GBufferDTexture = nullptr;

	bool IsValid() const
	{
		return SmoothedDepthTexture != nullptr &&
		       NormalTexture != nullptr &&
		       ThicknessTexture != nullptr;
	}

	void Reset()
	{
		SmoothedDepthTexture = nullptr;
		NormalTexture = nullptr;
		ThicknessTexture = nullptr;
		GBufferATexture = nullptr;
		GBufferBTexture = nullptr;
		GBufferCTexture = nullptr;
		GBufferDTexture = nullptr;
	}
};

/**
 * SDF Volume data for Ray Marching optimization
 * Baked 3D SDF texture for O(1) distance lookup
 */
struct FSDFVolumeData
{
	/** Whether to use SDF Volume optimization */
	bool bUseSDFVolume = false;

	/** Whether to use GPU bounds buffer (eliminates CPU readback) */
	bool bUseGPUBounds = false;

	/** SDF Volume texture SRV */
	FRDGTextureSRVRef SDFVolumeTextureSRV = nullptr;

	/** GPU Bounds buffer SRV (when bUseGPUBounds=true) */
	FRDGBufferSRVRef BoundsBufferSRV = nullptr;

	/** Volume bounds in world space (when bUseGPUBounds=false) */
	FVector3f VolumeMin = FVector3f::ZeroVector;
	FVector3f VolumeMax = FVector3f::ZeroVector;

	/** Volume resolution (voxels per dimension) */
	FIntVector VolumeResolution = FIntVector(64, 64, 64);

	bool IsValid() const
	{
		return bUseSDFVolume && SDFVolumeTextureSRV != nullptr;
	}

	void Reset()
	{
		bUseSDFVolume = false;
		bUseGPUBounds = false;
		SDFVolumeTextureSRV = nullptr;
		BoundsBufferSRV = nullptr;
		VolumeMin = FVector3f::ZeroVector;
		VolumeMax = FVector3f::ZeroVector;
		VolumeResolution = FIntVector(64, 64, 64);
	}
};

/**
 * Spatial Hash data for accelerated SDF evaluation
 * Uses GPU spatial hash grid for O(k) neighbor lookup
 */
struct FSpatialHashData
{
	/** Whether to use Spatial Hash acceleration */
	bool bUseSpatialHash = false;

	/** CellData buffer SRV: {startIndex, count} per cell */
	FRDGBufferSRVRef CellDataSRV = nullptr;

	/** ParticleIndices buffer SRV: sorted particle indices */
	FRDGBufferSRVRef ParticleIndicesSRV = nullptr;

	/** Cell size for spatial hashing */
	float CellSize = 0.0f;

	bool IsValid() const
	{
		return bUseSpatialHash && CellDataSRV != nullptr && ParticleIndicesSRV != nullptr;
	}

	void Reset()
	{
		bUseSpatialHash = false;
		CellDataSRV = nullptr;
		ParticleIndicesSRV = nullptr;
		CellSize = 0.0f;
	}
};

/**
 * Anisotropy data for ellipsoid rendering
 * Precomputed by FluidAnisotropyCompute shader
 */
struct FAnisotropyData
{
	/** Whether to use anisotropic rendering */
	bool bUseAnisotropy = false;

	/** Anisotropy Axis1 buffer SRV: float4(axis.xyz, scale.w) */
	FRDGBufferSRVRef AnisotropyAxis1SRV = nullptr;

	/** Anisotropy Axis2 buffer SRV: float4(axis.xyz, scale.w) */
	FRDGBufferSRVRef AnisotropyAxis2SRV = nullptr;

	/** Anisotropy Axis3 buffer SRV: float4(axis.xyz, scale.w) */
	FRDGBufferSRVRef AnisotropyAxis3SRV = nullptr;

	bool IsValid() const
	{
		return bUseAnisotropy &&
			AnisotropyAxis1SRV != nullptr &&
			AnisotropyAxis2SRV != nullptr &&
			AnisotropyAxis3SRV != nullptr;
	}

	void Reset()
	{
		bUseAnisotropy = false;
		AnisotropyAxis1SRV = nullptr;
		AnisotropyAxis2SRV = nullptr;
		AnisotropyAxis3SRV = nullptr;
	}
};

/**
 * RayMarching Pipeline data
 * Contains particle buffer information for ray marching
 */
struct FRayMarchingPipelineData
{
	/** Particle positions buffer (Legacy AoS - 32B per particle) */
	FRDGBufferSRVRef ParticleBufferSRV = nullptr;

	//========================================
	// SoA Buffers (Memory Bandwidth Optimized)
	// - 62% reduction: 32B/particle → 12B/particle for SDF
	//========================================

	/** Position buffer SRV (SoA - 12B per particle) */
	FRDGBufferSRVRef PositionBufferSRV = nullptr;

	/** Velocity buffer SRV (SoA - 12B per particle, for motion blur) */
	FRDGBufferSRVRef VelocityBufferSRV = nullptr;

	/** Whether to use SoA buffers for ray marching */
	bool bUseSoABuffers = false;

	/** Number of particles */
	int32 ParticleCount = 0;

	/** Average particle radius */
	float ParticleRadius = 0.0f;

	/** SDF Volume data for optimized ray marching */
	FSDFVolumeData SDFVolumeData;

	/** Spatial Hash data for accelerated evaluation */
	FSpatialHashData SpatialHashData;

	/** Anisotropy data for ellipsoid rendering (baduk stone shape) */
	FAnisotropyData AnisotropyData;

	bool IsValid() const
	{
		return ParticleBufferSRV != nullptr && ParticleCount > 0;
	}

	/** Check if SoA buffers are valid and ready for use */
	bool HasValidSoABuffers() const
	{
		return bUseSoABuffers && PositionBufferSRV != nullptr && ParticleCount > 0;
	}

	void Reset()
	{
		ParticleBufferSRV = nullptr;
		PositionBufferSRV = nullptr;
		VelocityBufferSRV = nullptr;
		bUseSoABuffers = false;
		ParticleCount = 0;
		ParticleRadius = 0.0f;
		SDFVolumeData.Reset();
		SpatialHashData.Reset();
		AnisotropyData.Reset();
	}
};
