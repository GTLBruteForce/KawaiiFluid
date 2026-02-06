// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUSpawnManager - Thread-safe particle spawn queue manager

#include "GPU/Managers/GPUSpawnManager.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUSpawnManager, Log, All);
DEFINE_LOG_CATEGORY(LogGPUSpawnManager);

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUSpawnManager::FGPUSpawnManager()
	: bIsInitialized(false)
	, MaxParticleCapacity(0)
{
}

FGPUSpawnManager::~FGPUSpawnManager()
{
	Release();
}

//=============================================================================
// Lifecycle
//=============================================================================

void FGPUSpawnManager::Initialize(int32 InMaxParticleCount)
{
	if (InMaxParticleCount <= 0)
	{
		UE_LOG(LogGPUSpawnManager, Warning, TEXT("Initialize called with invalid particle count: %d"), InMaxParticleCount);
		return;
	}

	MaxParticleCapacity = InMaxParticleCount;
	bIsInitialized = true;

	// Initialize source counter cache
	CachedSourceCounts.SetNumZeroed(EGPUParticleSource::MaxSourceCount);

	// Create source counter readback ring buffer
	SourceCounterReadbacks.SetNum(SourceCounterRingBufferSize);
	for (int32 i = 0; i < SourceCounterRingBufferSize; ++i)
	{
		SourceCounterReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("SourceCounterReadback_%d"), i));
	}
	SourceCounterWriteIndex = 0;
	SourceCounterReadIndex = 0;
	SourceCounterPendingCount = 0;

	UE_LOG(LogGPUSpawnManager, Log, TEXT("GPUSpawnManager initialized with capacity: %d, MaxSourceCount: %d"),
		MaxParticleCapacity, EGPUParticleSource::MaxSourceCount);
}

void FGPUSpawnManager::Release()
{
	{
		FScopeLock Lock(&SpawnLock);
		PendingSpawnRequests.Empty();
		ActiveSpawnRequests.Empty();
		bHasPendingSpawnRequests.store(false);
	}

	{
		FScopeLock Lock(&GPUDespawnLock);
		PendingGPUBrushDespawns.Empty();
		ActiveGPUBrushDespawns.Empty();
		PendingGPUSourceDespawns.Empty();
		ActiveGPUSourceDespawns.Empty();
		PendingGPUOldestSpawnCount = 0;
		ActiveGPUOldestSpawnCount = 0;
		PendingGPUExplicitRemoveCount = 0;
		ActiveGPUExplicitRemoveCount = 0;
		bHasPendingGPUDespawnRequests.store(false);
	}

	// Release source counter resources
	{
		FScopeLock Lock(&SourceCountLock);
		SourceCounterBuffer.SafeRelease();
		for (FRHIGPUBufferReadback* Readback : SourceCounterReadbacks)
		{
			if (Readback)
			{
				delete Readback;
			}
		}
		SourceCounterReadbacks.Empty();
		SourceCounterWriteIndex = 0;
		SourceCounterReadIndex = 0;
		SourceCounterPendingCount = 0;
		CachedSourceCounts.Empty();
	}

	// Release stream compaction buffers
	PersistentAliveMaskBuffer.SafeRelease();
	PersistentPrefixSumsBuffer.SafeRelease();
	PersistentBlockSumsBuffer.SafeRelease();
	PersistentCompactedBuffer[0].SafeRelease();
	PersistentCompactedBuffer[1].SafeRelease();
	CompactedBufferIndex = 0;
	StreamCompactionCapacity = 0;

	// Release oldest despawn histogram buffers
	PersistentIDHistogramBuffer.SafeRelease();
	PersistentOldestThresholdBuffer.SafeRelease();
	PersistentBoundaryCounterBuffer.SafeRelease();

	NextParticleID.store(0);
	bIsInitialized = false;
	MaxParticleCapacity = 0;

	UE_LOG(LogGPUSpawnManager, Log, TEXT("GPUSpawnManager released (all despawn state cleared)"));
}

//=============================================================================
// Thread-Safe Public API
//=============================================================================

void FGPUSpawnManager::AddSpawnRequest(const FVector3f& Position, const FVector3f& Velocity, float Mass)
{
	FScopeLock Lock(&SpawnLock);

	FGPUSpawnRequest Request;
	Request.Position = Position;
	Request.Velocity = Velocity;
	Request.Mass = Mass;
	Request.Radius = DefaultSpawnRadius;

	PendingSpawnRequests.Add(Request);
	bHasPendingSpawnRequests.store(true);

	UE_LOG(LogGPUSpawnManager, Verbose, TEXT("AddSpawnRequest: Pos=(%.2f, %.2f, %.2f), Vel=(%.2f, %.2f, %.2f)"),
		Position.X, Position.Y, Position.Z, Velocity.X, Velocity.Y, Velocity.Z);
}

void FGPUSpawnManager::AddSpawnRequests(const TArray<FGPUSpawnRequest>& Requests)
{
	if (Requests.Num() == 0)
	{
		return;
	}

	FScopeLock Lock(&SpawnLock);

	PendingSpawnRequests.Append(Requests);
	bHasPendingSpawnRequests.store(true);

	UE_LOG(LogGPUSpawnManager, Verbose, TEXT("AddSpawnRequests: Added %d requests (total pending: %d)"),
		Requests.Num(), PendingSpawnRequests.Num());
}

