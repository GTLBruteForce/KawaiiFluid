// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/FluidParticle.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Test/FluidDummyGenerationMode.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "KawaiiFluidTestDataComponent.generated.h"

class UKawaiiFluidRenderingModule;

/**
 * 새 아키텍처 테스트용 유체 데이터 제공 컴포넌트
 * 
 * IKawaiiFluidDataProvider를 구현하여 시뮬레이션 데이터만 제공합니다.
 * 렌더링은 별도의 UKawaiiFluidRenderController + Renderer 조합으로 처리합니다.
 * 
 * 특징:
 * - 순수 데이터 제공자 (렌더링 책임 없음)
 * - Transform 없음 (월드 좌표 기반 시뮬레이션)
 * - FFluidParticle 사용 (시뮬레이션 타입)
 * - 다양한 테스트 패턴 지원
 * 
 * 사용 예시 (Blueprint):
 * @code
 * Actor에 컴포넌트 추가:
 * 1. KawaiiFluidTestDataComponent (데이터 생성)
 * 2. KawaiiFluidRenderController (렌더링 제어)
 * 3. KawaiiFluidISMComponent (렌더러)
 * 
 * Play 시 자동으로 연결되어 렌더링됨
 * @endcode
 * 
 * @note 새 아키텍처 테스트/검증용. 프로덕션은 UKawaiiFluidSimulationComponent 사용
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent, DisplayName="Fluid Test Data (New Architecture)"))
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidTestDataComponent 
	: public UActorComponent
	, public IKawaiiFluidDataProvider
{
	GENERATED_BODY()

public:
	UKawaiiFluidTestDataComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	//========================================
	// 테스트 모드 설정
	//========================================

	/** 테스트 데이터 생성 모드 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Mode")
	EKawaiiFluidDummyGenMode DataMode = EKawaiiFluidDummyGenMode::Animated;

	//========================================
	// 파티클 설정
	//========================================

	/** 파티클 개수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Particles", meta = (ClampMin = "1", ClampMax = "10000"))
	int32 ParticleCount = 500;

	/** 파티클 반경 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Particles", meta = (ClampMin = "1.0", ClampMax = "50.0"))
	float ParticleRadius = 5.0f;

	/** 생성 영역 크기 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Particles")
	FVector SpawnExtent = FVector(100.0f, 100.0f, 100.0f);

	//========================================
	// 애니메이션 설정
	//========================================

	/** 애니메이션 속도 (DataMode가 Animated/Wave일 때) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Animation", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float AnimationSpeed = 1.0f;

	/** 파동 진폭 (Wave 모드, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Animation", meta = (ClampMin = "1.0", ClampMax = "100.0"))
	float WaveAmplitude = 20.0f;

	/** 파동 주파수 (Wave 모드) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Animation", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float WaveFrequency = 1.0f;

	//========================================
	// 블루프린트 함수
	//========================================

	/** 테스트 데이터 재생성 */
	UFUNCTION(BlueprintCallable, Category = "Test Data")
	void RegenerateTestData();

	/** 현재 파티클 수 반환 */
	UFUNCTION(BlueprintPure, Category = "Test Data")
	int32 GetCurrentParticleCount() const { return TestParticles.Num(); }

	/** 테스트 패턴 변경 */
	UFUNCTION(BlueprintCallable, Category = "Test Data")
	void SetTestPattern(EKawaiiFluidDummyGenMode NewMode);

	/** 파티클 개수 변경 */
	UFUNCTION(BlueprintCallable, Category = "Test Data")
	void SetParticleCount(int32 NewCount);

	/** 애니메이션 일시정지/재개 */
	UFUNCTION(BlueprintCallable, Category = "Test Data")
	void ToggleAnimation();

	//========================================
	// Rendering Control
	//========================================

	/** Enable rendering system */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Rendering")
	bool bEnableRendering = true;

	/** ISM Renderer Settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "ISM Settings"))
	FKawaiiFluidISMRendererSettings ISMSettings;

	/** Metaball Renderer Settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test Data|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "Metaball Settings"))
	FKawaiiFluidMetaballRendererSettings MetaballSettings;

	//========================================
	// IKawaiiFluidDataProvider 인터페이스 구현
	//========================================

	/** 시뮬레이션 파티클 데이터 반환 */
	virtual const TArray<FFluidParticle>& GetParticles() const override
	{
		return TestParticles;
	}

	/** 파티클 개수 반환 */
	virtual int32 GetParticleCount() const override
	{
		return TestParticles.Num();
	}

	/** 파티클 반경 반환 (시뮬레이션 실제 반경) */
	virtual float GetParticleRadius() const override
	{
		return ParticleRadius;
	}

	/** 데이터 유효성 확인 */
	virtual bool IsDataValid() const override
	{
		return TestParticles.Num() > 0;
	}

	/** 디버그 이름 반환 */
	virtual FString GetDebugName() const override
	{
		AActor* Owner = GetOwner();
		return FString::Printf(TEXT("FluidTestDataComponent_%s"), 
			Owner ? *Owner->GetName() : TEXT("NoOwner"));
	}

protected:
	//========================================
	// 테스트 데이터
	//========================================

	/** 테스트 파티클 배열 (시뮬레이션 데이터 형식) */
	TArray<FFluidParticle> TestParticles;

	/** 애니메이션 시간 */
	float AnimationTime = 0.0f;

	/** 애니메이션 일시정지 */
	bool bAnimationPaused = false;

	//========================================
	// 내부 렌더링 (Details Panel에 노출 안 됨)
	//========================================

	/** Internal rendering module (not exposed in Details panel) */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidRenderingModule> RenderingModule;

	//========================================
	// 내부 메서드
	//========================================

	/** 테스트 파티클 생성 */
	void GenerateTestParticles();

	/** 애니메이션 파티클 업데이트 */
	void UpdateAnimatedParticles(float DeltaTime);

	// 데이터 생성 헬퍼
	void GenerateStaticData();
	void GenerateGridPattern();
	void GenerateSpherePattern();
};

