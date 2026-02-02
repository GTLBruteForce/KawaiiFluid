// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FluidAnisotropy.generated.h"

/**
 * @brief Anisotropy calculation mode for ellipsoid rendering.
 * Based on NVIDIA FleX and Yu & Turk 2013 paper:
 * "Reconstructing surfaces of particle-based fluids using anisotropic kernels"
 */
UENUM(BlueprintType)
enum class EFluidAnisotropyMode : uint8
{
	/** Stretch ellipsoids along velocity direction */
	VelocityBased UMETA(DisplayName = "Velocity Based"),

	/** Calculate from neighbor particle distribution (covariance matrix) */
	DensityBased UMETA(DisplayName = "Density Based"),

	/** Combine velocity and density-based approaches */
	Hybrid UMETA(DisplayName = "Hybrid")
};

/**
 * @brief GPU Anisotropy mode (must match shader defines).
 */
enum class EGPUAnisotropyMode : uint8
{
	VelocityBased = 0,
	DensityBased = 1,
	Hybrid = 2
};

/**
 * @brief Parameters for anisotropy calculation.
 * Renders particles as ellipsoids instead of spheres for smoother fluid surfaces.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFluidAnisotropyParams
{
	GENERATED_BODY()

	/** Render particles as ellipsoids instead of spheres. Creates smoother, more realistic fluid surfaces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy")
	bool bEnabled = false;

	/** How ellipsoid shape is determined. Density-based analyzes neighbor distribution, Velocity-based stretches along movement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy", meta = (EditCondition = "bEnabled"))
	EFluidAnisotropyMode Mode = EFluidAnisotropyMode::DensityBased;

	/** Overall ellipsoid effect intensity. 0 = spheres, 1 = normal, higher = more stretched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy", meta = (EditCondition = "bEnabled", ClampMin = "0", ClampMax = "5", UIMin = "0", UIMax = "5"))
	float Strength = 1.0f;

	/** Shortest allowed axis ratio. Prevents extremely flat ellipsoids. (0.2 = can shrink to 20% of original) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy", meta = (EditCondition = "bEnabled", ClampMin = "0.1", ClampMax = "0.9", UIMin = "0.1", UIMax = "0.9"))
	float MinStretch = 0.2f;

	/** Longest allowed axis ratio. Limits maximum elongation. (2.0 = can stretch to 200% of original) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy", meta = (EditCondition = "bEnabled", ClampMin = "1.0", ClampMax = "5.0", UIMin = "1.0", UIMax = "5.0"))
	float MaxStretch = 2.0f;

	/** How much particle velocity affects stretching. Higher = faster particles stretch more. Clamped by MaxStretch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy", meta = (EditCondition = "bEnabled && (Mode == EFluidAnisotropyMode::VelocityBased || Mode == EFluidAnisotropyMode::Hybrid)", ClampMin = "0", ClampMax = "1.0", UIMin = "0", UIMax = "1.0"))
	float VelocityStretchFactor = 0.01f;

	/** Balance between velocity and density in Hybrid mode. 0 = velocity only, 1 = density only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy", meta = (EditCondition = "bEnabled && Mode == EFluidAnisotropyMode::Hybrid", ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	float DensityWeight = 0.5f;

	/** Frames between anisotropy updates. Higher = better performance but delayed response to movement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy|Optimization", meta = (EditCondition = "bEnabled", ClampMin = "1", ClampMax = "10", UIMin = "1", UIMax = "10"))
	int32 UpdateInterval = 1;

	/** Blend ellipsoid orientation with previous frame to reduce flickering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy|Temporal", meta = (EditCondition = "bEnabled"))
	bool bEnableTemporalSmoothing = true;

	/** How much previous frame affects current. 0 = no smoothing, 0.8 = smooth, 1.0 = frozen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy|Temporal",
		meta = (EditCondition = "bEnabled && bEnableTemporalSmoothing", ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	float TemporalSmoothFactor = 0.8f;

	/**
	 * Preserve ellipsoid volume (Scale1 * Scale2 * Scale3 = 1.0).
	 * When enabled, uses log-space processing to maintain unit volume (Yu & Turk style).
	 * When disabled, uses raw eigenvalues from covariance matrix (NVIDIA FleX style).
	 * FleX style may produce larger ellipsoids but matches the original FleX reference more closely.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy|Advanced", meta = (EditCondition = "bEnabled"))
	bool bPreserveVolume = true;

	/**
	 * Ellipsoid render scale for FleX style (only active when Preserve Volume is disabled).
	 * Controls overall ellipsoid size: 1.0 = default, 0.5 = half size, 2.0 = double size.
	 * Use this to match visual appearance with your particle radius.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy|Advanced",
		meta = (EditCondition = "bEnabled && !bPreserveVolume", ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.1", UIMax = "4.0"))
	float FlexRenderScale = 1.0f;
};

// FAnisotropyComputeParams is defined in GPU/FluidAnisotropyComputeShader.h
// to avoid including RenderGraphResources.h before .generated.h
