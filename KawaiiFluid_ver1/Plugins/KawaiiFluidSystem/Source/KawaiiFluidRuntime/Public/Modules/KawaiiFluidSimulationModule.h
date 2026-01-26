// Copyright 2026 Team_Bruteforce. All Rights Reserved.

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
class UKawaiiFluidVolumeComponent;
class AKawaiiFluidVolume;

/**
 * Fluid simulation data module (UObject-based)
 *
 * Owns data required for simulation and provides Blueprint API.
 * Actual simulation logic is handled by UKawaiiFluidSimulationContext,
 * orchestration is managed by UKawaiiFluidSimulatorSubsystem.
 *
 * Responsibilities:
 * - Owns particle array
 * - Owns SpatialHash (for Independent mode)
 * - Manages collider/interaction component references
 * - Manages Preset reference
 * - Accumulates external forces
 * - Particle spawn/despawn API
 *
 * Usage:
 * - Included as Instanced in UKawaiiFluidComponent
 * - Blueprint functions directly callable
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
	// Initialization / Cleanup
	//========================================

	/** Initialize module */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	virtual void Initialize(UKawaiiFluidPresetDataAsset* InPreset);

	/** Cleanup module */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void Shutdown();

	/** Check initialization state */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	bool IsInitialized() const { return bIsInitialized; }

	//========================================
	// Preset / Parameters
	//========================================

	/** Set preset */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SetPreset(UKawaiiFluidPresetDataAsset* InPreset);

	/** Get preset */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	UKawaiiFluidPresetDataAsset* GetPreset() const { return Preset; }

	/** Build simulation parameters */
	virtual FKawaiiFluidSimulationParams BuildSimulationParams() const;

	//========================================
	// Fluid Identification (for collision filtering)
	//========================================

	/**
	 * Fluid type (Water, Lava, Slime, etc.)
	 * Used to identify which fluid in collision events
	 * This type is passed in FluidInteractionComponent's OnBoneParticleCollision
	 * Can branch in BP using Switch on EFluidType
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Identification",
	          meta = (ToolTip = "Fluid type.\nUsed to distinguish which fluid collided in collision events.\nCan branch in BP using Switch on EFluidType."))
	EFluidType FluidType = EFluidType::None;

	/** Get fluid type */
	UFUNCTION(BlueprintPure, Category = "Fluid|Identification")
	EFluidType GetFluidType() const { return FluidType; }

	/** Set fluid type */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Identification")
	void SetFluidType(EFluidType InFluidType) { FluidType = InFluidType; }

	//========================================
	// Simulation Volume (Unified Volume System)
	//========================================

	/**
	 * Target Simulation Volume Actor (for sharing simulation space between multiple fluid components)
	 *
	 * When set, this module will use the external Volume's bounds for simulation.
	 * Multiple modules sharing the same Volume can interact with each other.
	 *
	 * When nullptr, the module uses its own internal volume settings configured below.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume", meta = (DisplayName = "Target Volume (External)"))
	TObjectPtr<AKawaiiFluidVolume> TargetSimulationVolume = nullptr;

	/**
	 * Use uniform (cube) size for simulation volume
	 * When checked, enter a single size value. When unchecked, enter separate X/Y/Z values.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides, DisplayName = "Uniform Size"))
	bool bUniformSize = true;

	/**
	 * Simulation volume size (cm) - cube dimensions when Uniform Size is checked
	 * Particles are confined within this box. Enter the full box size (not half).
	 *
	 * Example: 400 cm means a 400×400×400 cm cube.
	 * Default: 2560 cm (Medium Z-Order preset with CellSize=20)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr && bUniformSize", EditConditionHides,
			DisplayName = "Size", ClampMin = "10.0", ClampMax = "5120.0"))
	float UniformVolumeSize = 2560.0f;

	/**
	 * Simulation volume size (cm) - separate X/Y/Z dimensions
	 * Particles are confined within this box. Enter the full box size (not half).
	 *
	 * Example: (400, 300, 200) means a 400×300×200 cm box.
	 * Default: 2560 cm per axis (Medium Z-Order preset with CellSize=20)
	 *
	 * Note: Values exceeding the system maximum will be automatically clamped.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr && bUniformSize == false", EditConditionHides,
			DisplayName = "Size"))
	FVector VolumeSize = FVector(2560.0f, 2560.0f, 2560.0f);

	/**
	 * Simulation volume rotation
	 * Default (0,0,0) = axis-aligned box. Rotating creates an oriented box.
	 *
	 * Note: Large rotations may reduce the effective volume size due to internal constraints.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides, DisplayName = "Rotation"))
	FRotator VolumeRotation = FRotator::ZeroRotator;

	/**
	 * Grid resolution preset (auto-selected based on volume size)
	 * Read-only - the system automatically selects the optimal preset.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced",
		meta = (DisplayName = "Internal Grid Preset (Auto)"))
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;

	/** Internal cell size (auto-derived from fluid preset) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced",
		meta = (DisplayName = "Cell Size (Auto)"))
	float CellSize = 20.0f;

	/** Grid bits per axis */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	int32 GridAxisBits = 7;

	/** Grid resolution per axis */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	int32 GridResolution = 128;

	/** Total cell count */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	int32 MaxCells = 2097152;

	/** Internal bounds extent */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	float BoundsExtent = 2560.0f;

	/** World-space minimum bounds */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	FVector WorldBoundsMin = FVector(-1280.0f, -1280.0f, -1280.0f);

	/** World-space maximum bounds */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	FVector WorldBoundsMax = FVector(1280.0f, 1280.0f, 1280.0f);

	/** Get the target simulation volume actor (can be nullptr) */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	AKawaiiFluidVolume* GetTargetSimulationVolume() const { return TargetSimulationVolume; }

	/** Set the target simulation volume at runtime */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Simulation Volume")
	void SetTargetSimulationVolume(AKawaiiFluidVolume* NewSimulationVolume);

	/** Get the effective volume size (full size, cm)
	 * Returns UniformVolumeSize as FVector if bUniformSize is true, otherwise VolumeSize
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	FVector GetEffectiveVolumeSize() const
	{
		return bUniformSize ? FVector(UniformVolumeSize) : VolumeSize;
	}

	/** Get the volume half-extent (for internal collision/rendering use)
	 * Returns GetEffectiveVolumeSize() * 0.5
	 */
	FVector GetVolumeHalfExtent() const
	{
		return GetEffectiveVolumeSize() * 0.5f;
	}

	/** Recalculate internal volume bounds (call after changing size or owner location)
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

	/** 파티클 수 - IKawaiiFluidDataProvider::GetParticleCount() */
	/** GPU mode: returns GPU particle count, CPU mode: returns Particles.Num() */
	UFUNCTION(BlueprintPure, Category = "Fluid")
	virtual int32 GetParticleCount() const override
	{
		if (bGPUSimulationActive)
		{
			if (TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin())
			{
				// Include both existing GPU particles AND pending spawn requests
				return GPUSim->GetParticleCount() + GPUSim->GetPendingSpawnCount();
			}
		}
		return Particles.Num();
	}

	/**
	 * Get particle count for a specific source (component)
	 * Uses GPU source counters for per-component tracking (2-3 frame latency)
	 * @param SourceID - Source component ID (0 to MaxSourceCount-1)
	 * @return Particle count for the specified source, 0 if invalid or not found
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid")
	int32 GetParticleCountForSource(int32 SourceID) const;

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
	// Hexagonal Grid 스폰 함수 (안정적인 초기 상태)
	//========================================

	/** Hexagonal Close Packing으로 박스 내 파티클 스폰
	 * Cubic grid보다 안정적인 초기 밀도 분포 제공
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
	int32 SpawnParticlesBoxHexagonal(FVector Center, FVector Extent, float Spacing,
	                                  bool bJitter = true, float JitterAmount = 0.2f,
	                                  FVector Velocity = FVector::ZeroVector,
	                                  FRotator Rotation = FRotator::ZeroRotator);

	/** Hexagonal Close Packing으로 구체 내 파티클 스폰
	 * @param Center 구체 중심
	 * @param Radius 구체 반경
	 * @param Spacing 파티클 간격
	 * @param bJitter 랜덤 오프셋 적용 여부
	 * @param JitterAmount 랜덤 오프셋 비율 (0~0.5)
	 * @param Velocity 초기 속도
	 * @param Rotation 로컬→월드 회전
	 * @return 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesSphereHexagonal(FVector Center, float Radius, float Spacing,
	                                     bool bJitter = true, float JitterAmount = 0.2f,
	                                     FVector Velocity = FVector::ZeroVector,
	                                     FRotator Rotation = FRotator::ZeroRotator);

	/** Hexagonal Close Packing으로 원기둥 내 파티클 스폰
	 * @param Center 원기둥 중심
	 * @param Radius 원기둥 반경
	 * @param HalfHeight 원기둥 반높이
	 * @param Spacing 파티클 간격
	 * @param bJitter 랜덤 오프셋 적용 여부
	 * @param JitterAmount 랜덤 오프셋 비율 (0~0.5)
	 * @param Velocity 초기 속도
	 * @param Rotation 로컬→월드 회전
	 * @return 스폰된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesCylinderHexagonal(FVector Center, float Radius, float HalfHeight, float Spacing,
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

	/** C++ only: Batch version that collects requests without sending. Caller must send batch manually. */
	int32 SpawnParticleDirectionalHexLayerBatch(FVector Position, FVector Direction, float Speed,
	                                             float Radius, float Spacing, float Jitter,
	                                             TArray<FGPUSpawnRequest>& OutBatch);

	/** 모든 파티클 제거 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ClearAllParticles();

	/** 가장 오래된 파티클 N개 제거 (ParticleID가 낮은 순서)
	 * MaxParticle에 도달했을 때 새 파티클을 위한 공간 확보용
	 * GPU 모드: Readback 데이터 기반으로 가장 낮은 ID를 찾아 삭제 요청
	 * @param Count 제거할 파티클 수
	 * @return 실제 제거 요청된 파티클 수 (Readback 실패 시 0)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 RemoveOldestParticles(int32 Count);

	/** 특정 Source의 가장 오래된 파티클 N개 제거
	 * Emitter 등 외부 컴포넌트가 자신의 파티클만 제거할 때 사용
	 * @param SourceID 대상 Source ID (0~63)
	 * @param Count 제거할 파티클 수
	 * @return 실제 제거 요청된 파티클 수
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 RemoveOldestParticlesForSource(int32 SourceID, int32 Count);

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
	bool IsIndependentSimulation() const { return bIndependentSimulation; }

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
	// Volume Visualization
	//========================================

	/** Bounds wireframe color (green box showing simulation bounds in editor) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides, DisplayName = "Wireframe Color"))
	FColor VolumeWireframeColor = FColor::Green;

	/** Show internal grid space wireframe (for debugging spatial partitioning) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume|Advanced",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides, DisplayName = "Show Internal Grid Wireframe"))
	bool bShowZOrderSpaceWireframe = false;

	/** Internal grid wireframe color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume|Advanced",
		meta = (EditCondition = "TargetSimulationVolume == nullptr && bShowZOrderSpaceWireframe", EditConditionHides, DisplayName = "Grid Wireframe Color"))
	FColor ZOrderSpaceWireframeColor = FColor::Red;

	//========================================
	// Simulation Bounds API
	//========================================

	/** Set simulation bounds at runtime
	 * @param Size Full size of the simulation volume (cm)
	 * @param Rotation World-space rotation of the volume
	 * @param Bounce Wall bounce coefficient (0-1)
	 * @param Friction Wall friction coefficient (0-1)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Simulation Volume")
	void SetSimulationVolume(const FVector& Size, const FRotator& Rotation, float Bounce, float Friction);

	/** Resolve volume boundary collisions - keeps particles inside */
	void ResolveVolumeBoundaryCollisions();

	//========================================
	// Legacy API (Deprecated)
	//========================================

	/** @deprecated Use VolumeSize/UniformVolumeSize. Bounce/Friction are now from Preset (Restitution/Friction) */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Containment", meta = (DeprecatedFunction, DeprecationMessage = "Use SetSimulationVolume instead"))
	void SetContainment(bool bEnabled, const FVector& Center, const FVector& Extent,
	                    const FQuat& Rotation, float Restitution, float Friction);

	/** @deprecated Use ResolveVolumeBoundaryCollisions instead */
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

