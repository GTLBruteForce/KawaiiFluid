// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/FluidAnisotropy.h"
#include "Engine/TextureCube.h"
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
 * Depth smoothing filter type for SSFR.
 * Different filters have different characteristics for edge preservation and performance.
 */
UENUM(BlueprintType)
enum class EDepthSmoothingFilter : uint8
{
	/** Bilateral filter - classic approach with depth-aware smoothing */
	Bilateral UMETA(DisplayName = "Bilateral Filter"),

	/** Narrow-Range filter (Truong & Yuksel 2018) - better edge preservation, especially with anisotropy */
	NarrowRange UMETA(DisplayName = "Narrow-Range Filter")
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth", meta = (ClampMin = "0.5", ClampMax = "100.0"))
	float ParticleRenderRadius = 15.0f;

	/** Depth smoothing filter type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing")
	EDepthSmoothingFilter SmoothingFilter = EDepthSmoothingFilter::NarrowRange;

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

	/**
	 * Fresnel 강도 배율 (IOR에서 F0 자동 계산 후 적용)
	 * 1.0 = 물리적으로 정확한 반사, 2.0 = 과장된 반사, 0.5 = 약한 반사
	 * F0 = ((1-IOR)/(1+IOR))^2 * FresnelStrength
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float FresnelStrength = 1.0f;

	/** 굴절률 (IOR) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "1.0", ClampMax = "2.0"))
	float RefractiveIndex = 1.33f;

	/** 흡수 계수 (thickness 기반 색상 감쇠) - 전체 스케일 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float AbsorptionCoefficient = 2.0f;

	/**
	 * RGB별 흡수 계수 (Beer's Law)
	 * 물: R=0.4, G=0.1, B=0.05 (빨강을 많이 흡수하여 파랗게 보임)
	 * 슬라임: R=0.1, G=0.3, B=0.4 (파랑을 많이 흡수하여 녹색/노란색 계열)
	 * 높은 값 = 해당 색상이 더 빨리 흡수됨 (두꺼운 부분에서 안 보임)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor AbsorptionColorCoefficients = FLinearColor(0.4f, 0.1f, 0.05f, 1.0f);

	/** 스펙큘러 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SpecularStrength = 1.0f;

	/** 스펙큘러 거칠기 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float SpecularRoughness = 0.2f;

	/** 환경광 색상 (Cubemap 없을 때 fallback 색상, Ambient 기본색) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor EnvironmentLightColor = FLinearColor(0.8f, 0.9f, 1.0f, 1.0f);

	//========================================
	// Lighting Scale Parameters
	//========================================

	/**
	 * Ambient 조명 강도 스케일
	 * EnvironmentLightColor에 곱해지는 배율
	 * 0 = Ambient 없음 (완전히 어두운 면 가능), 1 = 강한 Ambient
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AmbientScale = 0.15f;

	/**
	 * Beer's Law 투과율 스케일
	 * 두께에 따른 빛 흡수 속도 조절
	 * 낮을수록 투명, 높을수록 두꺼운 부분이 불투명
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.001", ClampMax = "0.5"))
	float TransmittanceScale = 0.05f;

	/**
	 * Alpha 두께 스케일
	 * 두께가 Alpha에 영향을 미치는 정도
	 * 낮을수록 투명, 높을수록 불투명
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.001", ClampMax = "0.2"))
	float AlphaThicknessScale = 0.02f;

	/**
	 * 굴절 오프셋 스케일
	 * 굴절에 의한 UV 오프셋 강도
	 * 0 = 굴절 없음, 높을수록 강한 왜곡
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "0.2"))
	float RefractionScale = 0.05f;

	/**
	 * Fresnel 반사 혼합 비율
	 * BaseColor와 ReflectedColor 혼합 시 Fresnel 영향 정도
	 * 0 = 반사 없음, 1 = 강한 반사
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FresnelReflectionBlend = 0.5f;

	/**
	 * 흡수 바이어스 (Ray Marching용)
	 * BaseColor 혼합 시 흡수 기여도에 추가되는 값
	 * 높을수록 FluidColor가 더 강하게 보임
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AbsorptionBias = 0.7f;

	/**
	 * 환경 반사용 Cubemap (Reflection Capture)
	 * 설정하지 않으면 EnvironmentLightColor를 사용
	 * Scene의 Reflection Capture나 HDRI Cubemap 할당 가능
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	TObjectPtr<UTextureCube> ReflectionCubemap = nullptr;

	/** Cubemap 반사 강도 (0 = 반사 없음, 1 = 풀 반사) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float ReflectionIntensity = 1.0f;

	/** Cubemap Mip 레벨 (높을수록 블러리한 반사, Roughness 연동) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float ReflectionMipLevel = 2.0f;

	/** Thickness 렌더링 스케일 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Thickness", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float ThicknessScale = 1.0f;

	/** Render target 해상도 스케일 (1.0 = 화면 해상도) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Performance", meta = (ClampMin = "0.25", ClampMax = "1.0"))
	float RenderTargetScale = 1.0f;

	/** Anisotropy parameters for ellipsoid rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Anisotropy")
	FFluidAnisotropyParams AnisotropyParams;

	//========================================
	// Ray Marching SDF Mode Parameters
	//========================================

	/** SDF smoothness for metaball blending (higher = more stretchy/blobby) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching", ClampMin = "1.0", ClampMax = "64.0"))
	float SDFSmoothness = 12.0f;

	/** Maximum ray marching steps */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching", ClampMin = "16", ClampMax = "256"))
	int32 MaxRayMarchSteps = 128;