void FGPUSpawnManager::ClearSpawnRequests()
{
	FScopeLock Lock(&SpawnLock);
	PendingSpawnRequests.Empty();
	bHasPendingSpawnRequests.store(false);
}

int32 FGPUSpawnManager::GetPendingSpawnCount() const
{
	FScopeLock Lock(&SpawnLock);
	return PendingSpawnRequests.Num();
}

int32 FGPUSpawnManager::CancelPendingSpawnsForSource(int32 SourceID)
{
	FScopeLock Lock(&SpawnLock);

	const int32 OriginalCount = PendingSpawnRequests.Num();

	// Remove all pending spawn requests with matching SourceID
	PendingSpawnRequests.RemoveAll([SourceID](const FGPUSpawnRequest& Request)
	{
		return Request.SourceID == SourceID;
	});

	const int32 RemovedCount = OriginalCount - PendingSpawnRequests.Num();

	if (PendingSpawnRequests.Num() == 0)
	{
		bHasPendingSpawnRequests.store(false);
	}

	if (RemovedCount > 0)
	{
		UE_LOG(LogGPUSpawnManager, Log, TEXT("CancelPendingSpawnsForSource: Cancelled %d pending spawns for SourceID=%d"),
			RemovedCount, SourceID);
	}

	return RemovedCount;
}

//=============================================================================
// GPU-Driven Despawn API
//=============================================================================

void FGPUSpawnManager::AddGPUDespawnBrushRequest(const FVector3f& Center, float Radius)
{
	FScopeLock Lock(&GPUDespawnLock);
	PendingGPUBrushDespawns.Emplace(Center, Radius);
	bHasPendingGPUDespawnRequests.store(true);
}

void FGPUSpawnManager::AddGPUDespawnSourceRequest(int32 SourceID)
{
	if (SourceID < 0 || SourceID >= EGPUParticleSource::MaxSourceCount)
	{
		return;
	}

	FScopeLock Lock(&GPUDespawnLock);

	// Deduplicate SourceIDs within same frame
	if (!PendingGPUSourceDespawns.Contains(SourceID))
	{
		PendingGPUSourceDespawns.Add(SourceID);
	}
	bHasPendingGPUDespawnRequests.store(true);
}

void FGPUSpawnManager::AddGPUDespawnOldestRequest(int32 IncomingSpawnCount)
{
	if (IncomingSpawnCount <= 0)
	{
		return;
	}

	FScopeLock Lock(&GPUDespawnLock);
	PendingGPUOldestSpawnCount += IncomingSpawnCount;
	bHasPendingGPUDespawnRequests.store(true);
}

void FGPUSpawnManager::AddGPUExplicitRemoveOldestRequest(int32 RemoveCount)
{
	if (RemoveCount <= 0)
	{
		return;
	}

	FScopeLock Lock(&GPUDespawnLock);
	PendingGPUExplicitRemoveCount += RemoveCount;
	bHasPendingGPUDespawnRequests.store(true);
}

bool FGPUSpawnManager::SwapGPUDespawnBuffers()
{
	FScopeLock Lock(&GPUDespawnLock);

	ActiveGPUBrushDespawns = MoveTemp(PendingGPUBrushDespawns);
	PendingGPUBrushDespawns.Empty();

	ActiveGPUSourceDespawns = MoveTemp(PendingGPUSourceDespawns);
	PendingGPUSourceDespawns.Empty();

	ActiveGPUOldestSpawnCount = PendingGPUOldestSpawnCount;
	PendingGPUOldestSpawnCount = 0;

	ActiveGPUExplicitRemoveCount = PendingGPUExplicitRemoveCount;
	PendingGPUExplicitRemoveCount = 0;

	bHasPendingGPUDespawnRequests.store(false);

	const bool bHasAny = (ActiveGPUBrushDespawns.Num() > 0 ||
		ActiveGPUSourceDespawns.Num() > 0 ||
		ActiveGPUOldestSpawnCount > 0 ||
		ActiveGPUExplicitRemoveCount > 0);

	if (bHasAny)
	{
		UE_LOG(LogGPUSpawnManager, Verbose, TEXT("SwapGPUDespawnBuffers: Brush=%d, Source=%d, RecycleSpawn=%d, ExplicitRemove=%d"),
			ActiveGPUBrushDespawns.Num(), ActiveGPUSourceDespawns.Num(), ActiveGPUOldestSpawnCount, ActiveGPUExplicitRemoveCount);
	}

	return bHasAny;
}

