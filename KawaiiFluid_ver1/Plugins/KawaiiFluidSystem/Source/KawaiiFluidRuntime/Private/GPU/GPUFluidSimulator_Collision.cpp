// Copyright KawaiiFluid Team. All Rights Reserved.
// GPUFluidSimulator - Collision System Functions

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);

//=============================================================================
// Collision Primitives Upload
//=============================================================================

void FGPUFluidSimulator::UploadCollisionPrimitives(const FGPUCollisionPrimitives& Primitives)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&BufferLock);

	// Cache the primitive data (will be uploaded to GPU during simulation)
	CachedSpheres = Primitives.Spheres;
	CachedCapsules = Primitives.Capsules;
	CachedBoxes = Primitives.Boxes;
	CachedConvexHeaders = Primitives.Convexes;
	CachedConvexPlanes = Primitives.ConvexPlanes;
	CachedBoneTransforms = Primitives.BoneTransforms;

	// Check if we have any primitives
	if (Primitives.IsEmpty())
	{
		bCollisionPrimitivesValid = false;
		bBoneTransformsValid = false;
		return;
	}

	bCollisionPrimitivesValid = true;
	bBoneTransformsValid = CachedBoneTransforms.Num() > 0;

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Cached collision primitives: Spheres=%d, Capsules=%d, Boxes=%d, Convexes=%d, Planes=%d, BoneTransforms=%d"),
		CachedSpheres.Num(), CachedCapsules.Num(), CachedBoxes.Num(), CachedConvexHeaders.Num(), CachedConvexPlanes.Num(), CachedBoneTransforms.Num());
}

//=============================================================================
// Bounds Collision Pass
//=============================================================================

void FGPUFluidSimulator::AddBoundsCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FBoundsCollisionCS> ComputeShader(ShaderMap);

	FBoundsCollisionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundsCollisionCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = Params.ParticleRadius;

	// OBB parameters
	PassParameters->BoundsCenter = Params.BoundsCenter;
	PassParameters->BoundsExtent = Params.BoundsExtent;
	PassParameters->BoundsRotation = Params.BoundsRotation;
	PassParameters->bUseOBB = Params.bUseOBB;

	// Legacy AABB parameters
	PassParameters->BoundsMin = Params.BoundsMin;
	PassParameters->BoundsMax = Params.BoundsMax;

	// Collision response
	PassParameters->Restitution = Params.BoundsRestitution;
	PassParameters->Friction = Params.BoundsFriction;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FBoundsCollisionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::BoundsCollision"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Distance Field Collision Pass
//=============================================================================

void FGPUFluidSimulator::AddDistanceFieldCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if Distance Field collision is not enabled
	if (!DFCollisionParams.bEnabled || !CachedGDFTextureSRV)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FDistanceFieldCollisionCS> ComputeShader(ShaderMap);

	FDistanceFieldCollisionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDistanceFieldCollisionCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = DFCollisionParams.ParticleRadius;

	// Distance Field Volume Parameters
	PassParameters->GDFVolumeCenter = DFCollisionParams.VolumeCenter;
	PassParameters->GDFVolumeExtent = DFCollisionParams.VolumeExtent;
	PassParameters->GDFVoxelSize = FVector3f(DFCollisionParams.VoxelSize);
	PassParameters->GDFMaxDistance = DFCollisionParams.MaxDistance;

	// Collision Response Parameters
	PassParameters->DFCollisionRestitution = DFCollisionParams.Restitution;
	PassParameters->DFCollisionFriction = DFCollisionParams.Friction;
	PassParameters->DFCollisionThreshold = DFCollisionParams.CollisionThreshold;

	// Global Distance Field Texture
	PassParameters->GlobalDistanceFieldTexture = CachedGDFTextureSRV;
	PassParameters->GlobalDistanceFieldSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FDistanceFieldCollisionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::DistanceFieldCollision"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Primitive Collision Pass (Spheres, Capsules, Boxes, Convex)
