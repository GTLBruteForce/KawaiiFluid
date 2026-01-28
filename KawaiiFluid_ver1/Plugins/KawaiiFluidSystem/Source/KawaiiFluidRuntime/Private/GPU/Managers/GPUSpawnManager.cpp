// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUSpawnManager - Thread-safe particle spawn queue manager

#include "GPU/Managers/GPUSpawnManager.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include <algorithm>  // std::set_intersection, std::set_difference, std::merge, std::lower_bound

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
		FScopeLock Lock(&DespawnByIDLock);
		PendingDespawnByIDs.Empty();
		ActiveDespawnByIDs.Empty();
		AlreadyRequestedIDs.Empty();
		bHasPendingDespawnByIDRequests.store(false);
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

//=============================================================================
// ID-Based Despawn API
//=============================================================================

void FGPUSpawnManager::AddDespawnByIDRequest(int32 ParticleID)
{
	FScopeLock Lock(&DespawnByIDLock);

	// Check for duplicates using binary search O(log n)
	auto It = std::lower_bound(AlreadyRequestedIDs.GetData(),
		AlreadyRequestedIDs.GetData() + AlreadyRequestedIDs.Num(), ParticleID);

	if (It != AlreadyRequestedIDs.GetData() + AlreadyRequestedIDs.Num() && *It == ParticleID)
	{
		return;  // Already requested
	}

	// Insert at sorted position
	const int32 InsertIdx = It - AlreadyRequestedIDs.GetData();
	AlreadyRequestedIDs.Insert(ParticleID, InsertIdx);

	PendingDespawnByIDs.Add(ParticleID);
	bHasPendingDespawnByIDRequests.store(true);
}

void FGPUSpawnManager::CleanupCompletedRequests(const TArray<int32>& AliveParticleIDs)
{
	FScopeLock Lock(&DespawnByIDLock);

	if (AlreadyRequestedIDs.Num() == 0)
	{
		return;
	}

	if (AliveParticleIDs.Num() == 0)
	{
		// If no particles exist, all requests are completed
		AlreadyRequestedIDs.Empty();
		return;
	}

	// Both arrays are sorted → std::set_intersection O(n+m)
	// AlreadyRequestedIDs ∩ AliveParticleIDs = IDs that are still alive
	TArray<int32> StillPending;
	StillPending.SetNumUninitialized(AlreadyRequestedIDs.Num());  // Allocate maximum size

	int32* OutEnd = std::set_intersection(
		AlreadyRequestedIDs.GetData(), AlreadyRequestedIDs.GetData() + AlreadyRequestedIDs.Num(),
		AliveParticleIDs.GetData(), AliveParticleIDs.GetData() + AliveParticleIDs.Num(),
		StillPending.GetData());

	const int32 ResultCount = OutEnd - StillPending.GetData();
	StillPending.SetNum(ResultCount);  // Adjust to actual size

	const int32 RemovedCount = AlreadyRequestedIDs.Num() - ResultCount;
	AlreadyRequestedIDs = MoveTemp(StillPending);

	if (RemovedCount > 0)
	{
		UE_LOG(LogGPUSpawnManager, Verbose, TEXT("CleanupCompletedRequests: Cleared %d IDs"), RemovedCount);
	}
}

void FGPUSpawnManager::AddDespawnByIDRequests(const TArray<int32>& ParticleIDs)
{
	if (ParticleIDs.Num() == 0)
	{
		return;
	}

	FScopeLock Lock(&DespawnByIDLock);

	const int32 OriginalCount = ParticleIDs.Num();

	// ParticleIDs are already sorted (using sorted readback from RemoveOldestParticles)
	// Extract only new IDs using std::set_difference O(n+m)
	TArray<int32> NewIDs;
	NewIDs.SetNumUninitialized(OriginalCount);  // Allocate maximum size

	int32* DiffEnd = std::set_difference(
		ParticleIDs.GetData(), ParticleIDs.GetData() + ParticleIDs.Num(),
		AlreadyRequestedIDs.GetData(), AlreadyRequestedIDs.GetData() + AlreadyRequestedIDs.Num(),
		NewIDs.GetData());

	const int32 FilteredCount = DiffEnd - NewIDs.GetData();
	NewIDs.SetNum(FilteredCount);  // Adjust to actual size

	if (FilteredCount > 0)
	{
		// Add new IDs to PendingDespawnByIDs
		PendingDespawnByIDs.Append(NewIDs);

		// Merge into AlreadyRequestedIDs (maintaining sorted order)
		TArray<int32> Merged;
		Merged.SetNumUninitialized(AlreadyRequestedIDs.Num() + FilteredCount);

		std::merge(
			AlreadyRequestedIDs.GetData(), AlreadyRequestedIDs.GetData() + AlreadyRequestedIDs.Num(),
			NewIDs.GetData(), NewIDs.GetData() + FilteredCount,
			Merged.GetData());

		AlreadyRequestedIDs = MoveTemp(Merged);
		bHasPendingDespawnByIDRequests.store(true);
	}

	UE_LOG(LogGPUSpawnManager, Log, TEXT("AddDespawnByIDRequests: %d requested, %d new, %d duplicates (pending: %d)"),
		OriginalCount, FilteredCount, OriginalCount - FilteredCount, PendingDespawnByIDs.Num());
}

