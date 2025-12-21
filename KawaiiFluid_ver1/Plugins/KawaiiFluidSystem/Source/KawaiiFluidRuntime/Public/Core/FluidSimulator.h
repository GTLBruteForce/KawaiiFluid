// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiRenderParticle.h"
#include "FluidSimulator.generated.h"

class FSpatialHash;
class FDensityConstraint;
class FViscositySolver;
class FAdhesionSolver;
class UFluidCollider;
class FKawaiiFluidRenderResource;

/**
 * 유체 타입 열거형
 */
UENUM(BlueprintType)
enum class EFluidType : uint8
{
	Water    UMETA(DisplayName = "Water"),
	Honey    UMETA(DisplayName = "Honey"),
	Slime    UMETA(DisplayName = "Slime"),
	Custom   UMETA(DisplayName = "Custom")
};

/**
 * PBF 기반 유체 시뮬레이터
 * 점성 유체(슬라임, 꿀 등) 시뮬레이션의 메인 클래스
 */
UCLASS(BlueprintType)
class KAWAIIFLUIDRUNTIME_API AFluidSimulator : public AActor
{
	GENERATED_BODY()

public:
	AFluidSimulator();
	virtual ~AFluidSimulator();

	virtual void BeginPlay() override;
	virtual void BeginDestroy() override;
	virtual void Tick(float DeltaTime) override;

	//========================================
	// 유체 타입 프리셋
	//========================================

	/** 유체 타입 (프리셋) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Preset")
	EFluidType FluidType;

	/** 유체 타입에 따른 프리셋 적용 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyFluidTypePreset(EFluidType NewType);

	//========================================
	// 일반 설정
	//========================================

	/** 최대 입자 수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Basic", meta = (ClampMin = "1"))
	int32 MaxParticles;

	/** 시뮬레이션 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Basic")
	bool bSimulationEnabled;

	//========================================
	// 물리 파라미터
	//========================================

	/** 기준 밀도 (kg/m³) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.1"))
	float RestDensity;

	/** 입자 질량 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.01"))
	float ParticleMass;

	/** 스무딩 반경 (커널 반경 h, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "1.0"))
	float SmoothingRadius;

	/** 밀도 제약 솔버 반복 횟수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "1", ClampMax = "10"))
	int32 SolverIterations;

	/** 중력 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics")
	FVector Gravity;

	/** 안정성 상수 (Epsilon) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.0"))
	float Epsilon;

	//========================================
	// 점성 파라미터
	//========================================

	/** XSPH 점성 계수 (0=물, 0.5=슬라임, 0.8=꿀) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Viscosity", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ViscosityCoefficient;

	//========================================
	// 접착력 파라미터
	//========================================

	/** 접착 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AdhesionStrength;

	/** 접착 반경 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "1.0"))
	float AdhesionRadius;

	/** 분리 임계값 (이 힘 이상이면 떨어짐) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.0"))
	float DetachThreshold;

	//========================================
	// 월드 콜리전
	//========================================

	/** 월드 콜리전 사용 (스태틱 메시 등과 충돌) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision")
	bool bUseWorldCollision;

	/** 콜리전 채널 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision")
	TEnumAsByte<ECollisionChannel> CollisionChannel;

	//========================================
	// 디버그 시각화
	//========================================

	/** 디버그 렌더링 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug")
	bool bEnableDebugRendering;

	/** 디버그 입자 크기 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug", meta = (ClampMin = "0.1", ClampMax = "20.0", EditCondition = "bEnableDebugRendering"))
	float DebugParticleRadius;

	/** 디버그 메시 컴포넌트 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Debug")
	UInstancedStaticMeshComponent* DebugMeshComponent;

	/** 시작 시 자동 스폰 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug")
	bool bSpawnOnBeginPlay;

	/** 자동 스폰 입자 수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug", meta = (ClampMin = "1", ClampMax = "5000", EditCondition = "bSpawnOnBeginPlay"))
	int32 AutoSpawnCount;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug", meta = (ClampMin = "1.0", ClampMax = "500.0", EditCondition = "bSpawnOnBeginPlay"))
	float AutoSpawnRadius;

	//========================================
	// 블루프린트 함수
	//========================================

	/** 특정 위치에 입자들 생성 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void SpawnParticles(FVector Location, int32 Count, float SpawnRadius = 50.0f);

	/** 모든 입자에 외력 적용 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyExternalForce(FVector Force);

	/** 특정 입자에 힘 적용 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyForceToParticle(int32 ParticleIndex, FVector Force);

	/** 콜라이더 등록 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void RegisterCollider(UFluidCollider* Collider);

	/** 콜라이더 해제 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void UnregisterCollider(UFluidCollider* Collider);

	/** 입자 위치 배열 반환 (렌더링용) */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	TArray<FVector> GetParticlePositions() const;

