// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"

class FSceneView;
class UFluidRendererSubsystem;

/**
 * Distance-based dynamic smoothing parameters.
 * Blur radius automatically scales based on pixel depth.
 */
struct FDistanceBasedSmoothingParams
{
	/** Blur radius as multiple of particle screen size (0.5~5.0) */
	float WorldScale = 2.0f;

	/** Minimum blur radius in pixels (1~64) */
	int32 MinRadius = 4;

	/** Maximum blur radius in pixels (4~64, limited by LDS) */
	int32 MaxRadius = 32;
};

/**
 * Narrow-Range Filter for Fluid Depth Smoothing (Truong & Yuksel, i3D 2018)
 *
 * Uses hard threshold with dynamic range expansion instead of continuous
 * Gaussian range weighting. Better edge preservation than bilateral filter.
 * Blur radius automatically scales based on distance from camera.
 *
 * @param ParticleRadius  Particle radius for threshold calculation
 * @param ThresholdRatio  Threshold = ParticleRadius * ThresholdRatio (1.0~10.0)
 * @param ClampRatio  Clamp = ParticleRadius * ClampRatio (0.5~2.0)
 * @param GrazingBoost  Boost threshold at grazing angles (0 = none, 1 = 2x at grazing)
 * @param DistanceBasedParams  Distance-based dynamic smoothing parameters
 */
void RenderFluidNarrowRangeSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float ParticleRadius = 10.0f,
	float ThresholdRatio = 3.0f,
	float ClampRatio = 1.0f,
	int32 NumIterations = 3,
	float GrazingBoost = 1.0f,
	const FDistanceBasedSmoothingParams& DistanceBasedParams = FDistanceBasedSmoothingParams());

/**
 * Separable Gaussian Blur for Fluid Thickness Smoothing
 *
 * Uses horizontal + vertical passes for O(2n) instead of O(n²).
 * Smooths out individual particle contributions for cleaner Beer's Law absorption.
 *
 * @param BlurRadius  Spatial filter radius in pixels
 * @param NumIterations  Number of blur iterations (1-5)
 * @param bUseHalfRes  If true, downsample -> blur at half res -> upsample (~4x faster)
 */
void RenderFluidThicknessSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputThicknessTexture,
	FRDGTextureRef& OutSmoothedThicknessTexture,
	float BlurRadius = 5.0f,
	int32 NumIterations = 2,
	bool bUseHalfRes = true);

/**
 * Separable Gaussian Blur for Fluid Velocity Smoothing
 *
 * Smooths the velocity texture to soften foam boundaries between particles.
 * Without smoothing, foam edges appear sharp because each particle sprite
 * has a constant velocity, creating abrupt "rice grain" patterns at borders.
 *
 * Uses horizontal + vertical passes for O(2n) instead of O(n²).
 *
 * @param BlurRadius  Spatial filter radius in pixels (3~20 recommended)
 * @param NumIterations  Number of blur iterations (1-5, typically 1-2 is sufficient)
 */
void RenderFluidVelocitySmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputVelocityTexture,
	FRDGTextureRef& OutSmoothedVelocityTexture,
	float BlurRadius = 8.0f,
	int32 NumIterations = 1);
