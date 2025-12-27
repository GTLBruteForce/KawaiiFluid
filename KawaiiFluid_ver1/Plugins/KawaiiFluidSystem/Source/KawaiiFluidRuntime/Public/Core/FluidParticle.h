// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FluidParticle.generated.h"

/**
 * 유체 입자 구조체
 * PBF 시뮬레이션의 기본 단위
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFluidParticle
{
	GENERATED_BODY()

	// 위치
	UPROPERTY(BlueprintReadWrite, Category = "Particle")
	FVector Position;

	// 예측 위치 (솔버에서 사용)
	UPROPERTY(BlueprintReadOnly, Category = "Particle")
	FVector PredictedPosition;

	// 속도
	UPROPERTY(BlueprintReadWrite, Category = "Particle")
	FVector Velocity;

	// 질량
	UPROPERTY(BlueprintReadWrite, Category = "Particle")
	float Mass;

	// 밀도 (매 프레임 계산)
	UPROPERTY(BlueprintReadOnly, Category = "Particle")
	float Density;

	// 라그랑주 승수 (밀도 제약용)
	float Lambda;

	// 접착 상태
	UPROPERTY(BlueprintReadOnly, Category = "Particle")
	bool bIsAttached;

	// 접착된 액터
	UPROPERTY(BlueprintReadOnly, Category = "Particle")
	TWeakObjectPtr<AActor> AttachedActor;

	// 접착된 본 이름 (스켈레탈 메시용)
	UPROPERTY(BlueprintReadOnly, Category = "Particle")
	FName AttachedBoneName;

	// 본 로컬 좌표에서의 상대 위치 (본 이동 추적용)
	FVector AttachedLocalOffset;

	// 접착된 표면의 법선 (표면 미끄러짐 계산용)
	FVector AttachedSurfaceNormal;

	// 이번 프레임에 분리됨 (같은 프레임 재접착 방지)
	bool bJustDetached;

	// 바닥 근처에 있음 (접착 유지 마진 감소용)
	bool bNearGround;

	// 입자 ID
	UPROPERTY(BlueprintReadOnly, Category = "Particle")
	int32 ParticleID;

	// 이웃 입자 인덱스 (캐싱용)
	TArray<int32> NeighborIndices;

	//========================================
	// Slime-specific fields
	//========================================

	// 클러스터 ID (슬라임 분리/합체용)
	UPROPERTY(BlueprintReadOnly, Category = "Particle|Slime")
	int32 ClusterID;

	// Shape Matching용 초기 오프셋 (중심으로부터의 상대 위치)
	FVector RestOffset;

	// 표면 파티클 여부
	UPROPERTY(BlueprintReadOnly, Category = "Particle|Slime")
	bool bIsSurfaceParticle;

	// 표면 법선 (표면장력 계산용)
	FVector SurfaceNormal;

	// 코어 파티클 여부 (형태 유지의 앵커 역할)
	UPROPERTY(BlueprintReadOnly, Category = "Particle|Slime")
	bool bIsCoreParticle;

	// 코어로부터의 거리 비율 (0 = 중심, 1 = 표면)
	float DistanceFromCoreRatio;

	// 흔적 스폰 여부 (중복 방지용)
	bool bTrailSpawned;

	FFluidParticle()
		: Position(FVector::ZeroVector)
		, PredictedPosition(FVector::ZeroVector)
		, Velocity(FVector::ZeroVector)
		, Mass(1.0f)
		, Density(0.0f)
		, Lambda(0.0f)
		, bIsAttached(false)
		, AttachedBoneName(NAME_None)
		, AttachedLocalOffset(FVector::ZeroVector)
		, AttachedSurfaceNormal(FVector::UpVector)
		, bJustDetached(false)
		, bNearGround(false)
		, ParticleID(-1)
		, ClusterID(0)
		, RestOffset(FVector::ZeroVector)
		, bIsSurfaceParticle(false)
		, SurfaceNormal(FVector::ZeroVector)
		, bIsCoreParticle(false)
		, DistanceFromCoreRatio(1.0f)
		, bTrailSpawned(false)
	{
	}

	FFluidParticle(const FVector& InPosition, int32 InID)
		: Position(InPosition)
		, PredictedPosition(InPosition)
		, Velocity(FVector::ZeroVector)
		, Mass(1.0f)
		, Density(0.0f)
		, Lambda(0.0f)
		, bIsAttached(false)
		, AttachedBoneName(NAME_None)
		, AttachedLocalOffset(FVector::ZeroVector)
		, AttachedSurfaceNormal(FVector::UpVector)
		, bJustDetached(false)
		, bNearGround(false)
		, ParticleID(InID)
		, ClusterID(0)
		, RestOffset(FVector::ZeroVector)
		, bIsSurfaceParticle(false)
		, SurfaceNormal(FVector::ZeroVector)
		, bIsCoreParticle(false)
		, DistanceFromCoreRatio(1.0f)
		, bTrailSpawned(false)
	{
	}
};