	/** 입자 속도 배열 반환 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	TArray<FVector> GetParticleVelocities() const;

	/** 현재 입자 수 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 GetParticleCount() const;

	/** 모든 입자 제거 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ClearAllParticles();

	//========================================
	// Particles 직접 접근 (고급 사용자용)
	//========================================

	/** 파티클 배열 읽기 전용 접근 */
	const TArray<FFluidParticle>& GetParticles() const { return Particles; }

	/** 파티클 배열 쓰기 가능 접근 (주의: 직접 수정 시 물리 시뮬레이션 영향) */
	TArray<FFluidParticle>& GetParticlesMutable() { return Particles; }

private:
	//========================================
	// 시뮬레이션 데이터
	//========================================

	/** 입자 배열 */
	TArray<FFluidParticle> Particles;

	/** 외력 누적 (다음 프레임에 적용) */
	FVector AccumulatedExternalForce;

	/** 다음 입자 ID */
	int32 NextParticleID;

	//========================================
	// 솔버 및 유틸리티
	//========================================

	/** 공간 해싱 */
	TUniquePtr<FSpatialHash> SpatialHash;

	/** 밀도 제약 솔버 */
	TUniquePtr<FDensityConstraint> DensityConstraint;

	/** 점성 솔버 */
	TUniquePtr<FViscositySolver> ViscositySolver;

	/** 접착력 솔버 */
	TUniquePtr<FAdhesionSolver> AdhesionSolver;

	/** 등록된 콜라이더들 */
	UPROPERTY()
	TArray<UFluidCollider*> Colliders;

	//========================================
	// GPU 렌더 리소스
	//========================================

	/** GPU 렌더 리소스 (SharedPtr로 수명 관리) */
	TSharedPtr<FKawaiiFluidRenderResource> RenderResource;

	//========================================
	// 내부 메서드
	//========================================

	/** 솔버들 초기화 */
	void InitializeSolvers();

	/** 디버그 메시 초기화 */
	void InitializeDebugMesh();

	/** 디버그 인스턴스 업데이트 */
	void UpdateDebugInstances();

	/** GPU 렌더 리소스 초기화 */
	void InitializeRenderResource();

	/** GPU 렌더 데이터 업데이트 */
	void UpdateRenderData();

	/** 렌더 입자 변환 */
	TArray<FKawaiiRenderParticle> ConvertToRenderParticles() const;

	/** 1. 외력 적용 & 위치 예측 */
	void PredictPositions(float DeltaTime);

	/** 2. 이웃 탐색 업데이트 */
	void UpdateNeighbors();

	/** 3. 밀도 제약 해결 (반복) */
	void SolveDensityConstraints();

	/** 충돌 형상 캐싱 (프레임당 한 번) */
	void CacheColliderShapes();

	/** 4. 충돌 처리 */
	void HandleCollisions();

	/** 월드 콜리전 처리 */
	void HandleWorldCollision();

	/** 5. 속도 업데이트 & 위치 확정 */
	void FinalizePositions(float DeltaTime);

	/** 6. 점성 적용 */
	void ApplyViscosity();

	/** 7. 접착력 적용 */
	void ApplyAdhesion();
};
