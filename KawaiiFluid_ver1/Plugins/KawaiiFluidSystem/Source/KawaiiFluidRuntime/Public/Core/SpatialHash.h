// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 공간 해싱 클래스
 * 이웃 입자 탐색을 O(n²) -> O(n)으로 최적화
 */
class KAWAIIFLUIDRUNTIME_API FSpatialHash
{
public:
	FSpatialHash();
	FSpatialHash(float InCellSize);

	/** 그리드 초기화 */
	void Clear();

	/** 셀 크기 설정 */
	void SetCellSize(float NewCellSize);

	/** 입자를 그리드에 삽입 */
	void Insert(int32 ParticleIndex, const FVector& Position);

	/** 특정 위치 주변의 이웃 입자 인덱스 반환 */
	void GetNeighbors(const FVector& Position, float Radius, TArray<int32>& OutNeighbors) const;

	/** 박스 영역 내 파티클 인덱스 반환 (AABB 쿼리) */
	void QueryBox(const FBox& Box, TArray<int32>& OutIndices) const;

	/** 모든 입자를 한 번에 삽입 (벌크 연산) */
	void BuildFromPositions(const TArray<FVector>& Positions);

	/** 그리드 데이터 반환 (읽기 전용) */
	const TMap<FIntVector, TArray<int32>>& GetGrid() const { return Grid; }

	/** 셀 크기 반환 */
	float GetCellSize() const { return CellSize; }

private:
	/** 셀 크기 */
	float CellSize;

	/** 해시 그리드: 셀 좌표 -> 입자 인덱스 배열 */
	TMap<FIntVector, TArray<int32>> Grid;

	/** 캐싱된 입자 위치 (거리 필터링용) */
	TArray<FVector> CachedPositions;

	/** 빈 셀 정리용 카운터 */
	int32 RebuildCounter = 0;

	/** 빈 셀 정리 주기 */
	static constexpr int32 PurgeInterval = 300;  // 약 5초 (60fps 기준)

	/** 월드 좌표를 셀 좌표로 변환 */
	FIntVector GetCellCoord(const FVector& Position) const;
};
