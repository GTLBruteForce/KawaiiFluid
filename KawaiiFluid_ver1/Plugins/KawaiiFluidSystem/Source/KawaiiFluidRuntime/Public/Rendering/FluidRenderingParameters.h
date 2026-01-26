// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/FluidAnisotropy.h"
#include "FluidSurfaceDecoration.h"
#include "Engine/TextureCube.h"
#include "FluidRenderingParameters.generated.h"

/**
 * Metaball Pipeline Type
 * Defines how the fluid surface is computed.
 */
UENUM(BlueprintType)
enum class EMetaballPipelineType : uint8
{
	/** Screen Space pipeline: Depth -> Smoothing -> Normal -> Thickness */
	ScreenSpace UMETA(DisplayName = "Screen Space"),

	/** Ray Marching pipeline: Volumetric rendering with 3D density volume */
	RayMarching UMETA(DisplayName = "Ray Marching")
};

// Removed: EMetaballShadingMode (all pipelines use PostProcess lighting only)
// Removed: EShadingPassTiming (no longer needed)
// Removed: ESSFRRenderingMode (deprecated)

/**
 * Fluid Reflection Mode.
 * Determines how reflections are rendered on the fluid surface.
 */
UENUM(BlueprintType)
enum class EFluidReflectionMode : uint8
{
	/** No reflection (only base color and refraction) */
	None UMETA(DisplayName = "None"),

	/** Cubemap-based reflection (static environment map) */
	Cubemap UMETA(DisplayName = "Cubemap"),

	/** Screen Space Reflection (real-time scene reflection, no fallback) */
	ScreenSpaceReflection UMETA(DisplayName = "Screen Space Reflection"),

	/** SSR with Cubemap fallback (SSR hit uses scene, miss uses Cubemap) */
	ScreenSpaceReflectionWithCubemap UMETA(DisplayName = "SSR + Cubemap")
};

/**
 * SSR Debug visualization mode.
 * Used to diagnose Screen Space Reflection issues at runtime.
 */
UENUM(BlueprintType)
enum class EScreenSpaceReflectionDebugMode : uint8
{
	/** No debug visualization (normal rendering) */
	None = 0 UMETA(DisplayName = "None"),

	/** Hit/Miss - Red = hit (intensity varies), Blue = miss */
	HitMiss = 1 UMETA(DisplayName = "Hit/Miss"),

	/** Reflection Direction - RGB = XYZ (0.5 = 0) */
	ReflectionDirection = 2 UMETA(DisplayName = "Reflection Direction"),

	/** Hit Color - Direct display of hit color (Magenta = miss) */
	HitColor = 3 UMETA(DisplayName = "Hit Color"),

	/** Reflection Z - Green = into scene (SSR possible), Red = toward camera (SSR not possible) */
	ReflectionZ = 4 UMETA(DisplayName = "Reflection Z"),

	/** Depth Compare - Red = ray in front, Green = ray behind, Blue = scene depth */
	DepthCompare = 5 UMETA(DisplayName = "Depth Compare"),

	/** Exit Reason - Red = behind camera, Green = off screen, Blue = max steps, Yellow = hit */
	ExitReason = 6 UMETA(DisplayName = "Exit Reason"),

	/** Normal - Normal direction visualization (RGB = XYZ) */
	Normal = 7 UMETA(DisplayName = "Normal"),

	/** ViewDir - View direction visualization (RGB = XYZ) */
	ViewDir = 8 UMETA(DisplayName = "ViewDir"),

	/** ViewPos.z - View position Z (Blue = in front, Red = behind) */
	ViewPosZ = 9 UMETA(DisplayName = "ViewPos.z")
};

