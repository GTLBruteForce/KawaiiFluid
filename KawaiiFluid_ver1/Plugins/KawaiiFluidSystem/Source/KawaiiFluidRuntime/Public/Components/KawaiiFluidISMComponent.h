// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interfaces/IKawaiiFluidRenderer.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "KawaiiFluidISMComponent.generated.h"

/**
 * Instanced Static Mesh 기반 유체 렌더러
 * 
 * IKawaiiFluidRenderer를 구현하여 파티클을 ISM으로 렌더링합니다.
 * 각 파티클이 하나의 메시 인스턴스로 표시됩니다.
 * 
 * 특징:
 * - 높은 성능 (GPU 인스턴싱)
 * - 커스텀 메시/머티리얼 지원
 * - 속도 기반 색상 변화 옵션
 * - 절대 좌표 사용 (월드 좌표 직접 렌더링)
 * 
 * 사용 예시:
 * @code
 * ISMRenderer = CreateDefaultSubobject<UKawaiiFluidISMComponent>(TEXT("ISMRenderer"));
 * ISMRenderer->ParticleMesh = MySphereMesh;
 * @endcode
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent, DisplayName="Fluid ISM Renderer"))
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidISMComponent 
	: public UActorComponent
	, public IKawaiiFluidRenderer
{
	GENERATED_BODY()

public:
	UKawaiiFluidISMComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	//========================================
	// IKawaiiFluidRenderer 구현
	//========================================

	virtual void UpdateRendering(const IKawaiiFluidDataProvider* DataProvider, float DeltaTime) override;
	virtual bool IsEnabled() const override { return bEnableRendering; }
	virtual EKawaiiFluidRenderingMode GetRenderingMode() const override { return EKawaiiFluidRenderingMode::ISM; }
	virtual void SetEnabled(bool bInEnabled) override { bEnableRendering = bInEnabled; }

	//========================================
	// 기본 설정
	//========================================

	/** 렌더링 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering")
	bool bEnableRendering = true;

	/** 파티클로 사용할 메시 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering")
	TObjectPtr<UStaticMesh> ParticleMesh;

	/** 파티클 머티리얼 (nullptr이면 메시의 기본 머티리얼 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering")
	TObjectPtr<UMaterialInterface> ParticleMaterial;

	/** 파티클 크기 배율 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float ParticleScale = 1.0f;

	//========================================
	// 성능 옵션
	//========================================

	/** 최대 렌더링 파티클 수 (메모리/성능 절약) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering|Performance", meta = (ClampMin = "1", ClampMax = "100000"))
	int32 MaxRenderParticles = 10000;

	/** Culling Distance (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering|Performance", meta = (ClampMin = "0.0"))
	float CullDistance = 10000.0f;

	/** Cast Shadow */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering|Performance")
	bool bCastShadow = false;

	//========================================
	// 시각 효과
	//========================================

	/** 속도 기반 회전 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering|Visual")
	bool bRotateByVelocity = false;

	/** 속도 기반 색상 변화 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering|Visual")
	bool bColorByVelocity = false;

	/** 최소 속도 색상 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering|Visual", meta = (EditCondition = "bColorByVelocity"))
	FLinearColor MinVelocityColor = FLinearColor::Blue;

	/** 최대 속도 색상 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering|Visual", meta = (EditCondition = "bColorByVelocity"))
	FLinearColor MaxVelocityColor = FLinearColor::Red;

	/** 속도 정규화 값 (이 속도를 최대로 간주) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ISM Rendering|Visual", meta = (EditCondition = "bColorByVelocity", ClampMin = "1.0"))
	float MaxVelocityForColor = 1000.0f;

	//========================================
	// 컴포넌트 접근
	//========================================

	/** ISM 컴포넌트 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ISM Rendering")
	TObjectPtr<UInstancedStaticMeshComponent> ISMComponent;

	/** ISM 컴포넌트 가져오기 (Blueprint용) */
	UFUNCTION(BlueprintPure, Category = "ISM Rendering")
	UInstancedStaticMeshComponent* GetISMComponent() const { return ISMComponent; }

protected:
	//========================================
	// 내부 메서드
	//========================================

	/** ISM 컴포넌트 초기화 */
	void InitializeISM();

	/** 기본 파티클 메시 로드 */
	UStaticMesh* GetDefaultParticleMesh();

	/** 기본 머티리얼 로드 */
	UMaterialInterface* GetDefaultParticleMaterial();
};


