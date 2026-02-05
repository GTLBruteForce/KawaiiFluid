// Copyright 2026 Team_Bruteforce. All Rights Reserved.


#include "Landscape/LandscapeHeightmapExtractor.h"
#include "LandscapeComponent.h"
#include "EngineUtils.h"
#include "LandscapeProxy.h"
#include "Async/ParallelFor.h"

DEFINE_LOG_CATEGORY_STATIC(LogHeightmapExtractor, Log, All);

// Thread-local min/max tracking for parallel height sampling
struct FHeightMinMax
{
	float MinZ = FLT_MAX;
	float MaxZ = -FLT_MAX;
};

bool FLandscapeHeightmapExtractor::ExtractHeightmap(
	ALandscapeProxy* Landscape,
	TArray<float>& OutHeightData,
	int32& OutWidth,
	int32& OutHeight,
	FBox& OutBounds,
	int32 Resolution)
{
	if (!Landscape)
	{
		UE_LOG(LogHeightmapExtractor, Warning, TEXT("ExtractHeightmap: Landscape is null"));
		return false;
	}

	// Clamp resolution to power of 2
	Resolution = ClampToPowerOfTwo(Resolution);
	OutWidth = Resolution;
	OutHeight = Resolution;

	// Get landscape bounds
	OutBounds = Landscape->GetComponentsBoundingBox(true);

	if (!OutBounds.IsValid)
	{
		UE_LOG(LogHeightmapExtractor, Warning, TEXT("ExtractHeightmap: Invalid landscape bounds"));
		return false;
	}

	// Add small padding to bounds
	const float Padding = 10.0f;
	OutBounds = OutBounds.ExpandBy(FVector(Padding, Padding, 0.0f));

	const FVector BoundsSize = OutBounds.GetSize();
	const float StepX = BoundsSize.X / (float)(Resolution - 1);
	const float StepY = BoundsSize.Y / (float)(Resolution - 1);

	// Allocate height data
	OutHeightData.SetNumUninitialized(Resolution * Resolution);

	// Sample heights in a grid (parallel)
	float MinZ = OutBounds.Max.Z;
	float MaxZ = OutBounds.Min.Z;

	// Thread-local accumulators for min/max
	const int32 NumThreads = FMath::Max(1, FPlatformMisc::NumberOfWorkerThreadsToSpawn() + 1);
	TArray<FHeightMinMax> ThreadMinMax;
	ThreadMinMax.SetNum(NumThreads);

	// Parallel sampling - each thread handles a row
	ParallelFor(Resolution, [&](int32 y)
	{
		const int32 ThreadIdx = FPlatformTLS::GetCurrentThreadId() % NumThreads;
		FHeightMinMax& LocalMinMax = ThreadMinMax[ThreadIdx];

		const float WorldY = OutBounds.Min.Y + y * StepY;

		for (int32 x = 0; x < Resolution; ++x)
		{
			const float WorldX = OutBounds.Min.X + x * StepX;
			const float Height = SampleLandscapeHeight(Landscape, WorldX, WorldY);

			// Track height range (thread-local)
			LocalMinMax.MinZ = FMath::Min(LocalMinMax.MinZ, Height);
			LocalMinMax.MaxZ = FMath::Max(LocalMinMax.MaxZ, Height);

			// Store raw height
			OutHeightData[y * Resolution + x] = Height;
		}
	});

	// Merge thread-local min/max
	for (const FHeightMinMax& Local : ThreadMinMax)
	{
		MinZ = FMath::Min(MinZ, Local.MinZ);
		MaxZ = FMath::Max(MaxZ, Local.MaxZ);
	}

	// Update Z bounds to actual height range (with padding for collision margin)
	OutBounds.Min.Z = MinZ - Padding;
	OutBounds.Max.Z = MaxZ + Padding;

	// Normalize heights to 0-1 range
	// IMPORTANT: Use the PADDED bounds for normalization so shader lerp matches!
	// Shader does: terrainZ = lerp(WorldMin.z, WorldMax.z, normalizedHeight)
	// So we must normalize using the same range: (OutBounds.Min.Z, OutBounds.Max.Z)
	const float PaddedMinZ = OutBounds.Min.Z;
	const float PaddedMaxZ = OutBounds.Max.Z;
	const float HeightRange = PaddedMaxZ - PaddedMinZ;
	if (HeightRange > SMALL_NUMBER)
	{
		const float InvHeightRange = 1.0f / HeightRange;
		for (float& Height : OutHeightData)
		{
			Height = (Height - PaddedMinZ) * InvHeightRange;
		}
	}
	else
	{
		// Flat terrain
		for (float& Height : OutHeightData)
		{
			Height = 0.5f;
		}
	}

	UE_LOG(LogHeightmapExtractor, Log, TEXT("Extracted heightmap from %s: %dx%d, Bounds: (%.1f,%.1f,%.1f) - (%.1f,%.1f,%.1f)"),
		*Landscape->GetName(), OutWidth, OutHeight,
		OutBounds.Min.X, OutBounds.Min.Y, OutBounds.Min.Z,
		OutBounds.Max.X, OutBounds.Max.Y, OutBounds.Max.Z);

	return true;
}