void FGPUSpawnManager::AddGPUDespawnPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef& InOutParticleBuffer,
	int32& InOutParticleCount,
	int32 NextParticleIDHint,
	FRDGBufferRef ParticleCountBuffer)
{
	// Safety: no particles to process
	if (InOutParticleCount <= 0)
	{
		ActiveGPUBrushDespawns.Empty();
		ActiveGPUSourceDespawns.Empty();
		ActiveGPUOldestSpawnCount = 0;
		ActiveGPUExplicitRemoveCount = 0;
		return;
	}

	const bool bHasBrush = ActiveGPUBrushDespawns.Num() > 0;
	const bool bHasSource = ActiveGPUSourceDespawns.Num() > 0;
	const bool bHasOldest = ActiveGPUOldestSpawnCount > 0 || ActiveGPUExplicitRemoveCount > 0;

	if (!bHasBrush && !bHasSource && !bHasOldest)
	{
		return;
	}

	// Use MaxParticleCapacity for all dispatch/element counts.
	// CPU's InOutParticleCount can be STALE (readback lag) and LOWER than the actual GPU count.
	// Using MaxParticleCapacity guarantees full coverage — OOB threads write AliveMask=0 via
	// each mark shader's bounds check against ParticleCountBuffer[6] (GPU-accurate).
	const int32 DispatchCount = MaxParticleCapacity;

	FRDGBufferRef AliveMaskBuffer;
	FRDGBufferRef PrefixSumsBuffer;
	FRDGBufferRef BlockSumsBuffer;
	FRDGBufferUAVRef SourceCounterUAV;
	FGlobalShaderMap* ShaderMap;
	FRDGBufferSRVRef ParticleCountSRV;

	{
		RDG_EVENT_SCOPE(GraphBuilder, "GPUDespawn_Setup");

		EnsureStreamCompactionBuffers(GraphBuilder, DispatchCount);
		ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

		AliveMaskBuffer = GraphBuilder.RegisterExternalBuffer(PersistentAliveMaskBuffer, TEXT("AliveMask"));
		PrefixSumsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentPrefixSumsBuffer, TEXT("PrefixSums"));
		BlockSumsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentBlockSumsBuffer, TEXT("BlockSums"));
		SourceCounterUAV = RegisterSourceCounterUAV(GraphBuilder);

		// ParticleCountBuffer SRV for bounds checking in all mark shaders
		ParticleCountSRV = GraphBuilder.CreateSRV(ParticleCountBuffer);

		// Step 1: Clear AliveMask to 1 (all alive)
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AliveMaskBuffer), 1u);
	}

	// Step 2: Brush mark pass
	if (bHasBrush)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "GPUDespawn_MarkBrush");

		FRDGBufferRef BrushRequestBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUDespawnBrushRequests"),
			sizeof(FGPUDespawnBrushRequest),
			ActiveGPUBrushDespawns.Num(),
			ActiveGPUBrushDespawns.GetData(),
			ActiveGPUBrushDespawns.Num() * sizeof(FGPUDespawnBrushRequest),
			ERDGInitialDataFlags::None
		);

		TShaderMapRef<FMarkDespawnByBrushCS> BrushCS(ShaderMap);
		FMarkDespawnByBrushCS::FParameters* BrushParams = GraphBuilder.AllocParameters<FMarkDespawnByBrushCS::FParameters>();
		BrushParams->BrushRequests = GraphBuilder.CreateSRV(BrushRequestBuffer);
		BrushParams->Particles = GraphBuilder.CreateSRV(InOutParticleBuffer);
		BrushParams->OutAliveMask = GraphBuilder.CreateUAV(AliveMaskBuffer);
		BrushParams->ParticleCountBuffer = ParticleCountSRV;
		BrushParams->BrushRequestCount = ActiveGPUBrushDespawns.Num();

		const uint32 NumGroups = FMath::DivideAndRoundUp(DispatchCount, FMarkDespawnByBrushCS::ThreadGroupSize);
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::DespawnBrush(%d brushes)", ActiveGPUBrushDespawns.Num()),
			BrushCS, BrushParams, FIntVector(NumGroups, 1, 1));
	}

	// Step 3: Source mark pass
	if (bHasSource)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "GPUDespawn_MarkSource");

		FRDGBufferRef SourceIDBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUDespawnSourceIDs"),
			sizeof(int32),
			ActiveGPUSourceDespawns.Num(),
			ActiveGPUSourceDespawns.GetData(),
			ActiveGPUSourceDespawns.Num() * sizeof(int32),
			ERDGInitialDataFlags::None
		);

		TShaderMapRef<FMarkDespawnBySourceCS> SourceCS(ShaderMap);
		FMarkDespawnBySourceCS::FParameters* SourceParams = GraphBuilder.AllocParameters<FMarkDespawnBySourceCS::FParameters>();
		SourceParams->DespawnSourceIDs = GraphBuilder.CreateSRV(SourceIDBuffer);
		SourceParams->Particles = GraphBuilder.CreateSRV(InOutParticleBuffer);
		SourceParams->OutAliveMask = GraphBuilder.CreateUAV(AliveMaskBuffer);
		SourceParams->ParticleCountBuffer = ParticleCountSRV;
		SourceParams->DespawnSourceIDCount = ActiveGPUSourceDespawns.Num();

		const uint32 NumGroups = FMath::DivideAndRoundUp(DispatchCount, FMarkDespawnBySourceCS::ThreadGroupSize);
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::DespawnSource(%d sources)", ActiveGPUSourceDespawns.Num()),
			SourceCS, SourceParams, FIntVector(NumGroups, 1, 1));
	}

	// Step 4: Oldest mark pass (3-pass histogram)
	if (bHasOldest)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "GPUDespawn_MarkOldest");

		// Compute IDShiftBits = max(0, floor(log2(max(1, NextParticleID))) - 7)
		const int32 MaxID = FMath::Max(1, NextParticleIDHint);
		const int32 Log2MaxID = FMath::FloorLog2(MaxID);
		const int32 IDShiftBits = FMath::Max(0, Log2MaxID - 7);

		// Ensure histogram buffers exist
		FRDGBufferRef IDHistogramBuffer;
		if (!PersistentIDHistogramBuffer.IsValid())
		{
			FRDGBufferDesc HistDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 256);
			IDHistogramBuffer = GraphBuilder.CreateBuffer(HistDesc, TEXT("IDHistogram"));
			PersistentIDHistogramBuffer = GraphBuilder.ConvertToExternalBuffer(IDHistogramBuffer);
		}
		else
		{
			IDHistogramBuffer = GraphBuilder.RegisterExternalBuffer(PersistentIDHistogramBuffer, TEXT("IDHistogram"));
		}

		FRDGBufferRef OldestThresholdBuffer;
		if (!PersistentOldestThresholdBuffer.IsValid())
		{
			FRDGBufferDesc ThreshDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2);
			OldestThresholdBuffer = GraphBuilder.CreateBuffer(ThreshDesc, TEXT("OldestThreshold"));
			PersistentOldestThresholdBuffer = GraphBuilder.ConvertToExternalBuffer(OldestThresholdBuffer);
		}
		else
		{
			OldestThresholdBuffer = GraphBuilder.RegisterExternalBuffer(PersistentOldestThresholdBuffer, TEXT("OldestThreshold"));
		}

		FRDGBufferRef BoundaryCounterBuffer;
		if (!PersistentBoundaryCounterBuffer.IsValid())
		{
			FRDGBufferDesc CounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
			BoundaryCounterBuffer = GraphBuilder.CreateBuffer(CounterDesc, TEXT("BoundaryCounter"));
			PersistentBoundaryCounterBuffer = GraphBuilder.ConvertToExternalBuffer(BoundaryCounterBuffer);
		}
		else
		{
			BoundaryCounterBuffer = GraphBuilder.RegisterExternalBuffer(PersistentBoundaryCounterBuffer, TEXT("BoundaryCounter"));
		}

		// Clear histogram and boundary counter
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(IDHistogramBuffer), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BoundaryCounterBuffer), 0u);

		// Pass 1: BuildIDHistogramCS
		{
			TShaderMapRef<FBuildIDHistogramCS> HistCS(ShaderMap);
			FBuildIDHistogramCS::FParameters* HistParams = GraphBuilder.AllocParameters<FBuildIDHistogramCS::FParameters>();
			HistParams->Particles = GraphBuilder.CreateSRV(InOutParticleBuffer);
			HistParams->IDHistogram = GraphBuilder.CreateUAV(IDHistogramBuffer);
			HistParams->ParticleCountBuffer = ParticleCountSRV;
			HistParams->IDShiftBits = IDShiftBits;

			const uint32 NumGroups = FMath::DivideAndRoundUp(DispatchCount, FBuildIDHistogramCS::ThreadGroupSize);
			FComputeShaderUtils::AddPass(GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::DespawnOldest_Histogram"),
				HistCS, HistParams, FIntVector(NumGroups, 1, 1));
		}

		// Pass 2: FindOldestThresholdCS - GPU computes RemoveCount from exact counts
		{
			TShaderMapRef<FFindOldestThresholdCS> ThreshCS(ShaderMap);
			FFindOldestThresholdCS::FParameters* ThreshParams = GraphBuilder.AllocParameters<FFindOldestThresholdCS::FParameters>();
			ThreshParams->IDHistogram = GraphBuilder.CreateUAV(IDHistogramBuffer);
			ThreshParams->OldestThreshold = GraphBuilder.CreateUAV(OldestThresholdBuffer);
			ThreshParams->ParticleCountBuffer = ParticleCountSRV;
			ThreshParams->IncomingSpawnCount = ActiveGPUOldestSpawnCount;
			ThreshParams->MaxParticleCount = MaxParticleCapacity;
			ThreshParams->ExplicitRemoveCount = ActiveGPUExplicitRemoveCount;

			FComputeShaderUtils::AddPass(GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::DespawnOldest_Threshold(spawn=%d,explicit=%d,max=%d)",
					ActiveGPUOldestSpawnCount, ActiveGPUExplicitRemoveCount, MaxParticleCapacity),
				ThreshCS, ThreshParams, FIntVector(1, 1, 1));
		}

		// Pass 3: MarkOldestParticlesCS
		{
			TShaderMapRef<FMarkOldestParticlesCS> MarkCS(ShaderMap);
			FMarkOldestParticlesCS::FParameters* MarkParams = GraphBuilder.AllocParameters<FMarkOldestParticlesCS::FParameters>();
			MarkParams->Particles = GraphBuilder.CreateSRV(InOutParticleBuffer);
			MarkParams->OldestThreshold = GraphBuilder.CreateUAV(OldestThresholdBuffer);
			MarkParams->OutAliveMask = GraphBuilder.CreateUAV(AliveMaskBuffer);
			MarkParams->BoundaryCounter = GraphBuilder.CreateUAV(BoundaryCounterBuffer);
			MarkParams->ParticleCountBuffer = ParticleCountSRV;
			MarkParams->IDShiftBits = IDShiftBits;

			const uint32 NumGroups = FMath::DivideAndRoundUp(DispatchCount, FMarkOldestParticlesCS::ThreadGroupSize);
			FComputeShaderUtils::AddPass(GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::DespawnOldest_Mark"),
				MarkCS, MarkParams, FIntVector(NumGroups, 1, 1));
		}
	}

	// Step 5: UpdateSourceCountersDespawnCS
	{
		RDG_EVENT_SCOPE(GraphBuilder, "GPUDespawn_UpdateSourceCounters");

		TShaderMapRef<FUpdateSourceCountersDespawnCS> UpdateCS(ShaderMap);
		FUpdateSourceCountersDespawnCS::FParameters* UpdateParams = GraphBuilder.AllocParameters<FUpdateSourceCountersDespawnCS::FParameters>();
		UpdateParams->AliveMask = GraphBuilder.CreateSRV(AliveMaskBuffer);
		UpdateParams->Particles = GraphBuilder.CreateSRV(InOutParticleBuffer);
		UpdateParams->SourceCounters = SourceCounterUAV;
		UpdateParams->ParticleCountBuffer = ParticleCountSRV;
		UpdateParams->MaxSourceCount = EGPUParticleSource::MaxSourceCount;

		const uint32 NumGroups = FMath::DivideAndRoundUp(DispatchCount, FUpdateSourceCountersDespawnCS::ThreadGroupSize);
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::DespawnUpdateSourceCounters"),
			UpdateCS, UpdateParams, FIntVector(NumGroups, 1, 1));
	}

	// Step 6-7: PrefixSum + Compact (reuse existing stream compaction pipeline)
	const int32 PrefixSumBlockNumGroups = FMath::DivideAndRoundUp(DispatchCount, FPrefixSumBlockCS_RDG::ThreadGroupSize);

	{
		RDG_EVENT_SCOPE(GraphBuilder, "GPUDespawn_PrefixSum");

		TShaderMapRef<FPrefixSumBlockCS_RDG> PrefixSumBlock(ShaderMap);
		FPrefixSumBlockCS_RDG::FParameters* PrefixSumBlockParameters = GraphBuilder.AllocParameters<FPrefixSumBlockCS_RDG::FParameters>();
		PrefixSumBlockParameters->MarkedFlags = GraphBuilder.CreateSRV(AliveMaskBuffer);
		PrefixSumBlockParameters->PrefixSums = GraphBuilder.CreateUAV(PrefixSumsBuffer);
		PrefixSumBlockParameters->BlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);
		PrefixSumBlockParameters->ElementCount = DispatchCount;

		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::GPUDespawn_PrefixSumBlock"),
			PrefixSumBlock, PrefixSumBlockParameters,
			FIntVector(PrefixSumBlockNumGroups, 1, 1));

		TShaderMapRef<FScanBlockSumsCS_RDG> ScanBlockSums(ShaderMap);
		FScanBlockSumsCS_RDG::FParameters* ScanBlockSumsParameters = GraphBuilder.AllocParameters<FScanBlockSumsCS_RDG::FParameters>();
		ScanBlockSumsParameters->BlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);
		ScanBlockSumsParameters->BlockCount = PrefixSumBlockNumGroups;

		const int32 ScanBlockSumsNumGroups = FMath::DivideAndRoundUp(PrefixSumBlockNumGroups, FScanBlockSumsCS_RDG::ThreadGroupSize);
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::GPUDespawn_ScanBlockSums"),
			ScanBlockSums, ScanBlockSumsParameters,
			FIntVector(ScanBlockSumsNumGroups, 1, 1));

		TShaderMapRef<FAddBlockOffsetsCS_RDG> AddBlockOffsets(ShaderMap);
		FAddBlockOffsetsCS_RDG::FParameters* AddBlockOffsetsParameters = GraphBuilder.AllocParameters<FAddBlockOffsetsCS_RDG::FParameters>();
		AddBlockOffsetsParameters->PrefixSums = GraphBuilder.CreateUAV(PrefixSumsBuffer);
		AddBlockOffsetsParameters->BlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);
		AddBlockOffsetsParameters->ElementCount = DispatchCount;

		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::GPUDespawn_AddBlockOffsets"),
			AddBlockOffsets, AddBlockOffsetsParameters,
			FIntVector(PrefixSumBlockNumGroups, 1, 1));
	}

	{
		RDG_EVENT_SCOPE(GraphBuilder, "GPUDespawn_Compact");

		FRDGBufferRef CompactedParticlesBuffer = GraphBuilder.RegisterExternalBuffer(
			PersistentCompactedBuffer[CompactedBufferIndex], TEXT("CompactedParticlesGPU"));
		CompactedBufferIndex = 1 - CompactedBufferIndex;

		TShaderMapRef<FCompactParticlesCS_RDG> Compact(ShaderMap);
		FCompactParticlesCS_RDG::FParameters* CompactParameters = GraphBuilder.AllocParameters<FCompactParticlesCS_RDG::FParameters>();
		CompactParameters->Particles = GraphBuilder.CreateSRV(InOutParticleBuffer);
		CompactParameters->MarkedFlags = GraphBuilder.CreateSRV(AliveMaskBuffer);
		CompactParameters->PrefixSums = GraphBuilder.CreateSRV(PrefixSumsBuffer);
		CompactParameters->CompactedParticles = GraphBuilder.CreateUAV(CompactedParticlesBuffer);
		CompactParameters->ParticleCount = DispatchCount;

		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::GPUDespawn_Compact"),
			Compact, CompactParameters,
			FIntVector(PrefixSumBlockNumGroups, 1, 1));

		InOutParticleBuffer = CompactedParticlesBuffer;
	}

	// Clear active requests
	ActiveGPUBrushDespawns.Empty();
	ActiveGPUSourceDespawns.Empty();
	ActiveGPUOldestSpawnCount = 0;
	ActiveGPUExplicitRemoveCount = 0;
}

