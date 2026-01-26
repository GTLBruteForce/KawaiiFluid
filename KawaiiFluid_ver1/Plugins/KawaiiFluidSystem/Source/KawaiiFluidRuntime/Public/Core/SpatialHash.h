// Copyright 2026 Team_Bruteforce. All Rights Reserved.
//
// CPU-side Spatial Hash Implementation
// =====================================
// This is a genuine spatial hash implementation using TMap for CPU-side neighbor lookup.
// Unlike the GPU implementation (which uses Z-Order Morton code sorting via FGPUZOrderSortManager),
// this CPU version uses a traditional hash-table approach.
//
// For GPU-based neighbor search, see:
// - GPU/Managers/GPUZOrderSortManager.h (Z-Order Morton code sorting)
// - Shaders/Private/FluidMortonUtils.ush (Morton code functions)

#pragma once

#include "CoreMinimal.h"

/**
 * CPU spatial hashing class
 * Optimizes neighbor particle search from O(n²) to O(n)
 * GPU simulation uses Z-Order Morton code sorting instead.
 */
class KAWAIIFLUIDRUNTIME_API FSpatialHash
{
public:
	FSpatialHash();
	FSpatialHash(float InCellSize);

	/** Initialize grid */
	void Clear();

	/** Set cell size */
	void SetCellSize(float NewCellSize);

	/** Insert particle into grid */
	void Insert(int32 ParticleIndex, const FVector& Position);

	/** Get neighbor particle indices around a specific position */
	void GetNeighbors(const FVector& Position, float Radius, TArray<int32>& OutNeighbors) const;

	/** Get particle indices within box region (AABB query) */
	void QueryBox(const FBox& Box, TArray<int32>& OutIndices) const;

	/** Insert all particles at once (bulk operation) */
	void BuildFromPositions(const TArray<FVector>& Positions);

	/** Get grid data (read-only) */
	const TMap<FIntVector, TArray<int32>>& GetGrid() const { return Grid; }

	/** Get cell size */
	float GetCellSize() const { return CellSize; }

private:
	/** Cell size */
	float CellSize;

	/** Hash grid: cell coordinate -> particle index array */
	TMap<FIntVector, TArray<int32>> Grid;

	/** Cached particle positions (for distance filtering) */
	TArray<FVector> CachedPositions;

	/** Counter for empty cell purging */
	int32 RebuildCounter = 0;

	/** Empty cell purge interval */
	static constexpr int32 PurgeInterval = 300;  // Approx 5 seconds (at 60fps)

	/** Convert world coordinate to cell coordinate */
	FIntVector GetCellCoord(const FVector& Position) const;
};