bool FLandscapeHeightmapExtractor::ExtractCombinedHeightmap(
	const TArray<ALandscapeProxy*>& Landscapes,
	TArray<float>& OutHeightData,
	int32& OutWidth,
	int32& OutHeight,
	FBox& OutBounds,
	int32 Resolution)
{
	if (Landscapes.Num() == 0)
	{
		UE_LOG(LogHeightmapExtractor, Warning, TEXT("ExtractCombinedHeightmap: No landscapes provided"));
		return false;
	}

	// Single landscape - use simple extraction
	if (Landscapes.Num() == 1)
	{
		return ExtractHeightmap(Landscapes[0], OutHeightData, OutWidth, OutHeight, OutBounds, Resolution);
	}

	// Clamp resolution to power of 2
	Resolution = ClampToPowerOfTwo(Resolution);
	OutWidth = Resolution;
	OutHeight = Resolution;

	// Calculate combined bounds
	OutBounds = FBox(ForceInit);
	for (ALandscapeProxy* Landscape : Landscapes)
	{
		if (Landscape)
		{
			OutBounds += Landscape->GetComponentsBoundingBox(true);
		}
	}

	if (!OutBounds.IsValid)
	{
		UE_LOG(LogHeightmapExtractor, Warning, TEXT("ExtractCombinedHeightmap: Invalid combined bounds"));
		return false;
	}

	// Add padding
	const float Padding = 10.0f;
	OutBounds = OutBounds.ExpandBy(FVector(Padding, Padding, 0.0f));

	const FVector BoundsSize = OutBounds.GetSize();
	const float StepX = BoundsSize.X / (float)(Resolution - 1);
	const float StepY = BoundsSize.Y / (float)(Resolution - 1);

	// Allocate height data
	OutHeightData.SetNumUninitialized(Resolution * Resolution);

	// Sample heights from all landscapes (parallel)
	float MinZ = OutBounds.Max.Z;
	float MaxZ = OutBounds.Min.Z;

	// Thread-local accumulators for min/max
	const int32 NumThreads = FMath::Max(1, FPlatformMisc::NumberOfWorkerThreadsToSpawn() + 1);
	TArray<FHeightMinMax> ThreadMinMax;
	ThreadMinMax.SetNum(NumThreads);

	// Pre-cache landscape bounds (avoid repeated calls in parallel loop)
	TArray<FBox> LandscapeBoundsCache;
	LandscapeBoundsCache.SetNum(Landscapes.Num());
	for (int32 i = 0; i < Landscapes.Num(); ++i)
	{
		if (Landscapes[i])
		{
			LandscapeBoundsCache[i] = Landscapes[i]->GetComponentsBoundingBox(true);
		}
	}

	// Parallel sampling - each thread handles a row
	ParallelFor(Resolution, [&](int32 y)
	{
		const int32 ThreadIdx = FPlatformTLS::GetCurrentThreadId() % NumThreads;
		FHeightMinMax& LocalMinMax = ThreadMinMax[ThreadIdx];

		const float WorldY = OutBounds.Min.Y + y * StepY;

		for (int32 x = 0; x < Resolution; ++x)
		{
			const float WorldX = OutBounds.Min.X + x * StepX;

			// Try each landscape, use the one that contains this point
			float Height = OutBounds.Min.Z;
			bool bFoundHeight = false;

			for (int32 i = 0; i < Landscapes.Num(); ++i)
			{
				ALandscapeProxy* Landscape = Landscapes[i];
				if (!Landscape) continue;

				// Check if point is within this landscape's XY bounds (use cached)
				const FBox& LandscapeBounds = LandscapeBoundsCache[i];
				if (WorldX >= LandscapeBounds.Min.X && WorldX <= LandscapeBounds.Max.X &&
					WorldY >= LandscapeBounds.Min.Y && WorldY <= LandscapeBounds.Max.Y)
				{
					Height = SampleLandscapeHeight(Landscape, WorldX, WorldY);
					bFoundHeight = true;
					break;
				}
			}

			if (bFoundHeight)
			{
				LocalMinMax.MinZ = FMath::Min(LocalMinMax.MinZ, Height);
				LocalMinMax.MaxZ = FMath::Max(LocalMinMax.MaxZ, Height);
			}

			OutHeightData[y * Resolution + x] = Height;
		}
	});

	// Merge thread-local min/max
	for (const FHeightMinMax& Local : ThreadMinMax)
	{
		MinZ = FMath::Min(MinZ, Local.MinZ);
		MaxZ = FMath::Max(MaxZ, Local.MaxZ);
	}

	// Update Z bounds (with padding for collision margin)
	OutBounds.Min.Z = MinZ - Padding;
	OutBounds.Max.Z = MaxZ + Padding;

	// Normalize heights using PADDED bounds (must match shader's lerp range)
	const float PaddedMinZ = OutBounds.Min.Z;
	const float PaddedMaxZ = OutBounds.Max.Z;
	const float HeightRange = PaddedMaxZ - PaddedMinZ;
	if (HeightRange > SMALL_NUMBER)
	{
		const float InvHeightRange = 1.0f / HeightRange;
		for (float& Height : OutHeightData)
		{
			Height = FMath::Clamp((Height - PaddedMinZ) * InvHeightRange, 0.0f, 1.0f);
		}
	}
	else
	{
		for (float& Height : OutHeightData)
		{
			Height = 0.5f;
		}
	}

	UE_LOG(LogHeightmapExtractor, Log, TEXT("Extracted combined heightmap from %d landscapes: %dx%d"),
		Landscapes.Num(), OutWidth, OutHeight);

	return true;
}