//=============================================================================

void FGPUFluidSimulator::AddPrimitiveCollisionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if no collision primitives
	if (!bCollisionPrimitivesValid || GetCollisionPrimitiveCount() == 0)
	{
		return;
	}

	FRDGBufferSRVRef SpheresSRV = nullptr;
	FRDGBufferSRVRef CapsulesSRV = nullptr;
	FRDGBufferSRVRef BoxesSRV = nullptr;
	FRDGBufferSRVRef ConvexesSRV = nullptr;
	FRDGBufferSRVRef ConvexPlanesSRV = nullptr;

	// Dummy data for empty buffers (shader requires all SRVs to be valid)
	static FGPUCollisionSphere DummySphere;
	static FGPUCollisionCapsule DummyCapsule;
	static FGPUCollisionBox DummyBox;
	static FGPUCollisionConvex DummyConvex;
	static FGPUConvexPlane DummyPlane;

	// Create RDG buffers from cached data (or dummy for empty arrays)
	{
		const bool bHasData = CachedSpheres.Num() > 0;
		FRDGBufferRef SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionSpheres"),
			sizeof(FGPUCollisionSphere),
			bHasData ? CachedSpheres.Num() : 1,
			bHasData ? CachedSpheres.GetData() : &DummySphere,
			bHasData ? CachedSpheres.Num() * sizeof(FGPUCollisionSphere) : sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
		SpheresSRV = GraphBuilder.CreateSRV(SpheresBuffer);
	}

	{
		const bool bHasData = CachedCapsules.Num() > 0;
		FRDGBufferRef CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionCapsules"),
			sizeof(FGPUCollisionCapsule),
			bHasData ? CachedCapsules.Num() : 1,
			bHasData ? CachedCapsules.GetData() : &DummyCapsule,
			bHasData ? CachedCapsules.Num() * sizeof(FGPUCollisionCapsule) : sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
		CapsulesSRV = GraphBuilder.CreateSRV(CapsulesBuffer);
	}

	{
		const bool bHasData = CachedBoxes.Num() > 0;
		FRDGBufferRef BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionBoxes"),
			sizeof(FGPUCollisionBox),
			bHasData ? CachedBoxes.Num() : 1,
			bHasData ? CachedBoxes.GetData() : &DummyBox,
			bHasData ? CachedBoxes.Num() * sizeof(FGPUCollisionBox) : sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
		BoxesSRV = GraphBuilder.CreateSRV(BoxesBuffer);
	}

	{
		const bool bHasData = CachedConvexHeaders.Num() > 0;
		FRDGBufferRef ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionConvexes"),
			sizeof(FGPUCollisionConvex),
			bHasData ? CachedConvexHeaders.Num() : 1,
			bHasData ? CachedConvexHeaders.GetData() : &DummyConvex,
			bHasData ? CachedConvexHeaders.Num() * sizeof(FGPUCollisionConvex) : sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
		ConvexesSRV = GraphBuilder.CreateSRV(ConvexesBuffer);
	}

	{
		const bool bHasData = CachedConvexPlanes.Num() > 0;
		FRDGBufferRef ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUCollisionConvexPlanes"),
			sizeof(FGPUConvexPlane),
			bHasData ? CachedConvexPlanes.Num() : 1,
			bHasData ? CachedConvexPlanes.GetData() : &DummyPlane,
			bHasData ? CachedConvexPlanes.Num() * sizeof(FGPUConvexPlane) : sizeof(FGPUConvexPlane),
			ERDGInitialDataFlags::NoCopy
		);
		ConvexPlanesSRV = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	}

	// Create collision feedback buffers (for particle -> player interaction)
	FRDGBufferRef FeedbackBuffer = nullptr;
	FRDGBufferRef CounterBuffer = nullptr;

	// Create feedback buffer (persistent across frames for extraction)
	if (bCollisionFeedbackEnabled)
	{
		// Create or reuse feedback buffer
		if (CollisionFeedbackBuffer.IsValid())
		{
			FeedbackBuffer = GraphBuilder.RegisterExternalBuffer(CollisionFeedbackBuffer, TEXT("GPUCollisionFeedback"));
		}
		else
		{
			FRDGBufferDesc FeedbackDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUCollisionFeedback), MAX_COLLISION_FEEDBACK);
			FeedbackBuffer = GraphBuilder.CreateBuffer(FeedbackDesc, TEXT("GPUCollisionFeedback"));
		}

		// Create counter buffer (reset each frame)
		if (CollisionCounterBuffer.IsValid())
		{
			CounterBuffer = GraphBuilder.RegisterExternalBuffer(CollisionCounterBuffer, TEXT("GPUCollisionCounter"));
		}
		else
		{
			FRDGBufferDesc CounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
			CounterBuffer = GraphBuilder.CreateBuffer(CounterDesc, TEXT("GPUCollisionCounter"));
		}

		// Clear counter at start of frame
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CounterBuffer), 0);
	}
	else
	{
		// Create dummy buffers when feedback is disabled
		FRDGBufferDesc DummyFeedbackDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUCollisionFeedback), 1);
		FeedbackBuffer = GraphBuilder.CreateBuffer(DummyFeedbackDesc, TEXT("GPUCollisionFeedbackDummy"));

		FRDGBufferDesc DummyCounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		CounterBuffer = GraphBuilder.CreateBuffer(DummyCounterDesc, TEXT("GPUCollisionCounterDummy"));
	}

	// Create collider contact count buffer
	FRDGBufferRef ContactCountBuffer = nullptr;
	if (ColliderContactCountBuffer.IsValid())
	{
		ContactCountBuffer = GraphBuilder.RegisterExternalBuffer(ColliderContactCountBuffer, TEXT("ColliderContactCounts"));
	}
	else
	{
		FRDGBufferDesc ContactCountDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MAX_COLLIDER_COUNT);
		ContactCountBuffer = GraphBuilder.CreateBuffer(ContactCountDesc, TEXT("ColliderContactCounts"));
	}

	// Clear contact counts at start of frame
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ContactCountBuffer), 0);

	// Dispatch primitive collision shader directly (with feedback buffers)
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FPrimitiveCollisionCS> ComputeShader(GlobalShaderMap);

	FPrimitiveCollisionCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FPrimitiveCollisionCS::FParameters>();

	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = Params.ParticleRadius;
	PassParameters->CollisionThreshold = PrimitiveCollisionThreshold;

	PassParameters->CollisionSpheres = SpheresSRV;
	PassParameters->SphereCount = CachedSpheres.Num();

	PassParameters->CollisionCapsules = CapsulesSRV;
	PassParameters->CapsuleCount = CachedCapsules.Num();

	PassParameters->CollisionBoxes = BoxesSRV;
	PassParameters->BoxCount = CachedBoxes.Num();

	PassParameters->CollisionConvexes = ConvexesSRV;
	PassParameters->ConvexCount = CachedConvexHeaders.Num();

	PassParameters->ConvexPlanes = ConvexPlanesSRV;

	// Collision feedback parameters
	PassParameters->CollisionFeedback = GraphBuilder.CreateUAV(FeedbackBuffer);
	PassParameters->CollisionCounter = GraphBuilder.CreateUAV(CounterBuffer);
	PassParameters->MaxCollisionFeedback = MAX_COLLISION_FEEDBACK;
	PassParameters->bEnableCollisionFeedback = bCollisionFeedbackEnabled ? 1 : 0;

	// Collider contact count parameters
	PassParameters->ColliderContactCounts = GraphBuilder.CreateUAV(ContactCountBuffer);
	PassParameters->MaxColliderCount = MAX_COLLIDER_COUNT;

	const int32 ThreadGroupSize = FPrimitiveCollisionCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PrimitiveCollision(%d particles, %d primitives, feedback=%s)",
			CurrentParticleCount, CachedSpheres.Num() + CachedCapsules.Num() + CachedBoxes.Num() + CachedConvexHeaders.Num(),
			bCollisionFeedbackEnabled ? TEXT("ON") : TEXT("OFF")),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));

	// Extract feedback buffers for next frame (only if feedback is enabled)
	if (bCollisionFeedbackEnabled)
	{
		GraphBuilder.QueueBufferExtraction(
			FeedbackBuffer,
			&CollisionFeedbackBuffer,
			ERHIAccess::UAVCompute
		);

		GraphBuilder.QueueBufferExtraction(
			CounterBuffer,
			&CollisionCounterBuffer,
			ERHIAccess::UAVCompute
		);
	}

	// Always extract collider contact count buffer
	GraphBuilder.QueueBufferExtraction(
		ContactCountBuffer,
		&ColliderContactCountBuffer,
		ERHIAccess::UAVCompute
	);
}