//=============================================================================
// Render Thread API
//=============================================================================

void FGPUSpawnManager::SwapBuffers()
{
	FScopeLock Lock(&SpawnLock);

	// Move pending requests to active buffer
	ActiveSpawnRequests = MoveTemp(PendingSpawnRequests);
	PendingSpawnRequests.Empty();
	bHasPendingSpawnRequests.store(false);
}

void FGPUSpawnManager::AddSpawnParticlesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef ParticleCounterUAV,
	int32 MaxParticleCount)
{
	if (ActiveSpawnRequests.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSpawnParticlesCS> ComputeShader(ShaderMap);

	// Create spawn request buffer
	// IMPORTANT: Do NOT use NoCopy - SpawnRequests is temporary data that may be
	// invalidated before RDG pass executes. RDG must copy the data.
	FRDGBufferRef SpawnRequestBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidSpawnRequests"),
		sizeof(FGPUSpawnRequest),
		ActiveSpawnRequests.Num(),
		ActiveSpawnRequests.GetData(),
		ActiveSpawnRequests.Num() * sizeof(FGPUSpawnRequest),
		ERDGInitialDataFlags::None
	);

	// Get or create source counter buffer UAV
	FRDGBufferUAVRef SourceCounterUAV = RegisterSourceCounterUAV(GraphBuilder);

	FSpawnParticlesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSpawnParticlesCS::FParameters>();
	PassParameters->SpawnRequests = GraphBuilder.CreateSRV(SpawnRequestBuffer);
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCounter = ParticleCounterUAV;
	PassParameters->SourceCounters = SourceCounterUAV;
	PassParameters->SpawnRequestCount = ActiveSpawnRequests.Num();
	PassParameters->MaxParticleCount = MaxParticleCount;
	PassParameters->NextParticleID = NextParticleID.load();
	PassParameters->MaxSourceCount = EGPUParticleSource::MaxSourceCount;
	PassParameters->DefaultRadius = DefaultSpawnRadius;
	PassParameters->DefaultMass = DefaultSpawnMass;

	const uint32 NumGroups = FMath::DivideAndRoundUp(ActiveSpawnRequests.Num(), FSpawnParticlesCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SpawnParticles(%d)", ActiveSpawnRequests.Num()),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);

	// UE_LOG(LogGPUSpawnManager, Verbose, TEXT("SpawnParticlesPass: Spawning %d particles (NextID: %d)"),
	// 	ActiveSpawnRequests.Num(), NextParticleID.load());
}


void FGPUSpawnManager::OnSpawnComplete(int32 SpawnedCount)
{
	if (SpawnedCount > 0)
	{
		NextParticleID.fetch_add(SpawnedCount);
	}
}

//=============================================================================
// Source Counter API (Per-Component Particle Count Tracking)
//=============================================================================

FRDGBufferUAVRef FGPUSpawnManager::RegisterSourceCounterUAV(FRDGBuilder& GraphBuilder)
{
	// Create persistent buffer if not exists
	if (!SourceCounterBuffer.IsValid())
	{
		// Allocate on render thread via RDG
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), EGPUParticleSource::MaxSourceCount);
		FRDGBufferRef TempBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("GPUSourceCounters"));

		// Initialize to zero
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TempBuffer), 0u);

		// Extract to persistent buffer
		GraphBuilder.QueueBufferExtraction(TempBuffer, &SourceCounterBuffer);

		UE_LOG(LogGPUSpawnManager, Log, TEXT("Created SourceCounterBuffer with %d slots"), EGPUParticleSource::MaxSourceCount);

		return GraphBuilder.CreateUAV(TempBuffer);
	}

	// Register existing buffer
	FRDGBufferRef RegisteredBuffer = GraphBuilder.RegisterExternalBuffer(SourceCounterBuffer, TEXT("GPUSourceCounters"));
	return GraphBuilder.CreateUAV(RegisteredBuffer);
}

