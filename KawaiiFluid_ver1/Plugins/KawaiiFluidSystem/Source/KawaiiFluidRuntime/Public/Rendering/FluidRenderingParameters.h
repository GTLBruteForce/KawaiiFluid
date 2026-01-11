// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/FluidAnisotropy.h"
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
	ScreenSpace UMETA(DisplayName = "Screen Space")
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

	/** Bilateral/Narrow-Range filter 반경 (픽셀) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing", meta = (ClampMin = "1", ClampMax = "50"))
	int32 BilateralFilterRadius = 20;

	/** Depth threshold (bilateral filter용) - 동적 계산 사용으로 deprecated */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing", meta = (ClampMin = "0.001", ClampMax = "100.0"))
	float DepthThreshold = 10.0f;

	//========================================
	// Narrow-Range Filter Parameters
	//========================================

	/**
	 * Narrow-Range threshold 비율
	 * threshold = ParticleRadius × 이 값
	 * 낮을수록 엣지 보존 강함, 높을수록 스무딩 강함
	 * 1.0~3.0: 타이트한 엣지, 5.0~10.0: 부드러운 표면
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::NarrowRange",
		        ClampMin = "0.5", ClampMax = "20.0"))
	float NarrowRangeThresholdRatio = 3.0f;

	/**
	 * Narrow-Range clamp 비율
	 * 앞쪽(카메라 방향) 샘플 클램핑 강도
	 * ParticleRadius × 이 값으로 제한
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::NarrowRange",
		        ClampMin = "0.1", ClampMax = "5.0"))
	float NarrowRangeClampRatio = 1.0f;

	/**
	 * Narrow-Range Grazing Angle 부스트 강도
	 * 얕은 각도에서 threshold를 높여 더 많은 샘플 포함
	 * 0 = 부스트 없음, 1 = 그레이징 시 2배 threshold
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::NarrowRange",
		        ClampMin = "0.0", ClampMax = "2.0"))
	float NarrowRangeGrazingBoost = 1.0f;

	//========================================
	// Curvature Flow Parameters
	//========================================

	/**
	 * Curvature Flow 시간 단계 (Dt)
	 * 높을수록 한 iteration당 더 많이 스무딩됨
	 * 0.05~0.15 권장 (안정성)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::CurvatureFlow",
		        ClampMin = "0.01", ClampMax = "0.5"))
	float CurvatureFlowDt = 0.1f;

	/**
	 * Curvature Flow 깊이 임계값
	 * 이 값보다 큰 깊이 차이는 실루엣으로 간주하여 스무딩 안 함
	 * 파티클 반지름의 3-10배 권장
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::CurvatureFlow",
		        ClampMin = "1.0", ClampMax = "500.0"))
	float CurvatureFlowDepthThreshold = 100.0f;

	/**
	 * Curvature Flow iteration 횟수
	 * 높을수록 부드럽지만 비용 증가
	 * Grazing angle 해결에는 50+ 권장
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::CurvatureFlow",
		        ClampMin = "1", ClampMax = "200"))
	int32 CurvatureFlowIterations = 50;

	/**
	 * Grazing Angle 부스트 강도
	 * 얕은 각도에서 보일 때 스무딩을 더 강하게 적용
	 * 0 = 부스트 없음, 1 = 그레이징 시 2배 스무딩
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing",
		meta = (EditCondition = "SmoothingFilter == EDepthSmoothingFilter::CurvatureFlow",
		        ClampMin = "0.0", ClampMax = "2.0"))
	float CurvatureFlowGrazingBoost = 1.0f;

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
	 * 0.8+ 권장: grazing angle에서 반사가 강해져 표면 디테일이 자연스럽게 가려짐
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FresnelReflectionBlend = 0.8f;

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

	/** Subsurface scattering intensity (jelly effect) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SSSIntensity = 1.0f;

	/** Subsurface scattering color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor SSSColor = FLinearColor(1.0f, 0.5f, 0.3f, 1.0f);

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
	Hash = HashCombine(Hash, GetTypeHash(Params.RenderTargetScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.ThicknessScale));
	Hash = HashCombine(Hash, GetTypeHash(Params.Metallic));
	Hash = HashCombine(Hash, GetTypeHash(Params.Roughness));
	Hash = HashCombine(Hash, GetTypeHash(Params.SubsurfaceOpacity));
	// SSS parameters
	Hash = HashCombine(Hash, GetTypeHash(Params.SSSIntensity));
	Hash = HashCombine(Hash, GetTypeHash(Params.SSSColor.ToString()));
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
	       FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyScale, B.AnisotropyParams.AnisotropyScale, 0.001f) &&
	       FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyMin, B.AnisotropyParams.AnisotropyMin, 0.001f) &&
	       FMath::IsNearlyEqual(A.AnisotropyParams.AnisotropyMax, B.AnisotropyParams.AnisotropyMax, 0.001f) &&
	       FMath::IsNearlyEqual(A.RenderTargetScale, B.RenderTargetScale, 0.001f) &&
	       FMath::IsNearlyEqual(A.ThicknessScale, B.ThicknessScale, 0.001f) &&
	       FMath::IsNearlyEqual(A.Metallic, B.Metallic, 0.001f) &&
	       FMath::IsNearlyEqual(A.Roughness, B.Roughness, 0.001f) &&
	       FMath::IsNearlyEqual(A.SubsurfaceOpacity, B.SubsurfaceOpacity, 0.001f) &&
	       // SSS parameters
	       FMath::IsNearlyEqual(A.SSSIntensity, B.SSSIntensity, 0.001f) &&
	       A.SSSColor.Equals(B.SSSColor, 0.001f) &&
	       // Shadow parameters
	       A.bEnableShadowCasting == B.bEnableShadowCasting &&
	       A.VSMResolution == B.VSMResolution &&
	       FMath::IsNearlyEqual(A.VSMBlurRadius, B.VSMBlurRadius, 0.001f) &&
	       A.VSMBlurIterations == B.VSMBlurIterations &&
	       FMath::IsNearlyEqual(A.ShadowIntensity, B.ShadowIntensity, 0.001f);
}