//=============================================================================
// Collision Feedback Buffer Management
//=============================================================================

void FGPUFluidSimulator::AllocateCollisionFeedbackBuffers(FRHICommandListImmediate& RHICmdList)
{
	// Allocate FRHIGPUBufferReadback objects for truly async readback
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		if (FeedbackReadbacks[i] == nullptr)
		{
			FeedbackReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("CollisionFeedbackReadback_%d"), i));
		}

		if (CounterReadbacks[i] == nullptr)
		{
			CounterReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("CollisionCounterReadback_%d"), i));
		}

		if (ContactCountReadbacks[i] == nullptr)
		{
			ContactCountReadbacks[i] = new FRHIGPUBufferReadback(*FString::Printf(TEXT("ContactCountReadback_%d"), i));
		}
	}

	// Initialize ready feedback array
	ReadyFeedback.SetNum(MAX_COLLISION_FEEDBACK);
	ReadyFeedbackCount = 0;

	// Initialize ready contact counts array
	ReadyColliderContactCounts.SetNumZeroed(MAX_COLLIDER_COUNT);

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("Collision Feedback readback objects allocated (MaxFeedback=%d, NumBuffers=%d, MaxColliders=%d)"), MAX_COLLISION_FEEDBACK, NUM_FEEDBACK_BUFFERS, MAX_COLLIDER_COUNT);
}