int32 FGPUSpawnManager::SwapDespawnByIDBuffers()
{
	FScopeLock Lock(&DespawnByIDLock);
	ActiveDespawnByIDs = MoveTemp(PendingDespawnByIDs);
	PendingDespawnByIDs.Empty();
	bHasPendingDespawnByIDRequests.store(false);

	const int32 Count = ActiveDespawnByIDs.Num();

	// Sort for binary search optimization in shader
	if (Count > 0)
	{
		ActiveDespawnByIDs.Sort();
		UE_LOG(LogGPUSpawnManager, Verbose, TEXT("SwapDespawnByIDBuffers: %d IDs ready for processing"), Count);
	}

	return Count;
}

int32 FGPUSpawnManager::GetPendingDespawnByIDCount() const
{
	FScopeLock Lock(&DespawnByIDLock);
	return PendingDespawnByIDs.Num();
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

	UE_LOG(LogGPUSpawnManager, Verbose, TEXT("SpawnParticlesPass: Spawning %d particles (NextID: %d)"),
		ActiveSpawnRequests.Num(), NextParticleID.load());
}

void FGPUSpawnManager::AddDespawnByIDPass(FRDGBuilder& GraphBuilder, FRDGBufferRef& InOutParticleBuffer,
	int32& InOutParticleCount)
{
	if (ActiveDespawnByIDs.Num() == 0)
	{
		return;
	}

	FRDGBufferRef AliveMaskBuffer;
	FRDGBufferRef PrefixSumsBuffer;
	FRDGBufferRef BlockSumsBuffer;
	FRDGBufferRef DespawnIDsBuffer;
	FRDGBufferUAVRef SourceCounterUAV;
	FGlobalShaderMap* ShaderMap;

	{
		RDG_EVENT_SCOPE(GraphBuilder, "Despawn_Setup");

		// Ensure persistent stream compaction buffers are allocated
		EnsureStreamCompactionBuffers(GraphBuilder, InOutParticleCount);

		ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

		// Create sorted ID buffer for GPU binary search (small, varies per frame)
		DespawnIDsBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidDespawnByIDs"),
			sizeof(int32),
			ActiveDespawnByIDs.Num(),
			ActiveDespawnByIDs.GetData(),
			ActiveDespawnByIDs.Num() * sizeof(int32),
			ERDGInitialDataFlags::None
		);

		// Use persistent buffers for stream compaction (avoid per-frame allocation)
		AliveMaskBuffer = GraphBuilder.RegisterExternalBuffer(PersistentAliveMaskBuffer, TEXT("AliveMask"));
		PrefixSumsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentPrefixSumsBuffer, TEXT("PrefixSums"));
		BlockSumsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentBlockSumsBuffer, TEXT("BlockSums"));

		// Get or create source counter buffer UAV for decrementing
		SourceCounterUAV = RegisterSourceCounterUAV(GraphBuilder);
	}

	{
		RDG_EVENT_SCOPE(GraphBuilder, "Despawn_MarkPass");

		TShaderMapRef<FMarkDespawnByIDCS> MarkDespawnByIDCS(ShaderMap);

		// Mark particles for removal by ID matching (binary search)
		FMarkDespawnByIDCS::FParameters* MarkPassParameters = GraphBuilder.AllocParameters<FMarkDespawnByIDCS::FParameters>();
		MarkPassParameters->DespawnIDs = GraphBuilder.CreateSRV(DespawnIDsBuffer);
		MarkPassParameters->Particles = GraphBuilder.CreateSRV(InOutParticleBuffer);
		MarkPassParameters->OutAliveMask = GraphBuilder.CreateUAV(AliveMaskBuffer);
		MarkPassParameters->SourceCounters = SourceCounterUAV;
		MarkPassParameters->DespawnIDCount = ActiveDespawnByIDs.Num();
		MarkPassParameters->ParticleCount = InOutParticleCount;
		MarkPassParameters->MaxSourceCount = EGPUParticleSource::MaxSourceCount;

		const uint32 MarkPassNumGroups = FMath::DivideAndRoundUp(InOutParticleCount, FMarkDespawnByIDCS::ThreadGroupSize);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::DespawnByID_Mark(%d IDs)", ActiveDespawnByIDs.Num()),
			MarkDespawnByIDCS,
			MarkPassParameters,
			FIntVector(MarkPassNumGroups, 1, 1)
		);
	}

	// === Stream Compaction Pipeline (reuse existing shaders) ===
	const int32 PrefixSumBlockNumGroups = FMath::DivideAndRoundUp(InOutParticleCount, FPrefixSumBlockCS_RDG::ThreadGroupSize);

	{
		RDG_EVENT_SCOPE(GraphBuilder, "Despawn_PrefixSum");

		// Block-wise prefix sum
		TShaderMapRef<FPrefixSumBlockCS_RDG> PrefixSumBlock(ShaderMap);
		FPrefixSumBlockCS_RDG::FParameters* PrefixSumBlockParameters = GraphBuilder.AllocParameters<FPrefixSumBlockCS_RDG::FParameters>();
		PrefixSumBlockParameters->MarkedFlags = GraphBuilder.CreateSRV(AliveMaskBuffer);
		PrefixSumBlockParameters->PrefixSums = GraphBuilder.CreateUAV(PrefixSumsBuffer);
		PrefixSumBlockParameters->BlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);
		PrefixSumBlockParameters->ElementCount = InOutParticleCount;

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::DespawnByID_PrefixSumBlock"),
			PrefixSumBlock,
			PrefixSumBlockParameters,
			FIntVector(PrefixSumBlockNumGroups, 1, 1)
		);

		// Scan block sums
		TShaderMapRef<FScanBlockSumsCS_RDG> ScanBlockSums(ShaderMap);
		FScanBlockSumsCS_RDG::FParameters* ScanBlockSumsParameters = GraphBuilder.AllocParameters<FScanBlockSumsCS_RDG::FParameters>();
		ScanBlockSumsParameters->BlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);
		ScanBlockSumsParameters->BlockCount = PrefixSumBlockNumGroups;

		const int32 ScanBlockSumsNumGroups = FMath::DivideAndRoundUp(PrefixSumBlockNumGroups, FScanBlockSumsCS_RDG::ThreadGroupSize);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::DespawnByID_ScanBlockSums"),
			ScanBlockSums,
			ScanBlockSumsParameters,
			FIntVector(ScanBlockSumsNumGroups, 1, 1)
		);

		// Add block offsets
		TShaderMapRef<FAddBlockOffsetsCS_RDG> AddBlockOffsets(ShaderMap);
		FAddBlockOffsetsCS_RDG::FParameters* AddBlockOffsetsParameters = GraphBuilder.AllocParameters<FAddBlockOffsetsCS_RDG::FParameters>();
		AddBlockOffsetsParameters->PrefixSums = GraphBuilder.CreateUAV(PrefixSumsBuffer);
		AddBlockOffsetsParameters->BlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);
		AddBlockOffsetsParameters->ElementCount = InOutParticleCount;

		const int32 AddBlockOffsetsNumGroups = FMath::DivideAndRoundUp(InOutParticleCount, FPrefixSumBlockCS_RDG::ThreadGroupSize);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::DespawnByID_AddBlockOffsets"),
			AddBlockOffsets,
			AddBlockOffsetsParameters,
			FIntVector(AddBlockOffsetsNumGroups, 1, 1)
		);
	}

	{
		RDG_EVENT_SCOPE(GraphBuilder, "Despawn_Compact");

		// Use persistent double-buffered output (avoid per-frame allocation)
		FRDGBufferRef CompactedParticlesBuffer = GraphBuilder.RegisterExternalBuffer(
			PersistentCompactedBuffer[CompactedBufferIndex], TEXT("CompactedParticlesByID"));
		CompactedBufferIndex = 1 - CompactedBufferIndex;  // Toggle for next frame

		// Compact particles
		TShaderMapRef<FCompactParticlesCS_RDG> Compact(ShaderMap);
		FCompactParticlesCS_RDG::FParameters* CompactParameters = GraphBuilder.AllocParameters<FCompactParticlesCS_RDG::FParameters>();
		CompactParameters->Particles = GraphBuilder.CreateSRV(InOutParticleBuffer);
		CompactParameters->MarkedFlags = GraphBuilder.CreateSRV(AliveMaskBuffer);
		CompactParameters->PrefixSums = GraphBuilder.CreateSRV(PrefixSumsBuffer);
		CompactParameters->CompactedParticles = GraphBuilder.CreateUAV(CompactedParticlesBuffer);
		CompactParameters->ParticleCount = InOutParticleCount;

		const int32 CompactCSNumGroups = FMath::DivideAndRoundUp(InOutParticleCount, FPrefixSumBlockCS_RDG::ThreadGroupSize);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::DespawnByID_Compact"),
			Compact,
			CompactParameters,
			FIntVector(CompactCSNumGroups, 1, 1)
		);

		// Update buffer reference
		InOutParticleBuffer = CompactedParticlesBuffer;
	}

	// Clear active IDs after processing
	// AlreadyRequestedIDs is cleaned up via readback in AddDespawnByIDRequests
	ActiveDespawnByIDs.Empty();
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
	const int32 BlockCount = FMath::DivideAndRoundUp(Capacity, (int32)FPrefixSumBlockCS_RDG::ThreadGroupSize);

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