int32 FGPUSpawnManager::GetParticleCountForSource(int32 SourceID) const
{
	// Check SourceID range
	if (SourceID < 0 || SourceID >= EGPUParticleSource::MaxSourceCount)
	{
		return -1;  // Invalid SourceID
	}

	// If GPU buffer not yet created, no data available
	if (!SourceCounterBuffer.IsValid())
	{
		return -1;  // Data not ready
	}

	FScopeLock Lock(&SourceCountLock);
	if (SourceID < CachedSourceCounts.Num())
	{
		return CachedSourceCounts[SourceID];
	}
	return -1;
}

TArray<int32> FGPUSpawnManager::GetAllSourceCounts() const
{
	FScopeLock Lock(&SourceCountLock);
	return CachedSourceCounts;
}

void FGPUSpawnManager::EnqueueSourceCounterReadback(FRHICommandListImmediate& RHICmdList)
{
	if (!SourceCounterBuffer.IsValid())
	{
		return;
	}

	FRHIBuffer* SourceBuffer = SourceCounterBuffer->GetRHI();
	if (!SourceBuffer)
	{
		return;
	}

	// Ring buffer full - skip this frame
	if (SourceCounterPendingCount >= SourceCounterRingBufferSize)
	{
		UE_LOG(LogGPUSpawnManager, Verbose, TEXT("SourceCounter ring buffer full, skipping enqueue"));
		return;
	}

	// Enqueue to write slot
	if (SourceCounterReadbacks.IsValidIndex(SourceCounterWriteIndex) && SourceCounterReadbacks[SourceCounterWriteIndex])
	{
		const uint32 CopySize = EGPUParticleSource::MaxSourceCount * sizeof(uint32);

		// State transition for readback
		RHICmdList.Transition(FRHITransitionInfo(SourceBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
		SourceCounterReadbacks[SourceCounterWriteIndex]->EnqueueCopy(RHICmdList, SourceBuffer, CopySize);
		RHICmdList.Transition(FRHITransitionInfo(SourceBuffer, ERHIAccess::CopySrc, ERHIAccess::UAVCompute));

		// Advance write index
		SourceCounterWriteIndex = (SourceCounterWriteIndex + 1) % SourceCounterRingBufferSize;
		++SourceCounterPendingCount;
	}
}

void FGPUSpawnManager::ProcessSourceCounterReadback()
{
	// Nothing pending
	if (SourceCounterPendingCount <= 0)
	{
		return;
	}

	// Check if oldest readback is ready
	if (!SourceCounterReadbacks.IsValidIndex(SourceCounterReadIndex) ||
		!SourceCounterReadbacks[SourceCounterReadIndex] ||
		!SourceCounterReadbacks[SourceCounterReadIndex]->IsReady())
	{
		return;
	}

	const uint32 CopySize = EGPUParticleSource::MaxSourceCount * sizeof(uint32);
	const uint32* Data = static_cast<const uint32*>(SourceCounterReadbacks[SourceCounterReadIndex]->Lock(CopySize));

	if (Data)
	{
		FScopeLock Lock(&SourceCountLock);
		CachedSourceCounts.SetNum(EGPUParticleSource::MaxSourceCount);
		for (int32 i = 0; i < EGPUParticleSource::MaxSourceCount; ++i)
		{
			CachedSourceCounts[i] = static_cast<int32>(Data[i]);
		}
	}

	SourceCounterReadbacks[SourceCounterReadIndex]->Unlock();

	// Advance read index
	SourceCounterReadIndex = (SourceCounterReadIndex + 1) % SourceCounterRingBufferSize;
	--SourceCounterPendingCount;
}

void FGPUSpawnManager::ClearSourceCounters(FRDGBuilder& GraphBuilder)
{
	if (!SourceCounterBuffer.IsValid())
	{
		return;
	}

	FRDGBufferRef RegisteredBuffer = GraphBuilder.RegisterExternalBuffer(SourceCounterBuffer, TEXT("GPUSourceCounters"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RegisteredBuffer), 0u);

	// Also clear cached counts
	{
		FScopeLock Lock(&SourceCountLock);
		for (int32& Count : CachedSourceCounts)
		{
			Count = 0;
		}
	}

	UE_LOG(LogGPUSpawnManager, Log, TEXT("Cleared all source counters"));
}

void FGPUSpawnManager::InitializeSourceCountersFromParticles(const TArray<FGPUFluidParticle>& Particles)
{
	if (Particles.Num() == 0)
	{
		return;
	}

	// Count particles by SourceID
	TArray<int32> SourceCounts;
	SourceCounts.SetNumZeroed(EGPUParticleSource::MaxSourceCount);

	for (const FGPUFluidParticle& Particle : Particles)
	{
		const int32 SourceID = Particle.SourceID;
		if (SourceID >= 0 && SourceID < EGPUParticleSource::MaxSourceCount)
		{
			SourceCounts[SourceID]++;
		}
	}

	// Update CPU cache immediately
	{
		FScopeLock Lock(&SourceCountLock);
		CachedSourceCounts = SourceCounts;
	}

	// Create or update GPU buffer
	TArray<uint32> CountsUint32;
	CountsUint32.SetNumUninitialized(EGPUParticleSource::MaxSourceCount);
	for (int32 i = 0; i < EGPUParticleSource::MaxSourceCount; ++i)
	{
		CountsUint32[i] = static_cast<uint32>(SourceCounts[i]);
	}

	FGPUSpawnManager* Self = this;
	TArray<uint32> CountsCopy = CountsUint32;

	ENQUEUE_RENDER_COMMAND(InitializeSourceCounters)(
		[Self, CountsCopy](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("InitializeSourceCounters"));

			FRDGBufferRef CounterBuffer;
			if (!Self->SourceCounterBuffer.IsValid())
			{
				// Create buffer with initial data if not exists
				CounterBuffer = CreateStructuredBuffer(
					GraphBuilder,
					TEXT("GPUSourceCounters"),
					sizeof(uint32),
					EGPUParticleSource::MaxSourceCount,
					CountsCopy.GetData(),
					CountsCopy.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);
				GraphBuilder.QueueBufferExtraction(CounterBuffer, &Self->SourceCounterBuffer);
			}
			else
			{
				// Buffer exists, just upload data
				CounterBuffer = GraphBuilder.RegisterExternalBuffer(Self->SourceCounterBuffer, TEXT("GPUSourceCounters"));
				GraphBuilder.QueueBufferUpload(CounterBuffer, CountsCopy.GetData(), CountsCopy.Num() * sizeof(uint32));
			}

			GraphBuilder.Execute();
		}
	);

	FlushRenderingCommands();

	// Log source counts
	int32 TotalCounted = 0;
	for (int32 i = 0; i < EGPUParticleSource::MaxSourceCount; ++i)
	{
		if (CachedSourceCounts[i] > 0)
		{
			UE_LOG(LogGPUSpawnManager, Log, TEXT("InitializeSourceCounters: SourceID %d = %d particles"), i, CachedSourceCounts[i]);
			TotalCounted += CachedSourceCounts[i];
		}
	}
	UE_LOG(LogGPUSpawnManager, Log, TEXT("InitializeSourceCounters: Total %d particles from %d input"), TotalCounted, Particles.Num());
}

//=============================================================================
// Stream Compaction Buffers (Persistent)
//=============================================================================

void FGPUSpawnManager::EnsureStreamCompactionBuffers(FRDGBuilder& GraphBuilder, int32 RequiredCapacity)
{
	// Already allocated with sufficient capacity
	if (PersistentAliveMaskBuffer.IsValid() && StreamCompactionCapacity >= RequiredCapacity)
	{
		return;
	}

	// Use MaxParticleCapacity for fixed-size allocation (no dynamic resize)
	const int32 Capacity = FMath::Max(RequiredCapacity, MaxParticleCapacity);
	const int32 BlockCount = FMath::DivideAndRoundUp(Capacity, static_cast<int32>(FPrefixSumBlockCS_RDG::ThreadGroupSize));

	// Create AliveMask buffer (uint32 × Capacity)
	FRDGBufferDesc AliveMaskDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Capacity);
	FRDGBufferRef AliveMask = GraphBuilder.CreateBuffer(AliveMaskDesc, TEXT("PersistentAliveMask"));
	PersistentAliveMaskBuffer = GraphBuilder.ConvertToExternalBuffer(AliveMask);

	// Create PrefixSums buffer (uint32 × Capacity)
	FRDGBufferDesc PrefixSumsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Capacity);
	FRDGBufferRef PrefixSums = GraphBuilder.CreateBuffer(PrefixSumsDesc, TEXT("PersistentPrefixSums"));
	PersistentPrefixSumsBuffer = GraphBuilder.ConvertToExternalBuffer(PrefixSums);

	// Create BlockSums buffer (uint32 × BlockCount)
	FRDGBufferDesc BlockSumsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BlockCount);
	FRDGBufferRef BlockSums = GraphBuilder.CreateBuffer(BlockSumsDesc, TEXT("PersistentBlockSums"));
	PersistentBlockSumsBuffer = GraphBuilder.ConvertToExternalBuffer(BlockSums);

	// Create double-buffered CompactedParticles buffers (FGPUFluidParticle × Capacity)
	FRDGBufferDesc CompactedDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), Capacity);
	FRDGBufferRef Compacted0 = GraphBuilder.CreateBuffer(CompactedDesc, TEXT("PersistentCompacted0"));
	FRDGBufferRef Compacted1 = GraphBuilder.CreateBuffer(CompactedDesc, TEXT("PersistentCompacted1"));
	PersistentCompactedBuffer[0] = GraphBuilder.ConvertToExternalBuffer(Compacted0);
	PersistentCompactedBuffer[1] = GraphBuilder.ConvertToExternalBuffer(Compacted1);

	StreamCompactionCapacity = Capacity;

	const int32 CompactedSize = Capacity * sizeof(FGPUFluidParticle) * 2;  // Double buffer
	UE_LOG(LogGPUSpawnManager, Log, TEXT("EnsureStreamCompactionBuffers: Allocated %d capacity (%d KB total)"),
		Capacity, (Capacity * 2 * 4 + BlockCount * 4 + CompactedSize) / 1024);
}

//=============================================================================
// Stream Compaction Buffer Accessors
//=============================================================================

FRDGBufferSRVRef FGPUSpawnManager::GetLastPrefixSumsSRV(FRDGBuilder& GraphBuilder) const
{
	check(PersistentPrefixSumsBuffer.IsValid());
	FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(PersistentPrefixSumsBuffer, TEXT("DespawnPrefixSums"));
	return GraphBuilder.CreateSRV(Buffer);
}

FRDGBufferSRVRef FGPUSpawnManager::GetLastAliveMaskSRV(FRDGBuilder& GraphBuilder) const
{
	check(PersistentAliveMaskBuffer.IsValid());
	FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(PersistentAliveMaskBuffer, TEXT("DespawnAliveMask"));
	return GraphBuilder.CreateSRV(Buffer);
}
