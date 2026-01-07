// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FluidRenderingParameters.generated.h"

/**
 * SSFR (Screen Space Fluid Rendering) 품질 설정
 */
UENUM(BlueprintType)
enum class EFluidRenderingQuality : uint8
{
	Low       UMETA(DisplayName = "Low"),
	Medium    UMETA(DisplayName = "Medium"),
	High      UMETA(DisplayName = "High"),
	Ultra     UMETA(DisplayName = "Ultra")
};

/**
 * Metaball Pipeline Type
 * Defines how the fluid surface is computed.
 */
UENUM(BlueprintType)
enum class EMetaballPipelineType : uint8
{
	/** Screen Space pipeline: Depth -> Smoothing -> Normal -> Thickness */
	ScreenSpace UMETA(DisplayName = "Screen Space"),

	/** Ray Marching pipeline: Direct SDF computation from particle buffer */
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
	GBuffer UMETA(DisplayName = "G-Buffer"),

	/** Ray Marching SDF - smooth metaball surfaces for slime-like fluids */
	RayMarching UMETA(DisplayName = "Ray Marching SDF")
};

/**
 * 유체 렌더링 파라미터
 * SSFR 파이프라인 전반에 사용되는 설정들
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFluidRenderingParameters
{
	GENERATED_BODY()

	/** 렌더링 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bEnableRendering = true;

	/** 렌더링 품질 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	EFluidRenderingQuality Quality = EFluidRenderingQuality::Medium;

	/** Pipeline type (how surface is computed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	EMetaballPipelineType PipelineType = EMetaballPipelineType::ScreenSpace;

	/** Shading mode (how surface is lit/rendered) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	EMetaballShadingMode ShadingMode = EMetaballShadingMode::PostProcess;

	/** 파티클 렌더링 반경 (스크린 스페이스, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth", meta = (ClampMin = "1.0", ClampMax = "100.0"))
	float ParticleRenderRadius = 15.0f;

	/** Depth smoothing 강도 (0=없음, 1=최대) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SmoothingStrength = 0.5f;

	/** Bilateral filter 반경 (픽셀) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing", meta = (ClampMin = "1", ClampMax = "50"))
	int32 BilateralFilterRadius = 20;

	/** Depth threshold (bilateral filter용) - 동적 계산 사용으로 deprecated */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing", meta = (ClampMin = "0.001", ClampMax = "100.0"))
	float DepthThreshold = 10.0f;

	/** 유체 색상 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor FluidColor = FLinearColor(0.2f, 0.5f, 0.8f, 1.0f);

	/** Fresnel 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FresnelStrength = 0.02f;

	/** 굴절률 (IOR) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "1.0", ClampMax = "2.0"))
	float RefractiveIndex = 1.33f;

	/** 흡수 계수 (thickness 기반 색상 감쇠) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float AbsorptionCoefficient = 2.0f;

	/** 스펙큘러 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SpecularStrength = 1.0f;

	/** 스펙큘러 거칠기 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float SpecularRoughness = 0.2f;

	/** 환경광 색상 (Sky Light - 얇은 부분 반사색) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor EnvironmentLightColor = FLinearColor(0.8f, 0.9f, 1.0f, 1.0f);

	/** Thickness 렌더링 스케일 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Thickness", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float ThicknessScale = 1.0f;

	/** Render target 해상도 스케일 (1.0 = 화면 해상도) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Performance", meta = (ClampMin = "0.25", ClampMax = "1.0"))
	float RenderTargetScale = 1.0f;

	/** SSFR rendering mode (maps to PipelineType + ShadingMode internally) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	ESSFRRenderingMode SSFRMode = ESSFRRenderingMode::Custom;

	//========================================
	// Ray Marching SDF Mode Parameters
	//========================================

	/** SDF smoothness for metaball blending (higher = more stretchy/blobby) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching", ClampMin = "1.0", ClampMax = "64.0"))
	float SDFSmoothness = 12.0f;

	/** Maximum ray marching steps */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching", ClampMin = "16", ClampMax = "256"))
	int32 MaxRayMarchSteps = 128;

	/** Ray march hit threshold (surface detection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching", ClampMin = "0.0001", ClampMax = "1.0"))
	float RayMarchHitThreshold = 1.0f;

	/** Maximum ray march distance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching", ClampMin = "100.0", ClampMax = "10000.0"))
	float RayMarchMaxDistance = 2000.0f;

	/** Subsurface scattering intensity (jelly effect) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching", ClampMin = "0.0", ClampMax = "2.0"))
	float SSSIntensity = 1.0f;

	/** Subsurface scattering color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching"))
	FLinearColor SSSColor = FLinearColor(1.0f, 0.5f, 0.3f, 1.0f);

	/**
	 * Use SDF Volume optimization for Ray Marching
	 * When enabled, bakes SDF to 3D texture using compute shader (~400x faster)
	 * When disabled, uses direct particle iteration (legacy mode)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching"))
	bool bUseSDFVolumeOptimization = true;

	/** SDF Volume resolution (64 = 64x64x64 voxels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching && bUseSDFVolumeOptimization",
			ClampMin = "32", ClampMax = "256"))
	int32 SDFVolumeResolution = 64;

	/**
	 * Use Spatial Hash for hybrid SDF evaluation (with SDF Volume)
	 * HYBRID MODE: SDF Volume for fast 90% ray march + Spatial Hash for precise 10% final evaluation
	 * SDF Volume: O(1) texture sampling for fast approach to surface
	 * Spatial Hash: O(k) neighbor lookup for accurate final surface detection
	 * Note: Only available when bUseSDFVolumeOptimization is enabled
	 *
	 * HybridSwitchThreshold is auto-calculated: ParticleRadius * 2.0 + SDFSmoothness
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching && bUseSDFVolumeOptimization"))
	bool bUseSpatialHash = false;

	/** Number of history samples per particle (1 = current only, 2 = include previous frame). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching", ClampMin = "1", ClampMax = "2"))
	int32 AttachedRenderSampleCount = 2;

	/** Blend weight between previous and current position when spawning history samples (0 = exact previous, 1 = near current). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::RayMarching", ClampMin = "0.0", ClampMax = "1.0"))
	float AttachedRenderSpread = 0.25f;

	//========================================
	// Shadow Casting (VSM) - Legacy
	//========================================

	/** Enable fluid shadow casting (Legacy VSM approach) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Shadow",
		meta = (DisplayName = "Enable Shadow Casting (Legacy)"))
	bool bEnableShadowCasting = false;

	/** VSM shadow map resolution */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Shadow",
		meta = (EditCondition = "bEnableShadowCasting", ClampMin = "256", ClampMax = "4096"))
	int32 VSMResolution = 1024;

	/** VSM blur radius (higher = softer shadows) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Shadow",
		meta = (EditCondition = "bEnableShadowCasting", ClampMin = "0.0", ClampMax = "16.0"))
	float VSMBlurRadius = 4.0f;

	/** Number of VSM blur iterations */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Shadow",
		meta = (EditCondition = "bEnableShadowCasting", ClampMin = "0", ClampMax = "3"))
	int32 VSMBlurIterations = 1;

	/** Shadow intensity (0 = no shadow, 1 = full black) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Shadow",
		meta = (EditCondition = "bEnableShadowCasting", ClampMin = "0.0", ClampMax = "1.0"))
	float ShadowIntensity = 0.5f;

	//========================================
	// Debug Visualization
	//========================================

	/** Draw SDF Volume bounding box as debug lines */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugDrawSDFVolume = false;

	/** SDF Volume debug box color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FColor SDFVolumeDebugColor = FColor::Green;

	//========================================
	// G-Buffer Mode Parameters
	//========================================

	/** Metallic value for GBuffer (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float Metallic = 0.1f;

	/** Roughness value for GBuffer (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float Roughness = 0.3f;

	/** Subsurface scattering opacity (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "SSFRMode == ESSFRRenderingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float SubsurfaceOpacity = 0.5f;

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
	Hash = HashCombine(Hash, GetTypeHash(Params.SpecularStrength));
	Hash = HashCombine(Hash, GetTypeHash(Params.SpecularRoughness));
	Hash = HashCombine(Hash, GetTypeHash(Params.EnvironmentLightColor.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(Params.ParticleRenderRadius));
	Hash = HashCombine(Hash, GetTypeHash(Params.SmoothingStrength));
	Hash = HashCombine(Hash, GetTypeHash(Params.BilateralFilterRadius));
	Hash = HashCombine(Hash, GetTypeHash(Params.RenderTargetScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.ThicknessScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.Metallic));
	Hash = HashCombine(Hash, GetTypeHash(Params.Roughness));
	Hash = HashCombine(Hash, GetTypeHash(Params.SubsurfaceOpacity));
	// Ray Marching parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.SDFSmoothness));
	Hash = HashCombine(Hash, GetTypeHash(Params.MaxRayMarchSteps));
	Hash = HashCombine(Hash, GetTypeHash(Params.RayMarchHitThreshold));
	Hash = HashCombine(Hash, GetTypeHash(Params.RayMarchMaxDistance));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSSIntensity));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSSColor.ToString()));
	Hash = HashCombine(Hash, GetTypeHash(Params.bUseSDFVolumeOptimization));
	Hash = HashCombine(Hash, GetTypeHash(Params.SDFVolumeResolution));
	Hash = HashCombine(Hash, GetTypeHash(Params.bUseSpatialHash));
	// Shadow parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.bEnableShadowCasting));
	Hash = HashCombine(Hash, GetTypeHash(Params.VSMResolution));
	Hash = HashCombine(Hash, GetTypeHash(Params.VSMBlurRadius));
	Hash = HashCombine(Hash, GetTypeHash(Params.VSMBlurIterations));
	Hash = HashCombine(Hash, GetTypeHash(Params.ShadowIntensity));
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
	       FMath::IsNearlyEqual(A.SpecularStrength, B.SpecularStrength, 0.001f) &&
	       FMath::IsNearlyEqual(A.SpecularRoughness, B.SpecularRoughness, 0.001f) &&
	       A.EnvironmentLightColor.Equals(B.EnvironmentLightColor, 0.001f) &&
	       FMath::IsNearlyEqual(A.ParticleRenderRadius, B.ParticleRenderRadius, 0.001f) &&
	       FMath::IsNearlyEqual(A.SmoothingStrength, B.SmoothingStrength, 0.001f) &&
	       A.BilateralFilterRadius == B.BilateralFilterRadius &&
	       FMath::IsNearlyEqual(A.RenderTargetScale, B.RenderTargetScale, 0.001f) &&
	       FMath::IsNearlyEqual(A.ThicknessScale, B.ThicknessScale, 0.001f) &&
	       FMath::IsNearlyEqual(A.Metallic, B.Metallic, 0.001f) &&
	       FMath::IsNearlyEqual(A.Roughness, B.Roughness, 0.001f) &&
	       FMath::IsNearlyEqual(A.SubsurfaceOpacity, B.SubsurfaceOpacity, 0.001f) &&
	       // Ray Marching parameters
	       FMath::IsNearlyEqual(A.SDFSmoothness, B.SDFSmoothness, 0.001f) &&
	       A.MaxRayMarchSteps == B.MaxRayMarchSteps &&
	       FMath::IsNearlyEqual(A.RayMarchHitThreshold, B.RayMarchHitThreshold, 0.0001f) &&
	       FMath::IsNearlyEqual(A.RayMarchMaxDistance, B.RayMarchMaxDistance, 0.001f) &&
	       FMath::IsNearlyEqual(A.SSSIntensity, B.SSSIntensity, 0.001f) &&
	       A.SSSColor.Equals(B.SSSColor, 0.001f) &&
	       A.bUseSDFVolumeOptimization == B.bUseSDFVolumeOptimization &&
	       A.SDFVolumeResolution == B.SDFVolumeResolution &&
	       A.bUseSpatialHash == B.bUseSpatialHash &&
	       // Shadow parameters
	       A.bEnableShadowCasting == B.bEnableShadowCasting &&
	       A.VSMResolution == B.VSMResolution &&
	       FMath::IsNearlyEqual(A.VSMBlurRadius, B.VSMBlurRadius, 0.001f) &&
	       A.VSMBlurIterations == B.VSMBlurIterations &&
	       FMath::IsNearlyEqual(A.ShadowIntensity, B.ShadowIntensity, 0.001f);
}
