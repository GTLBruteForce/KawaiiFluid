// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "FluidPreviewSettings.generated.h"

/**
 * Preview environment settings exposed to Details Panel
 */
USTRUCT(BlueprintType)
struct FFluidPreviewSettings
{
	GENERATED_BODY()

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

	//========================================
	// Rendering Settings
	//========================================

	/** ISM Renderer Settings (debug visualization, per-preview) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (DisplayName = "ISM Settings"))
	FKawaiiFluidISMRendererSettings ISMSettings;

	// Note: Metaball settings come from Preset->RenderingParameters
	// This ensures preview matches actual runtime appearance
};
