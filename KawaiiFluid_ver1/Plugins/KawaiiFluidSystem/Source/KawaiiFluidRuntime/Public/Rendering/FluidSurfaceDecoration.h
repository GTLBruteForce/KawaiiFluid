// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "FluidSurfaceDecoration.generated.h"

/**
 * Texture Addressing Mode for UV coordinates.
 * Controls how textures are sampled when UV goes outside [0,1] range.
 */
UENUM(BlueprintType)
enum class ETextureAddressingMode : uint8
{
	/** Repeat the texture (default tiling) */
	Wrap UMETA(DisplayName = "Wrap (Repeat)"),

	/** Mirror the texture at boundaries (like decals) */
	Mirror UMETA(DisplayName = "Mirror")
};

/**
 * Surface Decoration Layer
 * Defines a single texture layer to overlay on the fluid surface.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FSurfaceDecorationLayer
{
	GENERATED_BODY()

	/** Enable this layer */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers")
	bool bEnabled = false;

	/** Layer texture (albedo/pattern) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers")
	TObjectPtr<UTexture2D> Texture = nullptr;

	/** UV addressing mode when texture coordinates exceed [0,1] range */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers", meta = (EditCondition = "Texture != nullptr"))
	ETextureAddressingMode AddressingMode = ETextureAddressingMode::Wrap;

	/** Normal map for this layer (optional) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers")
	TObjectPtr<UTexture2D> NormalMap = nullptr;

	/** Normal map strength (0 = no effect, 1 = full normal map) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers", meta = (EditCondition = "NormalMap != nullptr", ClampMin = "0.0", ClampMax = "2.0"))
	float NormalStrength = 1.0f;

	/** Texture tiling scale (0.01 = 1 tile per 100 units/1m, 0.1 = 1 tile per 10 units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers", meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float TilingScale = 0.01f;

	/** Layer opacity */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Opacity = 1.0f;

	/** Blend mode with base color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers")
	bool bMultiply = false;

	/** Apply only to surfaces above this normal Z threshold (-1 = all surfaces, 0.5 = mostly upward) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float NormalZThreshold = -1.0f;

	/** Flow map influence (0 = static, 1 = full flow animation) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowInfluence = 0.5f;

	/** Scroll speed for animated textures */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layers")
	FVector2D ScrollSpeed = FVector2D::ZeroVector;
};

