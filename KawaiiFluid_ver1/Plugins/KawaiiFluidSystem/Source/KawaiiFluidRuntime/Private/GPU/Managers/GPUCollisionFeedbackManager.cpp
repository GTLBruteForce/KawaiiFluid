// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUCollisionFeedbackManager - Collision feedback system with async GPU readback

#include "GPU/Managers/GPUCollisionFeedbackManager.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUCollisionFeedback, Log, All);
DEFINE_LOG_CATEGORY(LogGPUCollisionFeedback);

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUCollisionFeedbackManager::FGPUCollisionFeedbackManager()
	: bIsInitialized(false)
	, bFeedbackEnabled(false)
{
}

FGPUCollisionFeedbackManager::~FGPUCollisionFeedbackManager()
{
	Release();
}

//=============================================================================
// Lifecycle
//=============================================================================

void FGPUCollisionFeedbackManager::Initialize()
{
	bIsInitialized = true;

	// Initialize ready arrays for bone colliders
	ReadyFeedback.SetNum(MAX_COLLISION_FEEDBACK);
	ReadyFeedbackCount = 0;
	ReadyContactCounts.SetNumZeroed(MAX_COLLIDER_COUNT);

	// Initialize ready arrays for StaticMesh colliders (WorldCollision)
	ReadyStaticMeshFeedback.SetNum(MAX_STATICMESH_COLLISION_FEEDBACK);
	ReadyStaticMeshFeedbackCount = 0;

	// Initialize ready arrays for FluidInteraction StaticMesh colliders
	ReadyFluidInteractionSMFeedback.SetNum(MAX_FLUIDINTERACTION_SM_FEEDBACK);
	ReadyFluidInteractionSMFeedbackCount = 0;

	UE_LOG(LogGPUCollisionFeedback, Log, TEXT("GPUCollisionFeedbackManager initialized (BoneFeedback=%d, StaticMeshFeedback=%d, FluidInteractionSMFeedback=%d)"),
		MAX_COLLISION_FEEDBACK, MAX_STATICMESH_COLLISION_FEEDBACK, MAX_FLUIDINTERACTION_SM_FEEDBACK);
}

void FGPUCollisionFeedbackManager::Release()
{
	ReleaseReadbackObjects();

	// Release unified feedback buffer
	UnifiedFeedbackBuffer.SafeRelease();
	ColliderContactCountBuffer.SafeRelease();

	// Clear bone collider ready arrays
	ReadyFeedback.Empty();
	ReadyFeedbackCount = 0;
	ReadyContactCounts.Empty();

	// Clear StaticMesh collider ready arrays (WorldCollision)
	ReadyStaticMeshFeedback.Empty();
	ReadyStaticMeshFeedbackCount = 0;

	// Clear FluidInteraction StaticMesh collider ready arrays
	ReadyFluidInteractionSMFeedback.Empty();
	ReadyFluidInteractionSMFeedbackCount = 0;

	CurrentWriteIndex = 0;
	FeedbackFrameNumber = 0;
	ContactCountFrameNumber = 0;
	CompletedFeedbackFrame.store(-1);

	bIsInitialized = false;

	UE_LOG(LogGPUCollisionFeedback, Log, TEXT("GPUCollisionFeedbackManager released"));
}

//=============================================================================
// Buffer Management
//=============================================================================

void FGPUCollisionFeedbackManager::AllocateReadbackObjects(FRHICommandListImmediate& RHICmdList)
{
	// Unified buffer - allocate single readback per frame instead of 7
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		// Unified feedback readback (replaces 6 separate readbacks: FeedbackReadbacks, CounterReadbacks,
		// StaticMeshFeedbackReadbacks, StaticMeshCounterReadbacks, FluidInteractionSMFeedbackReadbacks,
		// FluidInteractionSMCounterReadbacks)
		if (UnifiedFeedbackReadbacks[i] == nullptr)
		{
			UnifiedFeedbackReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("UnifiedFeedbackReadback_%d"), i));
		}

		// Contact count readback (unchanged)
		if (ContactCountReadbacks[i] == nullptr)
		{
			ContactCountReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("ContactCountReadback_%d"), i));
		}
	}

	UE_LOG(LogGPUCollisionFeedback, Log, TEXT("Unified readback objects allocated (BufferSize=%d bytes, NumBuffers=%d, MaxColliders=%d)"),
		UNIFIED_BUFFER_SIZE, NUM_FEEDBACK_BUFFERS, MAX_COLLIDER_COUNT);
}

