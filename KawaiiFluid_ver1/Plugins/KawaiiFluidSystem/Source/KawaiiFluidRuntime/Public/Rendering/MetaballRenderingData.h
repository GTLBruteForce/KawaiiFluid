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

	/** SDF Volume texture SRV */
	FRDGTextureSRVRef SDFVolumeTextureSRV = nullptr;

	/** Volume bounds in world space */
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
		SDFVolumeTextureSRV = nullptr;
		VolumeMin = FVector3f::ZeroVector;
		VolumeMax = FVector3f::ZeroVector;
		VolumeResolution = FIntVector(64, 64, 64);
	}
};

/**
 * RayMarching Pipeline data
 * Contains particle buffer information for ray marching
 */
struct FRayMarchingPipelineData
{
	/** Particle positions buffer */
	FRDGBufferSRVRef ParticleBufferSRV = nullptr;

	/** Number of particles */
	int32 ParticleCount = 0;

	/** Average particle radius */
	float ParticleRadius = 0.0f;

	/** SDF Volume data for optimized ray marching */
	FSDFVolumeData SDFVolumeData;

	bool IsValid() const
	{
		return ParticleBufferSRV != nullptr && ParticleCount > 0;
	}

	void Reset()
	{
		ParticleBufferSRV = nullptr;
		ParticleCount = 0;
		ParticleRadius = 0.0f;
		SDFVolumeData.Reset();
	}
};
