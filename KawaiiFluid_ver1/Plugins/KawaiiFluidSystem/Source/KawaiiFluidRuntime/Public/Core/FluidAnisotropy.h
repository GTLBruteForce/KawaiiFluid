// Copyright KawaiiFluid Team. All Rights Reserved.

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
	/** No anisotropy - render as spheres */
	None UMETA(DisplayName = "None (Spheres)"),

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
 * @param bEnabled Enable anisotropy calculation.
 * @param Mode Calculation mode (velocity, density, hybrid).
 * @param AnisotropyScale Overall scale factor for ellipsoid stretching.
 * @param AnisotropyMin Minimum scale to prevent too thin ellipsoids.
 * @param AnisotropyMax Maximum scale to prevent excessive stretching.
 * @param VelocityStretchFactor How much velocity affects stretching.
 * @param DensityWeight Weight for density-based component in hybrid mode.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFluidAnisotropyParams
{
	GENERATED_BODY()

	/** Enable anisotropy calculation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy")
	bool bEnabled = false;

	/** Calculation mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy")
	EFluidAnisotropyMode Mode = EFluidAnisotropyMode::DensityBased;

	/** Overall anisotropy scale (higher = more stretched ellipsoids) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy", meta = (ClampMin = "0.5", ClampMax = "3", UIMin = "0.5", UIMax = "3"))
	float AnisotropyScale = 1.0f;

	/** Minimum ellipsoid scale (prevents too thin shapes) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy", meta = (ClampMin = "0.1", ClampMax = "1", UIMin = "0.1", UIMax = "1"))
	float AnisotropyMin = 0.2f;

	/** Maximum ellipsoid scale (prevents excessive stretching) - FleX recommends 1.0~2.0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy", meta = (ClampMin = "1", ClampMax = "3", UIMin = "1", UIMax = "3"))
	float AnisotropyMax = 2.0f;

	/** Velocity stretch factor (velocity-based mode) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy", meta = (ClampMin = "0", ClampMax = "0.1", UIMin = "0", UIMax = "0.1", EditCondition = "Mode == EFluidAnisotropyMode::VelocityBased || Mode == EFluidAnisotropyMode::Hybrid"))
	float VelocityStretchFactor = 0.01f;

	/** Weight for density-based component in hybrid mode (0 = velocity only, 1 = density only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy", meta = (ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1", EditCondition = "Mode == EFluidAnisotropyMode::Hybrid"))
	float DensityWeight = 0.5f;

	/** Update interval in frames (1 = every frame, 2 = every other frame, etc.)
	 *  Higher values reduce GPU cost but may cause visual lag on fast movement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy|Optimization", meta = (ClampMin = "1", ClampMax = "10", UIMin = "1", UIMax = "10"))
	int32 UpdateInterval = 1;
};

// FAnisotropyComputeParams is defined in GPU/FluidAnisotropyComputeShader.h
// to avoid including RenderGraphResources.h before .generated.h
