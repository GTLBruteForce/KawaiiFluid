// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"

class FSceneView;
class UFluidRendererSubsystem;

/**
 * Narrow-Range Filter for Fluid Depth Smoothing (Truong & Yuksel, i3D 2018)
 *
 * Uses hard threshold with dynamic range expansion instead of continuous
 * Gaussian range weighting. Better edge preservation than bilateral filter.
 *
 * @param FilterRadius  Spatial filter radius in pixels
 * @param ParticleRadius  Particle radius for threshold calculation
 * @param ThresholdRatio  Threshold = ParticleRadius * ThresholdRatio (1.0~10.0)
 * @param ClampRatio  Clamp = ParticleRadius * ClampRatio (0.5~2.0)
 * @param GrazingBoost  Boost threshold at grazing angles (0 = none, 1 = 2x at grazing)
 */
void RenderFluidNarrowRangeSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float FilterRadius = 5.0f,
	float ParticleRadius = 10.0f,
	float ThresholdRatio = 3.0f,
	float ClampRatio = 1.0f,
	int32 NumIterations = 3,
	float GrazingBoost = 1.0f);

/**
 * Separable Gaussian Blur for Fluid Thickness Smoothing
 *
 * Uses horizontal + vertical passes for O(2n) instead of O(n²).
 * Smooths out individual particle contributions for cleaner Beer's Law absorption.
 *
 * @param BlurRadius  Spatial filter radius in pixels
 * @param NumIterations  Number of blur iterations (1-5)
 */
void RenderFluidThicknessSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputThicknessTexture,
	FRDGTextureRef& OutSmoothedThicknessTexture,
	float BlurRadius = 5.0f,
	int32 NumIterations = 2);