/**
 * @brief Fluid rendering parameters.
 * Settings used throughout the SSFR pipeline.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFluidRenderingParameters
{
	GENERATED_BODY()

	//========================================
	// General
	//========================================

	/** Enable rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|General")
	bool bEnableRendering = true;

	/** Pipeline type (how surface is computed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|General")
	EMetaballPipelineType PipelineType = EMetaballPipelineType::ScreenSpace;

	//========================================
	// Color & Opacity
	//========================================

	/** Fluid color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Color",
		meta = (HideAlphaChannel))
	FLinearColor FluidColor = FLinearColor(0.2f, 0.5f, 0.8f, 1.0f);

	/**
	 * Fluid opacity (0 = fully transparent, 1 = fully opaque).
	 * Controls overall light absorption strength through the fluid.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Color",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Opacity = 0.5f;

	/** Thickness rendering scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Color",
		meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float ThicknessScale = 1.0f;

	/**
	 * Thickness sensitivity (0 = uniform opacity, 1 = thickness-dependent).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Color",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ThicknessSensitivity = 0.5f;

	//========================================
	// Material
	//========================================

	/** Index of Refraction (IOR). Water=1.33, Glass=1.5 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Material",
		meta = (ClampMin = "1.0", ClampMax = "2.0"))
	float RefractiveIndex = 1.33f;

	/**
	 * Fresnel strength multiplier.
	 * Scales the F0 value calculated from IOR: F0 = ((1-n)/(1+n))^2 * FresnelStrength
	 * Water (IOR=1.33): base F0 ~ 0.02
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Material",
		meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float FresnelStrength = 1.0f;

	/** Specular strength */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Material",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SpecularStrength = 1.0f;

	/** Specular roughness */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Material",
		meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float SpecularRoughness = 0.2f;

	//========================================
	// Lighting
	//========================================

	/**
	 * Ambient light intensity from SkyLight.
	 * Scales the SkyLightColor contribution to prevent over-brightness.
	 * Default 0.15 is tuned for UE5's default SkyLight intensity.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Lighting",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AmbientIntensity = 0.15f;

	//========================================
	// Absorption (Beer's Law)
	//========================================

	/**
	 * Per-channel absorption coefficients (Beer's Law).
	 * Water: R=0.4, G=0.1, B=0.05 (absorbs red, appears blue).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Absorption",
		meta = (HideAlphaChannel))
	FLinearColor AbsorptionColorCoefficients = FLinearColor(0.4f, 0.1f, 0.05f, 1.0f);

	/**
	 * Absorption bias (Ray Marching only). Higher = FluidColor appears stronger.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Absorption",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "0.0", ClampMax = "1.0"))
	float AbsorptionBias = 0.7f;

	//========================================
	// Refraction
	//========================================

	/**
	 * Refraction offset scale. 0 = no refraction, higher = stronger distortion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Refraction",
		meta = (ClampMin = "0.0", ClampMax = "0.2"))
	float RefractionScale = 0.05f;

	//========================================
	// Depth & Smoothing
	//========================================

	/** Particle render radius (screen space, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth & Smoothing",
		meta = (ClampMin = "0.5", ClampMax = "100.0"))
	float ParticleRenderRadius = 15.0f;

	/**
	 * Depth smoothing filter radius in pixels.
	 * Larger values produce smoother surfaces but may blur fine details.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth & Smoothing",
		meta = (ClampMin = "1", ClampMax = "50"))
	int32 SmoothingRadius = 20;

	/**
	 * Depth difference threshold ratio. Lower = sharper edges, higher = smoother.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth & Smoothing",
		meta = (ClampMin = "0.5", ClampMax = "20.0", DisplayName = "Threshold Ratio"))
	float NarrowRangeThresholdRatio = 3.0f;

	/**
	 * Clamp ratio for front-facing depth samples. Prevents holes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth & Smoothing",
		meta = (ClampMin = "0.1", ClampMax = "5.0", DisplayName = "Clamp Ratio"))
	float NarrowRangeClampRatio = 1.0f;

	/**
	 * Boost smoothing at shallow viewing angles (grazing angles).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth & Smoothing",
		meta = (ClampMin = "0.0", ClampMax = "2.0", DisplayName = "Grazing Angle Boost"))
	float NarrowRangeGrazingBoost = 1.0f;

	//========================================
	// Reflection
	//========================================

	/**
	 * Reflection Mode.
	 * None: No reflection. Cubemap: Static environment map.
	 * SSR: Real-time scene reflection. SSR+Cubemap: SSR with fallback.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection")
	EFluidReflectionMode ReflectionMode = EFluidReflectionMode::Cubemap;

	/**
	 * Fresnel reflection blend ratio for Cubemap reflection.
	 * Controls how much Cubemap reflection is mixed into the final color.
	 * This parameter multiplies the physical Fresnel term to determine the blend factor.
	 *
	 * At grazing angles (edge of fluid), Fresnel approaches 1.0, so:
	 * - FresnelReflectionBlend = 0.5 means 50% reflection at edges
	 * - FresnelReflectionBlend = 1.0 means 100% reflection at edges
	 *
	 * Note: SSR uses ScreenSpaceReflectionIntensity instead of this parameter.
	 *
	 * Recommended values:
	 * - 0.3~0.5: Natural water look with subtle reflections
	 * - 0.5~0.7: More reflective surfaces
	 * - 0.8~1.0: Mirror-like surfaces (may cause edge artifacts)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode != EFluidReflectionMode::None",
			ClampMin = "0.0", ClampMax = "1.0"))
	float FresnelReflectionBlend = 0.5f;

	/**
	 * Reflection Cubemap (environment map).
	 * If not set, uses scene Sky Light color as fallback.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::Cubemap || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap"))
	TObjectPtr<UTextureCube> ReflectionCubemap = nullptr;

	/** Cubemap reflection intensity */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::Cubemap || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap", ClampMin = "0.0", ClampMax = "2.0"))
	float ReflectionIntensity = 1.0f;

	/** Cubemap mip level (higher = blurrier reflection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::Cubemap || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap", ClampMin = "0.0", ClampMax = "10.0"))
	float ReflectionMipLevel = 2.0f;

	/** SSR ray march max steps */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::ScreenSpaceReflection || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap", ClampMin = "64", ClampMax = "512"))
	int32 ScreenSpaceReflectionMaxSteps = 256;

	/** SSR step size (in pixels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::ScreenSpaceReflection || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap", ClampMin = "0.5", ClampMax = "20.0"))
	float ScreenSpaceReflectionStepSize = 4.0f;

	/** SSR hit detection thickness */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::ScreenSpaceReflection || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap", ClampMin = "0.5", ClampMax = "5.0"))
	float ScreenSpaceReflectionThickness = 2.0f;

	/**
	 * SSR reflection intensity.
	 * Controls the blend amount for Screen Space Reflection, independent from FresnelReflectionBlend.
	 *
	 * In SSR-only mode (ReflectionMode = ScreenSpaceReflection):
	 * - BlendFactor = Fresnel * ScreenSpaceReflectionIntensity
	 *
	 * In SSR+Cubemap mode (ReflectionMode = ScreenSpaceReflectionWithCubemap):
	 * - SSR hit areas: BlendFactor = Fresnel * ScreenSpaceReflectionIntensity
	 * - SSR miss areas (Cubemap fallback): BlendFactor = Fresnel * FresnelReflectionBlend
	 * - Partial hits interpolate between the two
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::ScreenSpaceReflection || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap", ClampMin = "0.0", ClampMax = "1.0"))
	float ScreenSpaceReflectionIntensity = 0.8f;

	/** SSR screen edge fade */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::ScreenSpaceReflection || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap", ClampMin = "0.0", ClampMax = "0.5"))
	float ScreenSpaceReflectionEdgeFade = 0.1f;

	/** SSR debug visualization mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "ReflectionMode == EFluidReflectionMode::ScreenSpaceReflection || ReflectionMode == EFluidReflectionMode::ScreenSpaceReflectionWithCubemap"))
	EScreenSpaceReflectionDebugMode ScreenSpaceReflectionDebugMode = EScreenSpaceReflectionDebugMode::None;

	//========================================
	// Anisotropy
	//========================================

	/** Anisotropy parameters for ellipsoid rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy", meta = (ShowOnlyInnerProperties))
	FFluidAnisotropyParams AnisotropyParams;

	//========================================
	// Surface Decoration
	//========================================

	/** Surface decoration parameters (foam, emissive, texture overlays) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration", meta = (ShowOnlyInnerProperties))
	FSurfaceDecorationParams SurfaceDecoration;

	//========================================
	// SDF Ray Marching
	// (Active when PipelineType == RayMarching)
	//========================================

	/**
	 * Ray Marching volume resolution.
	 * Higher = more detail, more memory usage.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "64", ClampMax = "512"))
	int32 VolumeResolution = 256;

	/**
	 * Maximum ray march steps. Higher = more accurate but more expensive.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "16", ClampMax = "512"))
	int32 RayMarchMaxSteps = 128;

	/**
	 * Density threshold for surface detection.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "0.01", ClampMax = "2.0"))
	float DensityThreshold = 0.5f;

	/** Enable Occupancy Bitmask optimization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bEnableOccupancyMask = true;

	/** Enable Min-Max Mipmap optimization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bEnableMinMaxMipmap = true;

	/** Enable Tile-based Culling optimization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bEnableTileCulling = true;

	/** Enable Temporal Reprojection optimization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bEnableTemporalReprojection = true;

	/** Temporal blend factor (0 = current frame only, 1 = history only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bEnableTemporalReprojection",
			ClampMin = "0.0", ClampMax = "0.99"))
	float TemporalBlendFactor = 0.9f;

	/** Adaptive step size multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "1.0", ClampMax = "8.0"))
	float AdaptiveStepMultiplier = 4.0f;

	/** Early termination alpha threshold */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "0.9", ClampMax = "1.0"))
	float EarlyTerminationAlpha = 0.99f;

	// --- SDF Options ---

	/**
	 * Use SDF (Signed Distance Field) mode instead of density volume.
	 * SDF uses Sphere Tracing for efficient empty space skipping.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bUseSDF = true;

	/**
	 * SDF SmoothMin parameter (K). Controls surface blending smoothness.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.5", ClampMax = "100.0"))
	float SDFSmoothK = 5.0f;

	/** SDF Surface offset. Negative = larger volume, positive = smaller. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "-50.0", ClampMax = "50.0"))
	float SDFSurfaceOffset = 0.0f;

	/** SDF surface hit epsilon (in voxels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.1", ClampMax = "5.0"))
	float SDFSurfaceEpsilon = 1.0f;

	/** SDF Sphere Tracing relaxation factor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "1.0", ClampMax = "2.0"))
	float SDFRelaxationFactor = 1.2f;

	/** SDF translucency maximum depth */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "10.0", ClampMax = "500.0"))
	float SDFTranslucencyDepth = 100.0f;

	/** SDF translucency density */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.001", ClampMax = "0.1"))
	float SDFTranslucencyDensity = 0.02f;

	/** SDF subsurface scattering strength */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.0", ClampMax = "2.0"))
	float SDFSubsurfaceScatterStrength = 0.5f;

	/** SDF subsurface scattering color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF", HideAlphaChannel))
	FLinearColor SDFSubsurfaceColor = FLinearColor(0.8f, 0.6f, 0.4f, 1.0f);

	/** SDF reflection strength */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.0", ClampMax = "1.0"))
	float SDFReflectionStrength = 0.3f;

	/** Enable SDF Hybrid Mode (SDF Volume + Z-Order for precision) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	bool bEnableSDFHybridMode = true;

	/** Hybrid Mode threshold distance (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF && bEnableSDFHybridMode",
			ClampMin = "5.0", ClampMax = "100.0"))
	float SDFHybridThreshold = 30.0f;

	/** Use Tight AABB instead of simulation bounds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	bool bUseTightAABB = false;

	/** AABB padding multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF && bUseTightAABB",
			ClampMin = "1.0", ClampMax = "5.0"))
	float AABBPaddingMultiplier = 2.0f;

	/** Debug visualization for Tight AABB */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF && bUseTightAABB"))
	bool bDebugVisualizeTightAABB = false;

	/** Use Sparse Voxel structure */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	bool bUseSparseVoxel = false;

	/** Use Temporal Coherence */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	bool bUseTemporalCoherence = false;

	/** Temporal dirty threshold (cm/frame) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|SDF Ray Marching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF && bUseTemporalCoherence",
			ClampMin = "0.0", ClampMax = "50.0"))
	float TemporalDirtyThreshold = 5.0f;

	FFluidRenderingParameters() = default;
};

// Hash function for batching (TMap key)
FORCEINLINE uint32 GetTypeHash(const FFluidRenderingParameters& Params)
{
	uint32 Hash = GetTypeHash(Params.bEnableRendering);
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Params.PipelineType)));
	Hash = HashCombine(Hash, GetTypeHash(Params.FluidColor.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(Params.FresnelStrength));
	Hash = HashCombine(Hash, GetTypeHash(Params.RefractiveIndex));
	Hash = HashCombine(Hash, GetTypeHash(Params.Opacity));
	Hash = HashCombine(Hash, GetTypeHash(Params.AbsorptionColorCoefficients.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(Params.SpecularStrength));
	Hash = HashCombine(Hash, GetTypeHash(Params.SpecularRoughness));
	Hash = HashCombine(Hash, GetTypeHash(Params.AmbientIntensity));
	Hash = HashCombine(Hash, GetTypeHash(Params.ThicknessSensitivity));
	Hash = HashCombine(Hash, GetTypeHash(Params.RefractionScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.FresnelReflectionBlend));
	Hash = HashCombine(Hash, GetTypeHash(Params.AbsorptionBias));
	// Reflection Cubemap parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.ReflectionCubemap.Get()));
	Hash = HashCombine(Hash, GetTypeHash(Params.ReflectionIntensity));
	Hash = HashCombine(Hash, GetTypeHash(Params.ReflectionMipLevel));
	Hash = HashCombine(Hash, GetTypeHash(Params.ParticleRenderRadius));
	Hash = HashCombine(Hash, GetTypeHash(Params.SmoothingRadius));
	// Narrow-Range parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.NarrowRangeThresholdRatio));
	Hash = HashCombine(Hash, GetTypeHash(Params.NarrowRangeClampRatio));
	Hash = HashCombine(Hash, GetTypeHash(Params.NarrowRangeGrazingBoost));
	// Anisotropy parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Params.AnisotropyParams.Mode)));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.Strength));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.MinStretch));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.MaxStretch));
	// Surface Decoration parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.SurfaceDecoration.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(Params.SurfaceDecoration.Foam.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(Params.SurfaceDecoration.Emissive.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(Params.ThicknessScale));
	// Reflection parameters
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Params.ReflectionMode)));
	Hash = HashCombine(Hash, GetTypeHash(Params.ScreenSpaceReflectionMaxSteps));
	Hash = HashCombine(Hash, GetTypeHash(Params.ScreenSpaceReflectionStepSize));
	Hash = HashCombine(Hash, GetTypeHash(Params.ScreenSpaceReflectionThickness));
	Hash = HashCombine(Hash, GetTypeHash(Params.ScreenSpaceReflectionIntensity));
	Hash = HashCombine(Hash, GetTypeHash(Params.ScreenSpaceReflectionEdgeFade));
	// Ray Marching parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.VolumeResolution));
	Hash = HashCombine(Hash, GetTypeHash(Params.RayMarchMaxSteps));
	Hash = HashCombine(Hash, GetTypeHash(Params.DensityThreshold));
	Hash = HashCombine(Hash, GetTypeHash(Params.bEnableOccupancyMask));
	Hash = HashCombine(Hash, GetTypeHash(Params.bEnableMinMaxMipmap));
	Hash = HashCombine(Hash, GetTypeHash(Params.bEnableTileCulling));
	Hash = HashCombine(Hash, GetTypeHash(Params.bEnableTemporalReprojection));
	Hash = HashCombine(Hash, GetTypeHash(Params.TemporalBlendFactor));
	Hash = HashCombine(Hash, GetTypeHash(Params.AdaptiveStepMultiplier));
	Hash = HashCombine(Hash, GetTypeHash(Params.EarlyTerminationAlpha));
	// SDF Optimization parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.bUseTightAABB));
	Hash = HashCombine(Hash, GetTypeHash(Params.AABBPaddingMultiplier));
	Hash = HashCombine(Hash, GetTypeHash(Params.bDebugVisualizeTightAABB));
	Hash = HashCombine(Hash, GetTypeHash(Params.bUseSparseVoxel));
	Hash = HashCombine(Hash, GetTypeHash(Params.bUseTemporalCoherence));
	Hash = HashCombine(Hash, GetTypeHash(Params.TemporalDirtyThreshold));
	// SDF Hybrid Mode parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.bEnableSDFHybridMode));
	Hash = HashCombine(Hash, GetTypeHash(Params.SDFHybridThreshold));
	return Hash;
}

// Equality operator for TMap key usage
FORCEINLINE bool operator==(const FFluidRenderingParameters& A, const FFluidRenderingParameters& B)
{
	return A.bEnableRendering == B.bEnableRendering &&
		A.PipelineType == B.PipelineType &&
		A.FluidColor.Equals(B.FluidColor, 0.001f) &&
		FMath::IsNearlyEqual(A.FresnelStrength, B.FresnelStrength, 0.001f) &&
		FMath::IsNearlyEqual(A.RefractiveIndex, B.RefractiveIndex, 0.001f) &&
		FMath::IsNearlyEqual(A.Opacity, B.Opacity, 0.001f) &&
		A.AbsorptionColorCoefficients.Equals(B.AbsorptionColorCoefficients, 0.001f) &&
		FMath::IsNearlyEqual(A.SpecularStrength, B.SpecularStrength, 0.001f) &&
		FMath::IsNearlyEqual(A.SpecularRoughness, B.SpecularRoughness, 0.001f) &&
		FMath::IsNearlyEqual(A.AmbientIntensity, B.AmbientIntensity, 0.001f) &&
		FMath::IsNearlyEqual(A.ThicknessSensitivity, B.ThicknessSensitivity, 0.001f) &&
		FMath::IsNearlyEqual(A.RefractionScale, B.RefractionScale, 0.001f) &&
		FMath::IsNearlyEqual(A.FresnelReflectionBlend, B.FresnelReflectionBlend, 0.001f) &&
		FMath::IsNearlyEqual(A.AbsorptionBias, B.AbsorptionBias, 0.001f) &&
		// Reflection Cubemap parameters
		A.ReflectionCubemap == B.ReflectionCubemap &&
		FMath::IsNearlyEqual(A.ReflectionIntensity, B.ReflectionIntensity, 0.001f) &&
		FMath::IsNearlyEqual(A.ReflectionMipLevel, B.ReflectionMipLevel, 0.001f) &&
		FMath::IsNearlyEqual(A.ParticleRenderRadius, B.ParticleRenderRadius, 0.001f) &&
		A.SmoothingRadius == B.SmoothingRadius &&
		// Narrow-Range parameters
		FMath::IsNearlyEqual(A.NarrowRangeThresholdRatio, B.NarrowRangeThresholdRatio, 0.01f) &&
		FMath::IsNearlyEqual(A.NarrowRangeClampRatio, B.NarrowRangeClampRatio, 0.01f) &&
		FMath::IsNearlyEqual(A.NarrowRangeGrazingBoost, B.NarrowRangeGrazingBoost, 0.01f) &&
		// Anisotropy parameters
		A.AnisotropyParams.bEnabled == B.AnisotropyParams.bEnabled &&
		A.AnisotropyParams.Mode == B.AnisotropyParams.Mode &&
		FMath::IsNearlyEqual(A.AnisotropyParams.Strength, B.AnisotropyParams.Strength, 0.001f) &&
		FMath::IsNearlyEqual(A.AnisotropyParams.MinStretch, B.AnisotropyParams.MinStretch, 0.001f) &&
		FMath::IsNearlyEqual(A.AnisotropyParams.MaxStretch, B.AnisotropyParams.MaxStretch, 0.001f) &&
		// Surface Decoration parameters
		A.SurfaceDecoration.bEnabled == B.SurfaceDecoration.bEnabled &&
		A.SurfaceDecoration.Foam.bEnabled == B.SurfaceDecoration.Foam.bEnabled &&
		A.SurfaceDecoration.Emissive.bEnabled == B.SurfaceDecoration.Emissive.bEnabled &&
		FMath::IsNearlyEqual(A.ThicknessScale, B.ThicknessScale, 0.001f) &&
		// Reflection parameters
		A.ReflectionMode == B.ReflectionMode &&
		A.ScreenSpaceReflectionMaxSteps == B.ScreenSpaceReflectionMaxSteps &&
		FMath::IsNearlyEqual(A.ScreenSpaceReflectionStepSize, B.ScreenSpaceReflectionStepSize, 0.01f) &&
		FMath::IsNearlyEqual(A.ScreenSpaceReflectionThickness, B.ScreenSpaceReflectionThickness, 0.01f) &&
		FMath::IsNearlyEqual(A.ScreenSpaceReflectionIntensity, B.ScreenSpaceReflectionIntensity, 0.01f) &&
		FMath::IsNearlyEqual(A.ScreenSpaceReflectionEdgeFade, B.ScreenSpaceReflectionEdgeFade, 0.01f) &&
		// Ray Marching parameters
		A.VolumeResolution == B.VolumeResolution &&
		A.RayMarchMaxSteps == B.RayMarchMaxSteps &&
		FMath::IsNearlyEqual(A.DensityThreshold, B.DensityThreshold, 0.01f) &&
		A.bEnableOccupancyMask == B.bEnableOccupancyMask &&
		A.bEnableMinMaxMipmap == B.bEnableMinMaxMipmap &&
		A.bEnableTileCulling == B.bEnableTileCulling &&
		A.bEnableTemporalReprojection == B.bEnableTemporalReprojection &&
		FMath::IsNearlyEqual(A.TemporalBlendFactor, B.TemporalBlendFactor, 0.01f) &&
		FMath::IsNearlyEqual(A.AdaptiveStepMultiplier, B.AdaptiveStepMultiplier, 0.01f) &&
		FMath::IsNearlyEqual(A.EarlyTerminationAlpha, B.EarlyTerminationAlpha, 0.001f) &&
		// SDF Optimization parameters
		A.bUseTightAABB == B.bUseTightAABB &&
		FMath::IsNearlyEqual(A.AABBPaddingMultiplier, B.AABBPaddingMultiplier, 0.01f) &&
		A.bDebugVisualizeTightAABB == B.bDebugVisualizeTightAABB &&
		A.bUseSparseVoxel == B.bUseSparseVoxel &&
		A.bUseTemporalCoherence == B.bUseTemporalCoherence &&
		FMath::IsNearlyEqual(A.TemporalDirtyThreshold, B.TemporalDirtyThreshold, 0.01f) &&
		// SDF Hybrid Mode parameters
		A.bEnableSDFHybridMode == B.bEnableSDFHybridMode &&
		FMath::IsNearlyEqual(A.SDFHybridThreshold, B.SDFHybridThreshold, 0.1f);
}
