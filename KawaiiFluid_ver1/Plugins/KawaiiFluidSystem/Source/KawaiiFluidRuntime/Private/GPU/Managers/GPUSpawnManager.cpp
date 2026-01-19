// Copyright KawaiiFluid Team. All Rights Reserved.
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
	PendingDespawnByIDs.Add(ParticleID);
	bHasPendingDespawnByIDRequests.store(true);
}

void FGPUSpawnManager::AddDespawnByIDRequests(const TArray<int32>& ParticleIDs, const TArray<int32>& AllCurrentReadbackIDs)
{
	if (ParticleIDs.Num() == 0)
	{
		return;
	}

	FScopeLock Lock(&DespawnByIDLock);

	// Cleanup: Readback에 없는 ID는 AlreadyRequestedIDs에서 제거 (이미 GPU에서 제거됨)
	TSet<int32> ReadbackSet(AllCurrentReadbackIDs);
	TArray<int32> ToRemove;
	for (int32 ID : AlreadyRequestedIDs)
	{
		if (!ReadbackSet.Contains(ID))  // Readback에 없음 = 이미 제거됨
		{
			ToRemove.Add(ID);
		}
	}
	for (int32 ID : ToRemove)
	{
		AlreadyRequestedIDs.Remove(ID);
	}

	// 중복 필터링: 이미 제거 요청한 ID는 제외
	int32 OriginalCount = ParticleIDs.Num();
	int32 FilteredCount = 0;
	for (int32 ID : ParticleIDs)
	{
		if (!AlreadyRequestedIDs.Contains(ID))
		{
			PendingDespawnByIDs.Add(ID);
			AlreadyRequestedIDs.Add(ID);
			FilteredCount++;
		}
	}

	if (FilteredCount > 0)
	{
		bHasPendingDespawnByIDRequests.store(true);
	}

	UE_LOG(LogGPUSpawnManager, Log, TEXT("AddDespawnByIDRequests: %d requested, %d new, %d duplicates, %d cleaned (total pending: %d)"),
		OriginalCount, FilteredCount, OriginalCount - FilteredCount, ToRemove.Num(), PendingDespawnByIDs.Num());
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

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FMarkDespawnByIDCS> MarkDespawnByIDCS(ShaderMap);

	// Create sorted ID buffer for GPU binary search
	FRDGBufferRef DespawnIDsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidDespawnByIDs"),
		sizeof(int32),
		ActiveDespawnByIDs.Num(),
		ActiveDespawnByIDs.GetData(),
		ActiveDespawnByIDs.Num() * sizeof(int32),
		ERDGInitialDataFlags::None
	);

	FRDGBufferRef AliveMaskBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidOutAliveMaskByID"),
		sizeof(uint32),
		InOutParticleCount,
		nullptr,
		0,
		ERDGInitialDataFlags::None
	);

	// Get or create source counter buffer UAV for decrementing
	FRDGBufferUAVRef SourceCounterUAV = RegisterSourceCounterUAV(GraphBuilder);

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

	// === Stream Compaction Pipeline (reuse existing shaders) ===

	FRDGBufferRef PrefixSumsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("PrefixSumsByID"),
		sizeof(uint32),
		InOutParticleCount,
		nullptr,
		0,
		ERDGInitialDataFlags::None
	);

	FRDGBufferRef BlockSumsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("BlockSumsByID"),
		sizeof(uint32),
		FMath::DivideAndRoundUp(InOutParticleCount, FPrefixSumBlockCS_RDG::ThreadGroupSize),
		nullptr,
		0,
		ERDGInitialDataFlags::None
	);

	// Block-wise prefix sum
	TShaderMapRef<FPrefixSumBlockCS_RDG> PrefixSumBlock(ShaderMap);
	FPrefixSumBlockCS_RDG::FParameters* PrefixSumBlockParameters = GraphBuilder.AllocParameters<FPrefixSumBlockCS_RDG::FParameters>();
	PrefixSumBlockParameters->MarkedFlags = GraphBuilder.CreateSRV(AliveMaskBuffer);
	PrefixSumBlockParameters->PrefixSums = GraphBuilder.CreateUAV(PrefixSumsBuffer);
	PrefixSumBlockParameters->BlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);
	PrefixSumBlockParameters->ElementCount = InOutParticleCount;

	const int32 PrefixSumBlockNumGroups = FMath::DivideAndRoundUp(InOutParticleCount, FPrefixSumBlockCS_RDG::ThreadGroupSize);
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

	FRDGBufferRef CompactedParticlesBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("CompactedParticlesByID"),
		sizeof(FGPUFluidParticle),
		InOutParticleCount,
		nullptr,
		0,
		ERDGInitialDataFlags::None
	);

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

	UE_LOG(LogGPUSpawnManager, Log, TEXT("AddDespawnByIDPass: Processing %d IDs, %d particles"),
		ActiveDespawnByIDs.Num(), InOutParticleCount);

	// Clear active IDs after processing
	// AlreadyRequestedIDs는 AddDespawnByIDRequests에서 Readback 기반으로 cleanup됨
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
	// SourceID 범위 체크
	if (SourceID < 0 || SourceID >= EGPUParticleSource::MaxSourceCount)
	{
		return -1;  // 잘못된 SourceID
	}

	// GPU 버퍼가 아직 생성 안됐으면 데이터 없음
	if (!SourceCounterBuffer.IsValid())
	{
		return -1;  // 데이터 미준비
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
