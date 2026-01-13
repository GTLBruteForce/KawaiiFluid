// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FluidInteractionComponent.generated.h"

class UKawaiiFluidSimulatorSubsystem;
class UFluidCollider;
class UMeshFluidCollider;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFluidAttached, int32, ParticleCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnFluidDetached);

/**
 * Collider와 충돌 시작 (파티클이 Collider 안에 들어옴)
 * @param CollidingCount 충돌 중인 파티클 수 (붙은 것 + 겹친 것)
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFluidColliding, int32, CollidingCount);

/**
 * Collider 충돌 종료 (모든 파티클이 Collider에서 벗어남)
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnFluidStopColliding);

//========================================
// GPU Collision Feedback Delegates (Particle -> Player Interaction)
//========================================

/**
 * 특정 유체 영역에 진입했을 때 발생
 * @param FluidTag 유체 태그 (예: "Water", "Lava")
 * @param ParticleCount 충돌 중인 파티클 수
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnFluidEnter, FName, FluidTag, int32, ParticleCount);

/**
 * 특정 유체 영역에서 벗어났을 때 발생
 * @param FluidTag 유체 태그 (예: "Water", "Lava")
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFluidExit, FName, FluidTag);

/**
 * 유체로부터 받는 힘이 업데이트될 때 발생 (매 틱)
 * @param Force 유체로부터 받는 힘 벡터 (cm/s²)
 * @param Pressure 평균 압력 값
 * @param ContactCount 접촉 중인 파티클 수
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnFluidForceUpdate, FVector, Force, float, Pressure, int32, ContactCount);

/**
 * 유체 상호작용 컴포넌트
 * 캐릭터/오브젝트에 붙여서 유체와 상호작용
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UFluidInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFluidInteractionComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Cached subsystem reference */
	UPROPERTY(Transient)
	UKawaiiFluidSimulatorSubsystem* TargetSubsystem;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction")
	bool bCanAttachFluid;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float AdhesionMultiplier;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DragAlongStrength;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction")
	bool bAutoCreateCollider;

	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Status")
	int32 AttachedParticleCount;

	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Status")
	bool bIsWet;

	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidAttached OnFluidAttached;

	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidDetached OnFluidDetached;

	//========================================
	// Collision Detection (Collider 기반)
	//========================================

	/** Collider 기반 충돌 감지 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Collision Detection")
	bool bEnableCollisionDetection = false;

	/** 트리거를 위한 최소 파티클 수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Collision Detection", 
	          meta = (EditCondition = "bEnableCollisionDetection", ClampMin = "1"))
	int32 MinParticleCountForTrigger = 1;

	/** Collider와 충돌 중인 파티클 수 (붙은 것 + 겹친 것) */
	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Status")
	int32 CollidingParticleCount = 0;

	/** Collider 충돌 시작 이벤트 */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidColliding OnFluidColliding;

	/** Collider 충돌 종료 이벤트 */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidStopColliding OnFluidStopColliding;

	//========================================
	// Per-Polygon Collision (Phase 2)
	// GPU AABB Filtering + CPU Per-Polygon Collision
	//========================================

	/**
	 * Per-Polygon Collision 활성화
	 * 정밀한 스켈레탈 메시 충돌을 위해 GPU에서 AABB 필터링 후 CPU에서 삼각형 충돌 검사
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Polygon Collision",
	          meta = (ToolTip = "Per-Polygon Collision 활성화.\n체크 시 스켈레탈 메쉬의 실제 삼각형과 충돌 검사를 수행합니다.\nGPU AABB 필터링 → CPU 삼각형 충돌의 하이브리드 방식으로 성능 최적화."))
	bool bUsePerPolygonCollision = false;

	/**
	 * Per-Polygon Collision용 AABB 확장 (cm)
	 * 캐릭터 바운딩 박스를 이 값만큼 확장하여 후보 입자 검색
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Polygon Collision",
	          meta = (EditCondition = "bUsePerPolygonCollision", ClampMin = "0.0", ClampMax = "100.0",
	                  ToolTip = "AABB 확장 (cm).\n캐릭터 바운딩 박스를 이 값만큼 확장하여 후보 파티클을 검색합니다.\n너무 작으면 빠른 파티클이 누락될 수 있고, 너무 크면 CPU 부하 증가."))
	float PerPolygonAABBPadding = 10.0f;

	/**
	 * Per-Polygon Collision AABB 디버그 라인 표시
	 * 에디터/런타임에서 AABB를 시각적으로 확인
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Polygon Collision",
	          meta = (EditCondition = "bUsePerPolygonCollision",
	                  ToolTip = "디버그용 AABB 박스를 화면에 표시합니다.\n파티클 필터링 범위를 시각적으로 확인할 때 사용."))
	bool bDrawPerPolygonAABB = false;

	/**
	 * 충돌 감지 마진 (cm)
	 * 클수록 더 일찍/멀리서 충돌 감지. ParticleRadius와 비슷하게 설정 권장
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Polygon Collision",
	          meta = (EditCondition = "bUsePerPolygonCollision", ClampMin = "0.1", ClampMax = "20.0",
	                  ToolTip = "충돌 감지 마진 (cm). 클수록 더 일찍/멀리서 충돌 감지합니다.\n파티클이 메쉬를 뚫고 들어가면 이 값을 높여보세요.\nParticleRadius와 비슷하게 설정 권장 (3~10)"))
	float PerPolygonCollisionMargin = 3.0f;

	/**
	 * 표면 마찰 계수 (0-1)
	 * 높을수록 표면에서 느리게 흐름
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Polygon Collision",
	          meta = (EditCondition = "bUsePerPolygonCollision", ClampMin = "0.0", ClampMax = "1.0",
	                  ToolTip = "표면 마찰 계수 (0~1).\n0: 마찰 없음 (미끄러움)\n1: 최대 마찰 (표면에서 정지)\n유체가 자연스럽게 흐르려면 0.1~0.3 권장"))
	float PerPolygonFriction = 0.2f;

	/**
	 * 반발 계수 (0-1)
	 * 낮을수록 표면에 붙어서 흐름, 높을수록 튕겨나감
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Polygon Collision",
	          meta = (EditCondition = "bUsePerPolygonCollision", ClampMin = "0.0", ClampMax = "1.0",
	                  ToolTip = "반발 계수 (0~1).\n0: 반발 없음 (표면에 붙어서 흐름)\n1: 완전 탄성 (튕겨나감)\n유체가 표면을 타고 흐르려면 0.05~0.2 권장"))
	float PerPolygonRestitution = 0.1f;

	/** Per-Polygon Collision 활성화 여부 반환 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Per-Polygon Collision")
	bool IsPerPolygonCollisionEnabled() const { return bUsePerPolygonCollision; }

	/** Per-Polygon Collision용 필터 AABB 반환 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Per-Polygon Collision")
	FBox GetPerPolygonFilterAABB() const;

	//========================================
	// GPU Collision Feedback (Particle -> Player Interaction)
	// GPU에서 계산된 충돌 정보를 기반으로 힘 계산 및 이벤트 발생
	//========================================

	/**
	 * GPU 충돌 피드백 활성화
	 * 활성화 시 GPU에서 파티클-콜라이더 충돌 정보를 리드백하여 힘/이벤트 계산
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Force Feedback",
	          meta = (ToolTip = "GPU 충돌 피드백 활성화.\n활성화 시 GPU에서 파티클-콜라이더 충돌 정보를 리드백하여\n물리적 힘 계산 및 유체 이벤트(OnFluidEnter/Exit)를 발생시킵니다.\n2-3 프레임 지연이 있지만 FPS 영향은 최소화됩니다."))
	bool bEnableForceFeedback = false;

	/**
	 * 힘 스무딩 속도 (1/s)
	 * 높을수록 힘 변화가 빠르고, 낮을수록 부드럽게 변화
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback", ClampMin = "0.1", ClampMax = "50.0",
	                  ToolTip = "힘 스무딩 속도 (1/s).\n높을수록 힘 변화가 빠르고, 낮을수록 부드럽게 변화합니다.\n급격한 힘 변화를 방지하려면 5~15 권장."))
	float ForceSmoothingSpeed = 10.0f;

	/**
	 * 항력 계수 (C_d)
	 * 유체 항력 공식에서 사용되는 무차원 계수
	 * 구체: ~0.47, 캡슐/원통: ~1.0, 사람: ~1.0-1.3
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback", ClampMin = "0.1", ClampMax = "3.0",
	                  ToolTip = "항력 계수 (C_d).\n유체 항력 공식 F = ½ρCdA|v|² 에서 사용됩니다.\n구체: ~0.47, 캡슐/원통: ~1.0, 사람: ~1.0-1.3"))
	float DragCoefficient = 1.0f;

	/**
	 * 항력 → 힘 변환 배율
	 * 계산된 항력을 실제 게임 힘으로 변환할 때 적용되는 스케일
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback", ClampMin = "0.0001", ClampMax = "10.0",
	                  ToolTip = "항력 → 힘 변환 배율.\n물리적 항력 값을 게임에 맞게 스케일링합니다.\n파도가 캐릭터를 밀어내는 강도를 조절합니다."))
	float DragForceMultiplier = 0.01f;

	/**
	 * 유체 태그별 트리거를 위한 최소 파티클 수
	 * 이 수 이상의 파티클이 충돌해야 OnFluidEnter 이벤트 발생
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback", ClampMin = "1", ClampMax = "100",
	                  ToolTip = "OnFluidEnter/Exit 이벤트를 위한 최소 파티클 수.\n이 수 이상의 파티클이 충돌해야 이벤트가 발생합니다.\n노이즈 방지를 위해 5~20 권장."))
	int32 MinParticleCountForFluidEvent = 5;

	/** 현재 유체로부터 받는 힘 벡터 (스무딩 적용됨) */
	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Force Feedback")
	FVector CurrentFluidForce;

	/** 현재 접촉 중인 파티클 수 */
	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Force Feedback")
	int32 CurrentContactCount;

	/** 현재 평균 압력 값 */
	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Force Feedback")
	float CurrentAveragePressure;

	/** 유체 영역 진입 이벤트 (유체 태그별) */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidEnter OnFluidEnter;

	/** 유체 영역 이탈 이벤트 (유체 태그별) */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidExit OnFluidExit;

	/** 유체 힘 업데이트 이벤트 (매 틱, Force Feedback 활성화 시) */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidForceUpdate OnFluidForceUpdate;

	//========================================
	// Per-Bone Force Feedback (for Additive Animation / Spring)
	//========================================

	/**
	 * 본별 힘 계산 활성화
	 * 활성화 시 본별로 개별 항력을 계산하여 Additive Animation/Spring에 사용 가능
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Bone Force",
	          meta = (EditCondition = "bEnableForceFeedback",
	                  ToolTip = "본별 힘 계산 활성화.\n활성화 시 본별로 개별 항력을 계산합니다.\nAdditive Animation 또는 AnimDynamics Spring에 사용할 수 있습니다."))
	bool bEnablePerBoneForce = false;

	/**
	 * 본별 힘 스무딩 속도 (1/s)
	 * 높을수록 힘 변화가 빠르고, 낮을수록 부드럽게 변화
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Bone Force",
	          meta = (EditCondition = "bEnablePerBoneForce", ClampMin = "0.1", ClampMax = "50.0",
	                  ToolTip = "본별 힘 스무딩 속도 (1/s).\n높을수록 힘 변화가 빠르고, 낮을수록 부드럽게 변화합니다."))
	float PerBoneForceSmoothingSpeed = 10.0f;

	/**
	 * 본별 힘 배율
	 * 계산된 본별 힘에 적용되는 스케일
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Bone Force",
	          meta = (EditCondition = "bEnablePerBoneForce", ClampMin = "0.0001", ClampMax = "100.0",
	                  ToolTip = "본별 힘 배율.\nAdditive Animation이나 Spring에 적용할 힘의 강도를 조절합니다."))
	float PerBoneForceMultiplier = 1.0f;

	/**
	 * 특정 본의 현재 유체 힘 반환 (BoneIndex)
	 * @param BoneIndex 조회할 본 인덱스
	 * @return 해당 본에 적용되는 유체 힘 벡터. 본이 없으면 ZeroVector
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Per-Bone Force")
	FVector GetFluidForceForBone(int32 BoneIndex) const;

	/**
	 * 특정 본의 현재 유체 힘 반환 (BoneName)
	 * @param BoneName 조회할 본 이름
	 * @return 해당 본에 적용되는 유체 힘 벡터. 본이 없으면 ZeroVector
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Per-Bone Force")
	FVector GetFluidForceForBoneByName(FName BoneName) const;

	/**
	 * 모든 본의 유체 힘 맵 반환 (BoneIndex → Force)
	 * @return 본 인덱스 → 힘 벡터 맵
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Per-Bone Force")
	TMap<int32, FVector> GetAllBoneForces() const { return CurrentPerBoneForces; }

	/**
	 * 유체 힘이 적용된 본 인덱스 배열 반환
	 * @param OutBoneIndices 힘이 적용된 본 인덱스 배열
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Per-Bone Force")
	void GetActiveBoneIndices(TArray<int32>& OutBoneIndices) const;

	/**
	 * 가장 강한 힘을 받는 본 인덱스와 힘 반환
	 * @param OutBoneIndex 가장 강한 힘을 받는 본 인덱스 (-1 if none)
	 * @param OutForce 해당 본의 힘 벡터
	 * @return 힘을 받는 본이 있으면 true
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Per-Bone Force")
	bool GetStrongestBoneForce(int32& OutBoneIndex, FVector& OutForce) const;

	/** 현재 유체 힘 반환 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Force Feedback")
	FVector GetCurrentFluidForce() const { return CurrentFluidForce; }

	/** 현재 평균 압력 반환 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Force Feedback")
	float GetCurrentFluidPressure() const { return CurrentAveragePressure; }

	/**
	 * 유체 힘을 CharacterMovement에 적용
	 * CharacterMovementComponent가 있는 액터에서만 작동
	 * @param ForceScale 힘 스케일 배율 (기본 1.0)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Force Feedback")
	void ApplyFluidForceToCharacterMovement(float ForceScale = 1.0f);

	/**
	 * 특정 유체 태그와 현재 충돌 중인지 확인
	 * @param FluidTag 확인할 유체 태그 (예: "Water", "Lava")
	 * @return 해당 태그의 유체와 충돌 중이면 true
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Force Feedback")
	bool IsCollidingWithFluidTag(FName FluidTag) const;

	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	int32 GetAttachedParticleCount() const { return AttachedParticleCount; }

	/** Collider와 충돌 중인 파티클 수 반환 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	int32 GetCollidingParticleCount() const { return CollidingParticleCount; }

	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	void DetachAllFluid();

	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	void PushFluid(FVector Direction, float Force);

	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	bool IsWet() const { return bIsWet; }

	/** Check if subsystem is valid */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	bool HasValidTarget() const { return TargetSubsystem != nullptr; }

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY()
	UMeshFluidCollider* AutoCollider;

	/** 이전 프레임 충돌 상태 */
	bool bWasColliding = false;

	void CreateAutoCollider();
	void RegisterWithSimulator();
	void UnregisterFromSimulator();
	void UpdateAttachedParticleCount();

	/** Collider와 충돌 중인 파티클 감지 */
	void DetectCollidingParticles();

	//========================================
	// Force Feedback Internal State
	//========================================

	/** 스무딩 전 누적 힘 (스무딩에 사용) */
	FVector SmoothedForce = FVector::ZeroVector;

	/** 유체 태그별 이전 프레임 충돌 상태 (Enter/Exit 이벤트용) */
	TMap<FName, bool> PreviousFluidTagStates;

	/** 유체 태그별 현재 프레임 충돌 파티클 수 */
	TMap<FName, int32> CurrentFluidTagCounts;

	/** 이 컴포넌트와 연결된 콜라이더 인덱스 (GPU 피드백 필터링용) */
	int32 ColliderIndex = -1;

	/** GPU 피드백이 이미 활성화되었는지 여부 */
	bool bGPUFeedbackEnabled = false;

	//========================================
	// Per-Bone Force Internal State
	//========================================

	/** 본 인덱스별 현재 유체 힘 (스무딩 적용됨) */
	TMap<int32, FVector> CurrentPerBoneForces;

	/** 본 인덱스별 스무딩 전 힘 (스무딩 계산에 사용) */
	TMap<int32, FVector> SmoothedPerBoneForces;

	/** 본 인덱스 → 본 이름 캐시 (SkeletalMesh에서 가져옴) */
	TMap<int32, FName> BoneIndexToNameCache;

	/** 본 이름 캐시가 초기화되었는지 여부 */
	bool bBoneNameCacheInitialized = false;

	/** 디버그 로그 타이머 (3초마다 출력) */
	float PerBoneForceDebugTimer = 0.0f;

	/** 본 이름 캐시 초기화 */
	void InitializeBoneNameCache();

	/** 본별 힘 처리 (ProcessCollisionFeedback 내부에서 호출) */
	void ProcessPerBoneForces(float DeltaTime, const TArray<struct FGPUCollisionFeedback>& AllFeedback, int32 FeedbackCount);

	/** GPU 피드백 처리 (매 틱 호출) */
	void ProcessCollisionFeedback(float DeltaTime);

	/** 유체 태그 이벤트 업데이트 (Enter/Exit) */
	void UpdateFluidTagEvents();

	/** GPU Collision Feedback 자동 활성화 */
	void EnableGPUCollisionFeedbackIfNeeded();

	//========================================
	// Boundary Particles (Flex-style Adhesion)
	//========================================

