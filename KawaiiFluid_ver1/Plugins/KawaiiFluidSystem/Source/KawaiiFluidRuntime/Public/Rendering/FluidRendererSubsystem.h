// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "FluidRenderingParameters.h"
#include "FluidRendererSubsystem.generated.h"

class FFluidSceneViewExtension;
class UKawaiiFluidRenderingModule;

/**
 * 유체 렌더링 월드 서브시스템
 *
 * 역할:
 * - UKawaiiFluidRenderingModule 통합 관리
 * - SSFR 렌더링 파이프라인 제공 (ViewExtension)
 * - ISM 렌더링은 Unreal 기본 파이프라인 사용
 */
UCLASS()
class KAWAIIFLUIDRUNTIME_API UFluidRendererSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End of USubsystem interface

	//========================================
	// RenderingModule 관리
	//========================================

	/** RenderingModule 등록 (자동 호출됨) */
	void RegisterRenderingModule(UKawaiiFluidRenderingModule* Module);

	/** RenderingModule 해제 */
	void UnregisterRenderingModule(UKawaiiFluidRenderingModule* Module);

	/** 등록된 모든 RenderingModule 반환 */
	const TArray<UKawaiiFluidRenderingModule*>& GetAllRenderingModules() const { return RegisteredRenderingModules; }

	//========================================
	// 렌더링 파라미터
	//========================================

	/** 글로벌 렌더링 파라미터 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Rendering")
	FFluidRenderingParameters RenderingParameters;

	/** View Extension 접근자 */
	TSharedPtr<FFluidSceneViewExtension, ESPMode::ThreadSafe> GetViewExtension() const { return ViewExtension; }

private:
	/** 등록된 RenderingModule들 */
	UPROPERTY()
	TArray<UKawaiiFluidRenderingModule*> RegisteredRenderingModules;

	/** Scene View Extension (렌더링 파이프라인 인젝션) */
	TSharedPtr<FFluidSceneViewExtension, ESPMode::ThreadSafe> ViewExtension;
};
