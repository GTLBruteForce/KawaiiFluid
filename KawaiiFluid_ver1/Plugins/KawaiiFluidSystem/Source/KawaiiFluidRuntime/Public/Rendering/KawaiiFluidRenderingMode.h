// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "KawaiiFluidRenderingMode.generated.h"

/**
 * Kawaii Fluid 렌더링 방식
 */
UENUM(BlueprintType)
enum class EKawaiiFluidRenderingMode : uint8
{
	/** 렌더링 모드 선택 안함 (수동 제어) */
	None        UMETA(DisplayName = "None (Manual)"),

	/** Instanced Static Mesh 렌더링 */
	ISM         UMETA(DisplayName = "ISM"),

	/** Niagara 파티클 시스템 (GPU 최적화) */
	Niagara     UMETA(DisplayName = "Niagara"),

	/** SSFR 파이프라인 렌더링 (Screen Space Fluid Rendering) */
	SSFR        UMETA(DisplayName = "SSFR")
};
