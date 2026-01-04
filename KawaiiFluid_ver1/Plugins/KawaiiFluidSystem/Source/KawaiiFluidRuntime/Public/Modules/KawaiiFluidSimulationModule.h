// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "KawaiiFluidSimulationModule.generated.h"

/** Collision event callback type */
DECLARE_DELEGATE_OneParam(FOnModuleCollisionEvent, const FKawaiiFluidCollisionEvent&);

class FSpatialHash;
class UKawaiiFluidPresetDataAsset;
class UFluidCollider;
class UFluidInteractionComponent;

/**
 * 유체 시뮬레이션 데이터 모듈 (UObject 기반)
 *
 * 시뮬레이션에 필요한 데이터를 소유하고 Blueprint API를 제공합니다.
 * 실제 시뮬레이션 로직은 UKawaiiFluidSimulationContext가 처리하고,
 * 오케스트레이션은 UKawaiiFluidSimulatorSubsystem이 담당합니다.
 *
 * 책임:
 * - 파티클 배열 소유
 * - SpatialHash 소유 (Independent 모드용)
 * - 콜라이더/상호작용 컴포넌트 레퍼런스 관리
 * - Preset 레퍼런스 및 Override 관리
 * - 외력 누적
 * - 파티클 생성/삭제 API
 *
 * 사용:
 * - UKawaiiFluidComponent에 Instanced로 포함
 * - Blueprint에서 직접 함수 호출 가능
 */