void FGPUFluidSimulator::ReleaseCollisionFeedbackBuffers()
{
	CollisionFeedbackBuffer.SafeRelease();
	CollisionCounterBuffer.SafeRelease();
	ColliderContactCountBuffer.SafeRelease();

	// Delete readback objects
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		if (FeedbackReadbacks[i] != nullptr)
		{
			delete FeedbackReadbacks[i];
			FeedbackReadbacks[i] = nullptr;
		}
		if (CounterReadbacks[i] != nullptr)
		{
			delete CounterReadbacks[i];
			CounterReadbacks[i] = nullptr;
		}
		if (ContactCountReadbacks[i] != nullptr)
		{
			delete ContactCountReadbacks[i];
			ContactCountReadbacks[i] = nullptr;
		}
	}

	ContactCountFrameNumber = 0;

	ReadyFeedback.Empty();
	ReadyFeedbackCount = 0;
	ReadyColliderContactCounts.Empty();
	CurrentFeedbackWriteIndex = 0;
	CompletedFeedbackFrame.store(-1);
	FeedbackFrameNumber = 0;
}

void FGPUFluidSimulator::ProcessCollisionFeedbackReadback(FRHICommandListImmediate& RHICmdList)
{
	if (!bCollisionFeedbackEnabled)
	{
		return;
	}

	// Ensure readback objects are allocated
	if (FeedbackReadbacks[0] == nullptr)
	{
		return;  // Will be allocated in SimulateSubstep
	}

	// Read from readback that was enqueued 2 frames ago (allowing GPU latency)
	// Workaround: Search for any ready buffer instead of calculated index
	int32 ReadIdx = -1;
	for (int32 i = 0; i < NUM_FEEDBACK_BUFFERS; ++i)
	{
		if (CounterReadbacks[i] && CounterReadbacks[i]->IsReady())
		{
			ReadIdx = i;
			break;
		}
	}

	// Only read if we have completed at least 2 frames and found a ready buffer
	if (FeedbackFrameNumber >= 2 && ReadIdx >= 0)
	{
		// Read counter first
		uint32 FeedbackCount = 0;
		{
			const uint32* CounterData = (const uint32*)CounterReadbacks[ReadIdx]->Lock(sizeof(uint32));
			if (CounterData)
			{
				FeedbackCount = *CounterData;
			}
			CounterReadbacks[ReadIdx]->Unlock();
		}

		// Clamp to max
		FeedbackCount = FMath::Min(FeedbackCount, (uint32)MAX_COLLISION_FEEDBACK);

		// Read feedback data if any and if ready
		if (FeedbackCount > 0 && FeedbackReadbacks[ReadIdx]->IsReady())
		{
			FScopeLock Lock(&FeedbackLock);

			const uint32 CopySize = FeedbackCount * sizeof(FGPUCollisionFeedback);
			const FGPUCollisionFeedback* FeedbackData = (const FGPUCollisionFeedback*)FeedbackReadbacks[ReadIdx]->Lock(CopySize);

			if (FeedbackData)
			{
				ReadyFeedback.SetNum(FeedbackCount);
				FMemory::Memcpy(ReadyFeedback.GetData(), FeedbackData, CopySize);
				ReadyFeedbackCount = FeedbackCount;
			}

			FeedbackReadbacks[ReadIdx]->Unlock();

			UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Collision Feedback: Read %d entries from readback %d"), FeedbackCount, ReadIdx);
		}
		else if (FeedbackCount == 0)
		{
			FScopeLock Lock(&FeedbackLock);
			ReadyFeedbackCount = 0;
		}
	}
	// If not ready yet, skip this frame (data will be available next frame)

	// Note: Frame counter is incremented in SimulateSubstep AFTER EnqueueCopy, not here
}

