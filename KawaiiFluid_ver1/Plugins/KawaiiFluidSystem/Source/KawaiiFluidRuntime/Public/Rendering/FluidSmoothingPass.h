// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"

class FSceneView;
class UFluidRendererSubsystem;

/**
 * Bilateral Gaussian Blur for Fluid Depth Smoothing
 *
 * Applies separable bilateral filter (horizontal + vertical passes)
 * to smooth the depth buffer while preserving sharp edges.
 */
void RenderFluidSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float BlurRadius = 5.0f,
	float DepthFalloff = 0.05f,
	int32 NumIterations = 3);

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
 * Simple Gaussian Blur for Fluid Thickness Smoothing
 *
 * Applies a simple Gaussian blur to the thickness buffer to smooth out
 * individual particle profiles. Unlike depth smoothing, this does not
 * use bilateral filtering since thickness values are additive.
 *
 * @param BlurRadius  Spatial blur radius in pixels
 * @param NumIterations  Number of blur iterations
 */
void RenderFluidThicknessSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputThicknessTexture,
	FRDGTextureRef& OutSmoothedThicknessTexture,
	float BlurRadius = 5.0f,
	int32 NumIterations = 2);

/**
 * Mean Curvature Flow Smoothing for Fluid Depth
 * Based on "Screen Space Fluid Rendering with Curvature Flow" (van der Laan et al.)
 *
 * Diffuses depth based on mean curvature, which naturally:
 * - Smooths bumpy surfaces (high curvature regions)
 * - Preserves flat areas and sharp edges
 * - Reduces grazing angle artifacts by smoothing surface normals
 *
 * This is particularly effective for reducing the "bumpy" appearance
 * when viewing fluid surfaces at shallow angles.
 *
 * @param ParticleRadius  Particle radius for stability clamping
 * @param Dt  Time step for diffusion (0.05-0.15 recommended, higher = more smoothing)
 * @param DepthThreshold  Depth discontinuity threshold to preserve silhouettes
 * @param NumIterations  Number of diffusion iterations (50+ typically needed for grazing angles)
 * @param GrazingBoost  Boost smoothing at grazing angles (0 = none, 1 = 2x at grazing)
 */
void RenderFluidCurvatureFlowSmoothingPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef InputDepthTexture,
	FRDGTextureRef& OutSmoothedDepthTexture,
	float ParticleRadius = 10.0f,
	float Dt = 0.1f,
	float DepthThreshold = 100.0f,
	int32 NumIterations = 50,
	float GrazingBoost = 1.0f);