UCLASS(DefaultToInstanced, EditInlineNew, BlueprintType)
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidSimulationModule : public UObject, public IKawaiiFluidDataProvider
{
	GENERATED_BODY()

public:
	UKawaiiFluidSimulationModule();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	//========================================
	// 초기화 / 정리
	//========================================

	/** 모듈 초기화 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	virtual void Initialize(UKawaiiFluidPresetDataAsset* InPreset);

	/** 모듈 정리 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void Shutdown();

	/** 초기화 상태 확인 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	bool IsInitialized() const { return bIsInitialized; }

	//========================================
	// Preset / 파라미터
	//========================================

	/** Preset 설정 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SetPreset(UKawaiiFluidPresetDataAsset* InPreset);

	/** Preset 가져오기 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	UKawaiiFluidPresetDataAsset* GetPreset() const { return Preset; }

	/** 시뮬레이션 파라미터 빌드 */
	virtual FKawaiiFluidSimulationParams BuildSimulationParams() const;

	/** Override 적용된 Effective Preset 반환 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	UKawaiiFluidPresetDataAsset* GetEffectivePreset();

	/** RuntimePreset 갱신 */
	void UpdateRuntimePreset();

	/** RuntimePreset dirty 마크 */
	void MarkRuntimePresetDirty() { bRuntimePresetDirty = true; }

	/** Override 존재 여부 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	bool HasAnyOverride() const;

	//========================================
	// 파티클 데이터 접근 (IKawaiiFluidDataProvider 구현)
	//========================================

	/** 파티클 배열 (읽기 전용) - IKawaiiFluidDataProvider::GetParticles() */
	virtual const TArray<FFluidParticle>& GetParticles() const override { return Particles; }

	/** 파티클 배열 (수정 가능 - Subsystem/Context 용) */
	TArray<FFluidParticle>& GetParticlesMutable() { return Particles; }

	/** 다음 파티클 ID 가져오기 */
	int32 GetNextParticleID() const { return NextParticleID; }

	/** 다음 파티클 ID 설정 (InstanceData 복원용) */
	void SetNextParticleID(int32 InNextParticleID) { NextParticleID = InNextParticleID; }

	/** 파티클 수 - IKawaiiFluidDataProvider::GetParticleCount() */
	UFUNCTION(BlueprintPure, Category = "Fluid")
	virtual int32 GetParticleCount() const override { return Particles.Num(); }

	//========================================
	// 파티클 생성/삭제
	//========================================

	/** 단일 파티클 스폰 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticle(FVector Position, FVector Velocity = FVector::ZeroVector);

	/** 여러 파티클 스폰 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void SpawnParticles(FVector Location, int32 Count, float SpawnRadius);

	/** 모든 파티클 제거 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ClearAllParticles();

	/** 파티클 위치 배열 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	TArray<FVector> GetParticlePositions() const;

	/** 파티클 속도 배열 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	TArray<FVector> GetParticleVelocities() const;

	//========================================
	// 외력
	//========================================

	/** 모든 파티클에 외력 적용 (누적) */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyExternalForce(FVector Force);

	/** 특정 파티클에 힘 적용 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyForceToParticle(int32 ParticleIndex, FVector Force);

	/** 누적 외력 가져오기 */
	FVector GetAccumulatedExternalForce() const { return AccumulatedExternalForce; }

	/** 누적 외력 리셋 */
	void ResetExternalForce() { AccumulatedExternalForce = FVector::ZeroVector; }

	//========================================
	// 콜라이더 관리
	//========================================

	/** 콜라이더 등록 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void RegisterCollider(UFluidCollider* Collider);

	/** 콜라이더 해제 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void UnregisterCollider(UFluidCollider* Collider);

	/** 모든 콜라이더 해제 */
	void ClearColliders() { Colliders.Empty(); }

	/** 등록된 콜라이더 목록 */
	const TArray<UFluidCollider*>& GetColliders() const { return Colliders; }

	//========================================
	// 상호작용 컴포넌트 관리
	//========================================

	/** 상호작용 컴포넌트 등록 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void RegisterInteractionComponent(UFluidInteractionComponent* Component);

	/** 상호작용 컴포넌트 해제 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void UnregisterInteractionComponent(UFluidInteractionComponent* Component);

	/** 등록된 상호작용 컴포넌트 목록 */
	const TArray<UFluidInteractionComponent*>& GetInteractionComponents() const { return InteractionComponents; }

	//========================================
	// SpatialHash (Independent 모드용)
	//========================================

	/** SpatialHash 가져오기 */
	FSpatialHash* GetSpatialHash() const { return SpatialHash.Get(); }

	/** SpatialHash 초기화 */
	void InitializeSpatialHash(float CellSize);

	//========================================
	// 시간 관리 (Substep용)
	//========================================

	/** 누적 시간 */
	float GetAccumulatedTime() const { return AccumulatedTime; }
	void SetAccumulatedTime(float Time) { AccumulatedTime = Time; }

	//========================================
	// 쿼리
	//========================================

	/** 반경 내 파티클 인덱스 찾기 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	TArray<int32> GetParticlesInRadius(FVector Location, float Radius) const;

	/** 박스 내 파티클 인덱스 찾기 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	TArray<int32> GetParticlesInBox(FVector Center, FVector Extent) const;

	/** 파티클 정보 가져오기 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	bool GetParticleInfo(int32 ParticleIndex, FVector& OutPosition, FVector& OutVelocity, float& OutDensity) const;

	//========================================
	// 시뮬레이션 제어
	//========================================

	/** 시뮬레이션 활성화/비활성화 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SetSimulationEnabled(bool bEnabled) { bSimulationEnabled = bEnabled; }

	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	bool IsSimulationEnabled() const { return bSimulationEnabled; }

	/** Independent 모드 (Subsystem 배치 처리 안함) */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SetIndependentSimulation(bool bIndependent) { bIndependentSimulation = bIndependent; }

	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	bool IsIndependentSimulation() const { return bIndependentSimulation || HasAnyOverride(); }

	//========================================
	// Context (Outer 체인 캐시)
	//========================================

	/** Owner Actor 반환 (캐시됨) */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	AActor* GetOwnerActor() const;

	//========================================
	// Collision Settings
	//========================================

	/** World Collision 사용 여부 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision")
	bool bUseWorldCollision = true;

	//========================================
	// Event Settings
	//========================================

	/** 충돌 이벤트 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events")
	bool bEnableCollisionEvents = false;

	/** 이벤트 발생을 위한 최소 속도 (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "0.0", EditCondition = "bEnableCollisionEvents"))
	float MinVelocityForEvent = 50.0f;

	/** 프레임당 최대 이벤트 수 (0 = 무제한) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "0", EditCondition = "bEnableCollisionEvents"))
	int32 MaxEventsPerFrame = 10;

	/** 파티클별 이벤트 쿨다운 (초) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "0.0", EditCondition = "bEnableCollisionEvents"))
	float EventCooldownPerParticle = 0.1f;

	/** 충돌 이벤트 콜백 설정 */
	void SetCollisionEventCallback(FOnModuleCollisionEvent InCallback) { OnCollisionEventCallback = InCallback; }

	/** 충돌 이벤트 콜백 가져오기 */
	const FOnModuleCollisionEvent& GetCollisionEventCallback() const { return OnCollisionEventCallback; }

	//========================================
	// Preset (디테일 패널에 노출)
	//========================================

	/** Fluid preset data asset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Simulation")
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset;

	//========================================
	// Override Setters (런타임 + 에디터 공용)
	//========================================

	UFUNCTION(BlueprintCallable, Category = "Fluid|Override")
	void SetOverride_ParticleRadius(bool bEnable, float Value);

	UFUNCTION(BlueprintCallable, Category = "Fluid|Override")
	void SetOverride_SmoothingRadius(bool bEnable, float Value);

	UFUNCTION(BlueprintCallable, Category = "Fluid|Override")
	void SetOverride_RestDensity(bool bEnable, float Value);

	UFUNCTION(BlueprintCallable, Category = "Fluid|Override")
	void SetOverride_Compliance(bool bEnable, float Value);

	UFUNCTION(BlueprintCallable, Category = "Fluid|Override")
	void SetOverride_ViscosityCoefficient(bool bEnable, float Value);

	UFUNCTION(BlueprintCallable, Category = "Fluid|Override")
	void SetOverride_Gravity(bool bEnable, FVector Value);

	UFUNCTION(BlueprintCallable, Category = "Fluid|Override")
	void SetOverride_AdhesionStrength(bool bEnable, float Value);

private:
	//========================================
	// Override 값들 (private, Setter로만 수정)
	//========================================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", InlineEditConditionToggle))
	bool bOverride_ParticleRadius = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", EditCondition = "bOverride_ParticleRadius", ClampMin = "0.1", ClampMax = "50.0"))
	float Override_ParticleRadius = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", InlineEditConditionToggle))
	bool bOverride_SmoothingRadius = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", EditCondition = "bOverride_SmoothingRadius", ClampMin = "1.0"))
	float Override_SmoothingRadius = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", InlineEditConditionToggle))
	bool bOverride_RestDensity = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", EditCondition = "bOverride_RestDensity", ClampMin = "0.1"))
	float Override_RestDensity = 1200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", InlineEditConditionToggle))
	bool bOverride_Compliance = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", EditCondition = "bOverride_Compliance", ClampMin = "0.0"))
	float Override_Compliance = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", InlineEditConditionToggle))
	bool bOverride_ViscosityCoefficient = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", EditCondition = "bOverride_ViscosityCoefficient", ClampMin = "0.0", ClampMax = "1.0"))
	float Override_ViscosityCoefficient = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", InlineEditConditionToggle))
	bool bOverride_Gravity = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", EditCondition = "bOverride_Gravity"))
	FVector Override_Gravity = FVector(0.0f, 0.0f, -980.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", InlineEditConditionToggle))
	bool bOverride_AdhesionStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", EditCondition = "bOverride_AdhesionStrength", ClampMin = "0.0", ClampMax = "1.0"))
	float Override_AdhesionStrength = 0.5f;
	//========================================
	// Internal
	//========================================

	/** 충돌 이벤트 콜백 */
	FOnModuleCollisionEvent OnCollisionEventCallback;

	//========================================
	// 데이터
	//========================================

	/** 파티클 배열 - 에디터 직렬화 + SaveGame 모두 지원 */
	UPROPERTY()
	TArray<FFluidParticle> Particles;

	/** 공간 해싱 (Independent 모드용) */
	TSharedPtr<FSpatialHash> SpatialHash;

	/** 등록된 콜라이더 */
	UPROPERTY()
	TArray<UFluidCollider*> Colliders;

	/** 등록된 상호작용 컴포넌트 */
	UPROPERTY()
	TArray<UFluidInteractionComponent*> InteractionComponents;

	/** Override 적용된 런타임 Preset (Transient) */
	UPROPERTY(Transient)
	UKawaiiFluidPresetDataAsset* RuntimePreset = nullptr;

	/** RuntimePreset 갱신 필요 플래그 */
	bool bRuntimePresetDirty = true;

	/** 누적 외력 */
	FVector AccumulatedExternalForce = FVector::ZeroVector;

	/** 다음 파티클 ID - 에디터 직렬화 지원 */
	UPROPERTY()
	int32 NextParticleID = 0;

	/** 서브스텝 시간 누적 */
	float AccumulatedTime = 0.0f;

	/** 시뮬레이션 활성화 */
	bool bSimulationEnabled = true;

	/** Independent 모드 플래그 */
	bool bIndependentSimulation = false;

	/** 초기화 여부 */
	bool bIsInitialized = false;

	/** 파티클별 마지막 이벤트 시간 (쿨다운용) */
	TMap<int32, float> ParticleLastEventTime;

public:
	/** Get particle last event time map (for cooldown tracking) */
	TMap<int32, float>& GetParticleLastEventTimeMap() { return ParticleLastEventTime; }

	//========================================
	// IKawaiiFluidDataProvider Interface (나머지 메서드)
	//========================================

	/** Get particle radius (simulation actual radius) - IKawaiiFluidDataProvider */
	virtual float GetParticleRadius() const override;

	/** Data validity check - IKawaiiFluidDataProvider */
	virtual bool IsDataValid() const override { return Particles.Num() > 0; }

	/** Get debug name - IKawaiiFluidDataProvider */
	virtual FString GetDebugName() const override;

	//========================================
	// GPU Buffer Access (Phase 2) - IKawaiiFluidDataProvider
	//========================================

	/** Check if GPU simulation is active */
	virtual bool IsGPUSimulationActive() const override;

	/** Get GPU particle count */
	virtual int32 GetGPUParticleCount() const override;

	/** Get GPU simulator instance */
	virtual FGPUFluidSimulator* GetGPUSimulator() const override { return CachedGPUSimulator; }

	/** Set GPU simulator reference (called by Context when GPU mode is active) */
	void SetGPUSimulator(FGPUFluidSimulator* InSimulator) { CachedGPUSimulator = InSimulator; }

	/** Set GPU simulation active flag */
	void SetGPUSimulationActive(bool bActive) { bGPUSimulationActive = bActive; }

private:
	/** Cached GPU simulator pointer (owned by SimulationContext) */
	FGPUFluidSimulator* CachedGPUSimulator = nullptr;

	/** GPU simulation active flag */
	bool bGPUSimulationActive = false;
};
