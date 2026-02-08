// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "KawaiiFluidPreviewSettings.generated.h"

/**
 * Emitter mode for preview (matches UKawaiiFluidEmitterComponent)
 */
UENUM(BlueprintType)
enum class EPreviewEmitterMode : uint8
{
	/** One-time fill of a shape volume with particles (hexagonal pattern) */
	Fill UMETA(DisplayName = "Fill"),

	/** Continuous hexagonal stream emission (like a faucet) */
	Stream UMETA(DisplayName = "Stream")
};

/**
 * Shape type for Fill mode (matches UKawaiiFluidEmitterComponent)
 */
UENUM(BlueprintType)
enum class EPreviewEmitterShapeType : uint8
{
	/** Spherical volume */
	Sphere UMETA(DisplayName = "Sphere"),

	/** Cube volume */
	Cube UMETA(DisplayName = "Cube"),

	/** Cylindrical volume */
	Cylinder UMETA(DisplayName = "Cylinder")
};

/**
 * Preview spawn settings - matches UKawaiiFluidEmitterComponent structure
 * for consistent behavior between editor preview and runtime
 */
USTRUCT(BlueprintType)
struct FFluidPreviewSettings
{
	GENERATED_BODY()

	//========================================
	// Emitter Mode
	//========================================

	/** Emitter mode: Fill (one-time fill) or Stream (continuous emission) */
	UPROPERTY(EditAnywhere, Category = "Emitter")
	EPreviewEmitterMode EmitterMode = EPreviewEmitterMode::Stream;

	//========================================
	// Fill Mode Settings
	//========================================

	/** Shape type for Fill mode */
	UPROPERTY(EditAnywhere, Category = "Fill Shape",
		meta = (EditCondition = "EmitterMode == EPreviewEmitterMode::Fill", EditConditionHides))
	EPreviewEmitterShapeType ShapeType = EPreviewEmitterShapeType::Sphere;

	/** Sphere radius */
	UPROPERTY(EditAnywhere, Category = "Fill Shape",
		meta = (EditCondition = "EmitterMode == EPreviewEmitterMode::Fill && ShapeType == EPreviewEmitterShapeType::Sphere", EditConditionHides, ClampMin = "1.0"))
	float SphereRadius = 50.0f;

	/** Cube half-size (size / 2) */
	UPROPERTY(EditAnywhere, Category = "Fill Shape",
		meta = (EditCondition = "EmitterMode == EPreviewEmitterMode::Fill && ShapeType == EPreviewEmitterShapeType::Cube", EditConditionHides))
	FVector CubeHalfSize = FVector(50.0f, 50.0f, 50.0f);

	/** Cylinder radius */
	UPROPERTY(EditAnywhere, Category = "Fill Shape",
		meta = (EditCondition = "EmitterMode == EPreviewEmitterMode::Fill && ShapeType == EPreviewEmitterShapeType::Cylinder", EditConditionHides, ClampMin = "1.0"))
	float CylinderRadius = 30.0f;

	/** Cylinder half-height */
	UPROPERTY(EditAnywhere, Category = "Fill Shape",
		meta = (EditCondition = "EmitterMode == EPreviewEmitterMode::Fill && ShapeType == EPreviewEmitterShapeType::Cylinder", EditConditionHides, ClampMin = "1.0"))
	float CylinderHalfHeight = 50.0f;

	//========================================
	// Stream Mode Settings
	//========================================

	/** Stream cross-sectional radius */
	UPROPERTY(EditAnywhere, Category = "Stream",
		meta = (EditCondition = "EmitterMode == EPreviewEmitterMode::Stream", EditConditionHides, ClampMin = "1.0", ClampMax = "200.0"))
	float StreamRadius = 15.0f;

	//========================================
	// Velocity Settings (Both modes)
	//========================================

	/** Initial velocity direction for spawned particles */
	UPROPERTY(EditAnywhere, Category = "Velocity")
	FVector InitialVelocityDirection = FVector(0, 0, -1);

	/** Initial speed for spawned particles (cm/s) */
	UPROPERTY(EditAnywhere, Category = "Velocity",
		meta = (ClampMin = "0.0"))
	float InitialSpeed = 250.0f;

	//========================================
	// Limits
	//========================================

	/** Maximum particles for preview (0 = unlimited, clamped to GPU buffer size) */
	UPROPERTY(EditAnywhere, Category = "Limits",
		meta = (ClampMin = "0", ClampMax = "100000"))
	int32 MaxParticleCount = 10000;

	/** GPU buffer size (fixed allocation) */
	static constexpr int32 GPUBufferSize = 100000;

	/** Recycle oldest particles when MaxParticleCount is exceeded (instead of stopping spawn)
	 *  Only applicable to Stream mode - Fill mode spawns once and doesn't need recycling */
	UPROPERTY(EditAnywhere, Category = "Limits",
		meta = (EditCondition = "MaxParticleCount > 0 && EmitterMode == EPreviewEmitterMode::Stream", EditConditionHides))
	bool bContinuousSpawn = true;

	//========================================
	// Preview-specific Settings
	//========================================

	/** Spawn position offset for preview (added to origin) */
	UPROPERTY(EditAnywhere, Category = "Preview")
	FVector PreviewSpawnOffset = FVector(0.0f, 0.0f, 200.0f);

	/** Jitter amount for Fill mode (0.0 - 0.5) */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float JitterAmount = 0.2f;

	/** Jitter amount for Stream mode (0.0 - 0.5) */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StreamJitter = 0.15f;

	/** Layer spacing ratio for Stream mode (hexagonal layer distance) */
	UPROPERTY(EditAnywhere, Category = "Preview",
		meta = (ClampMin = "0.5", ClampMax = "1.0"))
	float StreamLayerSpacingRatio = 0.816f;

	//========================================
	// Helper Methods
	//========================================

	bool IsFillMode() const { return EmitterMode == EPreviewEmitterMode::Fill; }
	bool IsStreamMode() const { return EmitterMode == EPreviewEmitterMode::Stream; }
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

	// Note: Rendering settings come from Preset->RenderingParameters
};
