// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FluidInteractionComponent.generated.h"

class UKawaiiFluidSimulatorSubsystem;
class UFluidCollider;

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
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY()
	UFluidCollider* AutoCollider;

	/** 이전 프레임 충돌 상태 */
	bool bWasColliding = false;

	void CreateAutoCollider();
	void RegisterWithSimulator();
	void UnregisterFromSimulator();
	void UpdateAttachedParticleCount();

	/** Collider와 충돌 중인 파티클 감지 */
	void DetectCollidingParticles();
};
