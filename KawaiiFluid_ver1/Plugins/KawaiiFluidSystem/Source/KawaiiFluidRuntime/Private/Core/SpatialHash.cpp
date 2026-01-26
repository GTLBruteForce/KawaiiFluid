// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// CPU-side Spatial Hash Implementation
// See SpatialHash.h for documentation.

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

	// Periodically purge empty cells (prevent memory leaks)
	if (RebuildCounter >= PurgeInterval)
	{
		Grid.Empty();
		CachedPositions.Empty();
		RebuildCounter = 0;
	}
	else
	{
		// Clear without reallocation (maintain capacity)
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
	// Calculate cell coordinates
	FIntVector CellCoord = GetCellCoord(Position);

	// Add index to the corresponding cell
	Grid.FindOrAdd(CellCoord).Add(ParticleIndex);
}

void FSpatialHash::GetNeighbors(const FVector& Position, float Radius, TArray<int32>& OutNeighbors) const
{
	OutNeighbors.Reset();

	// Calculate cell range to search
	int32 CellRadius = FMath::CeilToInt(Radius / CellSize);
	FIntVector CenterCell = GetCellCoord(Position);

	// Squared radius for distance filtering
	const float RadiusSq = Radius * Radius;
	const bool bHasCachedPositions = CachedPositions.Num() > 0;

	// Iterate through neighboring cells
	for (int32 x = -CellRadius; x <= CellRadius; ++x)
	{
		for (int32 y = -CellRadius; y <= CellRadius; ++y)
		{
			for (int32 z = -CellRadius; z <= CellRadius; ++z)
			{
				FIntVector CellCoord = CenterCell + FIntVector(x, y, z);

				if (const TArray<int32>* CellParticles = Grid.Find(CellCoord))
				{
					// Distance filtering: only add particles within actual radius
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
						// Fallback: use existing method if no cache
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

	// Convert box to cell coordinates
	FIntVector MinCell = GetCellCoord(Box.Min);
	FIntVector MaxCell = GetCellCoord(Box.Max);

	// Iterate only through cells in range
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

	// Cache position array (for distance filtering)
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