public:
	/**
	 * 경계 입자 시스템 활성화
	 * 메시 표면에 입자를 생성하여 Flex 스타일의 자연스러운 Adhesion 구현
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (ToolTip = "경계 입자 시스템 활성화.\n메시 표면에 보이지 않는 입자를 생성하여\nFlex 스타일의 자연스러운 Adhesion/Cohesion을 구현합니다."))
	bool bEnableBoundaryParticles = false;

	/**
	 * 경계 입자 간격 (cm)
	 * ParticleRadius의 0.5~1.0배 권장
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bEnableBoundaryParticles", ClampMin = "1.0", ClampMax = "50.0",
	                  ToolTip = "경계 입자 간격 (cm).\n작을수록 정밀하지만 입자 수 증가.\nParticleRadius의 0.5~1.0배 권장."))
	float BoundaryParticleSpacing = 5.0f;

	/**
	 * 경계 입자 디버그 표시
	 * 체크 시 경계 입자를 화면에 시각화
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bEnableBoundaryParticles",
	                  ToolTip = "경계 입자 디버그 표시.\n체크 시 경계 입자 위치를 작은 구체로 시각화합니다."))
	bool bShowBoundaryParticles = false;

	/**
	 * 디버그 입자 색상
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bShowBoundaryParticles"))
	FColor BoundaryParticleDebugColor = FColor::Cyan;

	/**
	 * 디버그 입자 크기 (cm)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bShowBoundaryParticles", ClampMin = "0.5", ClampMax = "10.0"))
	float BoundaryParticleDebugSize = 2.0f;

	/** 경계 입자 수 반환 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Boundary Particles")
	int32 GetBoundaryParticleCount() const { return BoundaryParticlePositions.Num(); }

	/** 경계 입자 위치 배열 반환 (월드 좌표) */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Boundary Particles")
	const TArray<FVector>& GetBoundaryParticlePositions() const { return BoundaryParticlePositions; }

	/** 경계 입자 수동 재생성 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Boundary Particles")
	void RegenerateBoundaryParticles();

	/** GPU용 경계 입자 데이터 수집 (legacy - CPU 월드 좌표 업로드 방식) */
	void CollectGPUBoundaryParticles(struct FGPUBoundaryParticles& OutBoundaryParticles) const;

	/** GPU Skinning용 로컬 경계 입자 데이터 수집 (한 번만 업로드) */
	void CollectLocalBoundaryParticles(TArray<struct FGPUBoundaryParticleLocal>& OutLocalParticles) const;

	/** GPU Skinning용 본 트랜스폼 수집 (매 프레임 업로드) */
	void CollectBoneTransformsForBoundary(TArray<FMatrix>& OutBoneTransforms, FMatrix& OutComponentTransform) const;

	/** 컴포넌트 고유 ID 반환 (GPU Skinning Owner ID용) */
	int32 GetBoundaryOwnerID() const { return GetUniqueID(); }

	/** GPU Skinning 활성화 여부 (로컬 입자가 있으면 true) */
	bool HasLocalBoundaryParticles() const { return bBoundaryParticlesInitialized && BoundaryParticleLocalPositions.Num() > 0; }

	/** Boundary Adhesion 활성화 여부 */
	bool IsBoundaryAdhesionEnabled() const { return bEnableBoundaryParticles && bBoundaryParticlesInitialized && BoundaryParticlePositions.Num() > 0; }