void FGPUCollisionFeedbackManager::ReleaseReadbackObjects()
{
	// Release unified readbacks (replaces 7 separate releases per buffer)
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		// Release unified feedback readback
		if (UnifiedFeedbackReadbacks[i] != nullptr)
		{
			delete UnifiedFeedbackReadbacks[i];
			UnifiedFeedbackReadbacks[i] = nullptr;
		}

		// Release contact count readback
		if (ContactCountReadbacks[i] != nullptr)
		{
			delete ContactCountReadbacks[i];
			ContactCountReadbacks[i] = nullptr;
		}
	}
}

//=============================================================================
// Readback Processing
//=============================================================================

void FGPUCollisionFeedbackManager::ProcessFeedbackReadback(FRHICommandListImmediate& RHICmdList)
{
	if (!bFeedbackEnabled)
	{
		return;
	}

	// Ensure readback objects are allocated
	if (UnifiedFeedbackReadbacks[0] == nullptr)
	{
		return;
	}

	// Need at least 2 frames for triple buffering to work
	if (FeedbackFrameNumber < 2)
	{
		return;
	}

	// Process Unified Feedback Buffer
	// Single readback containing all feedback types with embedded counters
	// Layout: [Header:16B][BoneFeedback][SMFeedback][FISMFeedback]
	int32 ReadIdx = -1;
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		if (UnifiedFeedbackReadbacks[i] && UnifiedFeedbackReadbacks[i]->IsReady())
		{
			ReadIdx = i;
			break;
		}
	}

	if (ReadIdx < 0)
	{
		return;
	}

	// Lock entire unified buffer once
	const uint8* BufferData = (const uint8*)UnifiedFeedbackReadbacks[ReadIdx]->Lock(UNIFIED_BUFFER_SIZE);
	if (!BufferData)
	{
		UnifiedFeedbackReadbacks[ReadIdx]->Unlock();
		return;
	}

	// Read header (4 counters at offset 0)
	const uint32* Header = (const uint32*)BufferData;
	uint32 BoneCount = FMath::Min(Header[0], (uint32)MAX_COLLISION_FEEDBACK);
	uint32 SMCount = FMath::Min(Header[1], (uint32)MAX_STATICMESH_COLLISION_FEEDBACK);
	uint32 FISMCount = FMath::Min(Header[2], (uint32)MAX_FLUIDINTERACTION_SM_FEEDBACK);

	// Debug logging (every 60 frames)
	static int32 DebugCounter = 0;
	const bool bLogThisFrame = (++DebugCounter % 60 == 0);

	FScopeLock Lock(&FeedbackLock);

	// =====================================================
	// Process Bone Feedback (offset: BONE_FEEDBACK_OFFSET)
	// =====================================================
	if (BoneCount > 0)
	{
		const FGPUCollisionFeedback* BoneFeedback = (const FGPUCollisionFeedback*)(BufferData + BONE_FEEDBACK_OFFSET);
		ReadyFeedback.SetNum(BoneCount);
		FMemory::Memcpy(ReadyFeedback.GetData(), BoneFeedback, BoneCount * sizeof(FGPUCollisionFeedback));
		ReadyFeedbackCount = BoneCount;

		if (bLogThisFrame)
		{
			FString BoneIdxSamples;
			int32 SampleCount = FMath::Min(BoneCount, 5u);
			for (int32 s = 0; s < SampleCount; ++s)
			{
				BoneIdxSamples += FString::Printf(TEXT("[%d:OwnerID=%d] "), BoneFeedback[s].BoneIndex, BoneFeedback[s].ColliderOwnerID);
			}
			UE_LOG(LogTemp, Warning, TEXT("[UnifiedBuffer-Bone] Count=%d, Samples=%s"), BoneCount, *BoneIdxSamples);
		}
	}
	else
	{
		ReadyFeedbackCount = 0;
	}

	// =====================================================
	// Process StaticMesh Feedback (offset: SM_FEEDBACK_OFFSET)
	// =====================================================
	if (SMCount > 0)
	{
		const FGPUCollisionFeedback* SMFeedback = (const FGPUCollisionFeedback*)(BufferData + SM_FEEDBACK_OFFSET);
		ReadyStaticMeshFeedback.SetNum(SMCount);
		FMemory::Memcpy(ReadyStaticMeshFeedback.GetData(), SMFeedback, SMCount * sizeof(FGPUCollisionFeedback));
		ReadyStaticMeshFeedbackCount = SMCount;

		if (bLogThisFrame)
		{
			FString SMIdxSamples;
			int32 SampleCount = FMath::Min(SMCount, 5u);
			for (int32 s = 0; s < SampleCount; ++s)
			{
				SMIdxSamples += FString::Printf(TEXT("[%d:OwnerID=%d] "), SMFeedback[s].BoneIndex, SMFeedback[s].ColliderOwnerID);
			}
			UE_LOG(LogTemp, Warning, TEXT("[UnifiedBuffer-SM] Count=%d, Samples=%s"), SMCount, *SMIdxSamples);
		}
	}
	else
	{
		ReadyStaticMeshFeedbackCount = 0;
	}

	// =====================================================
	// Process FluidInteraction Feedback (offset: FISM_FEEDBACK_OFFSET)
	// =====================================================
	if (FISMCount > 0)
	{
		const FGPUCollisionFeedback* FISMFeedback = (const FGPUCollisionFeedback*)(BufferData + FISM_FEEDBACK_OFFSET);
		ReadyFluidInteractionSMFeedback.SetNum(FISMCount);
		FMemory::Memcpy(ReadyFluidInteractionSMFeedback.GetData(), FISMFeedback, FISMCount * sizeof(FGPUCollisionFeedback));
		ReadyFluidInteractionSMFeedbackCount = FISMCount;

		if (bLogThisFrame)
		{
			FString FISMIdxSamples;
			int32 SampleCount = FMath::Min(FISMCount, 5u);
			for (int32 s = 0; s < SampleCount; ++s)
			{
				FISMIdxSamples += FString::Printf(TEXT("[%d:OwnerID=%d] "), FISMFeedback[s].BoneIndex, FISMFeedback[s].ColliderOwnerID);
			}
			UE_LOG(LogTemp, Warning, TEXT("[UnifiedBuffer-FISM] Count=%d, Samples=%s"), FISMCount, *FISMIdxSamples);
		}
	}
	else
	{
		ReadyFluidInteractionSMFeedbackCount = 0;
	}

	UnifiedFeedbackReadbacks[ReadIdx]->Unlock();

	UE_LOG(LogGPUCollisionFeedback, Verbose, TEXT("Unified readback %d: Bone=%d, SM=%d, FISM=%d"), ReadIdx, BoneCount, SMCount, FISMCount);
}