/**
 * Foam Settings
 * Controls foam generation and appearance.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFoamSettings
{
	GENERATED_BODY()

	//========================================
	// Common
	//========================================

	/** Enable foam effect */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam")
	bool bEnabled = false;

	//========================================
	// Color
	//========================================

	/** Foam color (multiplied with texture) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled"))
	FLinearColor FoamColor = FLinearColor::White;

	/** Foam intensity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "5.0"))
	float Intensity = 1.0f;

	//========================================
	// Texture
	//========================================

	/** Foam texture (noise-based pattern, optional - solid color if not set) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled"))
	TObjectPtr<UTexture2D> FoamTexture = nullptr;

	/** UV addressing mode when texture coordinates exceed [0,1] range */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled && FoamTexture != nullptr"))
	ETextureAddressingMode AddressingMode = ETextureAddressingMode::Wrap;

	/** Texture tiling scale (0.01 = 1 tile per 100 units/1m, 0.1 = 1 tile per 10 units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled", ClampMin = "0.0001", ClampMax = "1.0"))
	float TilingScale = 0.02f;

	//========================================
	// Generation
	//========================================

	/** Velocity threshold to start generating foam (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1000.0"))
	float VelocityThreshold = 100.0f;

	/** Generate foam at wave crests (sharp depth changes) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled"))
	bool bWaveCrestFoam = true;

	/** Strength of wave crest foam (0~1 contribution) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled && bWaveCrestFoam", ClampMin = "0.0", ClampMax = "2.0"))
	float WaveCrestFoamStrength = 1.0f;

	//========================================
	// Thickness-based Foam (Phase 1)
	//========================================

	/** Generate foam in thin/edge regions (splash sheets, droplets) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled"))
	bool bThicknessFoam = true;

	/**
	 * Thickness below which foam starts appearing.
	 * Lower = only very thin areas get foam.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled && bThicknessFoam", ClampMin = "0.01", ClampMax = "5.0"))
	float ThicknessThreshold = 0.5f;

	/** Strength of thickness-based foam (0~1 contribution) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled && bThicknessFoam", ClampMin = "0.0", ClampMax = "1.0"))
	float ThicknessFoamStrength = 0.3f;
};

/**
 * Emissive Settings (for lava, magical fluids, etc.)
 * Controls glowing/emissive effects.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FEmissiveSettings
{
	GENERATED_BODY()

	/** Enable emissive effect */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive")
	bool bEnabled = false;

	/** Emissive color (HDR) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive", meta = (EditCondition = "bEnabled", HDR))
	FLinearColor EmissiveColor = FLinearColor(1.0f, 0.3f, 0.05f, 1.0f);

	//========================================
	// Brightness
	//========================================

	/**
	 * Base emissive brightness (HDR, always applied even when stationary).
	 * This is the minimum glow level. Velocity and Pulse add on top of this.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "100.0"))
	float MinEmissive = 2.0f;

	/**
	 * Intensity multiplier for dynamic emissive (velocity and pulse).
	 * Does NOT affect MinEmissive - that's a direct HDR value.
	 * Final = MinEmissive + (VelocityFactor + PulseFactor) * Intensity
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "100.0"))
	float Intensity = 10.0f;

	//========================================
	// Pulse
	//========================================

	/** Pulsation period in seconds (0 = no pulse, 2.0 = pulse every 2 seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "10.0"))
	float PulsePeriod = 0.0f;

	/** Pulsation amplitude (0~1 normalized, scaled by Intensity) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive", meta = (EditCondition = "bEnabled && PulsePeriod > 0", ClampMin = "0.0", ClampMax = "1.0"))
	float PulseAmplitude = 0.2f;

	//========================================
	// Velocity
	//========================================

	/**
	 * Velocity-based emissive: faster flow = brighter glow.
	 * Useful for lava (fast = hot = bright) or magic fluids.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive", meta = (EditCondition = "bEnabled"))
	bool bVelocityEmissive = true;

	/**
	 * How sensitive emissive is to velocity.
	 * Low (0.5): only fast flows glow brightly
	 * Normal (1.0): moderate response
	 * High (2.0): even slow flows glow brightly
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive", meta = (EditCondition = "bEnabled && bVelocityEmissive", ClampMin = "0.1", ClampMax = "5.0"))
	float VelocitySensitivity = 1.0f;
};

/**
 * Flow Map Settings
 * Controls flow-based UV distortion for animated surface effects.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFlowMapSettings
{
	GENERATED_BODY()

	/** Enable flow-based animation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow")
	bool bEnabled = false;

	/** Use particle velocity to generate flow map (runtime) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow", meta = (EditCondition = "bEnabled"))
	bool bUseParticleVelocity = true;

	/** Static flow map texture (if not using particle velocity) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow", meta = (EditCondition = "bEnabled && !bUseParticleVelocity"))
	TObjectPtr<UTexture2D> FlowMapTexture = nullptr;

	/** Flow speed multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "10.0"))
	float FlowSpeed = 1.0f;

	/** Flow distortion strength */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float DistortionStrength = 0.1f;

	//========================================
	// Flow Accumulation (Velocity-based)
	// When bUseParticleVelocity is true, these control how velocity is accumulated into UV offset
	// Still water: no accumulation (texture stays static)
	// Flowing water: accumulates offset (texture moves)
	//========================================

	/**
	 * Scale factor for velocity contribution to flow
	 * Higher = faster texture movement for same velocity
	 * Note: Particle velocity is in cm/sec, so 1.0 means 1 cm offset per 1 cm/sec per second
	 * Increase this if flow effect is too subtle
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow",
		meta = (EditCondition = "bEnabled && bUseParticleVelocity", ClampMin = "0.01", ClampMax = "100.0"))
	float VelocityScale = 1.0f;

	/**
	 * How quickly flow decays when velocity is zero
	 * 0 = no decay (offset accumulates indefinitely)
	 * Higher = texture slowly returns to original position when water stops
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow",
		meta = (EditCondition = "bEnabled && bUseParticleVelocity", ClampMin = "0.0", ClampMax = "5.0"))
	float FlowDecay = 0.1f;

	/**
	 * Maximum accumulated flow offset in world units (cm) before wrapping
	 * Used to prevent numerical overflow in long simulations
	 * Should be large enough to cover multiple texture tiles (e.g., 1000cm = 10m)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow",
		meta = (EditCondition = "bEnabled && bUseParticleVelocity", ClampMin = "10.0", ClampMax = "10000.0"))
	float MaxFlowOffset = 1000.0f;
};

/**
 * Surface Decoration Parameters
 * Main parameter struct for fluid surface decoration effects.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FSurfaceDecorationParams
{
	GENERATED_BODY()

	/** Enable surface decoration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration")
	bool bEnabled = false;

	//========================================
	// Foam (Water)
	//========================================

	/** Foam settings (primarily for water) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Foam", meta = (EditCondition = "bEnabled", ShowOnlyInnerProperties))
	FFoamSettings Foam;

	//========================================
	// Emissive (Lava, Magic)
	//========================================

	/** Emissive/glow settings (for lava, magical fluids) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Emissive", meta = (EditCondition = "bEnabled", ShowOnlyInnerProperties))
	FEmissiveSettings Emissive;

	//========================================
	// Flow Animation
	//========================================

	/** Flow map settings for animated surfaces */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Flow", meta = (EditCondition = "bEnabled", ShowOnlyInnerProperties))
	FFlowMapSettings FlowMap;

	//========================================
	// Custom Layer
	//========================================

	/** Surface texture layer (albedo, normal map, flow animation) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layer", meta = (EditCondition = "bEnabled", ShowOnlyInnerProperties))
	FSurfaceDecorationLayer Layer;

	//========================================
	// Layer Blending
	//========================================

	/** Final opacity for layer texture (multiplied with layer's own opacity) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layer", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float LayerFinalOpacity = 1.0f;

	/** Blend layer texture with base fluid color (0 = replace, 1 = multiply with fluid color) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layer", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float LayerBlendWithFluidColor = 0.5f;

	//========================================
	// Layer Lighting
	//========================================

	/** Apply lighting to layer texture (preserves fluid surface normals) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layer", meta = (EditCondition = "bEnabled"))
	bool bApplyLightingToLayer = true;

	/** Specular strength for layer texture */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layer", meta = (EditCondition = "bEnabled && bApplyLightingToLayer", ClampMin = "0.0", ClampMax = "2.0"))
	float LayerSpecularStrength = 0.3f;

	/** Specular roughness for layer texture (0 = sharp, 1 = diffuse) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration|Layer", meta = (EditCondition = "bEnabled && bApplyLightingToLayer", ClampMin = "0.0", ClampMax = "1.0"))
	float LayerSpecularRoughness = 0.5f;

	FSurfaceDecorationParams() = default;
};