FGPUHeightmapCollisionParams FLandscapeHeightmapExtractor::BuildCollisionParams(
	const FBox& Bounds,
	int32 Width,
	int32 Height,
	float ParticleRadius,
	float Friction,
	float Restitution)
{
	FGPUHeightmapCollisionParams Params;

	Params.WorldMin = FVector3f(Bounds.Min);
	Params.WorldMax = FVector3f(Bounds.Max);
	Params.TextureWidth = Width;
	Params.TextureHeight = Height;
	Params.ParticleRadius = ParticleRadius;
	Params.Friction = Friction;
	Params.Restitution = Restitution;
	Params.NormalStrength = 1.0f;
	Params.CollisionOffset = 0.0f;
	Params.bEnabled = 1;

	// Calculate inverse values
	Params.UpdateInverseValues();

	return Params;
}

void FLandscapeHeightmapExtractor::FindLandscapesInWorld(UWorld* World, TArray<ALandscapeProxy*>& OutLandscapes)
{
	OutLandscapes.Reset();

	if (!World)
	{
		return;
	}

	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		OutLandscapes.Add(*It);
	}

	UE_LOG(LogHeightmapExtractor, Log, TEXT("Found %d landscapes in world"), OutLandscapes.Num());
}

float FLandscapeHeightmapExtractor::SampleLandscapeHeight(ALandscapeProxy* Landscape, float WorldX, float WorldY)
{
	if (!Landscape)
	{
		return 0.0f;
	}

	FVector Location(WorldX, WorldY, 0.0f);
	TOptional<float> Height = Landscape->GetHeightAtLocation(Location);

	if (Height.IsSet())
	{
		return Height.GetValue();
	}

	// Fallback: try line trace
	FVector Start(WorldX, WorldY, 100000.0f);
	FVector End(WorldX, WorldY, -100000.0f);

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;

	if (Landscape->GetWorld() &&
		Landscape->GetWorld()->LineTraceSingleByChannel(HitResult, Start, End, ECC_WorldStatic, QueryParams))
	{
		return HitResult.ImpactPoint.Z;
	}

	// Return landscape center Z as fallback
	FBox Bounds = Landscape->GetComponentsBoundingBox(true);
	return (Bounds.Min.Z + Bounds.Max.Z) * 0.5f;
}

int32 FLandscapeHeightmapExtractor::ClampToPowerOfTwo(int32 Value, int32 MinValue, int32 MaxValue)
{
	Value = FMath::Clamp(Value, MinValue, MaxValue);

	// Round up to next power of 2
	Value--;
	Value |= Value >> 1;
	Value |= Value >> 2;
	Value |= Value >> 4;
	Value |= Value >> 8;
	Value |= Value >> 16;
	Value++;

	return FMath::Clamp(Value, MinValue, MaxValue);
}
