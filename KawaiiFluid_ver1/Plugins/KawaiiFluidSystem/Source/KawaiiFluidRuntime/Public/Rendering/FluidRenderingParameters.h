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

	/** 파티클 렌더링 반경 (스크린 스페이스, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Depth", meta = (ClampMin = "1.0", ClampMax = "100.0"))
	float ParticleRenderRadius = 15.0f;

	/** Depth smoothing 강도 (0=없음, 1=최대) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SmoothingStrength = 0.5f;

	/** Bilateral filter 반경 (픽셀) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing", meta = (ClampMin = "1", ClampMax = "20"))
	int32 BilateralFilterRadius = 20;

	/** Depth threshold (bilateral filter용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Smoothing", meta = (ClampMin = "0.001", ClampMax = "100.0"))
	float DepthThreshold = 50.0f;

	/** 유체 색상 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance")
	FLinearColor FluidColor = FLinearColor(0.2f, 0.5f, 0.8f, 1.0f);

	/** Fresnel 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Appearance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FresnelStrength = 0.7f;

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

	/** Thickness 렌더링 스케일 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Thickness", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float ThicknessScale = 1.0f;

	/** Render target 해상도 스케일 (1.0 = 화면 해상도) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Performance", meta = (ClampMin = "0.25", ClampMax = "1.0"))
	float RenderTargetScale = 1.0f;

	FFluidRenderingParameters() = default;
};