void FGPUFluidSimulator::ProcessColliderContactCountReadback(FRHICommandListImmediate& RHICmdList)
{
	// Debug logging (every 60 frames)
	static int32 ContactCountDebugFrame = 0;
	const bool bLogThisFrame = (ContactCountDebugFrame++ % 60 == 0);

	// Ensure readback objects are valid
	if (ContactCountReadbacks[0] == nullptr)
	{
		if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[ContactCount] Readback objects not allocated"));
		return;  // Will be allocated in SimulateSubstep
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

	if (bLogThisFrame)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("[ContactCount] FrameNum=%d, ReadIdx=%d (searched), Condition(>=2)=%s"),
			ContactCountFrameNumber, ReadIdx, ContactCountFrameNumber >= 2 ? TEXT("TRUE") : TEXT("FALSE"));
	}

	// Only read if we have completed at least 2 frames and found a ready buffer
	if (ContactCountFrameNumber >= 2 && ReadIdx >= 0)
	{
		// Read contact counts - GPU has already completed the copy
		const uint32* CountData = (const uint32*)ContactCountReadbacks[ReadIdx]->Lock(MAX_COLLIDER_COUNT * sizeof(uint32));

		if (CountData)
		{
			FScopeLock Lock(&FeedbackLock);

			// Copy to ready array
			ReadyColliderContactCounts.SetNumUninitialized(MAX_COLLIDER_COUNT);
			int32 TotalContactCount = 0;
			int32 NonZeroColliders = 0;
			for (int32 i = 0; i < MAX_COLLIDER_COUNT; ++i)
			{
				ReadyColliderContactCounts[i] = static_cast<int32>(CountData[i]);
				if (CountData[i] > 0)
				{
					TotalContactCount += CountData[i];
					NonZeroColliders++;
				}
			}

			if (bLogThisFrame)
			{
				UE_LOG(LogGPUFluidSimulator, Log, TEXT("[ContactCount] Read success: TotalContacts=%d, NonZeroColliders=%d"),
					TotalContactCount, NonZeroColliders);
			}
		}
		else
		{
			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[ContactCount] Lock() failed - nullptr returned"));
		}

		ContactCountReadbacks[ReadIdx]->Unlock();
	}
	else if (bLogThisFrame)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[ContactCount] No ready buffer (ReadIdx=%d) - skipping data update"), ReadIdx);
	}

	// Note: Frame counter is incremented in SimulateSubstep AFTER EnqueueCopy, not here
}