void FGPUCollisionFeedbackManager::ProcessContactCountReadback(FRHICommandListImmediate& RHICmdList)
{
	// Debug logging (every 60 frames)
	static int32 DebugFrame = 0;
	const bool bLogThisFrame = (DebugFrame++ % 60 == 0);

	// Ensure readback objects are valid
	if (ContactCountReadbacks[0] == nullptr)
	{
		return;
	}

	// Search for any ready buffer
	int32 ReadIdx = -1;
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		if (ContactCountReadbacks[i] && ContactCountReadbacks[i]->IsReady())
		{
			ReadIdx = i;
			break;
		}
	}

	// Only read if we have completed at least 2 frames and found a ready buffer
	if (ContactCountFrameNumber >= 2 && ReadIdx >= 0)
	{
		const uint32* CountData = (const uint32*)ContactCountReadbacks[ReadIdx]->Lock(MAX_COLLIDER_COUNT * sizeof(uint32));

		if (CountData)
		{
			FScopeLock Lock(&FeedbackLock);

			ReadyContactCounts.SetNumUninitialized(MAX_COLLIDER_COUNT);
			int32 TotalContactCount = 0;
			int32 NonZeroColliders = 0;

			for (int32 i = 0; i < MAX_COLLIDER_COUNT; ++i)
			{
				ReadyContactCounts[i] = static_cast<int32>(CountData[i]);
				if (CountData[i] > 0)
				{
					TotalContactCount += CountData[i];
					NonZeroColliders++;
				}
			}

			if (bLogThisFrame && NonZeroColliders > 0)
			{
				UE_LOG(LogGPUCollisionFeedback, Log, TEXT("Contact count: Total=%d, NonZeroColliders=%d"),
					TotalContactCount, NonZeroColliders);
			}
		}

		ContactCountReadbacks[ReadIdx]->Unlock();
	}
}

