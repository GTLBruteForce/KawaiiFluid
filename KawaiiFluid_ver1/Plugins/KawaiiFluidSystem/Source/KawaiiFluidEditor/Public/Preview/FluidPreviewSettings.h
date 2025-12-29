// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FluidPreviewSettings.generated.h"

/**
 * Render mode for fluid preview
 */
UENUM(BlueprintType)
enum class EFluidPreviewRenderMode : uint8
{
	/** Fast preview using instanced meshes (spheres) */
	ISM   UMETA(DisplayName = "Instanced Mesh (Fast)"),

	/** Realistic preview using Screen Space Fluid Rendering - same as runtime */
	SSFR  UMETA(DisplayName = "Screen Space Fluid (Realistic)")
};

/**
 * Preview environment settings exposed to Details Panel
 */
USTRUCT(BlueprintType)
struct FFluidPreviewSettings
{
	GENERATED_BODY()

	//========================================
	// Rendering Settings
	//========================================

	/** Render mode - ISM for fast preview, SSFR for realistic (same as runtime) */
	UPROPERTY(EditAnywhere, Category = "Rendering")
	EFluidPreviewRenderMode RenderMode = EFluidPreviewRenderMode::SSFR;

	/** [SSFR] Smoothing filter iterations for surface smoothness */
	UPROPERTY(EditAnywhere, Category = "Rendering|SSFR", meta = (EditCondition = "RenderMode == EFluidPreviewRenderMode::SSFR", ClampMin = "0", ClampMax = "5"))
	int32 SmoothingIterations = 2;

	/** [SSFR] Fluid thickness scale for refraction effect */
	UPROPERTY(EditAnywhere, Category = "Rendering|SSFR", meta = (EditCondition = "RenderMode == EFluidPreviewRenderMode::SSFR", ClampMin = "0.1", ClampMax = "5.0"))
	float ThicknessScale = 1.0f;

	//========================================
	// Continuous Spawn Settings
	//========================================

	/** Particles spawned per second */
	UPROPERTY(EditAnywhere, Category = "Spawn", meta = (ClampMin = "1", ClampMax = "500"))
	float ParticlesPerSecond = 60.0f;

	/** Maximum particle count */
	UPROPERTY(EditAnywhere, Category = "Spawn", meta = (ClampMin = "1", ClampMax = "5000"))
	int32 MaxParticleCount = 1000;

	/** Location to spawn particles */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	FVector SpawnLocation = FVector(0.0f, 0.0f, 200.0f);

	/** Initial velocity of spawned particles */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	FVector SpawnVelocity = FVector(0.0f, 0.0f, -50.0f);

	/** Radius of spawn sphere */
	UPROPERTY(EditAnywhere, Category = "Spawn", meta = (ClampMin = "1.0", ClampMax = "100.0"))
	float SpawnRadius = 15.0f;

};

/**
 * UObject wrapper for FFluidPreviewSettings for Details Panel
 */
UCLASS()
class KAWAIIFLUIDEDITOR_API UFluidPreviewSettingsObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Preview Settings", meta = (ShowOnlyInnerProperties))
	FFluidPreviewSettings Settings;
};