//=============================================================================
// Collision Feedback Query API
//=============================================================================

bool FGPUFluidSimulator::GetCollisionFeedbackForCollider(int32 ColliderIndex, TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	FScopeLock Lock(&FeedbackLock);

	OutFeedback.Reset();
	OutCount = 0;

	if (!bCollisionFeedbackEnabled || ReadyFeedbackCount == 0)
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

bool FGPUFluidSimulator::GetAllCollisionFeedback(TArray<FGPUCollisionFeedback>& OutFeedback, int32& OutCount)
{
	FScopeLock Lock(&FeedbackLock);

	OutCount = ReadyFeedbackCount;

	if (!bCollisionFeedbackEnabled || ReadyFeedbackCount == 0)
	{
		OutFeedback.Reset();
		return false;
	}

	OutFeedback.SetNum(ReadyFeedbackCount);
	FMemory::Memcpy(OutFeedback.GetData(), ReadyFeedback.GetData(), ReadyFeedbackCount * sizeof(FGPUCollisionFeedback));

	return true;
}

//=============================================================================
// Collider Contact Count API
//=============================================================================

int32 FGPUFluidSimulator::GetColliderContactCount(int32 ColliderIndex) const
{
	if (ColliderIndex < 0 || ColliderIndex >= ReadyColliderContactCounts.Num())
	{
		return 0;
	}
	return ReadyColliderContactCounts[ColliderIndex];
}

void FGPUFluidSimulator::GetAllColliderContactCounts(TArray<int32>& OutCounts) const
{
	OutCounts = ReadyColliderContactCounts;
}

int32 FGPUFluidSimulator::GetTotalColliderCount() const
{
	return CachedSpheres.Num() + CachedCapsules.Num() + CachedBoxes.Num() + CachedConvexHeaders.Num();
}

int32 FGPUFluidSimulator::GetContactCountForOwner(int32 OwnerID) const
{
	// Debug logging (every 60 frames)
	static int32 OwnerCountDebugFrame = 0;
	const bool bLogThisFrame = (OwnerCountDebugFrame++ % 60 == 0);

	int32 TotalCount = 0;
	int32 ColliderIndex = 0;
	int32 MatchedColliders = 0;

	// Spheres: indices 0 to SphereCount-1
	for (int32 i = 0; i < CachedSpheres.Num(); ++i)
	{
		if (CachedSpheres[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	// Capsules: indices SphereCount to SphereCount+CapsuleCount-1
	for (int32 i = 0; i < CachedCapsules.Num(); ++i)
	{
		if (CachedCapsules[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	// Boxes: indices SphereCount+CapsuleCount to SphereCount+CapsuleCount+BoxCount-1
	for (int32 i = 0; i < CachedBoxes.Num(); ++i)
	{
		if (CachedBoxes[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	// Convexes: remaining indices
	for (int32 i = 0; i < CachedConvexHeaders.Num(); ++i)
	{
		if (CachedConvexHeaders[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	if (bLogThisFrame && MatchedColliders > 0)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("[ContactCountForOwner] OwnerID=%d, MatchedColliders=%d, TotalCount=%d"),
			OwnerID, MatchedColliders, TotalCount);
	}

	return TotalCount;
}