void FGPUCollisionFeedbackManager::EnqueueReadbackCopy(FRHICommandListImmediate& RHICmdList)
{
	// This is called after the simulation pass to enqueue copies for next frame's readback
	if (!bIsInitialized)
	{
		return;
	}

	// Debug logging (every 60 frames)
	static int32 EnqueueDebugFrame = 0;
	const bool bLogThisFrame = (EnqueueDebugFrame++ % 60 == 0);

	// Ensure readback objects are allocated
	if (UnifiedFeedbackReadbacks[0] == nullptr)
	{
		AllocateReadbackObjects(RHICmdList);
	}

	const int32 WriteIdx = CurrentWriteIndex;

	// =====================================================
	// Single EnqueueCopy + 2 Transitions (was 6 EnqueueCopy + 12 Transitions)
	// Performance: 83% API call reduction
	// =====================================================
	if (bFeedbackEnabled && UnifiedFeedbackBuffer.IsValid())
	{
		// Single transition: UAVCompute → CopySrc
		RHICmdList.Transition(FRHITransitionInfo(
			UnifiedFeedbackBuffer->GetRHI(),
			ERHIAccess::UAVCompute,
			ERHIAccess::CopySrc));

		// Single EnqueueCopy for entire unified buffer
		UnifiedFeedbackReadbacks[WriteIdx]->EnqueueCopy(
			RHICmdList,
			UnifiedFeedbackBuffer->GetRHI(),
			UNIFIED_BUFFER_SIZE
		);

		// Single transition back: CopySrc → UAVCompute
		RHICmdList.Transition(FRHITransitionInfo(
			UnifiedFeedbackBuffer->GetRHI(),
			ERHIAccess::CopySrc,
			ERHIAccess::UAVCompute));

		if (bLogThisFrame)
		{
			UE_LOG(LogGPUCollisionFeedback, Log, TEXT("EnqueueCopy unified feedback (%d bytes) to readback %d"), UNIFIED_BUFFER_SIZE, WriteIdx);
		}
	}

	// =====================================================
	// Contact Count Readback (unchanged)
	// =====================================================
	if (ColliderContactCountBuffer.IsValid())
	{
		// Transition for copy
		RHICmdList.Transition(FRHITransitionInfo(
			ColliderContactCountBuffer->GetRHI(),
			ERHIAccess::UAVCompute,
			ERHIAccess::CopySrc));

		// EnqueueCopy - async copy to readback buffer (non-blocking!)
		ContactCountReadbacks[WriteIdx]->EnqueueCopy(
			RHICmdList,
			ColliderContactCountBuffer->GetRHI(),
			MAX_COLLIDER_COUNT * sizeof(uint32)
		);

		// Transition back for next frame
		RHICmdList.Transition(FRHITransitionInfo(
			ColliderContactCountBuffer->GetRHI(),
			ERHIAccess::CopySrc,
			ERHIAccess::UAVCompute));

		if (bLogThisFrame)
		{
			UE_LOG(LogGPUCollisionFeedback, Log, TEXT("EnqueueCopy contact counts to readback %d"), WriteIdx);
		}
	}

	// Increment frame counter AFTER EnqueueCopy
	IncrementFrameCounter();
}

