// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/SpatialHash.h"

FSpatialHash::FSpatialHash()
	: CellSize(1.0f)
{
}

FSpatialHash::FSpatialHash(float InCellSize)
	: CellSize(InCellSize)
{
}

void FSpatialHash::Clear()
{
	++RebuildCounter;

	// 주기적으로 빈 셀 정리 (메모리 누수 방지)
	if (RebuildCounter >= PurgeInterval)
	{
		Grid.Empty();
		CachedPositions.Empty();
		RebuildCounter = 0;
	}
	else
	{
		// 메모리 재할당 없이 비우기 (capacity 유지)
		for (auto& Pair : Grid)
		{
			Pair.Value.Reset();
		}
		CachedPositions.Reset();
	}
}

void FSpatialHash::SetCellSize(float NewCellSize)
{
	CellSize = FMath::Max(NewCellSize, 0.01f);
}

void FSpatialHash::Insert(int32 ParticleIndex, const FVector& Position)
{
	// 셀 좌표 계산 
	FIntVector CellCoord = GetCellCoord(Position);

	// 해당 셀에 인덱스 추가
	Grid.FindOrAdd(CellCoord).Add(ParticleIndex);
}

void FSpatialHash::GetNeighbors(const FVector& Position, float Radius, TArray<int32>& OutNeighbors) const
{
	OutNeighbors.Reset();

	// 검색할 셀 범위 계산
	int32 CellRadius = FMath::CeilToInt(Radius / CellSize);
	FIntVector CenterCell = GetCellCoord(Position);

	// 거리 필터링용 제곱 반경
	const float RadiusSq = Radius * Radius;
	const bool bHasCachedPositions = CachedPositions.Num() > 0;

	// 주변 셀들 순회
	for (int32 x = -CellRadius; x <= CellRadius; ++x)
	{
		for (int32 y = -CellRadius; y <= CellRadius; ++y)
		{
			for (int32 z = -CellRadius; z <= CellRadius; ++z)
			{
				FIntVector CellCoord = CenterCell + FIntVector(x, y, z);

				if (const TArray<int32>* CellParticles = Grid.Find(CellCoord))
				{
					// 거리 필터링: 실제 반경 내 입자만 추가
					if (bHasCachedPositions)
					{
						for (int32 Idx : *CellParticles)
						{
							if (FVector::DistSquared(Position, CachedPositions[Idx]) <= RadiusSq)
							{
								OutNeighbors.Add(Idx);
							}
						}
					}
					else
					{
						// 폴백: 캐시 없으면 기존 방식
						OutNeighbors.Append(*CellParticles);
					}
				}
			}
		}
	}
}

void FSpatialHash::QueryBox(const FBox& Box, TArray<int32>& OutIndices) const
{
	OutIndices.Reset();

	// 박스를 셀 좌표로 변환
	FIntVector MinCell = GetCellCoord(Box.Min);
	FIntVector MaxCell = GetCellCoord(Box.Max);

	// 해당 범위 셀들만 순회
	for (int32 x = MinCell.X; x <= MaxCell.X; ++x)
	{
		for (int32 y = MinCell.Y; y <= MaxCell.Y; ++y)
		{
			for (int32 z = MinCell.Z; z <= MaxCell.Z; ++z)
			{
				if (const TArray<int32>* Cell = Grid.Find(FIntVector(x, y, z)))
				{
					OutIndices.Append(*Cell);
				}
			}
		}
	}
}

void FSpatialHash::BuildFromPositions(const TArray<FVector>& Positions)
{
	Clear();

	// 위치 배열 캐싱 (거리 필터링용)
	CachedPositions = Positions;

	for (int32 i = 0; i < Positions.Num(); ++i)
	{
		Insert(i, Positions[i]);
	}
}

FIntVector FSpatialHash::GetCellCoord(const FVector& Position) const
{
	return FIntVector(
		FMath::FloorToInt(Position.X / CellSize),
		FMath::FloorToInt(Position.Y / CellSize),
		FMath::FloorToInt(Position.Z / CellSize)
	);
}