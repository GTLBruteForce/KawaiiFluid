// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "GPU/GPUFluidSimulator.h"
#include "Components/FluidInteractionComponent.h"
#include "KawaiiFluidSimulationModule.generated.h"

/** Collision event callback type */
DECLARE_DELEGATE_OneParam(FOnModuleCollisionEvent, const FKawaiiFluidCollisionEvent&);

class FSpatialHash;
class UKawaiiFluidPresetDataAsset;
class UFluidCollider;
class UFluidInteractionComponent;
class UKawaiiFluidSimulationContext;
class UKawaiiFluidSimulationVolumeComponent;
class AKawaiiFluidSimulationVolume;

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

	// UObject interface
	virtual void PostInitProperties() override;
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;

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
	// Fluid Identification (for collision filtering)
	//========================================

	/**
	 * 유체 타입 (Water, Lava, Slime 등)
	 * 충돌 이벤트에서 어떤 유체인지 식별하는 데 사용
	 * FluidInteractionComponent의 OnBoneParticleCollision에서 이 타입이 전달됨
	 * BP에서 Switch on EFluidType으로 분기 가능
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Identification",
	          meta = (ToolTip = "유체 타입.\n충돌 이벤트에서 어떤 유체와 충돌했는지 구분하는 데 사용됩니다.\nBP에서 Switch on EFluidType으로 분기할 수 있습니다."))
	EFluidType FluidType = EFluidType::None;

	/** 유체 타입 반환 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Identification")
	EFluidType GetFluidType() const { return FluidType; }

	/** 유체 타입 설정 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Identification")
	void SetFluidType(EFluidType InFluidType) { FluidType = InFluidType; }

	//========================================
	// Simulation Volume (Z-Order Space)
	//========================================

	/**
	 * Target Simulation Volume Actor for Z-Order space bounds
	 *
	 * When set, this module will use the external Volume's Z-Order space for simulation.
	 * Multiple modules sharing the same Volume can interact with each other.
	 *
	 * When nullptr, the module uses its own internal volume settings
	 * configured below (GridResolutionPreset).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume", meta = (DisplayName = "Target Volume (External)"))
	TObjectPtr<AKawaiiFluidSimulationVolume> TargetSimulationVolume = nullptr;

	/**
	 * Grid resolution preset for internal Z-Order sorting (when no External Volume)
	 * Controls the simulation bounds size and memory usage.
	 * - Small (64³): Compact bounds, fastest
	 * - Medium (128³): Balanced, recommended for 100k particles
	 * - Large (256³): Large bounds, more memory
	 *
	 * Ignored when TargetSimulationVolume is set (uses external volume's preset).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides))
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;

	/**
	 * Cell size for Z-Order grid (read-only, auto-derived).
	 *
	 * This value is automatically determined and cannot be set directly:
	 * - Internal Volume Mode: Derived from Preset's SmoothingRadius (CellSize = SmoothingRadius)
	 * - External Volume Mode: Inherited from TargetSimulationVolume's CellSize
	 *
	 * For optimal SPH neighbor search, CellSize should equal SmoothingRadius.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume",
		meta = (DisplayName = "Cell Size (Auto)"))
	float CellSize = 20.0f;

	/**
	 * Grid resolution bits per axis (derived from GridResolutionPreset or external volume)
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Info")
	int32 GridAxisBits = 7;

	/**
	 * Grid resolution per axis (2^GridAxisBits)
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Info")
	int32 GridResolution = 128;

	/**
	 * Total number of cells (GridResolution³)
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Info")
	int32 MaxCells = 2097152;

	/**
	 * Simulation bounds extent (GridResolution * CellSize)
	 * This is the total size of the Z-Order space per axis.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Info")
	float BoundsExtent = 2560.0f;

	/**
	 * World-space minimum bounds
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Info")
	FVector WorldBoundsMin = FVector(-1280.0f, -1280.0f, -1280.0f);

	/**
	 * World-space maximum bounds
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Info")
	FVector WorldBoundsMax = FVector(1280.0f, 1280.0f, 1280.0f);

	/** Get the target simulation volume actor (can be nullptr) */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	AKawaiiFluidSimulationVolume* GetTargetSimulationVolume() const { return TargetSimulationVolume; }

	/** Set the target simulation volume at runtime */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Simulation Volume")
	void SetTargetSimulationVolume(AKawaiiFluidSimulationVolume* NewSimulationVolume);

	/** Recalculate internal volume bounds (call after changing CellSize or owner location)
	 * Called automatically when properties change in editor
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Simulation Volume")
	void RecalculateVolumeBounds();

	/** Update volume info display from current effective source
	 * When TargetSimulationVolume is set, reads from external volume
	 * Otherwise uses internal CellSize to calculate
	 */
	void UpdateVolumeInfoDisplay();

	/**
	 * Called when Preset reference is changed externally (e.g., from owning Component)
	 * Handles delegate rebinding and CellSize synchronization
	 */
	void OnPresetChangedExternal(UKawaiiFluidPresetDataAsset* NewPreset);

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
	/** GPU mode: returns GPU particle count, CPU mode: returns Particles.Num() */
	UFUNCTION(BlueprintPure, Category = "Fluid")
	virtual int32 GetParticleCount() const override
	{
		if (bGPUSimulationActive && CachedGPUSimulator)
		{
			// Include both existing GPU particles AND pending spawn requests
			return CachedGPUSimulator->GetParticleCount() + CachedGPUSimulator->GetPendingSpawnCount();
		}
		return Particles.Num();
	}

	//========================================
	// 파티클 생성/삭제
	//========================================

	/** 단일 파티클 스폰 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticle(FVector Position, FVector Velocity = FVector::ZeroVector);

	/** 여러 파티클 랜덤 스폰 (Point 모드용, 기존 호환) */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void SpawnParticles(FVector Location, int32 Count, float SpawnRadius);

	/** 구형 격자 분포로 파티클 스폰 (Sphere 모드용)
	 * @param Center 구체 중심
	 * @param Radius 구체 반경
	 * @param Spacing 파티클 간격
	 * @param bJitter 랜덤 오프셋 적용 여부
	 * @param JitterAmount 랜덤 오프셋 비율 (0~0.5)
	 * @param Velocity 초기 속도
	 * @param Rotation 로컬→월드 회전 (Shape는 구형이므로 Velocity에만 적용)
	 * @return 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesSphere(FVector Center, float Radius, float Spacing,
	                           bool bJitter = true, float JitterAmount = 0.2f,
	                           FVector Velocity = FVector::ZeroVector,
	                           FRotator Rotation = FRotator::ZeroRotator);

	/** 박스 격자 분포로 파티클 스폰 (Box 모드용)
	 * @param Center 박스 중심
	 * @param Extent 박스 Half Extent (X, Y, Z)
	 * @param Spacing 파티클 간격
	 * @param bJitter 랜덤 오프셋 적용 여부
	 * @param JitterAmount 랜덤 오프셋 비율 (0~0.5)
	 * @param Velocity 초기 속도
	 * @param Rotation 로컬→월드 회전
	 * @return 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesBox(FVector Center, FVector Extent, float Spacing,
	                        bool bJitter = true, float JitterAmount = 0.2f,
	                        FVector Velocity = FVector::ZeroVector,
	                        FRotator Rotation = FRotator::ZeroRotator);

	/** 원기둥 격자 분포로 파티클 스폰 (Cylinder 모드용)
	 * @param Center 원기둥 중심
	 * @param Radius 원기둥 반경
	 * @param HalfHeight 원기둥 반높이 (Z축 기준)
	 * @param Spacing 파티클 간격
	 * @param bJitter 랜덤 오프셋 적용 여부
	 * @param JitterAmount 랜덤 오프셋 비율 (0~0.5)
	 * @param Velocity 초기 속도
	 * @param Rotation 로컬→월드 회전
	 * @return 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesCylinder(FVector Center, float Radius, float HalfHeight, float Spacing,
	                             bool bJitter = true, float JitterAmount = 0.2f,
	                             FVector Velocity = FVector::ZeroVector,
	                             FRotator Rotation = FRotator::ZeroRotator);

	//========================================
	// 명시적 개수 지정 스폰 함수
	//========================================

	/** 구형에 명시적 개수로 파티클 스폰
	 * @param Center 구체 중심
	 * @param Radius 구체 반경
	 * @param Count 스폰할 파티클 개수
	 * @param bJitter 랜덤 오프셋 적용 여부
	 * @param JitterAmount 랜덤 오프셋 비율 (0~0.5)
	 * @param Velocity 초기 속도
	 * @param Rotation 로컬→월드 회전 (Shape는 구형이므로 Velocity에만 적용)
	 * @return 실제 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesSphereByCount(FVector Center, float Radius, int32 Count,
	                                  bool bJitter = true, float JitterAmount = 0.2f,
	                                  FVector Velocity = FVector::ZeroVector,
	                                  FRotator Rotation = FRotator::ZeroRotator);

	/** 박스에 명시적 개수로 파티클 스폰
	 * @param Center 박스 중심
	 * @param Extent 박스 Half Extent (X, Y, Z)
	 * @param Count 스폰할 파티클 개수
	 * @param bJitter 랜덤 오프셋 적용 여부
	 * @param JitterAmount 랜덤 오프셋 비율 (0~0.5)
	 * @param Velocity 초기 속도
	 * @param Rotation 로컬→월드 회전
	 * @return 실제 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesBoxByCount(FVector Center, FVector Extent, int32 Count,
	                               bool bJitter = true, float JitterAmount = 0.2f,
	                               FVector Velocity = FVector::ZeroVector,
	                               FRotator Rotation = FRotator::ZeroRotator);

	/** 원기둥에 명시적 개수로 파티클 스폰
	 * @param Center 원기둥 중심
	 * @param Radius 원기둥 반경
	 * @param HalfHeight 원기둥 반높이 (Z축 기준)
	 * @param Count 스폰할 파티클 개수
	 * @param bJitter 랜덤 오프셋 적용 여부
	 * @param JitterAmount 랜덤 오프셋 비율 (0~0.5)
	 * @param Velocity 초기 속도
	 * @param Rotation 로컬→월드 회전
	 * @return 실제 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesCylinderByCount(FVector Center, float Radius, float HalfHeight, int32 Count,
	                                    bool bJitter = true, float JitterAmount = 0.2f,
	                                    FVector Velocity = FVector::ZeroVector,
	                                    FRotator Rotation = FRotator::ZeroRotator);

	/** 방향성 파티클 스폰 (Spout/Spray 모드용)
	 * @param Position 스폰 위치
	 * @param Direction 방출 방향 (정규화됨)
	 * @param Speed 초기 속도 크기
	 * @param Radius 스트림 반경 (분산 범위)
	 * @param ConeAngle 분사각 (도, 0이면 직선)
	 * @return 스폰된 파티클 ID
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticleDirectional(FVector Position, FVector Direction, float Speed,
	                               float Radius = 0.0f, float ConeAngle = 0.0f);

	/** Hexagonal Packing으로 원형 단면 레이어 스폰 (Stream 모드용)
	 * @param Position 레이어 중심 위치
	 * @param Direction 방출 방향 (정규화됨)
	 * @param Speed 초기 속도 크기
	 * @param Radius 스트림 반경
	 * @param Spacing 파티클 간격 (0이면 SmoothingRadius * 0.5 자동 계산)
	 * @param Jitter 위치 랜덤 오프셋 비율 (0~0.5, 0=완벽한 격자, 0.5=최대 자연스러움)
	 * @return 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticleDirectionalHexLayer(FVector Position, FVector Direction, float Speed,
	                                        float Radius, float Spacing = 0.0f, float Jitter = 0.15f);

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
	const TArray<TObjectPtr<UFluidCollider>>& GetColliders() const { return Colliders; }
	
	//========================================
	// SpatialHash (Independent 모드용)
	//========================================

	/** SpatialHash 가져오기 */
	FSpatialHash* GetSpatialHash() const { return SpatialHash.Get(); }

	/** SpatialHash 초기화 */
	void InitializeSpatialHash(float InCellSize);

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

	/** Enable containment volume - particles are confined within this box and cannot escape */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision")
	bool bEnableContainment = false;

	/** Containment volume half extent (cm) - centered on component location */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision",
	          meta = (EditCondition = "bEnableContainment", EditConditionHides))
	FVector ContainmentExtent = FVector(100.0f, 100.0f, 100.0f);

	/** Containment wall restitution (bounciness when particles hit the wall) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision",
	          meta = (EditCondition = "bEnableContainment", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float ContainmentRestitution = 0.3f;

	/** Containment wall friction */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision",
	          meta = (EditCondition = "bEnableContainment", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float ContainmentFriction = 0.1f;

	/** Show containment volume wireframe in editor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision",
	          meta = (EditCondition = "bEnableContainment", EditConditionHides))
	bool bShowContainmentWireframe = true;

	/** Containment wireframe color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision",
	          meta = (EditCondition = "bEnableContainment && bShowContainmentWireframe", EditConditionHides))
	FColor ContainmentWireframeColor = FColor::Green;

	//========================================
	// Containment Volume API
	//========================================

	/** Set containment volume parameters - confines particles within a box
	 * @param bEnabled Enable/disable containment
	 * @param Center World-space center of the containment volume
	 * @param Extent Half-extent of the containment volume (cm)
	 * @param Rotation World-space rotation of the containment volume
	 * @param Restitution Bounciness when particles hit walls (0-1)
	 * @param Friction Friction coefficient on walls (0-1)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Containment")
	void SetContainment(bool bEnabled, const FVector& Center, const FVector& Extent,
	                    const FQuat& Rotation, float Restitution, float Friction);

	/** Resolve containment collisions - keeps particles inside the volume */
	void ResolveContainmentCollisions();

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

	/** 충돌 피드백 처리 (GPU + CPU 통합)
	 * Subsystem에서 시뮬레이션 후 호출. GPU 버퍼 + CPU 버퍼 모두 처리.
	 * @param OwnerIDToIC - Subsystem에서 빌드한 OwnerID→IC 맵 (O(1) 조회용)
	 * @param CPUFeedbackBuffer - Subsystem의 CPU 충돌 피드백 버퍼 (SourceID로 필터링)
	 */
	void ProcessCollisionFeedback(
		const TMap<int32, UFluidInteractionComponent*>& OwnerIDToIC,
		const TArray<FKawaiiFluidCollisionEvent>& CPUFeedbackBuffer);

	//========================================
	// Preset (내부 캐시 - Component에서 설정)
	//========================================

	/** Cached preset reference (set by owning Component via Initialize/SetPreset)
	 * Note: Component owns the Preset, Module just caches reference for simulation
	 */
	UPROPERTY()
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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fluid Simulation|Override", meta = (AllowPrivateAccess = "true", EditCondition = "bOverride_AdhesionStrength", ClampMin = "0.0", ClampMax = "200.0"))
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
	TArray<TObjectPtr<UFluidCollider>> Colliders;

	/** Override 적용된 런타임 Preset (Transient) */
	UPROPERTY(Transient)
	TObjectPtr<UKawaiiFluidPresetDataAsset> RuntimePreset = nullptr;

	/** RuntimePreset 갱신 필요 플래그 */
	bool bRuntimePresetDirty = true;

	/** 누적 외력 */
	FVector AccumulatedExternalForce = FVector::ZeroVector;

	/** 다음 파티클 ID - 에디터 직렬화 지원 */
	UPROPERTY()
	int32 NextParticleID = 0;

	/** 서브스텝 시간 누적 */
	float AccumulatedTime = 0.0f;

	/** Containment center (world space, set dynamically from Component location) */
	FVector ContainmentCenter = FVector::ZeroVector;

	/** Containment rotation (world space, set dynamically from Component rotation) */
	FQuat ContainmentRotation = FQuat::Identity;

	/** 시뮬레이션 활성화 */
	bool bSimulationEnabled = true;

	/** Independent 모드 플래그 */
	bool bIndependentSimulation = true;

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
	/** GPU mode: checks GPU particle count, CPU mode: checks Particles.Num() */
	virtual bool IsDataValid() const override
	{
		if (bGPUSimulationActive && CachedGPUSimulator)
		{
			return CachedGPUSimulator->GetParticleCount() > 0 || CachedGPUSimulator->GetPendingSpawnCount() > 0;
		}
		return Particles.Num() > 0;
	}

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

	//========================================
	// GPU ↔ CPU Particle Sync (PIE/Serialization)
	//========================================

	/**
	 * GPU 파티클을 CPU Particles 배열로 동기화
	 * 저장(PreSave) 및 PIE 전환(PreBeginPIE) 시 호출됨
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SyncGPUParticlesToCPU();

	/**
	 * CPU Particles 배열을 GPU로 업로드
	 * 로드(PostLoad) 및 PIE 시작(BeginPlay) 시 호출됨
	 */
	void UploadCPUParticlesToGPU();

	//========================================
	// Simulation Context Reference
	//========================================

	/** Get the simulation context associated with this module
	 * Context is owned by SimulatorSubsystem and shared by all modules with same preset
	 * Returns nullptr if module is not registered with subsystem
	 */
	UKawaiiFluidSimulationContext* GetSimulationContext() const { return CachedSimulationContext; }

	/** Set the simulation context reference (called by SimulatorSubsystem on registration) */
	void SetSimulationContext(UKawaiiFluidSimulationContext* InContext) { CachedSimulationContext = InContext; }

	//========================================
	// Volume Component Access (Internal)
	//========================================

	/**
	 * Get the effective volume component for Z-Order space bounds
	 * Returns external TargetSimulationVolume's component if set,
	 * otherwise returns internal OwnedVolumeComponent
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	UKawaiiFluidSimulationVolumeComponent* GetTargetVolumeComponent() const;

	/**
	 * Get the internally owned volume component (always valid after initialization)
	 * This is used when no external TargetSimulationVolume is set
	 */
	UKawaiiFluidSimulationVolumeComponent* GetOwnedVolumeComponent() const { return OwnedVolumeComponent; }

	/**
	 * Check if using external volume (TargetSimulationVolume is set)
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	bool IsUsingExternalVolume() const { return TargetSimulationVolume != nullptr; }

	//========================================
	// Source Identification
	//========================================

	/** Set source ID for spawned particles (Component's unique ID) */
	void SetSourceID(int32 InSourceID);

	/** Get cached source ID */
	int32 GetSourceID() const { return CachedSourceID; }

private:
	/** Cached GPU simulator pointer (owned by SimulationContext) */
	FGPUFluidSimulator* CachedGPUSimulator = nullptr;

	/** GPU simulation active flag */
	bool bGPUSimulationActive = false;

	/** Cached simulation context pointer (owned by SimulatorSubsystem) */
	UKawaiiFluidSimulationContext* CachedSimulationContext = nullptr;

	/**
	 * Internally owned volume component for Z-Order space bounds
	 * Created automatically during Initialize(), used when TargetSimulationVolume is nullptr
	 */
	UPROPERTY(Transient)
	TObjectPtr<UKawaiiFluidSimulationVolumeComponent> OwnedVolumeComponent = nullptr;

	/**
	 * Previously registered volume component (for editor unregistration tracking)
	 * Used to properly unregister when TargetSimulationVolume changes in editor
	 */
	TWeakObjectPtr<UKawaiiFluidSimulationVolumeComponent> PreviousRegisteredVolume = nullptr;

	/** Called when the TargetSimulationVolume actor is destroyed */
	UFUNCTION()
	void OnTargetVolumeDestroyed(AActor* DestroyedActor);

	/** Bind/Unbind to TargetSimulationVolume's OnDestroyed delegate */
	void BindToVolumeDestroyedEvent();
	void UnbindFromVolumeDestroyedEvent();

	/** Whether we're currently bound to the volume's OnDestroyed event */
	bool bBoundToVolumeDestroyed = false;

#if WITH_EDITOR
	/** Callback when Preset's properties change (SmoothingRadius, etc.) */
	void OnPresetPropertyChanged(UKawaiiFluidPresetDataAsset* ChangedPreset);

	/** Bind/Unbind to Preset's OnPropertyChanged delegate */
	void BindToPresetPropertyChanged();
	void UnbindFromPresetPropertyChanged();

	/** Delegate handle for preset property changes */
	FDelegateHandle PresetPropertyChangedHandle;

	/** Callback when objects are replaced (e.g., asset reload) */
	void OnObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap);

	/** Bind/Unbind to FCoreUObjectDelegates::OnObjectsReplaced */
	void BindToObjectsReplaced();
	void UnbindFromObjectsReplaced();

	/** Delegate handle for objects replaced event */
	FDelegateHandle ObjectsReplacedHandle;

	/** PIE 시작 전 GPU 파티클을 CPU로 동기화 */
	void OnPreBeginPIE(bool bIsSimulating);

	/** Delegate handle for PreBeginPIE */
	FDelegateHandle PreBeginPIEHandle;
#endif

	/** Cached source ID for spawned particles (Component's unique ID, -1 = invalid) */
	int32 CachedSourceID = -1;
};