	/** Ray march hit threshold (surface detection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching", ClampMin = "0.0001", ClampMax = "1.0"))
	float RayMarchHitThreshold = 1.0f;

	/** Maximum ray march distance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching", ClampMin = "100.0", ClampMax = "10000.0"))
	float RayMarchMaxDistance = 2000.0f;

	/** Subsurface scattering intensity (jelly effect) - Works in both ScreenSpace and RayMarching */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SSSIntensity = 1.0f;

	/** Subsurface scattering color - Works in both ScreenSpace and RayMarching */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor SSSColor = FLinearColor(1.0f, 0.5f, 0.3f, 1.0f);

	/**
	 * Use SDF Volume optimization for Ray Marching
	 * When enabled, bakes SDF to 3D texture using compute shader (~400x faster)
	 * When disabled, uses direct particle iteration (legacy mode)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching"))
	bool bUseSDFVolumeOptimization = true;

	/** SDF Volume resolution (64 = 64x64x64 voxels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDFVolumeOptimization",
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
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching && bUseSDFVolumeOptimization"))
	bool bUseSpatialHash = false;

	/** Number of history samples per particle (1 = current only, 2 = include previous frame). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching", ClampMin = "1", ClampMax = "2"))
	int32 AttachedRenderSampleCount = 2;

	/** Blend weight between previous and current position when spawning history samples (0 = exact previous, 1 = near current). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|RayMarching",
		meta = (EditCondition = "PipelineType == EMetaballPipelineType::RayMarching", ClampMin = "0.0", ClampMax = "1.0"))
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
		meta = (EditCondition = "ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float Metallic = 0.1f;

	/** Roughness value for GBuffer (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float Roughness = 0.3f;

	/** Subsurface scattering opacity (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
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
	// Anisotropy parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.bEnabled));
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Params.AnisotropyParams.Mode)));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.AnisotropyScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.AnisotropyMin));
	Hash = HashCombine(Hash, GetTypeHash(Params.AnisotropyParams.AnisotropyMax));
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
	       // Anisotropy parameters
	       A.AnisotropyParams.bEnabled == B.AnisotropyParams.bEnabled &&
	       A.AnisotropyParams.Mode == B.AnisotropyParams.Mode &&
	       FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyScale, B.AnisotropyParams.AnisotropyScale, 0.001f) &&
	       FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyMin, B.AnisotropyParams.AnisotropyMin, 0.001f) &&
	       FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyMax, B.AnisotropyParams.AnisotropyMax, 0.001f) &&
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
