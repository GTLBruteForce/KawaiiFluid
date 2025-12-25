// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Interfaces/IKawaiiFluidRenderer.h"
#include "Core/KawaiiRenderParticle.h"
#include "KawaiiFluidRenderController.generated.h"

/**
 * 유체 렌더링 컨트롤러 컴포넌트
 * 
 * DataProvider로부터 시뮬레이션 데이터를 받아서
 * 등록된 Renderer들을 제어하고 렌더링을 요청합니다.
 * 
 * 책임:
 * - DataProvider와 Renderer 연결 및 제어
 * - 시뮬레이션 데이터 → 렌더링 데이터 변환
 * - 여러 렌더러 동시 관리 (ISM, Niagara, SSFR 등)
 * - FluidRendererSubsystem에 자동 등록
 * - 자동 업데이트 제어
 * 
 * 사용 예시:
 * @code
 * // Actor에 컴포넌트 추가
 * RenderController = CreateDefaultSubobject<UKawaiiFluidRenderController>(TEXT("RenderController"));
 * ISMRenderer = CreateDefaultSubobject<UKawaiiFluidISMComponent>(TEXT("ISMRenderer"));
 * 
 * // BeginPlay에서 자동으로 연결됨
 * @endcode
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent, DisplayName="Fluid Render Controller"))
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidRenderController : public UActorComponent
{
	GENERATED_BODY()

public:
	UKawaiiFluidRenderController();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	//========================================
	// DataProvider 연결
	//========================================

	/** 
	 * 시뮬레이션 데이터를 제공할 컴포넌트
	 * 
	 * 수동 설정 가능, 또는 bAutoFindDataProvider=true일 때 자동으로 찾음
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Rendering|Data Source")
	TScriptInterface<IKawaiiFluidDataProvider> DataProvider;

	/** 
	 * DataProvider 자동 찾기 활성화
	 * 
	 * BeginPlay 시 같은 Actor의 컴포넌트 중
	 * IKawaiiFluidDataProvider를 구현한 컴포넌트를 자동으로 찾아 연결
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Rendering|Data Source")
	bool bAutoFindDataProvider = true;

	/** 
	 * DataProvider 수동 설정
	 * 
	 * @param NewDataProvider 연결할 DataProvider
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Rendering")
	void SetDataProvider(TScriptInterface<IKawaiiFluidDataProvider> NewDataProvider);

	//========================================
	// Renderer 관리
	//========================================

	/** 
	 * 활성 렌더러 목록
	 * 
	 * UpdateRenderers() 호출 시 이 목록의 모든 렌더러가 업데이트됨
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid Rendering|Renderers")
	TArray<TScriptInterface<IKawaiiFluidRenderer>> ActiveRenderers;

	/** 
	 * Renderer 자동 찾기 활성화
	 * 
	 * BeginPlay 시 같은 Actor의 컴포넌트 중
	 * IKawaiiFluidRenderer를 구현한 컴포넌트를 자동으로 찾아 등록
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Rendering|Renderers")
	bool bAutoRegisterRenderers = true;

	/** 
	 * Primary 렌더링 모드 선택
	 * 
	 * 특정 렌더링 모드를 선택하면 해당 렌더러만 활성화됩니다.
	 * None으로 설정 시 모든 등록된 렌더러가 활성화됩니다 (다중 렌더링).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Rendering|Renderers", 
		meta = (EditCondition = "bAutoRegisterRenderers"))
	EKawaiiFluidRenderingMode PrimaryRenderMode = EKawaiiFluidRenderingMode::ISM;


	/** 
	 * 렌더러 등록
	 * 
	 * @param Renderer 등록할 렌더러
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Rendering")
	void RegisterRenderer(TScriptInterface<IKawaiiFluidRenderer> Renderer);

	/** 
	 * 렌더러 제거
	 * 
	 * @param Renderer 제거할 렌더러
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Rendering")
	void UnregisterRenderer(TScriptInterface<IKawaiiFluidRenderer> Renderer);

	/** 
	 * 모든 렌더러 제거
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Rendering")
	void ClearRenderers();

	//========================================
	// 렌더링 업데이트
	//========================================

	/** 
	 * 자동 업데이트 활성화
	 * 
	 * true일 때 Tick마다 자동으로 UpdateRenderers() 호출
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Rendering|Update")
	bool bAutoUpdateRenderers = true;

	/** 
	 * 모든 렌더러 업데이트
	 * 
	 * 1. DataProvider에서 시뮬레이션 데이터 가져오기
	 * 2. FFluidParticle → FKawaiiRenderParticle 변환
	 * 3. 모든 활성 렌더러에게 렌더링 요청
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Rendering")
	void UpdateRenderers();

	/** 
	 * 현재 렌더링 중인 파티클 수
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Rendering")
	int32 GetParticleCount() const { return RenderParticlesCache.Num(); }

private:
	//========================================
	// 내부 메서드
	//========================================

	/** DataProvider 자동 탐색 */
	void FindDataProvider();

	/** Renderer 자동 탐색 및 등록 */
	void FindAndRegisterRenderers();

	/** 
	 * Primary 렌더링 모드와 일치하는지 확인
	 * 
	 * @param Renderer 확인할 렌더러
	 * @return Primary 모드와 일치하거나 Primary 모드가 None이면 true
	 */
	bool IsMatchingPrimaryMode(const TScriptInterface<IKawaiiFluidRenderer>& Renderer) const;

	/** 변환 캐시 (성능 최적화) */
	TArray<FKawaiiRenderParticle> RenderParticlesCache;
};

