// Copyright KawaiiFluid Team. All Rights Reserved.

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

/**
 * Metaball Shading Mode
 * Defines how the fluid surface is rendered/lit.
 */
UENUM(BlueprintType)
enum class EMetaballShadingMode : uint8
{
	/** PostProcess: Custom lighting (Blinn-Phong, Fresnel, Beer's Law) */
	PostProcess UMETA(DisplayName = "Post Process"),

	/** GBuffer: Legacy GBuffer write for Lumen/VSM integration */
	GBuffer UMETA(DisplayName = "GBuffer (Legacy)"),

	/** Opaque: Experimental full GBuffer write approach */
	Opaque UMETA(DisplayName = "Opaque (Experimental)"),

	/** Translucent: Experimental Depth/Normal only to GBuffer, Color/Refraction later */
	Translucent UMETA(DisplayName = "Translucent (Experimental)")
};

/**
 * Shading Pass Timing
 * Defines when the shading pass is executed in the rendering pipeline.
 * Each timing corresponds to a specific UE render callback.
 * Used as bitmask flags.
 */
UENUM(BlueprintType, Meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EShadingPassTiming : uint8
{
	None = 0 UMETA(Hidden),

	/** PostBasePass: PostRenderBasePassDeferred_RenderThread - GBuffer write, Stencil marking */
	PostBasePass = 1 << 0 UMETA(DisplayName = "Post Base Pass"),

	/** PrePostProcess: PrePostProcessPass_RenderThread - Transparency compositing (Translucent) */
	PrePostProcess = 1 << 1 UMETA(DisplayName = "Pre Post Process"),

	/** Tonemap: SubscribeToPostProcessingPass(Tonemap) - PostProcess shading */
	Tonemap = 1 << 2 UMETA(DisplayName = "Tonemap")
};
ENUM_CLASS_FLAGS(EShadingPassTiming)

/**
 * SSFR Rendering Mode (DEPRECATED - use EMetaballPipelineType + EMetaballShadingMode)
 * Kept for backwards compatibility during migration.
 */
UENUM(BlueprintType)
enum class ESSFRRenderingMode : uint8
{
	/** Custom lighting implementation (Blinn-Phong, Fresnel, Beer's Law) */
	Custom UMETA(DisplayName = "Custom"),

	/** Write to GBuffer for Lumen/VSM integration */
	GBuffer UMETA(DisplayName = "G-Buffer")
};

/**
 * Depth smoothing filter type for SSFR.
 * Different filters have different characteristics for edge preservation and performance.
 */
UENUM(BlueprintType)
enum class EDepthSmoothingFilter : uint8
{
	/** Bilateral filter - classic approach with depth-aware smoothing */
	Bilateral UMETA(DisplayName = "Bilateral Filter"),

	/** Narrow-Range filter (Truong & Yuksel 2018) - better edge preservation, especially with anisotropy */
	NarrowRange UMETA(DisplayName = "Narrow-Range Filter"),

	/** Curvature Flow (van der Laan 2009) - Laplacian diffusion, reduces grazing angle artifacts */
	CurvatureFlow UMETA(DisplayName = "Curvature Flow")
};

/**
 * @brief Fluid rendering parameters.
 * Settings used throughout the SSFR pipeline.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFluidRenderingParameters
{
	GENERATED_BODY()

	/** Enable rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bEnableRendering = true;

	/** Pipeline type (how surface is computed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	EMetaballPipelineType PipelineType = EMetaballPipelineType::ScreenSpace;

	/** Shading mode (how surface is lit/rendered) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	EMetaballShadingMode ShadingMode = EMetaballShadingMode::PostProcess;

	/** Particle render radius (screen space, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth",
		meta = (ClampMin = "0.5", ClampMax = "100.0"))
	float ParticleRenderRadius = 15.0f;

	/** Depth smoothing filter type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing")
	EDepthSmoothingFilter SmoothingFilter = EDepthSmoothingFilter::NarrowRange;

	/** Depth smoothing strength (0=none, 1=max) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SmoothingStrength = 0.5f;

	/** Bilateral/Narrow-Range filter radius (pixels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (ClampMin = "1", ClampMax = "50"))
	int32 BilateralFilterRadius = 20;

	/** Depth threshold (for bilateral filter) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (ClampMin = "0.001", ClampMax = "100.0"))
	float DepthThreshold = 10.0f;

	//========================================
	// Narrow-Range Filter Parameters
	//========================================

	/**
	 * Narrow-Range threshold ratio.
	 * threshold = ParticleRadius * this value.
	 * Lower = stronger edge preservation, higher = more smoothing.
	 * 1.0~3.0: tight edges, 5.0~10.0: smooth surface.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::NarrowRange",
			ClampMin = "0.5", ClampMax = "20.0"))
	float NarrowRangeThresholdRatio = 3.0f;

	/**
	 * Narrow-Range clamp ratio.
	 * Front-facing (toward camera) sample clamping strength.
	 * Clamped to ParticleRadius * this value.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::NarrowRange",
			ClampMin = "0.1", ClampMax = "5.0"))
	float NarrowRangeClampRatio = 1.0f;

	/**
	 * Narrow-Range grazing angle boost strength.
	 * Increases threshold at shallow angles to include more samples.
	 * 0 = no boost, 1 = 2x threshold at grazing angles.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::NarrowRange",
			ClampMin = "0.0", ClampMax = "2.0"))
	float NarrowRangeGrazingBoost = 1.0f;

	//========================================
	// Curvature Flow Parameters
	//========================================

	/**
	 * Curvature Flow time step (Dt).
	 * Higher = more smoothing per iteration.
	 * 0.05~0.15 recommended (stability).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::CurvatureFlow",
			ClampMin = "0.01", ClampMax = "0.5"))
	float CurvatureFlowDt = 0.1f;

	/**
	 * Curvature Flow depth threshold.
	 * Depth differences larger than this are treated as silhouette (no smoothing).
	 * 3-10x particle radius recommended.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::CurvatureFlow",
			ClampMin = "1.0", ClampMax = "500.0"))
	float CurvatureFlowDepthThreshold = 100.0f;

	/**
	 * Curvature Flow iteration count.
	 * Higher = smoother but more expensive.
	 * 50+ recommended for grazing angle issues.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::CurvatureFlow",
			ClampMin = "1", ClampMax = "200"))
	int32 CurvatureFlowIterations = 50;

	/**
	 * Grazing angle boost strength.
	 * Applies stronger smoothing at shallow viewing angles.
	 * 0 = no boost, 1 = 2x smoothing at grazing angles.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::CurvatureFlow",
			ClampMin = "0.0", ClampMax = "2.0"))
	float CurvatureFlowGrazingBoost = 1.0f;

	/** Fluid color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor FluidColor = FLinearColor(0.2f, 0.5f, 0.8f, 1.0f);

	/**
	 * Fresnel strength multiplier (applied after F0 is auto-calculated from IOR).
	 * 1.0 = physically accurate reflection, 2.0 = exaggerated, 0.5 = weak.
	 * F0 = ((1-IOR)/(1+IOR))^2 * FresnelStrength.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float FresnelStrength = 1.0f;

	/** Index of Refraction (IOR) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "1.0", ClampMax = "2.0"))
	float RefractiveIndex = 1.33f;

	/** Absorption coefficient (thickness-based color attenuation) - overall scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float AbsorptionCoefficient = 2.0f;

	/**
	 * Per-channel absorption coefficients (Beer's Law).
	 * Water: R=0.4, G=0.1, B=0.05 (absorbs red, appears blue).
	 * Slime: R=0.1, G=0.3, B=0.4 (absorbs blue, appears green/yellow).
	 * Higher value = that color is absorbed faster (invisible in thick areas).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor AbsorptionColorCoefficients = FLinearColor(0.4f, 0.1f, 0.05f, 1.0f);

	/** Specular strength */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SpecularStrength = 1.0f;

	/** Specular roughness */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float SpecularRoughness = 0.2f;

	/** Environment light color (fallback when no Cubemap, base ambient color) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor EnvironmentLightColor = FLinearColor(0.8f, 0.9f, 1.0f, 1.0f);

	//========================================
	// Lighting Scale Parameters
	//========================================

	/**
	 * Ambient lighting intensity scale.
	 * Multiplied with EnvironmentLightColor.
	 * 0 = no ambient (fully dark surfaces possible), 1 = strong ambient.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AmbientScale = 0.15f;

	/**
	 * Beer's Law transmittance scale.
	 * Controls light absorption rate based on thickness.
	 * Lower = more transparent, higher = thick areas become opaque.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.001", ClampMax = "0.5"))
	float TransmittanceScale = 0.05f;

	/**
	 * Alpha thickness scale.
	 * How much thickness affects alpha.
	 * Lower = more transparent, higher = more opaque.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.001", ClampMax = "0.2"))
	float AlphaThicknessScale = 0.02f;

	/**
	 * Refraction offset scale.
	 * UV offset strength from refraction.
	 * 0 = no refraction, higher = stronger distortion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.0", ClampMax = "0.2"))
	float RefractionScale = 0.05f;

	/**
	 * Fresnel reflection blend ratio.
	 * How much Fresnel affects BaseColor/ReflectedColor blending.
	 * 0 = no reflection, 1 = strong reflection.
	 * 0.8+ recommended: at grazing angles, reflection naturally hides surface detail.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FresnelReflectionBlend = 0.8f;

	/**
	 * Absorption bias (for Ray Marching).
	 * Added to absorption contribution when blending BaseColor.
	 * Higher = FluidColor appears stronger.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AbsorptionBias = 0.7f;

	//========================================
	// Reflection (SSR + Cubemap Fallback)
	//========================================

	/**
	 * Enable SSR (Screen Space Reflections).
	 * Actual scene objects are reflected on the fluid surface.
	 * Falls back to Cubemap on SSR miss.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection")
	bool bEnableSSR = true;

	/**
	 * SSR ray march max steps.
	 * Higher = more accurate but more expensive.
	 * 8~16: low cost, 24~32: high quality.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "bEnableSSR", ClampMin = "4", ClampMax = "64"))
	int32 SSRMaxSteps = 16;

	/**
	 * SSR step size (in pixels).
	 * Smaller = more precise but shorter reach.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "bEnableSSR", ClampMin = "0.5", ClampMax = "20.0"))
	float SSRStepSize = 4.0f;

	/**
	 * SSR hit detection thickness.
	 * Higher = more lenient hit detection, lower = more precise.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "bEnableSSR", ClampMin = "0.1", ClampMax = "5.0"))
	float SSRThickness = 1.0f;

	/**
	 * SSR intensity (blended with Cubemap).
	 * 0 = Cubemap only, 1 = SSR only.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "bEnableSSR", ClampMin = "0.0", ClampMax = "1.0"))
	float SSRIntensity = 0.8f;

	/**
	 * SSR screen edge fade.
	 * Smoothly fades reflections going off screen.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (EditCondition = "bEnableSSR", ClampMin = "0.0", ClampMax = "0.5"))
	float SSREdgeFade = 0.1f;

	/**
	 * Fallback Cubemap (used on SSR miss).
	 * If not set, uses EnvironmentLightColor.
	 * Can assign scene Reflection Capture or HDRI Cubemap.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection")
	TObjectPtr<UTextureCube> ReflectionCubemap = nullptr;

	/** Cubemap reflection intensity (0 = no reflection, 1 = full reflection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float ReflectionIntensity = 1.0f;

	/** Cubemap mip level (higher = blurrier reflection, linked to roughness) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Reflection",
		meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float ReflectionMipLevel = 2.0f;

	/** Thickness rendering scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Thickness",
		meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float ThicknessScale = 1.0f;

	/** Render target resolution scale (1.0 = screen resolution) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Performance",
		meta = (ClampMin = "0.25", ClampMax = "1.0"))
	float RenderTargetScale = 1.0f;

	/** Anisotropy parameters for ellipsoid rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy")
	FFluidAnisotropyParams AnisotropyParams;

	//========================================
	// Surface Decoration (Foam, Lava, etc.)
	//========================================

	/** Surface decoration parameters (foam, emissive, texture overlays) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Surface Decoration")
	FSurfaceDecorationParams SurfaceDecoration;

	/** Subsurface scattering intensity (jelly effect) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance",
		meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SSSIntensity = 1.0f;

	/** Subsurface scattering color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor SSSColor = FLinearColor(1.0f, 0.5f, 0.3f, 1.0f);

	//========================================
	// G-Buffer Mode Parameters
	//========================================

	/** Metallic value for GBuffer (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0",
			ClampMax = "1.0"))
	float Metallic = 0.1f;

	/** Roughness value for GBuffer (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0",
			ClampMax = "1.0"))
	float Roughness = 0.3f;

	/** Subsurface scattering opacity (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0",
			ClampMax = "1.0"))
	float SubsurfaceOpacity = 0.5f;

	//========================================
	// Ray Marching Parameters
	//========================================

	/**
	 * Ray Marching volume resolution.
	 * Higher = more detail, more memory usage.
	 * 128: ~8MB, 256: ~64MB (with optimizations ~57MB total)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "64", ClampMax = "512"))
	int32 VolumeResolution = 256;

	/**
	 * Maximum ray march steps.
	 * Higher = more accurate but more expensive.
	 * 64~128 recommended with optimizations.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "16", ClampMax = "512"))
	int32 RayMarchMaxSteps = 128;

	/**
	 * Density threshold for surface detection.
	 * When accumulated density exceeds this, consider surface found.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "0.01", ClampMax = "2.0"))
	float DensityThreshold = 0.5f;

	/**
	 * Enable Occupancy Bitmask optimization.
	 * Uses 32³ bit mask (4KB) for O(1) empty block detection.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bEnableOccupancyMask = true;

	/**
	 * Enable Min-Max Mipmap optimization.
	 * Uses hierarchical volume for empty space skipping.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bEnableMinMaxMipmap = true;

	/**
	 * Enable Tile-based Culling optimization.
	 * Skips tiles with no fluid intersection.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bEnableTileCulling = true;

	/**
	 * Enable Temporal Reprojection optimization.
	 * Reuses ~90% of previous frame data.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bEnableTemporalReprojection = true;

	/**
	 * Temporal blend factor (0 = current frame only, 1 = history only).
	 * 0.9 recommended for fluid motion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bEnableTemporalReprojection",
			ClampMin = "0.0", ClampMax = "0.99"))
	float TemporalBlendFactor = 0.9f;

	/**
	 * Adaptive step size multiplier.
	 * Step size increases in empty regions for faster traversal.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "1.0", ClampMax = "8.0"))
	float AdaptiveStepMultiplier = 4.0f;

	/**
	 * Early termination alpha threshold.
	 * Stop ray marching when accumulated alpha exceeds this value.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching",
			ClampMin = "0.9", ClampMax = "1.0"))
	float EarlyTerminationAlpha = 0.99f;

	//========================================
	// SDF (Signed Distance Field) Parameters
	//========================================

	/**
	 * Use SDF (Signed Distance Field) mode instead of density volume.
	 * SDF uses Sphere Tracing for efficient empty space skipping.
	 * Better quality with translucency support.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bUseSDF = true;

	/**
	 * SDF SmoothMin parameter (K).
	 * Controls the smoothness of fluid surface blending.
	 * Higher = smoother surface, lower = more defined particles.
	 * Recommended: 0.5x ~ 1.5x of ParticleRadius
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.5", ClampMax = "100.0"))
	float SDFSmoothK = 5.0f;

	/**
	 * SDF Surface offset.
	 * Negative = larger fluid volume, positive = smaller.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "-50.0", ClampMax = "50.0"))
	float SDFSurfaceOffset = 0.0f;

	/**
	 * SDF surface hit epsilon (in voxels).
	 * Smaller = more precise surface detection.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.1", ClampMax = "5.0"))
	float SDFSurfaceEpsilon = 1.0f;

	/**
	 * SDF Sphere Tracing relaxation factor.
	 * 1.0 = standard Sphere Tracing, >1.0 = over-relaxation (faster but less stable).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "1.0", ClampMax = "2.0"))
	float SDFRelaxationFactor = 1.2f;

	/**
	 * SDF translucency maximum depth.
	 * How far light travels through the fluid for translucency calculation.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "10.0", ClampMax = "500.0"))
	float SDFTranslucencyDepth = 100.0f;

	/**
	 * SDF translucency density.
	 * Controls internal light absorption for translucency.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.001", ClampMax = "0.1"))
	float SDFTranslucencyDensity = 0.02f;

	/**
	 * SDF subsurface scattering strength.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.0", ClampMax = "2.0"))
	float SDFSubsurfaceScatterStrength = 0.5f;

	/**
	 * SDF subsurface scattering color.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	FLinearColor SDFSubsurfaceColor = FLinearColor(0.8f, 0.6f, 0.4f, 1.0f);

	/**
	 * SDF reflection strength.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF",
			ClampMin = "0.0", ClampMax = "1.0"))
	float SDFReflectionStrength = 0.3f;

	/**
	 * Enable SDF Hybrid Mode.
	 * Uses SDF Volume for fast approach + Z-Order neighbor search for precise near-surface evaluation.
	 * Provides better quality normals with minimal performance overhead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	bool bEnableSDFHybridMode = true;

	/**
	 * Hybrid Mode threshold distance (in cm).
	 * Switch from SDF Volume to Z-Order when distance to surface is less than this value.
	 * Lower = less Z-Order usage (faster), Higher = more precise normals.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF && bEnableSDFHybridMode",
			ClampMin = "5.0", ClampMax = "100.0"))
	float SDFHybridThreshold = 30.0f;

	//========================================
	// SDF Optimization Options
	//========================================

	/**
	 * Use Tight AABB (computed from actual fluid particles) instead of simulation bounds.
	 * Dramatically reduces SDF volume build time by focusing only on fluid region.
	 * NOTE: Currently disabled - requires async GPU readback implementation.
	 * See: Docs/SDF_Optimization_TODO.md
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	bool bUseTightAABB = false;

	/**
	 * AABB padding multiplier (multiplied by particle radius).
	 * Higher values provide more safety margin for fast-moving fluids.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF && bUseTightAABB",
			ClampMin = "1.0", ClampMax = "5.0"))
	float AABBPaddingMultiplier = 2.0f;

	/**
	 * Debug visualization for Tight AABB.
	 * Shows: R = bUseTightAABB, G = bounds valid, B = bounds size.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF && bUseTightAABB"))
	bool bDebugVisualizeTightAABB = false;

	/**
	 * Use Sparse Voxel structure (only compute SDF where particles exist).
	 * Additional optimization on top of Tight AABB.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	bool bUseSparseVoxel = false;

	/**
	 * Use Temporal Coherence (reuse previous frame's SDF, only update changed regions).
	 * Best for slow-moving or static fluids.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF|Optimization",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDF"))
	bool bUseTemporalCoherence = false;

	/**
	 * Temporal dirty threshold (cm/frame).
	 * Particles moving faster than this threshold are marked dirty and recomputed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching|SDF|Optimization",
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
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Params.ShadingMode)));
	Hash = HashCombine(Hash, GetTypeHash(Params.FluidColor.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(Params.FresnelStrength));
	Hash = HashCombine(Hash, GetTypeHash(Params.RefractiveIndex));
	Hash = HashCombine(Hash, GetTypeHash(Params.AbsorptionCoefficient));
	Hash = HashCombine(Hash, GetTypeHash(Params.AbsorptionColorCoefficients.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(Params.SpecularStrength));
	Hash = HashCombine(Hash, GetTypeHash(Params.SpecularRoughness));
	Hash = HashCombine(Hash, GetTypeHash(Params.EnvironmentLightColor.ToString()));
	// Lighting scale parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.AmbientScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.TransmittanceScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.AlphaThicknessScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.RefractionScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.FresnelReflectionBlend));
	Hash = HashCombine(Hash, GetTypeHash(Params.AbsorptionBias));
	// Reflection Cubemap parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.ReflectionCubemap.Get()));
	Hash = HashCombine(Hash, GetTypeHash(Params.ReflectionIntensity));
	Hash = HashCombine(Hash, GetTypeHash(Params.ReflectionMipLevel));
	Hash = HashCombine(Hash, GetTypeHash(Params.ParticleRenderRadius));
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Params.SmoothingFilter)));
	Hash = HashCombine(Hash, GetTypeHash(Params.SmoothingStrength));
	Hash = HashCombine(Hash, GetTypeHash(Params.BilateralFilterRadius));
	// Narrow-Range parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.NarrowRangeThresholdRatio));
	Hash = HashCombine(Hash, GetTypeHash(Params.NarrowRangeClampRatio));
	Hash = HashCombine(Hash, GetTypeHash(Params.NarrowRangeGrazingBoost));
	// Curvature Flow parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.CurvatureFlowDt));
	Hash = HashCombine(Hash, GetTypeHash(Params.CurvatureFlowDepthThreshold));
	Hash = HashCombine(Hash, GetTypeHash(Params.CurvatureFlowIterations));
	Hash = HashCombine(Hash, GetTypeHash(Params.CurvatureFlowGrazingBoost));
	// Anisotropy parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Params.AnisotropyParams.Mode)));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.AnisotropyScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.AnisotropyMin));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.AnisotropyMax));
	// Surface Decoration parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.SurfaceDecoration.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(Params.SurfaceDecoration.Foam.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(Params.SurfaceDecoration.Emissive.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(Params.RenderTargetScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.ThicknessScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.Metallic));
	Hash = HashCombine(Hash, GetTypeHash(Params.Roughness));
	Hash = HashCombine(Hash, GetTypeHash(Params.SubsurfaceOpacity));
	// SSS parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.SSSIntensity));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSSColor.ToString()));
	// SSR parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.bEnableSSR));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSRMaxSteps));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSRStepSize));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSRThickness));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSRIntensity));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSREdgeFade));
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
		A.ShadingMode == B.ShadingMode &&
		A.FluidColor.Equals(B.FluidColor, 0.001f) &&
		FMath::IsNearlyEqual(A.FresnelStrength, B.FresnelStrength, 0.001f) &&
		FMath::IsNearlyEqual(A.RefractiveIndex, B.RefractiveIndex, 0.001f) &&
		FMath::IsNearlyEqual(A.AbsorptionCoefficient, B.AbsorptionCoefficient, 0.001f) &&
		A.AbsorptionColorCoefficients.Equals(B.AbsorptionColorCoefficients, 0.001f) &&
		FMath::IsNearlyEqual(A.SpecularStrength, B.SpecularStrength, 0.001f) &&
		FMath::IsNearlyEqual(A.SpecularRoughness, B.SpecularRoughness, 0.001f) &&
		A.EnvironmentLightColor.Equals(B.EnvironmentLightColor, 0.001f) &&
		// Lighting scale parameters
		FMath::IsNearlyEqual(A.AmbientScale, B.AmbientScale, 0.001f) &&
		FMath::IsNearlyEqual(A.TransmittanceScale, B.TransmittanceScale, 0.0001f) &&
		FMath::IsNearlyEqual(A.AlphaThicknessScale, B.AlphaThicknessScale, 0.0001f) &&
		FMath::IsNearlyEqual(A.RefractionScale, B.RefractionScale, 0.001f) &&
		FMath::IsNearlyEqual(A.FresnelReflectionBlend, B.FresnelReflectionBlend, 0.001f) &&
		FMath::IsNearlyEqual(A.AbsorptionBias, B.AbsorptionBias, 0.001f) &&
		// Reflection Cubemap parameters
		A.ReflectionCubemap == B.ReflectionCubemap &&
		FMath::IsNearlyEqual(A.ReflectionIntensity, B.ReflectionIntensity, 0.001f) &&
		FMath::IsNearlyEqual(A.ReflectionMipLevel, B.ReflectionMipLevel, 0.001f) &&
		FMath::IsNearlyEqual(A.ParticleRenderRadius, B.ParticleRenderRadius, 0.001f) &&
		A.SmoothingFilter == B.SmoothingFilter &&
		FMath::IsNearlyEqual(A.SmoothingStrength, B.SmoothingStrength, 0.001f) &&
		A.BilateralFilterRadius == B.BilateralFilterRadius &&
		// Narrow-Range parameters
		FMath::IsNearlyEqual(A.NarrowRangeThresholdRatio, B.NarrowRangeThresholdRatio, 0.01f) &&
		FMath::IsNearlyEqual(A.NarrowRangeClampRatio, B.NarrowRangeClampRatio, 0.01f) &&
		FMath::IsNearlyEqual(A.NarrowRangeGrazingBoost, B.NarrowRangeGrazingBoost, 0.01f) &&
		// Curvature Flow parameters
		FMath::IsNearlyEqual(A.CurvatureFlowDt, B.CurvatureFlowDt, 0.001f) &&
		FMath::IsNearlyEqual(A.CurvatureFlowDepthThreshold, B.CurvatureFlowDepthThreshold, 0.1f) &&
		A.CurvatureFlowIterations == B.CurvatureFlowIterations &&
		FMath::IsNearlyEqual(A.CurvatureFlowGrazingBoost, B.CurvatureFlowGrazingBoost, 0.01f) &&
		// Anisotropy parameters
		A.AnisotropyParams.bEnabled == B.AnisotropyParams.bEnabled &&
		A.AnisotropyParams.Mode == B.AnisotropyParams.Mode &&
		FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyScale, B.AnisotropyParams.AnisotropyScale,
		                     0.001f) &&
		FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyMin, B.AnisotropyParams.AnisotropyMin,
		                     0.001f) &&
		FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyMax, B.AnisotropyParams.AnisotropyMax,
		                     0.001f) &&
		// Surface Decoration parameters
		A.SurfaceDecoration.bEnabled == B.SurfaceDecoration.bEnabled &&
		A.SurfaceDecoration.Foam.bEnabled == B.SurfaceDecoration.Foam.bEnabled &&
		A.SurfaceDecoration.Emissive.bEnabled == B.SurfaceDecoration.Emissive.bEnabled &&
		FMath::IsNearlyEqual(A.RenderTargetScale, B.RenderTargetScale, 0.001f) &&
		FMath::IsNearlyEqual(A.ThicknessScale, B.ThicknessScale, 0.001f) &&
		FMath::IsNearlyEqual(A.Metallic, B.Metallic, 0.001f) &&
		FMath::IsNearlyEqual(A.Roughness, B.Roughness, 0.001f) &&
		FMath::IsNearlyEqual(A.SubsurfaceOpacity, B.SubsurfaceOpacity, 0.001f) &&
		// SSS parameters
		FMath::IsNearlyEqual(A.SSSIntensity, B.SSSIntensity, 0.001f) &&
		A.SSSColor.Equals(B.SSSColor, 0.001f) &&
		// SSR parameters
		A.bEnableSSR == B.bEnableSSR &&
		A.SSRMaxSteps == B.SSRMaxSteps &&
		FMath::IsNearlyEqual(A.SSRStepSize, B.SSRStepSize, 0.01f) &&
		FMath::IsNearlyEqual(A.SSRThickness, B.SSRThickness, 0.01f) &&
		FMath::IsNearlyEqual(A.SSRIntensity, B.SSRIntensity, 0.01f) &&
		FMath::IsNearlyEqual(A.SSREdgeFade, B.SSREdgeFade, 0.01f) &&
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