void FGPUCollisionFeedbackManager::IncrementFrameCounter()
{
	CurrentWriteIndex = (CurrentWriteIndex + 1) % NUM_FEEDBACK_BUFFERS;
	FeedbackFrameNumber++;
	ContactCountFrameNumber++;
}

//=============================================================================
// Query API
//=============================================================================

bool FGPUCollisionFeedbackManager::GetFeedbackForCollider(int32 ColliderIndex, TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	FScopeLock Lock(&FeedbackLock);

	OutFeedback.Reset();
	OutCount = 0;

	if (!bFeedbackEnabled || ReadyFeedbackCount == 0)
	{
		return false;
	}

	// Filter feedback for this collider
	for (int32 i = 0; i < ReadyFeedbackCount; ++i)
	{
		if (ReadyFeedback[i].ColliderIndex == ColliderIndex)
		{
			OutFeedback.Add(ReadyFeedback[i]);
		}
	}

	OutCount = OutFeedback.Num();
	return OutCount > 0;
}

bool FGPUCollisionFeedbackManager::GetAllFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	FScopeLock Lock(&FeedbackLock);

	OutCount = ReadyFeedbackCount;

	if (!bFeedbackEnabled || ReadyFeedbackCount == 0)
	{
		OutFeedback.Reset();
		return false;
	}

	OutFeedback.SetNum(ReadyFeedbackCount);
	FMemory::Memcpy(OutFeedback.GetData(), ReadyFeedback.GetData(), ReadyFeedbackCount * sizeof(FGPUCollisionFeedback));

	return true;
}

int32 FGPUCollisionFeedbackManager::GetContactCount(int32 ColliderIndex) const
{
	FScopeLock Lock(&FeedbackLock);

	if (ColliderIndex < 0 || ColliderIndex >= ReadyContactCounts.Num())
	{
		return 0;
	}
	return ReadyContactCounts[ColliderIndex];
}

void FGPUCollisionFeedbackManager::GetAllContactCounts(TArray<int32>& OutCounts) const
{
	FScopeLock Lock(&FeedbackLock);
	OutCounts = ReadyContactCounts;
}

bool FGPUCollisionFeedbackManager::GetAllStaticMeshFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	FScopeLock Lock(&FeedbackLock);

	OutCount = ReadyStaticMeshFeedbackCount;

	if (!bFeedbackEnabled || ReadyStaticMeshFeedbackCount == 0)
	{
		OutFeedback.Reset();
		return false;
	}

	OutFeedback.SetNum(ReadyStaticMeshFeedbackCount);
	FMemory::Memcpy(OutFeedback.GetData(), ReadyStaticMeshFeedback.GetData(), ReadyStaticMeshFeedbackCount * sizeof(FGPUCollisionFeedback));

	return true;
}

bool FGPUCollisionFeedbackManager::GetAllFluidInteractionSMFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	FScopeLock Lock(&FeedbackLock);

	OutCount = ReadyFluidInteractionSMFeedbackCount;

	if (!bFeedbackEnabled || ReadyFluidInteractionSMFeedbackCount == 0)
	{
		OutFeedback.Reset();
		return false;
	}

	OutFeedback.SetNum(ReadyFluidInteractionSMFeedbackCount);
	FMemory::Memcpy(OutFeedback.GetData(), ReadyFluidInteractionSMFeedback.GetData(), ReadyFluidInteractionSMFeedbackCount * sizeof(FGPUCollisionFeedback));

	return true;
}