private:
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

	/** 누적 외력 */
	FVector AccumulatedExternalForce = FVector::ZeroVector;

	/** 서브스텝 시간 누적 */
	float AccumulatedTime = 0.0f;

	/** Volume center (world space, set dynamically from Component location) */
	FVector VolumeCenter = FVector::ZeroVector;

	/** Volume rotation as quaternion (computed from VolumeRotation) */
	FQuat VolumeRotationQuat = FQuat::Identity;

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
		if (bGPUSimulationActive)
		{
			if (TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin())
			{
				return GPUSim->GetParticleCount() > 0 || GPUSim->GetPendingSpawnCount() > 0;
			}
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
	virtual FGPUFluidSimulator* GetGPUSimulator() const override { return WeakGPUSimulator.Pin().Get(); }

	/** Set GPU simulator reference (called by Context when GPU mode is active) */
	void SetGPUSimulator(const TSharedPtr<FGPUFluidSimulator>& InSimulator) { WeakGPUSimulator = InSimulator; }

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
	UKawaiiFluidVolumeComponent* GetTargetVolumeComponent() const;

	/**
	 * Get the internally owned volume component (always valid after initialization)
	 * This is used when no external TargetSimulationVolume is set
	 */
	UKawaiiFluidVolumeComponent* GetOwnedVolumeComponent() const { return OwnedVolumeComponent; }

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
	/** Weak reference to GPU simulator (owned by SimulationContext via TSharedPtr) */
	TWeakPtr<FGPUFluidSimulator> WeakGPUSimulator;

	/** GPU simulation active flag */
	bool bGPUSimulationActive = false;

	/** Cached simulation context pointer (owned by SimulatorSubsystem) */
	UKawaiiFluidSimulationContext* CachedSimulationContext = nullptr;

	/**
	 * Internally owned volume component for Z-Order space bounds
	 * Created automatically during Initialize(), used when TargetSimulationVolume is nullptr
	 */
	UPROPERTY(Transient)
	TObjectPtr<UKawaiiFluidVolumeComponent> OwnedVolumeComponent = nullptr;

	/**
	 * Previously registered volume component (for editor unregistration tracking)
	 * Used to properly unregister when TargetSimulationVolume changes in editor
	 */
	TWeakObjectPtr<UKawaiiFluidVolumeComponent> PreviousRegisteredVolume = nullptr;

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