private:
	/** 경계 입자 월드 위치 (매 프레임 업데이트됨) */
	TArray<FVector> BoundaryParticlePositions;

	/** 경계 입자 로컬 위치 (메시 표면 기준, 초기화 시 생성) */
	TArray<FVector> BoundaryParticleLocalPositions;

	/** 경계 입자 표면 노멀 (월드 좌표, 매 프레임 업데이트됨) */
	TArray<FVector> BoundaryParticleNormals;

	/** 경계 입자 로컬 노멀 (메시 표면 기준, 초기화 시 생성) */
	TArray<FVector> BoundaryParticleLocalNormals;

	/** 경계 입자가 속한 본 인덱스 (-1 = 스태틱) */
	TArray<int32> BoundaryParticleBoneIndices;

	/** 스켈레탈 메시용: 정점 인덱스 (GetSkinnedVertexPosition 호출용) */
	TArray<int32> BoundaryParticleVertexIndices;

	/** 스켈레탈 메시 사용 여부 */
	bool bIsSkeletalMesh = false;

	/** 경계 입자 초기화 여부 */
	bool bBoundaryParticlesInitialized = false;

	/** 경계 입자 생성 (메시 표면 샘플링) */
	void GenerateBoundaryParticles();

	/** 경계 입자 위치 업데이트 (스켈레탈 메시의 경우 본 트랜스폼 적용) */
	void UpdateBoundaryParticlePositions();

	/** 디버그 경계 입자 그리기 */
	void DrawDebugBoundaryParticles();

	/** 삼각형 표면 샘플링 */
	void SampleTriangleSurface(const FVector& V0, const FVector& V1, const FVector& V2,
	                           float Spacing, TArray<FVector>& OutPoints);

	//=============================================================================
	// Physics Asset/Simple Collision 기반 표면 샘플링
	//=============================================================================

	/** Sphere 콜라이더 표면 샘플링 */
	void SampleSphereSurface(const struct FKSphereElem& Sphere, int32 BoneIndex, const FTransform& LocalTransform);

	/** Capsule(Sphyl) 콜라이더 표면 샘플링 */
	void SampleCapsuleSurface(const struct FKSphylElem& Capsule, int32 BoneIndex);

	/** Box 콜라이더 표면 샘플링 */
	void SampleBoxSurface(const struct FKBoxElem& Box, int32 BoneIndex);

	/** 반구 표면 샘플링 (캡슐 상/하단용) */
	void SampleHemisphere(const FTransform& Transform, float Radius, float ZOffset,
	                      int32 ZDirection, int32 BoneIndex, int32 NumSamples);

	/** AggGeom에서 모든 프리미티브 샘플링 */
	void SampleAggGeomSurfaces(const struct FKAggregateGeom& AggGeom, int32 BoneIndex);
};
