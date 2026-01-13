// Copyright KawaiiFluid Team. All Rights Reserved.

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/FluidAnisotropyComputeShader.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiFluidSimulationStats.h"
#include "Rendering/Shaders/FluidSpatialHashShaders.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"  // FRHIGPUBufferReadback for async GPU→CPU readback
#include "RenderingThread.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "PipelineStateCache.h"
#include "HAL/IConsoleManager.h"  // For console command execution
#include "Async/Async.h"  // For AsyncTask

DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);
DEFINE_LOG_CATEGORY(LogGPUFluidSimulator);

// =====================================================
// Debug CVars for RenderDoc Capture
// =====================================================
static int32 GFluidCaptureFirstFrame = 0;  // 0 = disabled by default
static FAutoConsoleVariableRef CVarFluidCaptureFirstFrame(
	TEXT("r.Fluid.CaptureFirstFrame"),
	GFluidCaptureFirstFrame,
	TEXT("Capture first GPU fluid simulation frame in RenderDoc.\n")
	TEXT("  0 = Disabled (default)\n")
	TEXT("  1 = Capture first frame\n")
	TEXT("Set to 1 and restart PIE to capture first frame."),
	ECVF_Default
);

static int32 GFluidCaptureFrameNumber = 0;  // 0 = use CaptureFirstFrame, >0 = capture specific frame
static FAutoConsoleVariableRef CVarFluidCaptureFrameNumber(
	TEXT("r.Fluid.CaptureFrameNumber"),
	GFluidCaptureFrameNumber,
	TEXT("Capture specific GPU fluid simulation frame number in RenderDoc.\n")
	TEXT("  0 = Use CaptureFirstFrame setting\n")
	TEXT("  N = Capture frame N (1-indexed)"),
	ECVF_Default
);

// Internal flag - reset when CVar is changed back to 1
static int32 GFluidCapturedFrame = 0;  // Tracks which frame was captured (0 = none)

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUFluidSimulator::FGPUFluidSimulator()
	: bIsInitialized(false)
	, MaxParticleCount(0)
	, CurrentParticleCount(0)
	, ExternalForce(FVector3f::ZeroVector)
	, MaxVelocity(50000.0f)   // Safety clamp: 50000 cm/s = 500 m/s
{
}

FGPUFluidSimulator::~FGPUFluidSimulator()
{
	if (bIsInitialized)
	{
		Release();
	}
}

//=============================================================================
// Initialization
//=============================================================================

void FGPUFluidSimulator::Initialize(int32 InMaxParticleCount)
{
	if (InMaxParticleCount <= 0)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("Initialize called with invalid particle count: %d"), InMaxParticleCount);
		return;
	}

	MaxParticleCount = InMaxParticleCount;

	// Initialize render resource on render thread
	BeginInitResource(this);

	bIsInitialized = true;

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Fluid Simulator initialized with capacity: %d particles"), MaxParticleCount);
}

void FGPUFluidSimulator::Release()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Release render resource synchronously to avoid "deleted without being released" error
	// BeginReleaseResource is async, so we need to flush to ensure completion before destruction
	ReleaseResource();  // Synchronous release - blocks until render thread completes

	bIsInitialized = false;
	MaxParticleCount = 0;
	CurrentParticleCount = 0;
	bHasValidGPUResults.store(false);
	ReadbackGPUParticles.Empty();
	CachedGPUParticles.Empty();
	PersistentParticleBuffer = nullptr;

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Fluid Simulator released"));
}

//=============================================================================
// FRenderResource Interface
//=============================================================================

void FGPUFluidSimulator::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (MaxParticleCount <= 0)
	{
		return;
	}

	ResizeBuffers(RHICmdList, MaxParticleCount);
}

void FGPUFluidSimulator::ReleaseRHI()
{
	FScopeLock Lock(&BufferLock);

	ParticleBufferRHI.SafeRelease();
	ParticleSRV.SafeRelease();
	ParticleUAV.SafeRelease();

	PositionBufferRHI.SafeRelease();
	PositionSRV.SafeRelease();
	PositionUAV.SafeRelease();

	StagingBufferRHI.SafeRelease();

	CellCountsBufferRHI.SafeRelease();
	CellCountsSRV.SafeRelease();
	CellCountsUAV.SafeRelease();

	ParticleIndicesBufferRHI.SafeRelease();
	ParticleIndicesSRV.SafeRelease();
	ParticleIndicesUAV.SafeRelease();

	CachedGPUParticles.Empty();

	// Release persistent pooled buffers
	PersistentParticleBuffer.SafeRelease();
	PersistentCellCountsBuffer.SafeRelease();
	PersistentParticleIndicesBuffer.SafeRelease();

	// Clear collision primitives
	CachedSpheres.Empty();
	CachedCapsules.Empty();
	CachedBoxes.Empty();
	CachedConvexHeaders.Empty();
	CachedConvexPlanes.Empty();
	bCollisionPrimitivesValid = false;

	// Release collision feedback buffers
	ReleaseCollisionFeedbackBuffers();
}

void FGPUFluidSimulator::ResizeBuffers(FRHICommandListBase& RHICmdList, int32 NewCapacity)
{
	FScopeLock Lock(&BufferLock);

	// Release existing buffers
	ParticleBufferRHI.SafeRelease();
	ParticleSRV.SafeRelease();
	ParticleUAV.SafeRelease();
	PositionBufferRHI.SafeRelease();
	PositionSRV.SafeRelease();
	PositionUAV.SafeRelease();
	StagingBufferRHI.SafeRelease();
	CellCountsBufferRHI.SafeRelease();
	CellCountsSRV.SafeRelease();
	CellCountsUAV.SafeRelease();
	ParticleIndicesBufferRHI.SafeRelease();
	ParticleIndicesSRV.SafeRelease();
	ParticleIndicesUAV.SafeRelease();

	if (NewCapacity <= 0)
	{
		return;
	}

	// Create main particle buffer (UAV + SRV)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidParticleBuffer"), NewCapacity * sizeof(FGPUFluidParticle), sizeof(FGPUFluidParticle))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		ParticleBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

		ParticleSRV = RHICmdList.CreateShaderResourceView(ParticleBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(ParticleBufferRHI));
		ParticleUAV = RHICmdList.CreateUnorderedAccessView(ParticleBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(ParticleBufferRHI));
	}

	// Create position buffer for spatial hash (float3 only)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidPositionBuffer"), NewCapacity * sizeof(FVector3f), sizeof(FVector3f))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		PositionBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

		PositionSRV = RHICmdList.CreateShaderResourceView(PositionBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(PositionBufferRHI));
		PositionUAV = RHICmdList.CreateUnorderedAccessView(PositionBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(PositionBufferRHI));
	}

	// Create staging buffer for CPU readback
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidStagingBuffer"), NewCapacity * sizeof(FGPUFluidParticle), sizeof(FGPUFluidParticle))
			.AddUsage(BUF_None)
			.SetInitialState(ERHIAccess::CopyDest);
		StagingBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
	}

	// Create spatial hash buffers
	{
		// Cell counts buffer
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidCellCounts"), GPU_SPATIAL_HASH_SIZE * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		CellCountsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

		CellCountsSRV = RHICmdList.CreateShaderResourceView(CellCountsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(CellCountsBufferRHI));
		CellCountsUAV = RHICmdList.CreateUnorderedAccessView(CellCountsBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(CellCountsBufferRHI));

		// Particle indices buffer
		const uint32 TotalSlots = GPU_SPATIAL_HASH_SIZE * GPU_MAX_PARTICLES_PER_CELL;
		const FRHIBufferCreateDesc BufferDesc2 =
			FRHIBufferCreateDesc::CreateStructured(TEXT("GPUFluidParticleIndices"), TotalSlots * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		ParticleIndicesBufferRHI = RHICmdList.CreateBuffer(BufferDesc2);

		ParticleIndicesSRV = RHICmdList.CreateShaderResourceView(ParticleIndicesBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(ParticleIndicesBufferRHI));
		ParticleIndicesUAV = RHICmdList.CreateUnorderedAccessView(ParticleIndicesBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(ParticleIndicesBufferRHI));
	}

	// Resize cached array
	CachedGPUParticles.SetNumUninitialized(NewCapacity);

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Resized GPU buffers to capacity: %d"), NewCapacity);
}

//=============================================================================
// Data Transfer
//=============================================================================

FGPUFluidParticle FGPUFluidSimulator::ConvertToGPU(const FFluidParticle& CPUParticle)
{
	FGPUFluidParticle GPUParticle;

	GPUParticle.Position = FVector3f(CPUParticle.Position);
	GPUParticle.Mass = CPUParticle.Mass;
	GPUParticle.PredictedPosition = FVector3f(CPUParticle.PredictedPosition);
	GPUParticle.Density = CPUParticle.Density;
	GPUParticle.Velocity = FVector3f(CPUParticle.Velocity);
	GPUParticle.Lambda = CPUParticle.Lambda;
	GPUParticle.ParticleID = CPUParticle.ParticleID;
	GPUParticle.SourceID = CPUParticle.SourceID;

	// Pack flags
	uint32 Flags = 0;
	if (CPUParticle.bIsAttached) Flags |= EGPUParticleFlags::IsAttached;
	if (CPUParticle.bIsSurfaceParticle) Flags |= EGPUParticleFlags::IsSurface;
	if (CPUParticle.bIsCoreParticle) Flags |= EGPUParticleFlags::IsCore;
	if (CPUParticle.bJustDetached) Flags |= EGPUParticleFlags::JustDetached;
	if (CPUParticle.bNearGround) Flags |= EGPUParticleFlags::NearGround;
	GPUParticle.Flags = Flags;

	// NeighborCount is calculated on GPU during density solve
	GPUParticle.NeighborCount = 0;

	return GPUParticle;
}

void FGPUFluidSimulator::ConvertFromGPU(FFluidParticle& OutCPUParticle, const FGPUFluidParticle& GPUParticle)
{
	// Safety check: validate GPU data before converting
	// If data is NaN or invalid, keep the original CPU values
	FVector NewPosition = FVector(GPUParticle.Position);
	FVector NewVelocity = FVector(GPUParticle.Velocity);

	// Check for NaN or extremely large values (indicates invalid data)
	const float MaxValidValue = 1000000.0f;
	bool bValidPosition = !NewPosition.ContainsNaN() && NewPosition.GetAbsMax() < MaxValidValue;
	bool bValidVelocity = !NewVelocity.ContainsNaN() && NewVelocity.GetAbsMax() < MaxValidValue;

	if (!bValidPosition || !bValidVelocity)
	{
		// Invalid GPU data - don't update the particle
		// This can happen if readback hasn't completed yet
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ConvertFromGPU: Invalid data detected (NaN or extreme values) - skipping update"));
			bLoggedOnce = true;
		}
		return;
	}

	OutCPUParticle.Position = NewPosition;
	OutCPUParticle.PredictedPosition = FVector(GPUParticle.PredictedPosition);
	OutCPUParticle.Velocity = NewVelocity;
	OutCPUParticle.Mass = FMath::IsFinite(GPUParticle.Mass) ? GPUParticle.Mass : OutCPUParticle.Mass;
	OutCPUParticle.Density = FMath::IsFinite(GPUParticle.Density) ? GPUParticle.Density : OutCPUParticle.Density;
	OutCPUParticle.Lambda = FMath::IsFinite(GPUParticle.Lambda) ? GPUParticle.Lambda : OutCPUParticle.Lambda;

	// Unpack flags
	OutCPUParticle.bJustDetached = (GPUParticle.Flags & EGPUParticleFlags::JustDetached) != 0;
	OutCPUParticle.bNearGround = (GPUParticle.Flags & EGPUParticleFlags::NearGround) != 0;

	// Note: bIsAttached is not updated from GPU - CPU handles attachment state
}

void FGPUFluidSimulator::UploadParticles(const TArray<FFluidParticle>& CPUParticles)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("UploadParticles: Simulator not initialized"));
		return;
	}

	const int32 NewCount = CPUParticles.Num();
	if (NewCount == 0)
	{
		CurrentParticleCount = 0;
		CachedGPUParticles.Empty();
		return;
	}

	if (NewCount > MaxParticleCount)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("UploadParticles: Particle count (%d) exceeds capacity (%d)"),
			NewCount, MaxParticleCount);
		return;
	}

	FScopeLock Lock(&BufferLock);

	// Store old count for comparison BEFORE updating
	const int32 OldCount = CurrentParticleCount;

	// Determine upload strategy based on persistent buffer state and particle count changes
	const bool bHasPersistentBuffer = PersistentParticleBuffer.IsValid() && OldCount > 0;
	const bool bSameCount = bHasPersistentBuffer && (NewCount == OldCount);
	const bool bCanAppend = bHasPersistentBuffer && (NewCount > OldCount);

	if (bSameCount)
	{
		// Same particle count - NO UPLOAD needed, reuse GPU buffer entirely
		// GPU simulation results are preserved in PersistentParticleBuffer
		NewParticleCount = 0;
		NewParticlesToAppend.Empty();
		// Note: Don't set bNeedsFullUpload = false here, it should already be false

		static int32 ReuseLogCounter = 0;
		if (++ReuseLogCounter % 60 == 0)  // Log every 60 frames
		{
			UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadParticles: Reusing GPU buffer (no upload, %d particles)"), OldCount);
		}
		return;  // Skip upload entirely!
	}
	else if (bCanAppend)
	{
		// Only cache the NEW particles (indices OldCount to NewCount-1)
		const int32 NumNewParticles = NewCount - OldCount;
		NewParticlesToAppend.SetNumUninitialized(NumNewParticles);

		for (int32 i = 0; i < NumNewParticles; ++i)
		{
			NewParticlesToAppend[i] = ConvertToGPU(CPUParticles[OldCount + i]);
		}

		NewParticleCount = NumNewParticles;
		CurrentParticleCount = NewCount;

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadParticles: Appending %d new particles (total: %d)"),
			NumNewParticles, NewCount);
	}
	else
	{
		// Full upload needed: first frame, buffer invalid, or particles reduced
		CachedGPUParticles.SetNumUninitialized(NewCount);

		// Convert particles to GPU format
		for (int32 i = 0; i < NewCount; ++i)
		{
			CachedGPUParticles[i] = ConvertToGPU(CPUParticles[i]);
		}

		// Simulation bounds for Morton code (Z-Order sorting) are set via SetSimulationBounds()
		// from SimulateGPU before this call (preset bounds + component location offset)
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadParticles: Using bounds: Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f)"),
			SimulationBoundsMin.X, SimulationBoundsMin.Y, SimulationBoundsMin.Z,
			SimulationBoundsMax.X, SimulationBoundsMax.Y, SimulationBoundsMax.Z);

		NewParticleCount = 0;
		NewParticlesToAppend.Empty();
		CurrentParticleCount = NewCount;
		bNeedsFullUpload = true;
	}
}

void FGPUFluidSimulator::DownloadParticles(TArray<FFluidParticle>& OutCPUParticles)
{
	if (!bIsInitialized || CurrentParticleCount == 0)
	{
		return;
	}

	// Only download if we have valid GPU results from a previous simulation
	if (!bHasValidGPUResults.load())
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogGPUFluidSimulator, Log, TEXT("DownloadParticles: No valid GPU results yet, skipping"));
			bLoggedOnce = true;
		}
		return;
	}

	FScopeLock Lock(&BufferLock);

	// Read from separate readback buffer (not CachedGPUParticles)
	const int32 Count = ReadbackGPUParticles.Num();
	if (Count == 0)
	{
		return;
	}

	// Build ParticleID → CPU index map for matching
	TMap<int32, int32> ParticleIDToIndex;
	ParticleIDToIndex.Reserve(OutCPUParticles.Num());
	for (int32 i = 0; i < OutCPUParticles.Num(); ++i)
	{
		ParticleIDToIndex.Add(OutCPUParticles[i].ParticleID, i);
	}

	// Debug: Log first particle before conversion
	static int32 DebugFrameCounter = 0;
	if (DebugFrameCounter++ % 60 == 0)
	{
		const FGPUFluidParticle& P = ReadbackGPUParticles[0];
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("DownloadParticles: GPUCount=%d, CPUCount=%d, Readback[0] Pos=(%.2f, %.2f, %.2f)"),
			Count, OutCPUParticles.Num(), P.Position.X, P.Position.Y, P.Position.Z);
	}

	// Update existing particles by matching ParticleID (don't overwrite newly spawned ones)
	// Also track bounds to detect Black Hole Cell potential
	int32 UpdatedCount = 0;
	int32 OutOfBoundsCount = 0;
	const float BoundsMargin = 100.0f;  // Warn if particles within 100 units of bounds edge

	for (int32 i = 0; i < Count; ++i)
	{
		const FGPUFluidParticle& GPUParticle = ReadbackGPUParticles[i];
		if (int32* CPUIndex = ParticleIDToIndex.Find(GPUParticle.ParticleID))
		{
			ConvertFromGPU(OutCPUParticles[*CPUIndex], GPUParticle);
			++UpdatedCount;

			// Check if particle is near or outside bounds
			const FVector3f& Pos = GPUParticle.PredictedPosition;
			if (Pos.X < SimulationBoundsMin.X + BoundsMargin ||
				Pos.Y < SimulationBoundsMin.Y + BoundsMargin ||
				Pos.Z < SimulationBoundsMin.Z + BoundsMargin ||
				Pos.X > SimulationBoundsMax.X - BoundsMargin ||
				Pos.Y > SimulationBoundsMax.Y - BoundsMargin ||
				Pos.Z > SimulationBoundsMax.Z - BoundsMargin)
			{
				OutOfBoundsCount++;
			}
		}
	}

	// Warn if many particles are near bounds edge (potential Black Hole Cell issue)
	static int32 LastBoundsWarningFrame = -1000;
	if (OutOfBoundsCount > Count / 10 && (GFrameCounter - LastBoundsWarningFrame) > 300)  // >10% near edge, warn every 5 sec
	{
		LastBoundsWarningFrame = GFrameCounter;
		UE_LOG(LogGPUFluidSimulator, Warning,
			TEXT("Z-Order WARNING: %d/%d particles (%.1f%%) are near simulation bounds edge! "
			     "This may cause Black Hole Cell problem with Z-Order sorting. "
			     "Bounds: Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f)"),
			OutOfBoundsCount, Count, 100.0f * OutOfBoundsCount / Count,
			SimulationBoundsMin.X, SimulationBoundsMin.Y, SimulationBoundsMin.Z,
			SimulationBoundsMax.X, SimulationBoundsMax.Y, SimulationBoundsMax.Z);
	}

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("DownloadParticles: Updated %d/%d particles"), UpdatedCount, Count);
}

bool FGPUFluidSimulator::GetAllGPUParticles(TArray<FFluidParticle>& OutParticles)
{
	if (!bIsInitialized || CurrentParticleCount == 0)
	{
		return false;
	}

	// Only download if we have valid GPU results from a previous simulation
	if (!bHasValidGPUResults.load())
	{
		return false;
	}

	FScopeLock Lock(&BufferLock);

	// Read from readback buffer
	const int32 Count = ReadbackGPUParticles.Num();
	if (Count == 0)
	{
		return false;
	}

	// Create new particles from GPU data (no ParticleID matching required)
	OutParticles.SetNum(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		const FGPUFluidParticle& GPUParticle = ReadbackGPUParticles[i];
		FFluidParticle& OutParticle = OutParticles[i];

		// Initialize with default values
		OutParticle = FFluidParticle();

		// Convert GPU data to CPU particle
		FVector NewPosition = FVector(GPUParticle.Position);
		FVector NewVelocity = FVector(GPUParticle.Velocity);

		// Validate data
		const float MaxValidValue = 1000000.0f;
		bool bValidPosition = !NewPosition.ContainsNaN() && NewPosition.GetAbsMax() < MaxValidValue;
		bool bValidVelocity = !NewVelocity.ContainsNaN() && NewVelocity.GetAbsMax() < MaxValidValue;

		if (bValidPosition)
		{
			OutParticle.Position = NewPosition;
			OutParticle.PredictedPosition = FVector(GPUParticle.PredictedPosition);
		}

		if (bValidVelocity)
		{
			OutParticle.Velocity = NewVelocity;
		}

		OutParticle.Mass = FMath::IsFinite(GPUParticle.Mass) ? GPUParticle.Mass : 1.0f;
		OutParticle.Density = FMath::IsFinite(GPUParticle.Density) ? GPUParticle.Density : 0.0f;
		OutParticle.Lambda = FMath::IsFinite(GPUParticle.Lambda) ? GPUParticle.Lambda : 0.0f;
		OutParticle.ParticleID = GPUParticle.ParticleID;
		OutParticle.SourceID = GPUParticle.SourceID;

		// Unpack flags
		OutParticle.bIsAttached = (GPUParticle.Flags & EGPUParticleFlags::IsAttached) != 0;
		OutParticle.bIsSurfaceParticle = (GPUParticle.Flags & EGPUParticleFlags::IsSurface) != 0;
		OutParticle.bIsCoreParticle = (GPUParticle.Flags & EGPUParticleFlags::IsCore) != 0;
		OutParticle.bJustDetached = (GPUParticle.Flags & EGPUParticleFlags::JustDetached) != 0;
		OutParticle.bNearGround = (GPUParticle.Flags & EGPUParticleFlags::NearGround) != 0;

		// Set neighbor count (resize array so NeighborIndices.Num() returns the count)
		// GPU stores count only, not actual indices (computed on-the-fly during spatial hash queries)
		if (GPUParticle.NeighborCount > 0)
		{
			OutParticle.NeighborIndices.SetNum(GPUParticle.NeighborCount);
		}
	}

	static int32 DebugFrameCounter = 0;
	if (++DebugFrameCounter % 60 == 0)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GetAllGPUParticles: Retrieved %d particles"), Count);
	}

	return true;
}

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
// Boundary Particles Upload (Flex-style Adhesion)
//=============================================================================

void FGPUFluidSimulator::UploadBoundaryParticles(const FGPUBoundaryParticles& BoundaryParticles)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&BufferLock);

	// Cache the boundary particle data (will be uploaded to GPU during simulation)
	CachedBoundaryParticles = BoundaryParticles.Particles;

	// Check if we have any boundary particles
	if (BoundaryParticles.IsEmpty())
	{
		bBoundaryParticlesValid = false;
		return;
	}

	bBoundaryParticlesValid = true;

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("Cached boundary particles: Count=%d"),
		CachedBoundaryParticles.Num());
}

//=============================================================================
// GPU Boundary Skinning (Persistent Local Boundary + GPU Transform)
//=============================================================================

void FGPUFluidSimulator::UploadLocalBoundaryParticles(int32 OwnerID, const TArray<FGPUBoundaryParticleLocal>& LocalParticles)
{
	if (!bIsInitialized || LocalParticles.Num() == 0)
	{
		return;
	}

	FScopeLock Lock(&BoundarySkinningLock);

	// Find or create skinning data for this owner
	FGPUBoundarySkinningData& SkinningData = BoundarySkinningDataMap.FindOrAdd(OwnerID);
	SkinningData.OwnerID = OwnerID;
	SkinningData.LocalParticles = LocalParticles;
	SkinningData.bLocalParticlesUploaded = false;  // Mark for upload
	bBoundarySkinningDataDirty = true;

	// Recalculate total count
	TotalLocalBoundaryParticleCount = 0;
	for (const auto& Pair : BoundarySkinningDataMap)
	{
		TotalLocalBoundaryParticleCount += Pair.Value.LocalParticles.Num();
	}

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("UploadLocalBoundaryParticles: OwnerID=%d, Count=%d, TotalCount=%d"),
		OwnerID, LocalParticles.Num(), TotalLocalBoundaryParticleCount);
}

void FGPUFluidSimulator::UploadBoneTransformsForBoundary(int32 OwnerID, const TArray<FMatrix44f>& BoneTransforms, const FMatrix44f& ComponentTransform)
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&BoundarySkinningLock);

	FGPUBoundarySkinningData* SkinningData = BoundarySkinningDataMap.Find(OwnerID);
	if (SkinningData)
	{
		SkinningData->BoneTransforms = BoneTransforms;
		SkinningData->ComponentTransform = ComponentTransform;
	}
}

void FGPUFluidSimulator::RemoveBoundarySkinningData(int32 OwnerID)
{
	FScopeLock Lock(&BoundarySkinningLock);

	if (BoundarySkinningDataMap.Remove(OwnerID) > 0)
	{
		// Remove persistent buffer for this owner
		PersistentLocalBoundaryBuffers.Remove(OwnerID);

		// Recalculate total count
		TotalLocalBoundaryParticleCount = 0;
		for (const auto& Pair : BoundarySkinningDataMap)
		{
			TotalLocalBoundaryParticleCount += Pair.Value.LocalParticles.Num();
		}

		bBoundarySkinningDataDirty = true;

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("RemoveBoundarySkinningData: OwnerID=%d, TotalCount=%d"),
			OwnerID, TotalLocalBoundaryParticleCount);
	}
}

void FGPUFluidSimulator::ClearAllBoundarySkinningData()
{
	FScopeLock Lock(&BoundarySkinningLock);

	BoundarySkinningDataMap.Empty();
	PersistentLocalBoundaryBuffers.Empty();
	PersistentWorldBoundaryBuffer.SafeRelease();
	WorldBoundaryBufferCapacity = 0;
	TotalLocalBoundaryParticleCount = 0;
	bBoundarySkinningDataDirty = true;

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("ClearAllBoundarySkinningData"));
}

//=============================================================================
// Simulation Execution
//=============================================================================

void FGPUFluidSimulator::SimulateSubstep(const FGPUFluidSimulationParams& Params)
{
	if (!bIsInitialized)
	{
		return;
	}

	// Check if we have particles OR pending spawn requests
	// (Allow simulation to start with spawn requests even if CurrentParticleCount == 0)
	const bool bHasPendingSpawns = bHasPendingSpawnRequests.load();
	if (CurrentParticleCount == 0 && !bHasPendingSpawns)
	{
		return;
	}

	FGPUFluidSimulator* Self = this;
	FGPUFluidSimulationParams ParamsCopy = Params;

	// =====================================================
	// PRE-SIMULATION: Check IsReady() and Lock readback buffers (2 frames ago)
	// Using FRHIGPUBufferReadback - only Lock when IsReady() == true (no flush!)
	// =====================================================
	ENQUEUE_RENDER_COMMAND(GPUFluidReadback)(
		[Self](FRHICommandListImmediate& RHICmdList)
		{
			// Process collision feedback readback (reads from readback enqueued 2 frames ago)
			if (Self->bCollisionFeedbackEnabled && Self->FeedbackReadbacks[0] != nullptr)
			{
				Self->ProcessCollisionFeedbackReadback(RHICmdList);
			}

			// Process collider contact count readback (reads from readback enqueued 2 frames ago)
			if (Self->ContactCountReadbacks[0] != nullptr)
			{
				Self->ProcessColliderContactCountReadback(RHICmdList);
			}
		}
	);

	// =====================================================
	// MAIN SIMULATION: Execute GPU simulation and EnqueueCopy to readback buffers
	// Lock is NOT here - it's in the previous render command (only when IsReady)
	// =====================================================
	ENQUEUE_RENDER_COMMAND(GPUFluidSimulate)(
		[Self, ParamsCopy](FRHICommandListImmediate& RHICmdList)
		{
			// Limit logging to first 10 frames
			static int32 RenderFrameCounter = 0;
			const bool bLogThisFrame = (RenderFrameCounter++ < 10);

			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RENDER COMMAND START (Frame %d)"), RenderFrameCounter);

			// Note: Don't reset PersistentParticleBuffer here - it breaks Phase 2 GPU→GPU rendering
			// The buffer is maintained across frames for renderer access

			// Build and execute RDG
			FRDGBuilder GraphBuilder(RHICmdList);
			Self->SimulateSubstep_RDG(GraphBuilder, ParamsCopy);

			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RDG EXECUTE START"));
			GraphBuilder.Execute();
			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> RDG EXECUTE COMPLETE"));

			// =====================================================
			// Phase 2: Conditional readback for detailed stats only
			// Normal mode: GPU buffer is source of truth (no CPU readback)
			// Detailed stats mode: Perform readback for velocity/density analysis
			// =====================================================

			// Check if any readback is needed (detailed stats OR debug visualization)
			const bool bNeedReadback = GetFluidStatsCollector().IsAnyReadbackNeeded();

			if (bNeedReadback && Self->StagingBufferRHI.IsValid() && Self->PersistentParticleBuffer.IsValid())
			{
				const int32 ParticleCount = Self->CurrentParticleCount;
				const int32 CopySize = ParticleCount * sizeof(FGPUFluidParticle);

				if (ParticleCount > 0 && CopySize > 0)
				{
					// Transition persistent buffer for copy
					RHICmdList.Transition(FRHITransitionInfo(
						Self->PersistentParticleBuffer->GetRHI(),
						ERHIAccess::UAVCompute,
						ERHIAccess::CopySrc));

					// Copy from GPU buffer to staging buffer
					RHICmdList.CopyBufferRegion(
						Self->StagingBufferRHI,
						0,
						Self->PersistentParticleBuffer->GetRHI(),
						0,
						CopySize);

					// Read from staging buffer to CPU
					{
						FScopeLock Lock(&Self->BufferLock);
						Self->ReadbackGPUParticles.SetNumUninitialized(ParticleCount);

						FGPUFluidParticle* DataPtr = (FGPUFluidParticle*)RHICmdList.LockBuffer(
							Self->StagingBufferRHI, 0, CopySize, RLM_ReadOnly);
						FMemory::Memcpy(Self->ReadbackGPUParticles.GetData(), DataPtr, CopySize);
						RHICmdList.UnlockBuffer(Self->StagingBufferRHI);
					}

					// Transition back for next frame's compute
					RHICmdList.Transition(FRHITransitionInfo(
						Self->PersistentParticleBuffer->GetRHI(),
						ERHIAccess::CopySrc,
						ERHIAccess::UAVCompute));

					if (bLogThisFrame)
					{
						UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> READBACK: Copied %d particles for detailed stats"), ParticleCount);
					}
				}
			}

			// =====================================================
			// Phase 3: Collision Feedback Readback (Particle -> Player Interaction)
			// Using FRHIGPUBufferReadback for truly async readback (no flush!)
			// =====================================================

			if (Self->bCollisionFeedbackEnabled && Self->CollisionFeedbackBuffer.IsValid() && Self->CollisionCounterBuffer.IsValid())
			{
				// Ensure readback objects are allocated
				if (Self->FeedbackReadbacks[0] == nullptr)
				{
					Self->AllocateCollisionFeedbackBuffers(RHICmdList);
				}

				const int32 WriteIdx = Self->FeedbackFrameNumber % Self->NUM_FEEDBACK_BUFFERS;

				// Transition buffers for copy
				RHICmdList.Transition(FRHITransitionInfo(
					Self->CollisionFeedbackBuffer->GetRHI(),
					ERHIAccess::UAVCompute,
					ERHIAccess::CopySrc));

				RHICmdList.Transition(FRHITransitionInfo(
					Self->CollisionCounterBuffer->GetRHI(),
					ERHIAccess::UAVCompute,
					ERHIAccess::CopySrc));

				// EnqueueCopy - async copy to readback buffer (non-blocking!)
				Self->FeedbackReadbacks[WriteIdx]->EnqueueCopy(
					RHICmdList,
					Self->CollisionFeedbackBuffer->GetRHI(),
					Self->MAX_COLLISION_FEEDBACK * sizeof(FGPUCollisionFeedback)
				);

				Self->CounterReadbacks[WriteIdx]->EnqueueCopy(
					RHICmdList,
					Self->CollisionCounterBuffer->GetRHI(),
					sizeof(uint32)
				);

				// Transition back for next frame
				RHICmdList.Transition(FRHITransitionInfo(
					Self->CollisionFeedbackBuffer->GetRHI(),
					ERHIAccess::CopySrc,
					ERHIAccess::UAVCompute));

				RHICmdList.Transition(FRHITransitionInfo(
					Self->CollisionCounterBuffer->GetRHI(),
					ERHIAccess::CopySrc,
					ERHIAccess::UAVCompute));

				// Increment frame counter AFTER EnqueueCopy
				Self->FeedbackFrameNumber++;

				if (bLogThisFrame)
				{
					UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> COLLISION FEEDBACK: EnqueueCopy to readback %d"), WriteIdx);
				}
			}

			// =====================================================
			// Copy Collider Contact Counts (FRHIGPUBufferReadback)
			// EnqueueCopy → WriteIdx, IsReady+Lock → ReadIdx (2프레임 전)
			// =====================================================
			if (Self->ColliderContactCountBuffer.IsValid())
			{
				// Ensure readback objects are allocated
				if (Self->ContactCountReadbacks[0] == nullptr)
				{
					Self->AllocateCollisionFeedbackBuffers(RHICmdList);
				}

				// Triple buffer index for this frame
				const int32 WriteIdx = Self->ContactCountFrameNumber % Self->NUM_FEEDBACK_BUFFERS;

				// 디버그 로그 (60프레임마다)
				static int32 EnqueueDebugFrame = 0;
				if (EnqueueDebugFrame++ % 60 == 0)
				{
					UE_LOG(LogGPUFluidSimulator, Log, TEXT("[ContactCount EnqueueCopy] WriteIdx=%d, FrameNum=%d, BufferValid=YES"),
						WriteIdx, Self->ContactCountFrameNumber);
				}

				// Transition for copy
				RHICmdList.Transition(FRHITransitionInfo(
					Self->ColliderContactCountBuffer->GetRHI(),
					ERHIAccess::UAVCompute,
					ERHIAccess::CopySrc));

				// EnqueueCopy - async copy to readback buffer (non-blocking!)
				Self->ContactCountReadbacks[WriteIdx]->EnqueueCopy(
					RHICmdList,
					Self->ColliderContactCountBuffer->GetRHI(),
					Self->MAX_COLLIDER_COUNT * sizeof(uint32)
				);

				// Transition back for next frame
				RHICmdList.Transition(FRHITransitionInfo(
					Self->ColliderContactCountBuffer->GetRHI(),
					ERHIAccess::CopySrc,
					ERHIAccess::UAVCompute));

				// Increment frame counter AFTER EnqueueCopy
				Self->ContactCountFrameNumber++;
			}
			else
			{
				// 버퍼가 유효하지 않음!
				static int32 InvalidBufferLogFrame = 0;
				if (InvalidBufferLogFrame++ % 60 == 0)
				{
					UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[ContactCount EnqueueCopy] ColliderContactCountBuffer가 유효하지 않음!"));
				}
			}

			// Mark that we have valid GPU results (buffer is ready for rendering)
			Self->bHasValidGPUResults.store(true);

			// Update PersistentParticleCount AFTER SimulateSubstep_RDG
			// CurrentParticleCount is updated inside SimulateSubstep_RDG during spawn
			Self->PersistentParticleCount.store(Self->CurrentParticleCount);

			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> AFTER EXECUTE: PersistentBuffer Valid=%s, PersistentCount=%d"),
				Self->PersistentParticleBuffer.IsValid() ? TEXT("YES") : TEXT("NO"),
				Self->PersistentParticleCount.load());
		}
	);
}

void FGPUFluidSimulator::SimulateSubstep_RDG(FRDGBuilder& GraphBuilder, const FGPUFluidSimulationParams& Params)
{
	// =====================================================
	// Phase 1: Swap spawn requests (double buffer for thread safety)
	// Game thread writes to PendingSpawnRequests
	// Render thread consumes from ActiveSpawnRequests
	// =====================================================
	{
		FScopeLock Lock(&SpawnRequestLock);
		ActiveSpawnRequests = MoveTemp(PendingSpawnRequests);
		PendingSpawnRequests.Empty();
		bHasPendingSpawnRequests.store(false);
	}

	const bool bHasSpawnRequests = ActiveSpawnRequests.Num() > 0;
	const int32 SpawnCount = ActiveSpawnRequests.Num();

	// Allow simulation to start even with 0 particles if we have spawn requests
	if (CurrentParticleCount == 0 && !bHasSpawnRequests)
	{
		return;
	}

	// Need either cached particles (for full upload), persistent buffer (for reuse/append), or spawn requests
	if (CachedGPUParticles.Num() == 0 && !PersistentParticleBuffer.IsValid() && !bHasSpawnRequests)
	{
		return;
	}

	// Calculate expected particle count after spawning
	const int32 ExpectedParticleCount = CurrentParticleCount + SpawnCount;

	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluidSimulation (Particles: %d, Spawning: %d)", CurrentParticleCount, SpawnCount);

	// =====================================================
	// Phase 2: Persistent GPU Buffer with Append Support
	// - First frame: full upload from CPU
	// - New particles added: copy existing + append new
	// - Same count: reuse GPU buffer (no CPU upload)
	// =====================================================

	FRDGBufferRef ParticleBuffer;

	// Determine operation mode
	const bool bHasNewParticles = (NewParticleCount > 0 && NewParticlesToAppend.Num() > 0);
	const bool bNeedFullUpload = bNeedsFullUpload || !PersistentParticleBuffer.IsValid();
	// First spawn: have spawn requests, no current particles, and no persistent buffer
	// Note: Check !PersistentParticleBuffer.IsValid() instead of !bNeedFullUpload because
	// bNeedsFullUpload may be true even when using GPU spawn system (it's not cleared by AddSpawnRequests)
	const bool bFirstSpawnOnly = bHasSpawnRequests && CurrentParticleCount == 0 && !PersistentParticleBuffer.IsValid();

	// DEBUG: Log which path is being taken (only first 10 frames to avoid spam)
	static int32 DebugFrameCounter = 0;
	const bool bShouldLog = (DebugFrameCounter++ < 10);

	// =====================================================
	// DEBUG: Trigger RenderDoc capture on configurable GPU simulation frame
	// This helps diagnose Z-Order sorting issues by capturing the first frame
	// where RadixSort runs on fresh data (not corrupted by previous frames)
	//
	// CVars:
	//   r.Fluid.CaptureFirstFrame 1  - Capture first simulation frame (default)
	//   r.Fluid.CaptureFrameNumber N - Capture specific frame N
	// =====================================================
	{
		bool bShouldCapture = false;
		int32 TargetFrame = 0;

		if (GFluidCaptureFrameNumber > 0)
		{
			// Capture specific frame number
			TargetFrame = GFluidCaptureFrameNumber;
			bShouldCapture = (DebugFrameCounter == TargetFrame && GFluidCapturedFrame != TargetFrame);
		}
		else if (GFluidCaptureFirstFrame != 0)
		{
			// Capture first frame
			TargetFrame = 1;
			bShouldCapture = (DebugFrameCounter == 1 && GFluidCapturedFrame == 0);
		}

		if (bShouldCapture)
		{
			GFluidCapturedFrame = TargetFrame;
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT(">>> TRIGGERING RENDERDOC CAPTURE ON GPU SIMULATION FRAME %d <<<"), DebugFrameCounter);

			// Trigger RenderDoc capture using console command
			// Execute on game thread since we're in render thread context
			// This works when editor is launched with -RenderDoc flag
			AsyncTask(ENamedThreads::GameThread, []()
			{
				IConsoleObject* CaptureCmd = IConsoleManager::Get().FindConsoleObject(TEXT("renderdoc.CaptureFrame"));
				if (CaptureCmd)
				{
					IConsoleCommand* Cmd = CaptureCmd->AsCommand();
					if (Cmd)
					{
						Cmd->Execute(TArray<FString>(), nullptr, *GLog);
						UE_LOG(LogGPUFluidSimulator, Warning, TEXT(">>> RenderDoc capture command executed successfully"));
					}
				}
				else
				{
					UE_LOG(LogGPUFluidSimulator, Warning, TEXT(">>> RenderDoc not available. Launch editor with -RenderDoc flag."));
				}
			});
		}
	}

	if (bShouldLog)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("=== BUFFER PATH DEBUG (Frame %d) ==="), DebugFrameCounter);
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bFirstSpawnOnly: %s"), bFirstSpawnOnly ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bNeedFullUpload: %s"), bNeedFullUpload ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bHasNewParticles: %s"), bHasNewParticles ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  bHasSpawnRequests: %s"), bHasSpawnRequests ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  PersistentBuffer Valid: %s"), PersistentParticleBuffer.IsValid() ? TEXT("TRUE") : TEXT("FALSE"));
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("  CurrentParticleCount: %d, SpawnCount: %d"), CurrentParticleCount, SpawnCount);
	}

	if (bFirstSpawnOnly)
	{
		// =====================================================
		// FIRST SPAWN: No existing particles, just spawn requests
		// Create buffer and use spawn pass to initialize
		// =====================================================
		const int32 BufferCapacity = FMath::Min(SpawnCount, MaxParticleCount);

		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> FIRST SPAWN PATH: BufferCapacity=%d, MaxParticleCount=%d"), BufferCapacity, MaxParticleCount);

		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		// Create counter buffer initialized to 0
		// IMPORTANT: Do NOT use NoCopy - InitialCounterData is a local variable that goes
		// out of scope before RDG executes. RDG must copy the data.
		TArray<uint32> InitialCounterData;
		InitialCounterData.Add(0);

		FRDGBufferRef CounterBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidParticleCounter"),
			sizeof(uint32),
			1,
			InitialCounterData.GetData(),
			sizeof(uint32),
			ERDGInitialDataFlags::None
		);
		FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(CounterBuffer);

		// Run spawn particles pass
		FRDGBufferUAVRef ParticleUAVForSpawn = GraphBuilder.CreateUAV(ParticleBuffer);
		AddSpawnParticlesPass(GraphBuilder, ParticleUAVForSpawn, CounterUAV, ActiveSpawnRequests);

		// Update particle count
		CurrentParticleCount = FMath::Min(SpawnCount, MaxParticleCount);
		PreviousParticleCount = CurrentParticleCount;

		// IMPORTANT: Clear the full upload flag - we've successfully created the buffer via spawn
		// Without this, Frame 2+ would incorrectly take the full upload path
		bNeedsFullUpload = false;

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: First spawn - created %d particles"), CurrentParticleCount);

		ActiveSpawnRequests.Empty();
	}
	else if (bNeedFullUpload && CachedGPUParticles.Num() > 0)
	{
		// Full upload from CPU - create new buffer with all particles
		ParticleBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidParticles"),
			sizeof(FGPUFluidParticle),
			CurrentParticleCount,
			CachedGPUParticles.GetData(),
			CurrentParticleCount * sizeof(FGPUFluidParticle),
			ERDGInitialDataFlags::NoCopy
		);

		bNeedsFullUpload = false;
		PreviousParticleCount = CurrentParticleCount;
		NewParticleCount = 0;
		NewParticlesToAppend.Empty();

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: Full upload (%d particles)"), CurrentParticleCount);
	}
	else if (bHasNewParticles)
	{
		// Append new particles while preserving existing GPU simulation results
		// 1. Create new buffer with expanded capacity
		// 2. Copy existing particles from persistent buffer using compute shader
		// 3. Upload only new particles to the end using compute shader

		const int32 ExistingCount = PreviousParticleCount;
		const int32 TotalCount = CurrentParticleCount;

		// Create new buffer for total count
		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), TotalCount);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		// Register existing persistent buffer as source
		FRDGBufferRef ExistingBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleBuffer, TEXT("GPUFluidParticlesOld"));

		// Use compute shader to copy existing particles (more reliable than AddCopyBufferPass)
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FCopyParticlesCS> CopyShader(ShaderMap);

			FCopyParticlesCS::FParameters* CopyParams = GraphBuilder.AllocParameters<FCopyParticlesCS::FParameters>();
			CopyParams->SourceParticles = GraphBuilder.CreateSRV(ExistingBuffer);
			CopyParams->DestParticles = GraphBuilder.CreateUAV(ParticleBuffer);
			CopyParams->SourceOffset = 0;
			CopyParams->DestOffset = 0;
			CopyParams->CopyCount = ExistingCount;

			const uint32 NumGroups = FMath::DivideAndRoundUp(ExistingCount, FCopyParticlesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::CopyExistingParticles(%d)", ExistingCount),
				CopyShader,
				CopyParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		// Upload and copy new particles to the end of the buffer
		if (NewParticleCount > 0)
		{
			// Create upload buffer for new particles only
			FRDGBufferRef NewParticlesUploadBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidNewParticles"),
				sizeof(FGPUFluidParticle),
				NewParticleCount,
				NewParticlesToAppend.GetData(),
				NewParticleCount * sizeof(FGPUFluidParticle),
				ERDGInitialDataFlags::NoCopy
			);

			// Use compute shader to copy new particles to the end
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FCopyParticlesCS> CopyShader(ShaderMap);

			FCopyParticlesCS::FParameters* CopyParams = GraphBuilder.AllocParameters<FCopyParticlesCS::FParameters>();
			CopyParams->SourceParticles = GraphBuilder.CreateSRV(NewParticlesUploadBuffer);
			CopyParams->DestParticles = GraphBuilder.CreateUAV(ParticleBuffer);
			CopyParams->SourceOffset = 0;
			CopyParams->DestOffset = ExistingCount;
			CopyParams->CopyCount = NewParticleCount;

			const uint32 NumGroups = FMath::DivideAndRoundUp(NewParticleCount, FCopyParticlesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::CopyNewParticles(%d)", NewParticleCount),
				CopyShader,
				CopyParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		PreviousParticleCount = CurrentParticleCount;
		NewParticleCount = 0;
		NewParticlesToAppend.Empty();

		UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: Appended %d new particles (existing: %d, total: %d)"),
			TotalCount - ExistingCount, ExistingCount, TotalCount);
	}
	else if (bHasSpawnRequests && PersistentParticleBuffer.IsValid())
	{
		// =====================================================
		// NEW: GPU-based spawning path (eliminates race condition)
		// Expand buffer and use spawn pass to add particles atomically
		// =====================================================
		const int32 ExistingCount = CurrentParticleCount;
		const int32 TotalCount = ExpectedParticleCount;

		// Capacity check
		if (TotalCount > MaxParticleCount)
		{
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT("GPU Spawn: Total count (%d) exceeds capacity (%d), clamping spawn requests"),
				TotalCount, MaxParticleCount);
			// Will be handled by atomic counter in shader
		}

		// Create new buffer with expanded capacity
		const int32 BufferCapacity = FMath::Min(TotalCount, MaxParticleCount);
		FRDGBufferDesc NewBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), BufferCapacity);
		ParticleBuffer = GraphBuilder.CreateBuffer(NewBufferDesc, TEXT("GPUFluidParticles"));

		// Copy existing particles from persistent buffer
		FRDGBufferRef ExistingBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleBuffer, TEXT("GPUFluidParticlesOld"));
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FCopyParticlesCS> CopyShader(ShaderMap);

			FCopyParticlesCS::FParameters* CopyParams = GraphBuilder.AllocParameters<FCopyParticlesCS::FParameters>();
			CopyParams->SourceParticles = GraphBuilder.CreateSRV(ExistingBuffer);
			CopyParams->DestParticles = GraphBuilder.CreateUAV(ParticleBuffer);
			CopyParams->SourceOffset = 0;
			CopyParams->DestOffset = 0;
			CopyParams->CopyCount = ExistingCount;

			const uint32 NumGroups = FMath::DivideAndRoundUp(ExistingCount, FCopyParticlesCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GPUFluid::CopyExistingForSpawn(%d)", ExistingCount),
				CopyShader,
				CopyParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		// Create counter buffer with current particle count
		// The shader will atomically increment this counter
		// IMPORTANT: Do NOT use NoCopy - InitialCounterData is a local variable that goes
		// out of scope before RDG executes. RDG must copy the data.
		TArray<uint32> InitialCounterData;
		InitialCounterData.Add(static_cast<uint32>(ExistingCount));

		FRDGBufferRef CounterBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidParticleCounter"),
			sizeof(uint32),
			1,
			InitialCounterData.GetData(),
			sizeof(uint32),
			ERDGInitialDataFlags::None
		);
		FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(CounterBuffer);

		// Run spawn particles pass
		FRDGBufferUAVRef ParticleUAVForSpawn = GraphBuilder.CreateUAV(ParticleBuffer);
		AddSpawnParticlesPass(GraphBuilder, ParticleUAVForSpawn, CounterUAV, ActiveSpawnRequests);

		// Update particle count (after spawning, assuming all spawn requests succeed within capacity)
		CurrentParticleCount = FMath::Min(ExpectedParticleCount, MaxParticleCount);
		PreviousParticleCount = CurrentParticleCount;

		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("GPU Buffer: GPU Spawn path - spawned %d particles (existing: %d, total: %d)"),SpawnCount, ExistingCount, CurrentParticleCount);

		// Clear spawn requests
		ActiveSpawnRequests.Empty();
	}
	else
	{
		// Reuse persistent buffer from previous frame (no CPU upload!)
		// This is the key path for proper GPU simulation - gravity should work here!
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> REUSE PATH: Attempting RegisterExternalBuffer"));

		if (!PersistentParticleBuffer.IsValid())
		{
			UE_LOG(LogGPUFluidSimulator, Error, TEXT(">>> REUSE PATH FAILED: PersistentParticleBuffer is NULL/Invalid!"));
			return;
		}

		ParticleBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleBuffer, TEXT("GPUFluidParticles"));
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> REUSE PATH: RegisterExternalBuffer succeeded, ParticleCount=%d"), CurrentParticleCount);
	}

	// Clear active spawn requests if any remaining (shouldn't happen, but safety)
	ActiveSpawnRequests.Empty();

	// Create transient position buffer for spatial hash
	FRDGBufferDesc PositionBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), CurrentParticleCount);
	FRDGBufferRef PositionBuffer = GraphBuilder.CreateBuffer(PositionBufferDesc, TEXT("GPUFluidPositions"));

	FRDGBufferUAVRef ParticlesUAVLocal = GraphBuilder.CreateUAV(ParticleBuffer);
	FRDGBufferSRVRef ParticlesSRVLocal = GraphBuilder.CreateSRV(ParticleBuffer);
	FRDGBufferUAVRef PositionsUAVLocal = GraphBuilder.CreateUAV(PositionBuffer);
	FRDGBufferSRVRef PositionsSRVLocal = GraphBuilder.CreateSRV(PositionBuffer);

	// Debug: Log simulation parameters
	static int32 SimDebugCounter = 0;
	if (++SimDebugCounter % 60 == 0)
	{
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("=== SIMULATION DEBUG ==="));
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("  ParticleCount: %d"), CurrentParticleCount);
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("  Gravity: (%.2f, %.2f, %.2f)"), Params.Gravity.X, Params.Gravity.Y, Params.Gravity.Z);
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("  DeltaTime: %.4f"), Params.DeltaTime);
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("  PersistentBuffer Valid: %s"), PersistentParticleBuffer.IsValid() ? TEXT("YES") : TEXT("NO"));
	}

	// Pass 0.5: Update attached particles (move with bones) - before physics simulation
	// Only run if attachment buffer exists AND matches current particle count
	FRDGBufferRef AttachmentBufferForUpdate = nullptr;
	if (IsAdhesionEnabled() && bBoneTransformsValid && PersistentAttachmentBuffer.IsValid() && AttachmentBufferSize >= CurrentParticleCount)
	{
		AttachmentBufferForUpdate = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsUpdate"));
		FRDGBufferUAVRef AttachmentUAVForUpdate = GraphBuilder.CreateUAV(AttachmentBufferForUpdate);
		AddUpdateAttachedPositionsPassInternal(GraphBuilder, ParticlesUAVLocal, AttachmentUAVForUpdate, Params);
	}

	// Pass 1: Predict Positions
	AddPredictPositionsPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 2: Extract positions for spatial hash (use predicted positions)
	AddExtractPositionsPass(GraphBuilder, ParticlesSRVLocal, PositionsUAVLocal, CurrentParticleCount, true);

	//=========================================================================
	// Pass 3: Spatial Data Structure
	// Option A: Z-Order Sorting (cache-coherent memory access)
	// Option B: Hash Table (linked-list based)
	//=========================================================================

	// CellStart/End for Z-Order sorting (used when bUseZOrderSorting is true)
	FRDGBufferUAVRef CellStartUAVLocal = nullptr;
	FRDGBufferSRVRef CellStartSRVLocal = nullptr;
	FRDGBufferUAVRef CellEndUAVLocal = nullptr;
	FRDGBufferSRVRef CellEndSRVLocal = nullptr;

	// CellCounts/ParticleIndices for hash table (used by physics shaders)
	FRDGBufferRef CellCountsBuffer = nullptr;
	FRDGBufferRef ParticleIndicesBuffer = nullptr;
	FRDGBufferSRVRef CellCountsSRVLocal = nullptr;
	FRDGBufferSRVRef ParticleIndicesSRVLocal = nullptr;

	if (bUseZOrderSorting)
	{
		//=====================================================================
		// Z-Order Sorting Pipeline
		// Morton Code → Radix Sort → Reorder → CellStart/End
		//=====================================================================
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> Z-ORDER SORTING: Building with ParticleCount=%d"), CurrentParticleCount);

		// Execute Z-Order sorting and get sorted particle buffer
		FRDGBufferRef SortedParticleBuffer = ExecuteZOrderSortingPipeline(
			GraphBuilder,
			ParticleBuffer,
			CellStartUAVLocal,
			CellStartSRVLocal,
			CellEndUAVLocal,
			CellEndSRVLocal,
			Params);

		// Replace particle buffer with sorted version for subsequent passes
		ParticleBuffer = SortedParticleBuffer;
		ParticlesUAVLocal = GraphBuilder.CreateUAV(ParticleBuffer);
		ParticlesSRVLocal = GraphBuilder.CreateSRV(ParticleBuffer);

		// Extract positions from sorted particles for hash table build
		AddExtractPositionsPass(GraphBuilder, ParticlesSRVLocal, PositionsUAVLocal, CurrentParticleCount, true);

		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> Z-ORDER SORTING: Completed, particles reordered"));

		//=====================================================================
		// Also build hash table for compatibility with existing shaders
		// Sorted particles → better cache locality during neighbor iteration
		//=====================================================================
		if (!PersistentCellCountsBuffer.IsValid())
		{
			FRDGBufferDesc CellCountsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_SPATIAL_HASH_SIZE);
			CellCountsBuffer = GraphBuilder.CreateBuffer(CellCountsDesc, TEXT("SpatialHash.CellCounts"));

			const uint32 TotalSlots = GPU_SPATIAL_HASH_SIZE * GPU_MAX_PARTICLES_PER_CELL;
			FRDGBufferDesc ParticleIndicesDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalSlots);
			ParticleIndicesBuffer = GraphBuilder.CreateBuffer(ParticleIndicesDesc, TEXT("SpatialHash.ParticleIndices"));
		}
		else
		{
			CellCountsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentCellCountsBuffer, TEXT("SpatialHash.CellCounts"));
			ParticleIndicesBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleIndicesBuffer, TEXT("SpatialHash.ParticleIndices"));
		}

		CellCountsSRVLocal = GraphBuilder.CreateSRV(CellCountsBuffer);
		FRDGBufferUAVRef CellCountsUAVLocal = GraphBuilder.CreateUAV(CellCountsBuffer);
		ParticleIndicesSRVLocal = GraphBuilder.CreateSRV(ParticleIndicesBuffer);
		FRDGBufferUAVRef ParticleIndicesUAVLocal = GraphBuilder.CreateUAV(ParticleIndicesBuffer);

		// GPU Clear pass
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FClearCellDataCS> ClearShader(ShaderMap);

			FClearCellDataCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellDataCS::FParameters>();
			ClearParams->CellCounts = CellCountsUAVLocal;

			const uint32 NumGroups = FMath::DivideAndRoundUp<uint32>(GPU_SPATIAL_HASH_SIZE, SPATIAL_HASH_THREAD_GROUP_SIZE);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SpatialHash::GPUClear(ZOrder)"),
				ClearShader,
				ClearParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		// GPU Build pass with sorted positions
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FBuildSpatialHashSimpleCS> BuildShader(ShaderMap);

			FBuildSpatialHashSimpleCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FBuildSpatialHashSimpleCS::FParameters>();
			BuildParams->ParticlePositions = PositionsSRVLocal;
			BuildParams->ParticleCount = CurrentParticleCount;
			BuildParams->ParticleRadius = Params.ParticleRadius;
			BuildParams->CellSize = Params.CellSize;
			BuildParams->CellCounts = CellCountsUAVLocal;
			BuildParams->ParticleIndices = ParticleIndicesUAVLocal;

			const uint32 NumGroups = FMath::DivideAndRoundUp<uint32>(CurrentParticleCount, SPATIAL_HASH_THREAD_GROUP_SIZE);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SpatialHash::GPUBuild(ZOrder)"),
				BuildShader,
				BuildParams,
				FIntVector(NumGroups, 1, 1)
			);
		}
	}
	else
	{
		//=====================================================================
		// Traditional Hash Table (fallback)
		//=====================================================================
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Building with ParticleCount=%d, Radius=%.2f, CellSize=%.2f"),
			CurrentParticleCount, Params.ParticleRadius, Params.CellSize);

		if (!PersistentCellCountsBuffer.IsValid())
		{
			// First frame: create buffers
			FRDGBufferDesc CellCountsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_SPATIAL_HASH_SIZE);
			CellCountsBuffer = GraphBuilder.CreateBuffer(CellCountsDesc, TEXT("SpatialHash.CellCounts"));

			const uint32 TotalSlots = GPU_SPATIAL_HASH_SIZE * GPU_MAX_PARTICLES_PER_CELL;
			FRDGBufferDesc ParticleIndicesDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalSlots);
			ParticleIndicesBuffer = GraphBuilder.CreateBuffer(ParticleIndicesDesc, TEXT("SpatialHash.ParticleIndices"));

			if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Created new persistent buffers"));
		}
		else
		{
			// Subsequent frames: reuse persistent buffers
			CellCountsBuffer = GraphBuilder.RegisterExternalBuffer(PersistentCellCountsBuffer, TEXT("SpatialHash.CellCounts"));
			ParticleIndicesBuffer = GraphBuilder.RegisterExternalBuffer(PersistentParticleIndicesBuffer, TEXT("SpatialHash.ParticleIndices"));

			if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: Reusing persistent buffers"));
		}

		CellCountsSRVLocal = GraphBuilder.CreateSRV(CellCountsBuffer);
		FRDGBufferUAVRef CellCountsUAVLocal = GraphBuilder.CreateUAV(CellCountsBuffer);
		ParticleIndicesSRVLocal = GraphBuilder.CreateSRV(ParticleIndicesBuffer);
		FRDGBufferUAVRef ParticleIndicesUAVLocal = GraphBuilder.CreateUAV(ParticleIndicesBuffer);

		// GPU Clear pass - clears CellCounts to 0 entirely on GPU
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FClearCellDataCS> ClearShader(ShaderMap);

			FClearCellDataCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellDataCS::FParameters>();
			ClearParams->CellCounts = CellCountsUAVLocal;

			const uint32 NumGroups = FMath::DivideAndRoundUp<uint32>(GPU_SPATIAL_HASH_SIZE, SPATIAL_HASH_THREAD_GROUP_SIZE);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SpatialHash::GPUClear"),
				ClearShader,
				ClearParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		// GPU Build pass - writes particle indices into hash grid
		{
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FBuildSpatialHashSimpleCS> BuildShader(ShaderMap);

			FBuildSpatialHashSimpleCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FBuildSpatialHashSimpleCS::FParameters>();
			BuildParams->ParticlePositions = PositionsSRVLocal;
			BuildParams->ParticleCount = CurrentParticleCount;
			BuildParams->ParticleRadius = Params.ParticleRadius;
			BuildParams->CellSize = Params.CellSize;
			BuildParams->CellCounts = CellCountsUAVLocal;
			BuildParams->ParticleIndices = ParticleIndicesUAVLocal;

			const uint32 NumGroups = FMath::DivideAndRoundUp<uint32>(CurrentParticleCount, SPATIAL_HASH_THREAD_GROUP_SIZE);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SpatialHash::GPUBuild"),
				BuildShader,
				BuildParams,
				FIntVector(NumGroups, 1, 1)
			);
		}

		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SPATIAL HASH: GPU clear+build completed"));

		// Create dummy CellStart/CellEnd buffers for shader parameter validation
		// These are not used when bUseZOrderSorting = 0, but RDG requires valid SRVs
		// Must use QueueBufferUpload to mark buffer as "produced" for RDG validation
		{
			FRDGBufferDesc DummyCellDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
			FRDGBufferRef DummyCellStartBuffer = GraphBuilder.CreateBuffer(DummyCellDesc, TEXT("DummyCellStart"));
			FRDGBufferRef DummyCellEndBuffer = GraphBuilder.CreateBuffer(DummyCellDesc, TEXT("DummyCellEnd"));
			uint32 InvalidIndex = 0xFFFFFFFF;
			GraphBuilder.QueueBufferUpload(DummyCellStartBuffer, &InvalidIndex, sizeof(uint32));
			GraphBuilder.QueueBufferUpload(DummyCellEndBuffer, &InvalidIndex, sizeof(uint32));
			CellStartSRVLocal = GraphBuilder.CreateSRV(DummyCellStartBuffer);
			CellEndSRVLocal = GraphBuilder.CreateSRV(DummyCellEndBuffer);
		}
	}

	// Pass 3.5: GPU Boundary Skinning (if using GPU skinning system)
	// Transforms bone-local boundary particles to world space on GPU
	// Must run before density/pressure solver so boundary particles are in correct positions
	if (IsGPUBoundarySkinningEnabled())
	{
		AddBoundarySkinningPass(GraphBuilder, Params);
		if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> BOUNDARY SKINNING: GPU skinning pass completed"));
	}

	// Pass 4-5: XPBD Density Constraint Solver (OPTIMIZED: Combined Density + Pressure + Neighbor Caching)
	// Neighbor Caching: First iteration builds neighbor list, subsequent iterations reuse
	// Each iteration:
	//   - Single neighbor traversal computes both Density/Lambda AND Position corrections
	//   - Uses previous iteration's Lambda for Jacobi-style update (parallel-safe)
	// Performance: Hash lookup only in first iteration, cached for subsequent iterations

	// Create/resize neighbor caching buffers if needed
	FRDGBufferRef NeighborListRDGBuffer = nullptr;
	FRDGBufferRef NeighborCountsRDGBuffer = nullptr;
	FRDGBufferUAVRef NeighborListUAVLocal = nullptr;
	FRDGBufferUAVRef NeighborCountsUAVLocal = nullptr;
	FRDGBufferSRVRef NeighborListSRVLocal = nullptr;
	FRDGBufferSRVRef NeighborCountsSRVLocal = nullptr;

	const int32 NeighborListSize = CurrentParticleCount * GPU_MAX_NEIGHBORS_PER_PARTICLE;

	if (NeighborBufferParticleCapacity < CurrentParticleCount || !NeighborListBuffer.IsValid())
	{
		// Create new buffers
		NeighborListRDGBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NeighborListSize),
			TEXT("GPUFluidNeighborList"));
		NeighborCountsRDGBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CurrentParticleCount),
			TEXT("GPUFluidNeighborCounts"));

		NeighborBufferParticleCapacity = CurrentParticleCount;
	}
	else
	{
		// Reuse existing buffers
		NeighborListRDGBuffer = GraphBuilder.RegisterExternalBuffer(NeighborListBuffer, TEXT("GPUFluidNeighborList"));
		NeighborCountsRDGBuffer = GraphBuilder.RegisterExternalBuffer(NeighborCountsBuffer, TEXT("GPUFluidNeighborCounts"));
	}

	NeighborListUAVLocal = GraphBuilder.CreateUAV(NeighborListRDGBuffer);
	NeighborCountsUAVLocal = GraphBuilder.CreateUAV(NeighborCountsRDGBuffer);

	for (int32 i = 0; i < Params.SolverIterations; ++i)
	{
		// Combined pass: Compute Density + Lambda + Apply Position Corrections
		// First iteration (i=0) builds neighbor cache, subsequent iterations reuse it
		// When bUseZOrderSorting is true, CellStartSRVLocal/CellEndSRVLocal are valid and shader uses sequential access
		AddSolveDensityPressurePass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal,
			CellStartSRVLocal, CellEndSRVLocal, NeighborListUAVLocal, NeighborCountsUAVLocal, i, Params);
	}

	// Create SRVs from neighbor cache buffers for use in Viscosity and Cohesion passes
	NeighborListSRVLocal = GraphBuilder.CreateSRV(NeighborListRDGBuffer);
	NeighborCountsSRVLocal = GraphBuilder.CreateSRV(NeighborCountsRDGBuffer);

	// Extract neighbor buffers for next frame (persist across substeps)
	GraphBuilder.QueueBufferExtraction(NeighborListRDGBuffer, &NeighborListBuffer, ERHIAccess::UAVCompute);
	GraphBuilder.QueueBufferExtraction(NeighborCountsRDGBuffer, &NeighborCountsBuffer, ERHIAccess::UAVCompute);

	// Pass 6: Bounds Collision
	AddBoundsCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 6.5: Distance Field Collision (if enabled)
	AddDistanceFieldCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 6.6: Primitive Collision (spheres, capsules, boxes, convexes from FluidCollider)
	AddPrimitiveCollisionPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 6.7: Adhesion - Create attachments to bone colliders (GPU-based)
	if (IsAdhesionEnabled() && bBoneTransformsValid)
	{
		// Check if we need to create or resize attachment buffer
		const bool bNeedNewBuffer = !PersistentAttachmentBuffer.IsValid() || AttachmentBufferSize < CurrentParticleCount;

		FRDGBufferRef AttachmentBuffer;
		if (bNeedNewBuffer)
		{
			// Create new buffer with initialized data (PrimitiveType = -1 means no attachment)
			TArray<FGPUParticleAttachment> InitialAttachments;
			InitialAttachments.SetNum(CurrentParticleCount);
			for (int32 i = 0; i < CurrentParticleCount; ++i)
			{
				InitialAttachments[i].PrimitiveType = -1;
				InitialAttachments[i].PrimitiveIndex = -1;
				InitialAttachments[i].BoneIndex = -1;
				InitialAttachments[i].AdhesionStrength = 0.0f;
				InitialAttachments[i].LocalOffset = FVector3f::ZeroVector;
				InitialAttachments[i].AttachmentTime = 0.0f;
			}

			AttachmentBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidAttachments"),
				sizeof(FGPUParticleAttachment),
				CurrentParticleCount,
				InitialAttachments.GetData(),
				CurrentParticleCount * sizeof(FGPUParticleAttachment),
				ERDGInitialDataFlags::None  // Copy the data, don't use NoCopy
			);

			// If we had existing data, copy it to the new buffer
			if (PersistentAttachmentBuffer.IsValid() && AttachmentBufferSize > 0)
			{
				FRDGBufferRef OldBuffer = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsOld"));

				// Copy existing attachment data (preserve attached particles)
				AddCopyBufferPass(GraphBuilder, AttachmentBuffer, 0, OldBuffer, 0, AttachmentBufferSize * sizeof(FGPUParticleAttachment));
			}

			// Update tracked size
			AttachmentBufferSize = CurrentParticleCount;
		}
		else
		{
			AttachmentBuffer = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachments"));
		}
		FRDGBufferUAVRef AttachmentUAV = GraphBuilder.CreateUAV(AttachmentBuffer);

		AddAdhesionPass(GraphBuilder, ParticlesUAVLocal, AttachmentUAV, Params);

		// Extract attachment buffer for next frame
		GraphBuilder.QueueBufferExtraction(
			AttachmentBuffer,
			&PersistentAttachmentBuffer,
			ERHIAccess::UAVCompute
		);
	}

	// Pass 7: Finalize Positions (update Position from PredictedPosition, recalculate Velocity: v = (x* - x) / dt)
	AddFinalizePositionsPass(GraphBuilder, ParticlesUAVLocal, Params);

	// Pass 8: Apply Viscosity (XSPH velocity smoothing) - Applied AFTER velocity is finalized per PBF paper
	// Uses cached neighbor list from DensityPressure pass for faster lookup
	AddApplyViscosityPass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal,
		NeighborListSRVLocal, NeighborCountsSRVLocal, Params);

	// Pass 8.5: Apply Cohesion (surface tension between particles)
	// Uses cached neighbor list from DensityPressure pass for faster lookup
	AddApplyCohesionPass(GraphBuilder, ParticlesUAVLocal, CellCountsSRVLocal, ParticleIndicesSRVLocal,
		NeighborListSRVLocal, NeighborCountsSRVLocal, Params);

	// Pass 8.6: Apply Stack Pressure (weight transfer from stacked attached particles)
	if (Params.StackPressureScale > 0.0f && IsAdhesionEnabled() && PersistentAttachmentBuffer.IsValid())
	{
		FRDGBufferRef AttachmentBufferForStackPressure = GraphBuilder.RegisterExternalBuffer(PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsStackPressure"));
		FRDGBufferSRVRef AttachmentSRVForStackPressure = GraphBuilder.CreateSRV(AttachmentBufferForStackPressure);
		AddStackPressurePass(GraphBuilder, ParticlesUAVLocal, AttachmentSRVForStackPressure, CellCountsSRVLocal, ParticleIndicesSRVLocal, Params);
	}

	// Pass 9: Clear just-detached flag at end of frame
	AddClearDetachedFlagPass(GraphBuilder, ParticlesUAVLocal);

	// Pass 9.5: Boundary Adhesion (Flex-style adhesion from mesh surface particles)
	if (IsBoundaryAdhesionEnabled())
	{
		AddBoundaryAdhesionPass(GraphBuilder, ParticlesUAVLocal, Params);
	}

	// Pass 10: Anisotropy calculation (for ellipsoid rendering)
	// Runs after simulation is complete, uses spatial hash for neighbor lookup
	if (CachedAnisotropyParams.bEnabled && CurrentParticleCount > 0)
	{
		// Update interval optimization: skip calculation on some frames
		const int32 UpdateInterval = FMath::Max(1, CachedAnisotropyParams.UpdateInterval);
		++AnisotropyFrameCounter;
		const bool bShouldUpdateAnisotropy = (AnisotropyFrameCounter >= UpdateInterval)
			|| !PersistentAnisotropyAxis1Buffer.IsValid();  // Always update if no buffer exists

		if (bShouldUpdateAnisotropy)
		{
			AnisotropyFrameCounter = 0;  // Reset counter
		}
		else
		{
			// Skip this frame - reuse existing anisotropy buffers
			// No extraction needed, buffers persist from previous frame
			goto AnisotropyPassEnd;
		}

		// Create anisotropy output buffers
		FRDGBufferRef Axis1Buffer = nullptr;
		FRDGBufferRef Axis2Buffer = nullptr;
		FRDGBufferRef Axis3Buffer = nullptr;
		FFluidAnisotropyPassBuilder::CreateAnisotropyBuffers(
			GraphBuilder, CurrentParticleCount, Axis1Buffer, Axis2Buffer, Axis3Buffer);

		if (Axis1Buffer && Axis2Buffer && Axis3Buffer && CellCountsBuffer && ParticleIndicesBuffer)
		{
			// Prepare anisotropy compute parameters
			FAnisotropyComputeParams AnisotropyParams;
			AnisotropyParams.PhysicsParticlesSRV = GraphBuilder.CreateSRV(ParticleBuffer);

			// Pass Attachment buffer for attached particle anisotropy
			// Always create a valid SRV (dummy if adhesion disabled) to satisfy shader requirements
			if (IsAdhesionEnabled() && PersistentAttachmentBuffer.IsValid())
			{
				FRDGBufferRef AttachmentBufferForAnisotropy = GraphBuilder.RegisterExternalBuffer(
					PersistentAttachmentBuffer, TEXT("GPUFluidAttachmentsAnisotropy"));
				AnisotropyParams.AttachmentsSRV = GraphBuilder.CreateSRV(AttachmentBufferForAnisotropy);
			}
			else
			{
				// Create dummy attachment buffer (shader requires valid SRV)
				// Must upload data so RDG marks buffer as "produced" (bQueuedForUpload = true)
				FRDGBufferRef DummyAttachmentBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUParticleAttachment), 1),
					TEXT("DummyAttachmentBuffer"));
				FGPUParticleAttachment ZeroData = {};
				GraphBuilder.QueueBufferUpload(DummyAttachmentBuffer, &ZeroData, sizeof(FGPUParticleAttachment));
				AnisotropyParams.AttachmentsSRV = GraphBuilder.CreateSRV(DummyAttachmentBuffer);
			}

			// Create fresh SRVs for Anisotropy pass to avoid RDG resource state conflicts
			// Previous SRVs may have been used in passes that also had UAV writes to same buffers
			AnisotropyParams.CellCountsSRV = GraphBuilder.CreateSRV(CellCountsBuffer);
			AnisotropyParams.ParticleIndicesSRV = GraphBuilder.CreateSRV(ParticleIndicesBuffer);
			AnisotropyParams.OutAxis1UAV = GraphBuilder.CreateUAV(Axis1Buffer);
			AnisotropyParams.OutAxis2UAV = GraphBuilder.CreateUAV(Axis2Buffer);
			AnisotropyParams.OutAxis3UAV = GraphBuilder.CreateUAV(Axis3Buffer);
			AnisotropyParams.ParticleCount = CurrentParticleCount;

			// Map anisotropy mode
			switch (CachedAnisotropyParams.Mode)
			{
			case EFluidAnisotropyMode::VelocityBased:
				AnisotropyParams.Mode = EGPUAnisotropyMode::VelocityBased;
				break;
			case EFluidAnisotropyMode::DensityBased:
				AnisotropyParams.Mode = EGPUAnisotropyMode::DensityBased;
				break;
			case EFluidAnisotropyMode::Hybrid:
				AnisotropyParams.Mode = EGPUAnisotropyMode::Hybrid;
				break;
			default:
				AnisotropyParams.Mode = EGPUAnisotropyMode::DensityBased;
				break;
			}

			AnisotropyParams.VelocityStretchFactor = CachedAnisotropyParams.VelocityStretchFactor;
			AnisotropyParams.AnisotropyScale = CachedAnisotropyParams.AnisotropyScale;
			AnisotropyParams.AnisotropyMin = CachedAnisotropyParams.AnisotropyMin;
			AnisotropyParams.AnisotropyMax = CachedAnisotropyParams.AnisotropyMax;
			AnisotropyParams.DensityWeight = CachedAnisotropyParams.DensityWeight;

			// Isolated particle handling params
			AnisotropyParams.MinNeighborsForAnisotropy = CachedAnisotropyParams.MinNeighborsForAnisotropy;
			AnisotropyParams.bFadeIsolatedParticles = CachedAnisotropyParams.bFadeIsolatedParticles;
			AnisotropyParams.MinIsolatedScale = CachedAnisotropyParams.MinIsolatedScale;
			AnisotropyParams.bStretchIsolatedByVelocity = CachedAnisotropyParams.bStretchIsolatedByVelocity;
			AnisotropyParams.bFadeSlowIsolated = CachedAnisotropyParams.bFadeSlowIsolated;
			AnisotropyParams.IsolationFadeSpeed = CachedAnisotropyParams.IsolationFadeSpeed;

			// Density-based anisotropy needs wider neighbor search than simulation
			// Use 2.5x smoothing radius to find enough neighbors for reliable covariance
			AnisotropyParams.SmoothingRadius = Params.SmoothingRadius * 2.5f;
			AnisotropyParams.CellSize = Params.CellSize;

			// Morton-sorted spatial lookup (cache-friendly sequential access)
			// When bUseZOrderSorting=true, ParticleBuffer is already sorted by Morton code
			AnisotropyParams.bUseZOrderSorting = bUseZOrderSorting;
			if (bUseZOrderSorting && CellStartSRVLocal && CellEndSRVLocal)
			{
				AnisotropyParams.CellStartSRV = CellStartSRVLocal;
				AnisotropyParams.CellEndSRV = CellEndSRVLocal;
				// IMPORTANT: Must use SimulationBoundsMin (same as simulation passes)
				// Using Params.BoundsMin causes cell ID mismatch and neighbor lookup failure
				AnisotropyParams.MortonBoundsMin = SimulationBoundsMin;
			}

			// Debug log for density-based anisotropy parameters
			static int32 AnisotropyDebugCounter = 0;
			if (++AnisotropyDebugCounter % 60 == 0)
			{
				UE_LOG(LogGPUFluidSimulator, Warning,
					TEXT("Anisotropy Pass: CachedMode=%d -> GPUMode=%d, SmoothingRadius=%.2f, CellSize=%.2f, CellRadius=%d"),
					static_cast<int32>(CachedAnisotropyParams.Mode),
					static_cast<int32>(AnisotropyParams.Mode),
					AnisotropyParams.SmoothingRadius,
					AnisotropyParams.CellSize,
					AnisotropyParams.CellSize > 0.01f ? (int32)FMath::CeilToInt(AnisotropyParams.SmoothingRadius / AnisotropyParams.CellSize) : 0);
			}

			// Add anisotropy compute pass
			FFluidAnisotropyPassBuilder::AddAnisotropyPass(GraphBuilder, AnisotropyParams);

			// Extract anisotropy buffers for rendering
			GraphBuilder.QueueBufferExtraction(
				Axis1Buffer,
				&PersistentAnisotropyAxis1Buffer,
				ERHIAccess::SRVCompute);
			GraphBuilder.QueueBufferExtraction(
				Axis2Buffer,
				&PersistentAnisotropyAxis2Buffer,
				ERHIAccess::SRVCompute);
			GraphBuilder.QueueBufferExtraction(
				Axis3Buffer,
				&PersistentAnisotropyAxis3Buffer,
				ERHIAccess::SRVCompute);

			if (bShouldLog)
			{
				UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> ANISOTROPY: Pass added (mode=%d, particles=%d)"),
					static_cast<int32>(AnisotropyParams.Mode), CurrentParticleCount);
			}
		}
	}
AnisotropyPassEnd:

	// Debug: log that we reached the end of simulation
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> SIMULATION COMPLETE: All passes added for %d particles"), CurrentParticleCount);

	// Phase 2: Extract buffers to persistent storage for next frame reuse
	// This keeps simulation results on GPU without CPU readback
	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> EXTRACTION: Queuing buffer extraction..."));

	// Extract particle buffer
	GraphBuilder.QueueBufferExtraction(
		ParticleBuffer,
		&PersistentParticleBuffer,
		ERHIAccess::UAVCompute  // Ready for next frame's compute passes
	);

	// Extract spatial hash buffers for reuse next frame
	GraphBuilder.QueueBufferExtraction(
		CellCountsBuffer,
		&PersistentCellCountsBuffer,
		ERHIAccess::UAVCompute
	);
	GraphBuilder.QueueBufferExtraction(
		ParticleIndicesBuffer,
		&PersistentParticleIndicesBuffer,
		ERHIAccess::UAVCompute
	);

	if (bShouldLog) UE_LOG(LogGPUFluidSimulator, Log, TEXT(">>> EXTRACTION: Buffer extraction queued successfully"));
}

//=============================================================================
// RDG Pass Implementations
//=============================================================================

void FGPUFluidSimulator::AddPredictPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FPredictPositionsCS> ComputeShader(ShaderMap);

	FPredictPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPredictPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->Gravity = Params.Gravity;
	PassParameters->ExternalForce = ExternalForce;

	// Debug: log gravity and delta time
	static int32 DebugCounter = 0;
	if (++DebugCounter % 60 == 0)
	{
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("PredictPositions: Gravity=(%.2f, %.2f, %.2f), DeltaTime=%.4f, Particles=%d"),Params.Gravity.X, Params.Gravity.Y, Params.Gravity.Z, Params.DeltaTime, CurrentParticleCount);
	}

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FPredictPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PredictPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddExtractPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticlesSRV,
	FRDGBufferUAVRef PositionsUAV,
	int32 ParticleCount,
	bool bUsePredictedPosition)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FExtractPositionsCS> ComputeShader(ShaderMap);

	FExtractPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FExtractPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesSRV;
	PassParameters->Positions = PositionsUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->bUsePredictedPosition = bUsePredictedPosition ? 1 : 0;

	const uint32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FExtractPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ExtractPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddComputeDensityPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FComputeDensityCS> ComputeShader(ShaderMap);

	FComputeDensityCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeDensityCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Compliance = Params.Compliance;
	PassParameters->DeltaTimeSq = Params.DeltaTimeSq;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FComputeDensityCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ComputeDensity"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddSolvePressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSolvePressureCS> ComputeShader(ShaderMap);

	FSolvePressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSolvePressureCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->CellSize = Params.CellSize;
	// Tensile Instability Correction (PBF Eq.13-14)
	PassParameters->bEnableTensileInstability = Params.bEnableTensileInstability;
	PassParameters->TensileK = Params.TensileK;
	PassParameters->TensileN = Params.TensileN;
	PassParameters->InvW_DeltaQ = Params.InvW_DeltaQ;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FSolvePressureCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SolvePressure"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddSolveDensityPressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	FRDGBufferSRVRef InCellStartSRV,
	FRDGBufferSRVRef InCellEndSRV,
	FRDGBufferUAVRef InNeighborListUAV,
	FRDGBufferUAVRef InNeighborCountsUAV,
	int32 IterationIndex,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSolveDensityPressureCS> ComputeShader(ShaderMap);

	FSolveDensityPressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSolveDensityPressureCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	// Hash table mode (legacy)
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	// Z-Order sorted mode (new)
	PassParameters->CellStart = InCellStartSRV;
	PassParameters->CellEnd = InCellEndSRV;
	// Use member variable directly - don't check nullptr because dummy buffers are created for RDG validation
	PassParameters->bUseZOrderSorting = bUseZOrderSorting ? 1 : 0;
	// Morton bounds for Z-Order cell ID calculation (must match FluidMortonCode.usf)
	PassParameters->MortonBoundsMin = SimulationBoundsMin;
	PassParameters->MortonBoundsExtent = SimulationBoundsMax - SimulationBoundsMin;
	// Neighbor caching buffers
	PassParameters->NeighborList = InNeighborListUAV;
	PassParameters->NeighborCounts = InNeighborCountsUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Compliance = Params.Compliance;
	PassParameters->DeltaTimeSq = Params.DeltaTimeSq;
	// Tensile Instability Correction (PBF Eq.13-14)
	PassParameters->bEnableTensileInstability = Params.bEnableTensileInstability;
	PassParameters->TensileK = Params.TensileK;
	PassParameters->TensileN = Params.TensileN;
	PassParameters->InvW_DeltaQ = Params.InvW_DeltaQ;
	// Iteration control for neighbor caching
	PassParameters->IterationIndex = IterationIndex;

	// Boundary Particles for density contribution (Akinci 2012)
	// Priority: GPU skinned buffer > CPU uploaded buffer
	const bool bUseGPUSkinning = IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();
	const bool bUseCPUBoundary = !bUseGPUSkinning && IsBoundaryAdhesionEnabled() && CachedBoundaryParticles.Num() > 0;

	if (bUseGPUSkinning)
	{
		// Use GPU-skinned world boundary buffer (populated by AddBoundarySkinningPass)
		FRDGBufferRef BoundaryBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_Density"));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		PassParameters->bUseBoundaryDensity = 1;
	}
	else if (bUseCPUBoundary)
	{
		// Use CPU-uploaded boundary particles (legacy path)
		const int32 BoundaryCount = CachedBoundaryParticles.Num();
		FRDGBufferRef BoundaryBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles_Density"),
			sizeof(FGPUBoundaryParticle),
			BoundaryCount,
			CachedBoundaryParticles.GetData(),
			BoundaryCount * sizeof(FGPUBoundaryParticle),
			ERDGInitialDataFlags::NoCopy
		);
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = BoundaryCount;
		PassParameters->bUseBoundaryDensity = 1;
	}
	else
	{
		// Create dummy buffer for RDG validation
		// Must use QueueBufferUpload to mark buffer as "produced" for RDG validation
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidBoundaryParticles_Density_Dummy")
		);
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(DummyBuffer);
		PassParameters->BoundaryParticleCount = 0;
		PassParameters->bUseBoundaryDensity = 0;
	}

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FSolveDensityPressureCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SolveDensityPressure (Iter %d)", IterationIndex),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddApplyViscosityPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	FRDGBufferSRVRef InNeighborListSRV,
	FRDGBufferSRVRef InNeighborCountsSRV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyViscosityCS> ComputeShader(ShaderMap);

	FApplyViscosityCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyViscosityCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->NeighborList = InNeighborListSRV;
	PassParameters->NeighborCounts = InNeighborCountsSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->ViscosityCoefficient = Params.ViscosityCoefficient;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->bUseNeighborCache = (InNeighborListSRV != nullptr && InNeighborCountsSRV != nullptr) ? 1 : 0;

	// Boundary Particles for viscosity contribution
	// Priority: GPU skinned buffer > CPU uploaded buffer
	const bool bUseGPUSkinning = IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();
	const bool bUseCPUBoundary = !bUseGPUSkinning && IsBoundaryAdhesionEnabled() && CachedBoundaryParticles.Num() > 0;

	if (bUseGPUSkinning)
	{
		// Use GPU-skinned world boundary buffer (populated by AddBoundarySkinningPass)
		FRDGBufferRef BoundaryBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_Viscosity"));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		PassParameters->bUseBoundaryViscosity = 1;
	}
	else if (bUseCPUBoundary)
	{
		// Use CPU-uploaded boundary particles (legacy path)
		const int32 BoundaryCount = CachedBoundaryParticles.Num();
		FRDGBufferRef BoundaryBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles_Viscosity"),
			sizeof(FGPUBoundaryParticle),
			BoundaryCount,
			CachedBoundaryParticles.GetData(),
			BoundaryCount * sizeof(FGPUBoundaryParticle),
			ERDGInitialDataFlags::NoCopy
		);
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = BoundaryCount;
		PassParameters->bUseBoundaryViscosity = 1;
	}
	else
	{
		// Create dummy buffer for RDG validation
		// Must use QueueBufferUpload to mark buffer as "produced" for RDG validation
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidBoundaryParticles_Viscosity_Dummy")
		);
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(DummyBuffer);
		PassParameters->BoundaryParticleCount = 0;
		PassParameters->bUseBoundaryViscosity = 0;
	}

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FApplyViscosityCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyViscosity"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddApplyCohesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	FRDGBufferSRVRef InNeighborListSRV,
	FRDGBufferSRVRef InNeighborCountsSRV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if cohesion is disabled
	if (Params.CohesionStrength <= 0.0f)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyCohesionCS> ComputeShader(ShaderMap);

	FApplyCohesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyCohesionCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->NeighborList = InNeighborListSRV;
	PassParameters->NeighborCounts = InNeighborCountsSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->CohesionStrength = Params.CohesionStrength;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->bUseNeighborCache = (InNeighborListSRV != nullptr && InNeighborCountsSRV != nullptr) ? 1 : 0;
	// Akinci 2013 surface tension parameters
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	// MaxSurfaceTensionForce: limit force to prevent instability
	// Scale based on particle mass and smoothing radius
	// Typical value: CohesionStrength * RestDensity * h^3 * 1000 (empirical)
	const float h_m = Params.SmoothingRadius * 0.01f;  // cm to m
	PassParameters->MaxSurfaceTensionForce = Params.CohesionStrength * Params.RestDensity * h_m * h_m * h_m * 1000.0f;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FApplyCohesionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyCohesion"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddStackPressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InAttachmentSRV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if stack pressure is disabled or no attachments
	if (Params.StackPressureScale <= 0.0f || !InAttachmentSRV)
	{
		return;
	}

	// Skip if no bone colliders (no attachments possible)
	if (!bBoneTransformsValid || CachedBoneTransforms.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FStackPressureCS> ComputeShader(ShaderMap);

	// Create collision primitive buffers (same as Adhesion pass)
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(4, 1);

	FRDGBufferRef SpheresBuffer = CachedSpheres.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Spheres"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), sizeof(FGPUCollisionSphere) * CachedSpheres.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheres"));

	FRDGBufferRef CapsulesBuffer = CachedCapsules.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Capsules"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), sizeof(FGPUCollisionCapsule) * CachedCapsules.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsules"));

	FRDGBufferRef BoxesBuffer = CachedBoxes.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Boxes"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), sizeof(FGPUCollisionBox) * CachedBoxes.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxes"));

	FRDGBufferRef ConvexesBuffer = CachedConvexHeaders.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Convexes"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), sizeof(FGPUCollisionConvex) * CachedConvexHeaders.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexes"));

	FRDGBufferRef ConvexPlanesBuffer = CachedConvexPlanes.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_ConvexPlanes"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), sizeof(FGPUConvexPlane) * CachedConvexPlanes.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanes"));

	FStackPressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStackPressureCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->Attachments = InAttachmentSRV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;

	// Collision primitives for surface normal calculation
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);

	// Parameters
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->StackPressureScale = Params.StackPressureScale;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Gravity = FVector3f(Params.Gravity);
	PassParameters->DeltaTime = Params.DeltaTime;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FStackPressureCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::StackPressure"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

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

	// Create collider contact count buffer (항상 생성 - 간단한 충돌 감지용)
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

	// Collider contact count parameters (항상 활성화 - 간단한 충돌 감지용)
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

	// Always extract collider contact count buffer (간단한 충돌 감지용)
	GraphBuilder.QueueBufferExtraction(
		ContactCountBuffer,
		&ColliderContactCountBuffer,
		ERHIAccess::UAVCompute
	);
}

void FGPUFluidSimulator::AddFinalizePositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFinalizePositionsCS> ComputeShader(ShaderMap);

	FFinalizePositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFinalizePositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->MaxVelocity = MaxVelocity;  // Safety clamp (50000 cm/s = 500 m/s)
	PassParameters->GlobalDamping = Params.GlobalDamping;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FFinalizePositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::FinalizePositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// GPU Particle Spawn System Implementation
//=============================================================================

void FGPUFluidSimulator::AddSpawnRequest(const FVector3f& Position, const FVector3f& Velocity, float Mass)
{
	FScopeLock Lock(&SpawnRequestLock);

	FGPUSpawnRequest Request;
	Request.Position = Position;
	Request.Velocity = Velocity;
	Request.Mass = Mass;
	Request.Radius = DefaultSpawnRadius;

	PendingSpawnRequests.Add(Request);
	bHasPendingSpawnRequests.store(true);

	UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("AddSpawnRequest: Pos=(%.2f, %.2f, %.2f), Vel=(%.2f, %.2f, %.2f)"),
		Position.X, Position.Y, Position.Z, Velocity.X, Velocity.Y, Velocity.Z);
}

void FGPUFluidSimulator::AddSpawnRequests(const TArray<FGPUSpawnRequest>& Requests)
{
	if (Requests.Num() == 0)
	{
		return;
	}

	FScopeLock Lock(&SpawnRequestLock);

	PendingSpawnRequests.Append(Requests);
	bHasPendingSpawnRequests.store(true);

	//UE_LOG(LogGPUFluidSimulator, Log, TEXT("AddSpawnRequests: Added %d spawn requests (total pending: %d)"),Requests.Num(), PendingSpawnRequests.Num());
}

void FGPUFluidSimulator::ClearSpawnRequests()
{
	FScopeLock Lock(&SpawnRequestLock);
	PendingSpawnRequests.Empty();
	bHasPendingSpawnRequests.store(false);
}

int32 FGPUFluidSimulator::GetPendingSpawnCount() const
{
	FScopeLock Lock(&SpawnRequestLock);
	return PendingSpawnRequests.Num();
}

void FGPUFluidSimulator::AddSpawnParticlesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef ParticleCounterUAV,
	const TArray<FGPUSpawnRequest>& SpawnRequests)
{
	if (SpawnRequests.Num() == 0)
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
		SpawnRequests.Num(),
		SpawnRequests.GetData(),
		SpawnRequests.Num() * sizeof(FGPUSpawnRequest),
		ERDGInitialDataFlags::None
	);

	FSpawnParticlesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSpawnParticlesCS::FParameters>();
	PassParameters->SpawnRequests = GraphBuilder.CreateSRV(SpawnRequestBuffer);
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCounter = ParticleCounterUAV;
	PassParameters->SpawnRequestCount = SpawnRequests.Num();
	PassParameters->MaxParticleCount = MaxParticleCount;
	PassParameters->NextParticleID = NextParticleID.load();
	PassParameters->DefaultRadius = DefaultSpawnRadius;
	PassParameters->DefaultMass = DefaultSpawnMass;

	const uint32 NumGroups = FMath::DivideAndRoundUp(SpawnRequests.Num(), FSpawnParticlesCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SpawnParticles(%d)", SpawnRequests.Num()),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);

	// Update next particle ID (atomic increment)
	NextParticleID.fetch_add(SpawnRequests.Num());

	//UE_LOG(LogGPUFluidSimulator, Log, TEXT("SpawnParticlesPass: Spawned %d particles (NextID: %d)"), SpawnRequests.Num(), NextParticleID.load());
}

//=============================================================================
// FGPUFluidSimulationTask Implementation
//=============================================================================

void FGPUFluidSimulationTask::Execute(
	FGPUFluidSimulator* Simulator,
	const FGPUFluidSimulationParams& Params,
	int32 NumSubsteps)
{
	if (!Simulator || !Simulator->IsReady())
	{
		return;
	}

	FGPUFluidSimulationParams SubstepParams = Params;
	SubstepParams.DeltaTime = Params.DeltaTime / FMath::Max(1, NumSubsteps);
	SubstepParams.DeltaTimeSq = SubstepParams.DeltaTime * SubstepParams.DeltaTime;
	SubstepParams.TotalSubsteps = NumSubsteps;

	for (int32 i = 0; i < NumSubsteps; ++i)
	{
		SubstepParams.SubstepIndex = i;
		Simulator->SimulateSubstep(SubstepParams);
	}
}

//=============================================================================
// Stream Compaction Implementation (Phase 2 - Per-Polygon Collision)
//=============================================================================

void FGPUFluidSimulator::AllocateStreamCompactionBuffers(FRHICommandListImmediate& RHICmdList)
{
	if (bStreamCompactionBuffersAllocated || MaxParticleCount <= 0)
	{
		return;
	}

	const int32 BlockSize = 256;
	const int32 NumBlocks = FMath::DivideAndRoundUp(MaxParticleCount, BlockSize);

	// Marked flags buffer (uint per particle)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_MarkedFlags"), MaxParticleCount * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		MarkedFlagsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		MarkedFlagsSRV = RHICmdList.CreateShaderResourceView(MarkedFlagsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(MarkedFlagsBufferRHI));
		MarkedFlagsUAV = RHICmdList.CreateUnorderedAccessView(MarkedFlagsBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(MarkedFlagsBufferRHI));
	}

	// Marked AABB index buffer (int per particle)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_MarkedAABBIndex"), MaxParticleCount * sizeof(int32), sizeof(int32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		MarkedAABBIndexBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		MarkedAABBIndexSRV = RHICmdList.CreateShaderResourceView(MarkedAABBIndexBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(MarkedAABBIndexBufferRHI));
		MarkedAABBIndexUAV = RHICmdList.CreateUnorderedAccessView(MarkedAABBIndexBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(MarkedAABBIndexBufferRHI));
	}

	// Prefix sums buffer
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_PrefixSums"), MaxParticleCount * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		PrefixSumsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		PrefixSumsSRV = RHICmdList.CreateShaderResourceView(PrefixSumsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(PrefixSumsBufferRHI));
		PrefixSumsUAV = RHICmdList.CreateUnorderedAccessView(PrefixSumsBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(PrefixSumsBufferRHI));
	}

	// Block sums buffer
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_BlockSums"), NumBlocks * sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		BlockSumsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		BlockSumsSRV = RHICmdList.CreateShaderResourceView(BlockSumsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(BlockSumsBufferRHI));
		BlockSumsUAV = RHICmdList.CreateUnorderedAccessView(BlockSumsBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(BlockSumsBufferRHI));
	}

	// Compacted candidates buffer (worst case: all particles)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_CompactedCandidates"), MaxParticleCount * sizeof(FGPUCandidateParticle), sizeof(FGPUCandidateParticle))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		CompactedCandidatesBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		CompactedCandidatesUAV = RHICmdList.CreateUnorderedAccessView(CompactedCandidatesBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(CompactedCandidatesBufferRHI));
	}

	// Total count buffer (single uint)
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_TotalCount"), sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_UnorderedAccess | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVMask);
		TotalCountBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
		TotalCountUAV = RHICmdList.CreateUnorderedAccessView(TotalCountBufferRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(TotalCountBufferRHI));
	}

	// Staging buffers for readback
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_TotalCountStaging"), sizeof(uint32), sizeof(uint32))
			.AddUsage(BUF_None)
			.SetInitialState(ERHIAccess::CopyDest);
		TotalCountStagingBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
	}
	{
		const FRHIBufferCreateDesc BufferDesc =
			FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_CandidatesStaging"), MaxParticleCount * sizeof(FGPUCandidateParticle), sizeof(FGPUCandidateParticle))
			.AddUsage(BUF_None)
			.SetInitialState(ERHIAccess::CopyDest);
		CandidatesStagingBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
	}

	bStreamCompactionBuffersAllocated = true;
	UE_LOG(LogGPUFluidSimulator, Log, TEXT("Stream Compaction buffers allocated (MaxParticles=%d, NumBlocks=%d)"), MaxParticleCount, NumBlocks);
}

void FGPUFluidSimulator::ReleaseStreamCompactionBuffers()
{
	MarkedFlagsBufferRHI.SafeRelease();
	MarkedFlagsSRV.SafeRelease();
	MarkedFlagsUAV.SafeRelease();

	MarkedAABBIndexBufferRHI.SafeRelease();
	MarkedAABBIndexSRV.SafeRelease();
	MarkedAABBIndexUAV.SafeRelease();

	PrefixSumsBufferRHI.SafeRelease();
	PrefixSumsSRV.SafeRelease();
	PrefixSumsUAV.SafeRelease();

	BlockSumsBufferRHI.SafeRelease();
	BlockSumsSRV.SafeRelease();
	BlockSumsUAV.SafeRelease();

	CompactedCandidatesBufferRHI.SafeRelease();
	CompactedCandidatesUAV.SafeRelease();

	TotalCountBufferRHI.SafeRelease();
	TotalCountUAV.SafeRelease();

	FilterAABBsBufferRHI.SafeRelease();
	FilterAABBsSRV.SafeRelease();

	TotalCountStagingBufferRHI.SafeRelease();
	CandidatesStagingBufferRHI.SafeRelease();

	bStreamCompactionBuffersAllocated = false;
	bHasFilteredCandidates = false;
	FilteredCandidateCount = 0;
}

void FGPUFluidSimulator::ExecuteAABBFiltering(const TArray<FGPUFilterAABB>& FilterAABBs)
{
	if (!bIsInitialized || FilterAABBs.Num() == 0 || CurrentParticleCount == 0)
	{
		bHasFilteredCandidates = false;
		FilteredCandidateCount = 0;
		return;
	}

	// Make a copy of the filter AABBs for the render thread
	TArray<FGPUFilterAABB> FilterAABBsCopy = FilterAABBs;
	FGPUFluidSimulator* Self = this;

	ENQUEUE_RENDER_COMMAND(ExecuteAABBFiltering)(
		[Self, FilterAABBsCopy](FRHICommandListImmediate& RHICmdList)
		{
			// Allocate buffers if needed
			if (!Self->bStreamCompactionBuffersAllocated)
			{
				Self->AllocateStreamCompactionBuffers(RHICmdList);
			}

			// Upload filter AABBs
			const int32 NumAABBs = FilterAABBsCopy.Num();
			if (!Self->FilterAABBsBufferRHI.IsValid() || Self->CurrentFilterAABBCount < NumAABBs)
			{
				Self->FilterAABBsBufferRHI.SafeRelease();
				Self->FilterAABBsSRV.SafeRelease();

				const FRHIBufferCreateDesc BufferDesc =
					FRHIBufferCreateDesc::CreateStructured(TEXT("StreamCompaction_FilterAABBs"), NumAABBs * sizeof(FGPUFilterAABB), sizeof(FGPUFilterAABB))
					.AddUsage(BUF_ShaderResource)
					.SetInitialState(ERHIAccess::SRVMask);
				Self->FilterAABBsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);
				Self->FilterAABBsSRV = RHICmdList.CreateShaderResourceView(Self->FilterAABBsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(Self->FilterAABBsBufferRHI));
				Self->CurrentFilterAABBCount = NumAABBs;
			}

			// Upload AABB data
			void* AABBData = RHICmdList.LockBuffer(Self->FilterAABBsBufferRHI, 0,
				NumAABBs * sizeof(FGPUFilterAABB), RLM_WriteOnly);
			FMemory::Memcpy(AABBData, FilterAABBsCopy.GetData(), NumAABBs * sizeof(FGPUFilterAABB));
			RHICmdList.UnlockBuffer(Self->FilterAABBsBufferRHI);

			// Get the correct particle SRV - use PersistentParticleBuffer if available (GPU simulation mode)
			FShaderResourceViewRHIRef ParticleSRVToUse = Self->ParticleSRV;
			if (Self->PersistentParticleBuffer.IsValid())
			{
				FBufferRHIRef PersistentRHI = Self->PersistentParticleBuffer->GetRHI();
				if (PersistentRHI.IsValid())
				{
					ParticleSRVToUse = RHICmdList.CreateShaderResourceView(PersistentRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(PersistentRHI));
					UE_LOG(LogGPUFluidSimulator, Log, TEXT("AABB Filtering: Using PersistentParticleBuffer SRV (GPU simulation mode)"));
				}
			}
			else
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("AABB Filtering: PersistentParticleBuffer not valid, using fallback ParticleSRV"));
			}

			// Execute stream compaction using direct RHI dispatch
			Self->DispatchStreamCompactionShaders(RHICmdList, Self->CurrentParticleCount, NumAABBs, ParticleSRVToUse);
		}
	);
}

void FGPUFluidSimulator::DispatchStreamCompactionShaders(FRHICommandListImmediate& RHICmdList, int32 ParticleCount, int32 NumAABBs, FShaderResourceViewRHIRef InParticleSRV)
{
	const int32 BlockSize = 256;
	const int32 NumBlocks = FMath::DivideAndRoundUp(ParticleCount, BlockSize);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Pass 1: AABB Mark - Mark particles that are inside any AABB
	{
		TShaderMapRef<FAABBMarkCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FAABBMarkCS::FParameters Parameters;
		Parameters.Particles = InParticleSRV;  // Use the passed SRV (from PersistentParticleBuffer)
		Parameters.FilterAABBs = FilterAABBsSRV;
		Parameters.MarkedFlags = MarkedFlagsUAV;
		Parameters.MarkedAABBIndex = MarkedAABBIndexUAV;
		Parameters.ParticleCount = ParticleCount;
		Parameters.NumAABBs = NumAABBs;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FAABBMarkCS::ThreadGroupSize);
		RHICmdList.DispatchComputeShader(NumGroups, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier between passes
	RHICmdList.Transition(FRHITransitionInfo(MarkedFlagsBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 2a: Prefix Sum Block - Blelloch scan within each block
	{
		TShaderMapRef<FPrefixSumBlockCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FPrefixSumBlockCS::FParameters Parameters;
		Parameters.MarkedFlags = MarkedFlagsSRV;
		Parameters.PrefixSums = PrefixSumsUAV;
		Parameters.BlockSums = BlockSumsUAV;
		Parameters.ElementCount = ParticleCount;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		RHICmdList.DispatchComputeShader(NumBlocks, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(BlockSumsBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 2b: Scan Block Sums - Sequential scan of block sums
	{
		TShaderMapRef<FScanBlockSumsCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FScanBlockSumsCS::FParameters Parameters;
		Parameters.BlockSums = BlockSumsUAV;
		Parameters.BlockCount = NumBlocks;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		RHICmdList.DispatchComputeShader(1, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(BlockSumsBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 2c: Add Block Offsets - Add scanned block sums to each element
	{
		TShaderMapRef<FAddBlockOffsetsCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FAddBlockOffsetsCS::FParameters Parameters;
		Parameters.PrefixSums = PrefixSumsUAV;
		Parameters.BlockSums = BlockSumsUAV;
		Parameters.ElementCount = ParticleCount;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		RHICmdList.DispatchComputeShader(NumBlocks, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(PrefixSumsBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 3: Compact - Write marked particles to compacted output
	{
		TShaderMapRef<FCompactCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FCompactCS::FParameters Parameters;
		Parameters.Particles = InParticleSRV;  // Use same buffer as AABB Mark pass!
		Parameters.MarkedFlags = MarkedFlagsSRV;
		Parameters.PrefixSums = PrefixSumsSRV;
		Parameters.MarkedAABBIndex = MarkedAABBIndexSRV;
		Parameters.CompactedParticles = CompactedCandidatesUAV;
		Parameters.ParticleCount = ParticleCount;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		const int32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FCompactCS::ThreadGroupSize);
		RHICmdList.DispatchComputeShader(NumGroups, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(CompactedCandidatesBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Pass 4: Write Total Count
	{
		TShaderMapRef<FWriteTotalCountCS> ComputeShader(ShaderMap);
		FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
		SetComputePipelineState(RHICmdList, ShaderRHI);

		FWriteTotalCountCS::FParameters Parameters;
		Parameters.MarkedFlagsForCount = MarkedFlagsSRV;
		Parameters.PrefixSumsForCount = PrefixSumsSRV;
		Parameters.TotalCount = TotalCountUAV;
		Parameters.ParticleCount = ParticleCount;
		SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

		RHICmdList.DispatchComputeShader(1, 1, 1);

		UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);
	}

	// Readback total count
	RHICmdList.Transition(FRHITransitionInfo(TotalCountBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
	RHICmdList.CopyBufferRegion(TotalCountStagingBufferRHI, 0, TotalCountBufferRHI, 0, sizeof(uint32));

	uint32* CountPtr = (uint32*)RHICmdList.LockBuffer(TotalCountStagingBufferRHI, 0, sizeof(uint32), RLM_ReadOnly);
	FilteredCandidateCount = static_cast<int32>(*CountPtr);
	RHICmdList.UnlockBuffer(TotalCountStagingBufferRHI);

	bHasFilteredCandidates = (FilteredCandidateCount > 0);

	UE_LOG(LogGPUFluidSimulator, Log, TEXT("AABB Filtering complete: %d/%d particles matched %d AABBs"),
		FilteredCandidateCount, ParticleCount, NumAABBs);
}

bool FGPUFluidSimulator::GetFilteredCandidates(TArray<FGPUCandidateParticle>& OutCandidates)
{
	if (!bHasFilteredCandidates || FilteredCandidateCount == 0 || !CompactedCandidatesBufferRHI.IsValid())
	{
		OutCandidates.Empty();
		return false;
	}

	FGPUFluidSimulator* Self = this;
	TArray<FGPUCandidateParticle>* OutPtr = &OutCandidates;
	const int32 Count = FilteredCandidateCount;

	// Synchronous readback (blocks until GPU is ready)
	ENQUEUE_RENDER_COMMAND(GetFilteredCandidates)(
		[Self, OutPtr, Count](FRHICommandListImmediate& RHICmdList)
		{
			if (!Self->CompactedCandidatesBufferRHI.IsValid())
			{
				return;
			}

			const uint32 CopySize = Count * sizeof(FGPUCandidateParticle);

			// Transition buffer for copy
			RHICmdList.Transition(FRHITransitionInfo(Self->CompactedCandidatesBufferRHI, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));

			// Copy to staging buffer
			RHICmdList.CopyBufferRegion(Self->CandidatesStagingBufferRHI, 0, Self->CompactedCandidatesBufferRHI, 0, CopySize);

			// Read back
			OutPtr->SetNumUninitialized(Count);
			FGPUCandidateParticle* DataPtr = (FGPUCandidateParticle*)RHICmdList.LockBuffer(
				Self->CandidatesStagingBufferRHI, 0, CopySize, RLM_ReadOnly);
			FMemory::Memcpy(OutPtr->GetData(), DataPtr, CopySize);
			RHICmdList.UnlockBuffer(Self->CandidatesStagingBufferRHI);
		}
	);

	// Wait for render command to complete
	FlushRenderingCommands();

	return OutCandidates.Num() > 0;
}

//=============================================================================
// Per-Polygon Collision Correction Implementation
//=============================================================================

void FGPUFluidSimulator::ApplyCorrections(const TArray<FParticleCorrection>& Corrections)
{
	if (!bIsInitialized || Corrections.Num() == 0 || !PersistentParticleBuffer.IsValid())
	{
		return;
	}

	// Make a copy of corrections for the render thread
	TArray<FParticleCorrection> CorrectionsCopy = Corrections;
	FGPUFluidSimulator* Self = this;
	const int32 CorrectionCount = Corrections.Num();

	ENQUEUE_RENDER_COMMAND(ApplyPerPolygonCorrections)(
		[Self, CorrectionsCopy, CorrectionCount](FRHICommandListImmediate& RHICmdList)
		{
			if (!Self->PersistentParticleBuffer.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyCorrections: PersistentParticleBuffer not valid"));
				return;
			}

			// Create corrections buffer
			const FRHIBufferCreateDesc BufferDesc =
				FRHIBufferCreateDesc::CreateStructured(TEXT("PerPolygonCorrections"), CorrectionCount * sizeof(FParticleCorrection), sizeof(FParticleCorrection))
				.AddUsage(BUF_ShaderResource)
				.SetInitialState(ERHIAccess::SRVMask);
			FBufferRHIRef CorrectionsBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

			// Upload corrections data
			void* CorrectionData = RHICmdList.LockBuffer(CorrectionsBufferRHI, 0,
				CorrectionCount * sizeof(FParticleCorrection), RLM_WriteOnly);
			FMemory::Memcpy(CorrectionData, CorrectionsCopy.GetData(), CorrectionCount * sizeof(FParticleCorrection));
			RHICmdList.UnlockBuffer(CorrectionsBufferRHI);

			// Create SRV for corrections
			FShaderResourceViewRHIRef CorrectionsSRV = RHICmdList.CreateShaderResourceView(CorrectionsBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(CorrectionsBufferRHI));

			// Create UAV for particles from PersistentParticleBuffer
			FBufferRHIRef ParticleRHI = Self->PersistentParticleBuffer->GetRHI();
			if (!ParticleRHI.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyCorrections: Failed to get ParticleRHI from PersistentParticleBuffer"));
				return;
			}
			FUnorderedAccessViewRHIRef ParticlesUAV = RHICmdList.CreateUnorderedAccessView(ParticleRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(ParticleRHI));

			// Dispatch ApplyCorrections compute shader
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FApplyCorrectionsCS> ComputeShader(ShaderMap);
			FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
			SetComputePipelineState(RHICmdList, ShaderRHI);

			FApplyCorrectionsCS::FParameters Parameters;
			Parameters.Corrections = CorrectionsSRV;
			Parameters.Particles = ParticlesUAV;
			Parameters.CorrectionCount = CorrectionCount;
			SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

			const int32 NumGroups = FMath::DivideAndRoundUp(CorrectionCount, FApplyCorrectionsCS::ThreadGroupSize);
			RHICmdList.DispatchComputeShader(NumGroups, 1, 1);

			UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);

			UE_LOG(LogGPUFluidSimulator, Log, TEXT("ApplyCorrections: Applied %d corrections"), CorrectionCount);
		}
	);
}

void FGPUFluidSimulator::ApplyAttachmentUpdates(const TArray<FAttachedParticleUpdate>& Updates)
{
	if (!bIsInitialized || Updates.Num() == 0 || !PersistentParticleBuffer.IsValid())
	{
		return;
	}

	// Make a copy of updates for the render thread
	TArray<FAttachedParticleUpdate> UpdatesCopy = Updates;
	FGPUFluidSimulator* Self = this;
	const int32 UpdateCount = Updates.Num();

	ENQUEUE_RENDER_COMMAND(ApplyAttachmentUpdates)(
		[Self, UpdatesCopy, UpdateCount](FRHICommandListImmediate& RHICmdList)
		{
			if (!Self->PersistentParticleBuffer.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyAttachmentUpdates: PersistentParticleBuffer not valid"));
				return;
			}

			// Create updates buffer
			const FRHIBufferCreateDesc BufferDesc =
				FRHIBufferCreateDesc::CreateStructured(TEXT("AttachmentUpdates"), UpdateCount * sizeof(FAttachedParticleUpdate), sizeof(FAttachedParticleUpdate))
				.AddUsage(BUF_ShaderResource)
				.SetInitialState(ERHIAccess::SRVMask);
			FBufferRHIRef UpdatesBufferRHI = RHICmdList.CreateBuffer(BufferDesc);

			// Upload updates data
			void* UpdateData = RHICmdList.LockBuffer(UpdatesBufferRHI, 0,
				UpdateCount * sizeof(FAttachedParticleUpdate), RLM_WriteOnly);
			FMemory::Memcpy(UpdateData, UpdatesCopy.GetData(), UpdateCount * sizeof(FAttachedParticleUpdate));
			RHICmdList.UnlockBuffer(UpdatesBufferRHI);

			// Create SRV for updates
			FShaderResourceViewRHIRef UpdatesSRV = RHICmdList.CreateShaderResourceView(UpdatesBufferRHI, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(UpdatesBufferRHI));

			// Create UAV for particles from PersistentParticleBuffer
			FBufferRHIRef ParticleRHI = Self->PersistentParticleBuffer->GetRHI();
			if (!ParticleRHI.IsValid())
			{
				UE_LOG(LogGPUFluidSimulator, Warning, TEXT("ApplyAttachmentUpdates: Failed to get ParticleRHI from PersistentParticleBuffer"));
				return;
			}
			FUnorderedAccessViewRHIRef ParticlesUAV = RHICmdList.CreateUnorderedAccessView(ParticleRHI, FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(ParticleRHI));

			// Dispatch ApplyAttachmentUpdates compute shader
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FApplyAttachmentUpdatesCS> ComputeShader(ShaderMap);
			FRHIComputeShader* ShaderRHI = ComputeShader.GetComputeShader();
			SetComputePipelineState(RHICmdList, ShaderRHI);

			FApplyAttachmentUpdatesCS::FParameters Parameters;
			Parameters.AttachmentUpdates = UpdatesSRV;
			Parameters.Particles = ParticlesUAV;
			Parameters.UpdateCount = UpdateCount;
			SetShaderParameters(RHICmdList, ComputeShader, ShaderRHI, Parameters);

			const int32 NumGroups = FMath::DivideAndRoundUp(UpdateCount, FApplyAttachmentUpdatesCS::ThreadGroupSize);
			RHICmdList.DispatchComputeShader(NumGroups, 1, 1);

			UnsetShaderUAVs(RHICmdList, ComputeShader, ShaderRHI);

			UE_LOG(LogGPUFluidSimulator, Verbose, TEXT("ApplyAttachmentUpdates: Applied %d updates"), UpdateCount);
		}
	);
}

//=============================================================================
// Adhesion Pass Implementations (GPU-based bone attachment)
//=============================================================================

void FGPUFluidSimulator::AddAdhesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef AttachmentUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (!IsAdhesionEnabled() || !bBoneTransformsValid || CachedBoneTransforms.Num() == 0 || CurrentParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FAdhesionCS> ComputeShader(ShaderMap);

	// Upload bone transforms
	FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidBoneTransforms"),
		sizeof(FGPUBoneTransform),
		CachedBoneTransforms.Num(),
		CachedBoneTransforms.GetData(),
		CachedBoneTransforms.Num() * sizeof(FGPUBoneTransform),
		ERDGInitialDataFlags::NoCopy
	);
	FRDGBufferSRVRef BoneTransformsSRVLocal = GraphBuilder.CreateSRV(BoneTransformsBuffer);

	// Upload collision primitives for adhesion check
	FRDGBufferRef SpheresBuffer = nullptr;
	FRDGBufferRef CapsulesBuffer = nullptr;
	FRDGBufferRef BoxesBuffer = nullptr;
	FRDGBufferRef ConvexesBuffer = nullptr;
	FRDGBufferRef ConvexPlanesBuffer = nullptr;

	if (CachedSpheres.Num() > 0)
	{
		SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionSpheres"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), CachedSpheres.Num() * sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedCapsules.Num() > 0)
	{
		CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionCapsules"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), CachedCapsules.Num() * sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedBoxes.Num() > 0)
	{
		BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionBoxes"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), CachedBoxes.Num() * sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexHeaders.Num() > 0)
	{
		ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionConvexes"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), CachedConvexHeaders.Num() * sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexPlanes.Num() > 0)
	{
		ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidConvexPlanes"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), CachedConvexPlanes.Num() * sizeof(FGPUConvexPlane),
			ERDGInitialDataFlags::NoCopy
		);
	}

	// Dummy buffers for empty arrays
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(16, 1);
	if (!SpheresBuffer) SpheresBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheres"));
	if (!CapsulesBuffer) CapsulesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsules"));
	if (!BoxesBuffer) BoxesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxes"));
	if (!ConvexesBuffer) ConvexesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexes"));
	if (!ConvexPlanesBuffer) ConvexPlanesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanes"));

	FAdhesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FAdhesionCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = Params.ParticleRadius;
	PassParameters->Attachments = AttachmentUAV;
	PassParameters->BoneTransforms = BoneTransformsSRVLocal;
	PassParameters->BoneCount = CachedBoneTransforms.Num();
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	PassParameters->AdhesionStrength = CachedAdhesionParams.AdhesionStrength;
	PassParameters->AdhesionRadius = CachedAdhesionParams.AdhesionRadius;
	PassParameters->DetachAccelThreshold = CachedAdhesionParams.DetachAccelThreshold;
	PassParameters->DetachDistanceThreshold = CachedAdhesionParams.DetachDistanceThreshold;
	PassParameters->ColliderContactOffset = CachedAdhesionParams.ColliderContactOffset;
	PassParameters->BoneVelocityScale = CachedAdhesionParams.BoneVelocityScale;
	PassParameters->SlidingFriction = CachedAdhesionParams.SlidingFriction;
	PassParameters->CurrentTime = Params.CurrentTime;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->bEnableAdhesion = CachedAdhesionParams.bEnableAdhesion;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FAdhesionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::Adhesion"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddUpdateAttachedPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef AttachmentSRV,
	FRDGBufferSRVRef InBoneTransformsSRV,
	const FGPUFluidSimulationParams& Params)
{
	// This signature is for external calls. We'll use the internal version.
}

void FGPUFluidSimulator::AddUpdateAttachedPositionsPassInternal(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef AttachmentUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (!IsAdhesionEnabled() || !bBoneTransformsValid || CachedBoneTransforms.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FUpdateAttachedPositionsCS> ComputeShader(ShaderMap);

	// Upload bone transforms
	FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidBoneTransformsUpdate"),
		sizeof(FGPUBoneTransform),
		CachedBoneTransforms.Num(),
		CachedBoneTransforms.GetData(),
		CachedBoneTransforms.Num() * sizeof(FGPUBoneTransform),
		ERDGInitialDataFlags::NoCopy
	);
	FRDGBufferSRVRef BoneTransformsSRVLocal = GraphBuilder.CreateSRV(BoneTransformsBuffer);

	// Upload collision primitives for detachment distance check
	FRDGBufferRef SpheresBuffer = nullptr;
	FRDGBufferRef CapsulesBuffer = nullptr;
	FRDGBufferRef BoxesBuffer = nullptr;
	FRDGBufferRef ConvexesBuffer = nullptr;
	FRDGBufferRef ConvexPlanesBuffer = nullptr;

	if (CachedSpheres.Num() > 0)
	{
		SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionSpheresUpdate"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), CachedSpheres.Num() * sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedCapsules.Num() > 0)
	{
		CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionCapsulesUpdate"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), CachedCapsules.Num() * sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedBoxes.Num() > 0)
	{
		BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionBoxesUpdate"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), CachedBoxes.Num() * sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexHeaders.Num() > 0)
	{
		ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionConvexesUpdate"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), CachedConvexHeaders.Num() * sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexPlanes.Num() > 0)
	{
		ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidConvexPlanesUpdate"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), CachedConvexPlanes.Num() * sizeof(FGPUConvexPlane),
			ERDGInitialDataFlags::NoCopy
		);
	}

	// Dummy buffers for empty arrays
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(16, 1);
	if (!SpheresBuffer) SpheresBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheresUpdate"));
	if (!CapsulesBuffer) CapsulesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsulesUpdate"));
	if (!BoxesBuffer) BoxesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxesUpdate"));
	if (!ConvexesBuffer) ConvexesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexesUpdate"));
	if (!ConvexPlanesBuffer) ConvexPlanesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanesUpdate"));

	FUpdateAttachedPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpdateAttachedPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->Attachments = AttachmentUAV;
	PassParameters->BoneTransforms = BoneTransformsSRVLocal;
	PassParameters->BoneCount = CachedBoneTransforms.Num();
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	PassParameters->DetachAccelThreshold = CachedAdhesionParams.DetachAccelThreshold;
	PassParameters->DetachDistanceThreshold = CachedAdhesionParams.DetachDistanceThreshold;
	PassParameters->ColliderContactOffset = CachedAdhesionParams.ColliderContactOffset;
	PassParameters->BoneVelocityScale = CachedAdhesionParams.BoneVelocityScale;
	PassParameters->SlidingFriction = CachedAdhesionParams.SlidingFriction;
	PassParameters->DeltaTime = Params.DeltaTime;

	// Gravity sliding parameters
	PassParameters->Gravity = CachedAdhesionParams.Gravity;
	PassParameters->GravitySlidingScale = CachedAdhesionParams.GravitySlidingScale;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FUpdateAttachedPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::UpdateAttachedPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddClearDetachedFlagPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV)
{
	if (!IsAdhesionEnabled())
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FClearDetachedFlagCS> ComputeShader(ShaderMap);

	FClearDetachedFlagCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearDetachedFlagCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FClearDetachedFlagCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ClearDetachedFlag"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Boundary Adhesion Pass (Flex-style with Spatial Hash Optimization)
//=============================================================================

// Spatial hash constants (must match shader)
static constexpr int32 BOUNDARY_HASH_SIZE = 65536;  // 2^16 cells
static constexpr int32 BOUNDARY_MAX_PARTICLES_PER_CELL = 16;

void FGPUFluidSimulator::AddBoundaryAdhesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	// Check if we have boundary particles from either GPU skinning or CPU upload
	const bool bUseGPUSkinning = IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();
	const bool bUseCPUBoundary = !bUseGPUSkinning && CachedBoundaryParticles.Num() > 0;

	// Early exit if no boundary particles or adhesion disabled
	if (!IsBoundaryAdhesionEnabled() || (!bUseGPUSkinning && !bUseCPUBoundary) || CurrentParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	int32 BoundaryParticleCount;
	FRDGBufferRef BoundaryParticleBuffer;
	FRDGBufferSRVRef BoundaryParticlesSRV;

	if (bUseGPUSkinning)
	{
		// Use GPU-skinned world boundary buffer
		BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		BoundaryParticleBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_Adhesion"));
		BoundaryParticlesSRV = GraphBuilder.CreateSRV(BoundaryParticleBuffer);
	}
	else
	{
		// Use CPU-uploaded boundary particles (legacy path)
		BoundaryParticleCount = CachedBoundaryParticles.Num();
		BoundaryParticleBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles"),
			sizeof(FGPUBoundaryParticle),
			BoundaryParticleCount,
			CachedBoundaryParticles.GetData(),
			BoundaryParticleCount * sizeof(FGPUBoundaryParticle),
			ERDGInitialDataFlags::NoCopy
		);
		BoundaryParticlesSRV = GraphBuilder.CreateSRV(BoundaryParticleBuffer);
	}

	// Cell size = AdhesionRadius (particles within this range can interact)
	const float BoundaryCellSize = CachedBoundaryAdhesionParams.AdhesionRadius;

	// Create spatial hash buffers
	FRDGBufferRef AdhesionCellCountsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BOUNDARY_HASH_SIZE),
		TEXT("GPUFluidBoundaryCellCounts")
	);
	FRDGBufferRef AdhesionParticleIndicesBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BOUNDARY_HASH_SIZE * BOUNDARY_MAX_PARTICLES_PER_CELL),
		TEXT("GPUFluidBoundaryParticleIndices")
	);

	// Pass 1: Clear spatial hash
	{
		TShaderMapRef<FClearBoundaryHashCS> ClearShader(ShaderMap);
		FClearBoundaryHashCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearBoundaryHashCS::FParameters>();
		ClearParams->RWBoundaryCellCounts = GraphBuilder.CreateUAV(AdhesionCellCountsBuffer);

		const uint32 ClearGroups = FMath::DivideAndRoundUp(BOUNDARY_HASH_SIZE, FClearBoundaryHashCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ClearBoundaryHash"),
			ClearShader,
			ClearParams,
			FIntVector(ClearGroups, 1, 1)
		);
	}

	// Pass 2: Build spatial hash
	{
		TShaderMapRef<FBuildBoundaryHashCS> BuildShader(ShaderMap);
		FBuildBoundaryHashCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FBuildBoundaryHashCS::FParameters>();
		BuildParams->BoundaryParticles = BoundaryParticlesSRV;
		BuildParams->BoundaryParticleCount = BoundaryParticleCount;
		BuildParams->BoundaryCellSize = BoundaryCellSize;
		BuildParams->RWBoundaryCellCounts = GraphBuilder.CreateUAV(AdhesionCellCountsBuffer);
		BuildParams->RWBoundaryParticleIndices = GraphBuilder.CreateUAV(AdhesionParticleIndicesBuffer);

		const uint32 BuildGroups = FMath::DivideAndRoundUp(BoundaryParticleCount, FBuildBoundaryHashCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::BuildBoundaryHash"),
			BuildShader,
			BuildParams,
			FIntVector(BuildGroups, 1, 1)
		);
	}

	// Pass 3: Boundary adhesion (using spatial hash)
	{
		TShaderMapRef<FBoundaryAdhesionCS> AdhesionShader(ShaderMap);
		FBoundaryAdhesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundaryAdhesionCS::FParameters>();
		PassParameters->Particles = ParticlesUAV;
		PassParameters->ParticleCount = CurrentParticleCount;
		PassParameters->BoundaryParticles = BoundaryParticlesSRV;
		PassParameters->BoundaryParticleCount = BoundaryParticleCount;
		PassParameters->BoundaryCellCounts = GraphBuilder.CreateSRV(AdhesionCellCountsBuffer);
		PassParameters->BoundaryParticleIndices = GraphBuilder.CreateSRV(AdhesionParticleIndicesBuffer);
		PassParameters->BoundaryCellSize = BoundaryCellSize;
		PassParameters->AdhesionStrength = CachedBoundaryAdhesionParams.AdhesionStrength;
		PassParameters->AdhesionRadius = CachedBoundaryAdhesionParams.AdhesionRadius;
		PassParameters->CohesionStrength = CachedBoundaryAdhesionParams.CohesionStrength;
		PassParameters->SmoothingRadius = Params.SmoothingRadius;
		PassParameters->DeltaTime = Params.DeltaTime;
		PassParameters->RestDensity = Params.RestDensity;
		PassParameters->Poly6Coeff = Params.Poly6Coeff;

		const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FBoundaryAdhesionCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::BoundaryAdhesion"),
			AdhesionShader,
			PassParameters,
			FIntVector(NumGroups, 1, 1)
		);
	}
}

//=============================================================================
// GPU Boundary Skinning Pass
// Transforms bone-local boundary particles to world space using GPU compute
//=============================================================================

void FGPUFluidSimulator::AddBoundarySkinningPass(
	FRDGBuilder& GraphBuilder,
	const FGPUFluidSimulationParams& Params)
{
	FScopeLock Lock(&BoundarySkinningLock);

	if (TotalLocalBoundaryParticleCount <= 0 || BoundarySkinningDataMap.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FBoundarySkinningCS> SkinningShader(ShaderMap);

	// Ensure world boundary buffer is large enough
	if (WorldBoundaryBufferCapacity < TotalLocalBoundaryParticleCount)
	{
		PersistentWorldBoundaryBuffer.SafeRelease();
		WorldBoundaryBufferCapacity = TotalLocalBoundaryParticleCount;
	}

	// Create or reuse world boundary buffer
	FRDGBufferRef WorldBoundaryBuffer;
	if (PersistentWorldBoundaryBuffer.IsValid())
	{
		WorldBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidWorldBoundaryParticles"));
	}
	else
	{
		WorldBoundaryBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), WorldBoundaryBufferCapacity),
			TEXT("GPUFluidWorldBoundaryParticles")
		);
	}
	FRDGBufferUAVRef WorldBoundaryUAV = GraphBuilder.CreateUAV(WorldBoundaryBuffer);

	// Track output offset for concatenating multiple owners
	int32 OutputOffset = 0;

	// Process each owner's boundary particles
	for (auto& Pair : BoundarySkinningDataMap)
	{
		const int32 OwnerID = Pair.Key;
		FGPUBoundarySkinningData& SkinningData = Pair.Value;

		if (SkinningData.LocalParticles.Num() == 0)
		{
			continue;
		}

		const int32 LocalParticleCount = SkinningData.LocalParticles.Num();

		// Upload or reuse local boundary particles buffer
		TRefCountPtr<FRDGPooledBuffer>& LocalBuffer = PersistentLocalBoundaryBuffers.FindOrAdd(OwnerID);
		FRDGBufferRef LocalBoundaryBuffer;

		if (!SkinningData.bLocalParticlesUploaded || !LocalBuffer.IsValid())
		{
			// Upload local particles (first time or after change)
			LocalBoundaryBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidLocalBoundaryParticles"),
				sizeof(FGPUBoundaryParticleLocal),
				LocalParticleCount,
				SkinningData.LocalParticles.GetData(),
				LocalParticleCount * sizeof(FGPUBoundaryParticleLocal),
				ERDGInitialDataFlags::NoCopy
			);
			SkinningData.bLocalParticlesUploaded = true;

			// Extract pooled buffer for persistence
			GraphBuilder.QueueBufferExtraction(LocalBoundaryBuffer, &LocalBuffer);
		}
		else
		{
			// Reuse existing buffer
			LocalBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(LocalBuffer, TEXT("GPUFluidLocalBoundaryParticles"));
		}
		FRDGBufferSRVRef LocalBoundarySRV = GraphBuilder.CreateSRV(LocalBoundaryBuffer);

		// Upload bone transforms (each frame)
		FRDGBufferRef BoneTransformsBuffer;
		const int32 BoneCount = SkinningData.BoneTransforms.Num();
		if (BoneCount > 0)
		{
			BoneTransformsBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidBoneTransforms"),
				sizeof(FMatrix44f),
				BoneCount,
				SkinningData.BoneTransforms.GetData(),
				BoneCount * sizeof(FMatrix44f),
				ERDGInitialDataFlags::NoCopy
			);
		}
		else
		{
			// Create dummy buffer with identity matrix
			FMatrix44f Identity = FMatrix44f::Identity;
			BoneTransformsBuffer = CreateStructuredBuffer(
				GraphBuilder,
				TEXT("GPUFluidBoneTransforms"),
				sizeof(FMatrix44f),
				1,
				&Identity,
				sizeof(FMatrix44f),
				ERDGInitialDataFlags::NoCopy
			);
		}
		FRDGBufferSRVRef SkinningBoneTransformsSRV = GraphBuilder.CreateSRV(BoneTransformsBuffer);

		// Setup skinning shader parameters
		FBoundarySkinningCS::FParameters* PassParams = GraphBuilder.AllocParameters<FBoundarySkinningCS::FParameters>();
		PassParams->LocalBoundaryParticles = LocalBoundarySRV;
		PassParams->WorldBoundaryParticles = WorldBoundaryUAV;
		PassParams->BoneTransforms = SkinningBoneTransformsSRV;
		PassParams->BoundaryParticleCount = LocalParticleCount;
		PassParams->BoneCount = FMath::Max(1, BoneCount);
		PassParams->OwnerID = OwnerID;
		PassParams->ComponentTransform = SkinningData.ComponentTransform;

		const uint32 NumGroups = FMath::DivideAndRoundUp(LocalParticleCount, FBoundarySkinningCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::BoundarySkinning(Owner=%d, Count=%d)", OwnerID, LocalParticleCount),
			SkinningShader,
			PassParams,
			FIntVector(NumGroups, 1, 1)
		);

		OutputOffset += LocalParticleCount;
	}

	// Extract world boundary buffer for use in density/viscosity passes
	GraphBuilder.QueueBufferExtraction(WorldBoundaryBuffer, &PersistentWorldBoundaryBuffer);

	// Update cached boundary particles with world-space positions
	// This is done by reading back from GPU (or we can use the skinned buffer directly in passes)
	// For now, we use the persistent world buffer in subsequent passes
}

//=============================================================================
// Collision Feedback Implementation (Particle -> Player Interaction)
// GPU collision data readback for force calculation and event triggering
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
	// FRHIGPUBufferReadback: Check IsReady() before Lock() to avoid flush

	// 디버그 로깅 (60프레임마다, 약 1초)
	static int32 ContactCountDebugFrame = 0;
	const bool bLogThisFrame = (ContactCountDebugFrame++ % 60 == 0);

	// Ensure readback objects are valid
	if (ContactCountReadbacks[0] == nullptr)
	{
		if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[ContactCount] Readback 객체 미할당"));
		return;  // Will be allocated in SimulateSubstep
	}

	// Read from readback that was enqueued 2 frames ago (allowing GPU latency)
	// Workaround: Search for any ready buffer instead of calculated index
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
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("[ContactCount] FrameNum=%d, ReadIdx=%d (searched), 조건(>=2)=%s"),
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
				UE_LOG(LogGPUFluidSimulator, Log, TEXT("[ContactCount] 읽기 성공: 총접촉=%d, 비영콜라이더=%d"),
					TotalContactCount, NonZeroColliders);
			}
		}
		else
		{
			if (bLogThisFrame) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[ContactCount] Lock() 실패 - nullptr 반환"));
		}

		ContactCountReadbacks[ReadIdx]->Unlock();
	}
	else if (bLogThisFrame)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[ContactCount] Ready 버퍼 없음 (ReadIdx=%d) - 데이터 업데이트 건너뜀"), ReadIdx);
	}

	// Note: Frame counter is incremented in SimulateSubstep AFTER EnqueueCopy, not here
}

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
// Collider Contact Count API Implementation
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
	// 디버그 로깅 (60프레임마다)
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

	// Convexes: indices SphereCount+CapsuleCount+BoxCount to Total-1
	for (int32 i = 0; i < CachedConvexHeaders.Num(); ++i)
	{
		if (CachedConvexHeaders[i].OwnerID == OwnerID)
		{
			TotalCount += GetColliderContactCount(ColliderIndex);
			MatchedColliders++;
		}
		ColliderIndex++;
	}

	if (bLogThisFrame)
	{
		UE_LOG(LogGPUFluidSimulator, Log, TEXT("[GetContactCountForOwner] OwnerID=%d, 매칭콜라이더=%d, 총접촉=%d, ReadyArray크기=%d"),
			OwnerID, MatchedColliders, TotalCount, ReadyColliderContactCounts.Num());
	}

	return TotalCount;
}

//=============================================================================
// Z-Order (Morton Code) Sorting Pipeline Implementation
// Provides cache-coherent particle access for neighbor search
//=============================================================================

FRDGBufferRef FGPUFluidSimulator::ExecuteZOrderSortingPipeline(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef InParticleBuffer,
	FRDGBufferUAVRef& OutCellStartUAV,
	FRDGBufferSRVRef& OutCellStartSRV,
	FRDGBufferUAVRef& OutCellEndUAV,
	FRDGBufferSRVRef& OutCellEndSRV,
	const FGPUFluidSimulationParams& Params)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid::ZOrderSorting");

	if (CurrentParticleCount <= 0)
	{
		return InParticleBuffer;
	}

	// Check if we need to allocate/resize buffers
	const bool bNeedResize = ZOrderBufferParticleCapacity < CurrentParticleCount;
	const int32 NumBlocks = FMath::DivideAndRoundUp(CurrentParticleCount, 256);
	const int32 CellCount = GPU_MAX_CELLS;  // 21-bit Morton code = Cell ID (128^3 = 2,097,152 cells)

	//=========================================================================
	// Step 1: Create/reuse Morton code and index buffers
	//=========================================================================
	FRDGBufferRef MortonCodesRDG;
	FRDGBufferRef MortonCodesTempRDG;
	FRDGBufferRef SortIndicesRDG;
	FRDGBufferRef SortIndicesTempRDG;
	FRDGBufferRef HistogramRDG;
	FRDGBufferRef BucketOffsetsRDG;
	FRDGBufferRef CellStartRDG;
	FRDGBufferRef CellEndRDG;

	// Morton codes and indices
	{
		FRDGBufferDesc MortonDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CurrentParticleCount);
		MortonCodesRDG = GraphBuilder.CreateBuffer(MortonDesc, TEXT("GPUFluid.MortonCodes"));
		MortonCodesTempRDG = GraphBuilder.CreateBuffer(MortonDesc, TEXT("GPUFluid.MortonCodesTemp"));
		SortIndicesRDG = GraphBuilder.CreateBuffer(MortonDesc, TEXT("GPUFluid.SortIndices"));
		SortIndicesTempRDG = GraphBuilder.CreateBuffer(MortonDesc, TEXT("GPUFluid.SortIndicesTemp"));
	}

	// Radix sort histogram: 16 buckets * NumBlocks
	{
		FRDGBufferDesc HistogramDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE * NumBlocks);
		HistogramRDG = GraphBuilder.CreateBuffer(HistogramDesc, TEXT("GPUFluid.RadixHistogram"));

		FRDGBufferDesc BucketDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE);
		BucketOffsetsRDG = GraphBuilder.CreateBuffer(BucketDesc, TEXT("GPUFluid.RadixBucketOffsets"));
	}

	// Cell Start/End (use hash size for compatibility with existing neighbor search)
	{
		FRDGBufferDesc CellDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CellCount);
		CellStartRDG = GraphBuilder.CreateBuffer(CellDesc, TEXT("GPUFluid.CellStart"));
		CellEndRDG = GraphBuilder.CreateBuffer(CellDesc, TEXT("GPUFluid.CellEnd"));
	}

	//=========================================================================
	// Step 2: Compute Morton codes
	//=========================================================================
	{
		FRDGBufferSRVRef ParticlesSRV = GraphBuilder.CreateSRV(InParticleBuffer);
		FRDGBufferUAVRef MortonCodesUAV = GraphBuilder.CreateUAV(MortonCodesRDG);
		FRDGBufferUAVRef IndicesUAV = GraphBuilder.CreateUAV(SortIndicesRDG);

		AddComputeMortonCodesPass(GraphBuilder, ParticlesSRV, MortonCodesUAV, IndicesUAV, Params);
	}

	//=========================================================================
	// Step 3: Radix Sort (6 passes for 21-bit Morton codes)
	//=========================================================================
	AddRadixSortPasses(GraphBuilder, MortonCodesRDG, SortIndicesRDG, CurrentParticleCount);

	//=========================================================================
	// Step 4: Reorder particle data based on sorted indices
	//=========================================================================
	FRDGBufferRef SortedParticleBuffer;
	{
		FRDGBufferDesc SortedDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), CurrentParticleCount);
		SortedParticleBuffer = GraphBuilder.CreateBuffer(SortedDesc, TEXT("GPUFluid.SortedParticles"));

		FRDGBufferSRVRef OldParticlesSRV = GraphBuilder.CreateSRV(InParticleBuffer);
		FRDGBufferSRVRef SortedIndicesSRV = GraphBuilder.CreateSRV(SortIndicesRDG);
		FRDGBufferUAVRef SortedParticlesUAV = GraphBuilder.CreateUAV(SortedParticleBuffer);

		AddReorderParticlesPass(GraphBuilder, OldParticlesSRV, SortedIndicesSRV, SortedParticlesUAV);
	}

	//=========================================================================
	// Step 5: Compute Cell Start/End indices
	//=========================================================================
	{
		FRDGBufferSRVRef SortedMortonSRV = GraphBuilder.CreateSRV(MortonCodesRDG);
		OutCellStartUAV = GraphBuilder.CreateUAV(CellStartRDG);
		OutCellEndUAV = GraphBuilder.CreateUAV(CellEndRDG);

		AddComputeCellStartEndPass(GraphBuilder, SortedMortonSRV, OutCellStartUAV, OutCellEndUAV);

		OutCellStartSRV = GraphBuilder.CreateSRV(CellStartRDG);
		OutCellEndSRV = GraphBuilder.CreateSRV(CellEndRDG);
	}

	// Update capacity tracking
	ZOrderBufferParticleCapacity = CurrentParticleCount;

	return SortedParticleBuffer;
}

void FGPUFluidSimulator::AddComputeMortonCodesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticlesSRV,
	FRDGBufferUAVRef MortonCodesUAV,
	FRDGBufferUAVRef InParticleIndicesUAV,
	const FGPUFluidSimulationParams& Params)
{
	// Validate CellSize is valid (prevent division by zero in shader)
	if (Params.CellSize <= 0.0f)
	{
		UE_LOG(LogGPUFluidSimulator, Error,
			TEXT("Morton code ERROR: Invalid CellSize (%.4f)! Must be > 0. Using default 2.0."),
			Params.CellSize);
		// We'll use the value anyway since we can't modify Params, but the shader will handle it
	}

	// Validate bounds fit within Morton code capacity (10 bits per axis)
	const float CellSizeToUse = FMath::Max(Params.CellSize, 0.001f);
	const float MaxExtent = GPU_MORTON_GRID_SIZE * CellSizeToUse;
	const FVector3f BoundsExtent = SimulationBoundsMax - SimulationBoundsMin;

	// Warn if bounds exceed Morton code capacity
	if (BoundsExtent.X > MaxExtent || BoundsExtent.Y > MaxExtent || BoundsExtent.Z > MaxExtent)
	{
		UE_LOG(LogGPUFluidSimulator, Warning,
			TEXT("Morton code bounds overflow! BoundsExtent(%.1f, %.1f, %.1f) exceeds MaxExtent(%.1f). "
			     "Reduce simulation bounds or increase CellSize."),
			BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z, MaxExtent);
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FComputeMortonCodesCS> ComputeShader(ShaderMap);

	FComputeMortonCodesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeMortonCodesCS::FParameters>();
	PassParameters->Particles = ParticlesSRV;
	PassParameters->MortonCodes = MortonCodesUAV;
	PassParameters->ParticleIndices = InParticleIndicesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->BoundsMin = SimulationBoundsMin;
	PassParameters->BoundsExtent = SimulationBoundsMax - SimulationBoundsMin;
	PassParameters->CellSize = Params.CellSize;

	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FComputeMortonCodesCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ComputeMortonCodes(%d)", CurrentParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddRadixSortPasses(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef& InOutMortonCodes,
	FRDGBufferRef& InOutParticleIndices,
	int32 ParticleCount)
{
	if (ParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const int32 NumBlocks = FMath::DivideAndRoundUp(ParticleCount, GPU_RADIX_ELEMENTS_PER_GROUP);

	//=========================================================================
	// Create TRANSIENT RDG BUFFERS for RadixSort internal state
	// These buffers only live within this frame's RDG execution.
	// RDG correctly tracks dependencies between passes, preventing aliasing
	// issues that occur with incorrectly managed external buffers.
	//
	// Unlike UE's GPUSort which uses direct RHI (no RDG), we use RDG transient
	// buffers because our simulation pipeline is already RDG-based.
	//=========================================================================

	const int32 RequiredHistogramSize = GPU_RADIX_SIZE * NumBlocks;

	// Create transient ping-pong buffers for RadixSort
	FRDGBufferDesc KeysTempDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
	FRDGBufferRef KeysTemp = GraphBuilder.CreateBuffer(KeysTempDesc, TEXT("RadixSort.KeysTemp"));

	FRDGBufferDesc ValuesTempDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
	FRDGBufferRef ValuesTemp = GraphBuilder.CreateBuffer(ValuesTempDesc, TEXT("RadixSort.ValuesTemp"));

	// Create transient Histogram buffer [NumBlocks * RADIX_SIZE]
	FRDGBufferDesc HistogramDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RequiredHistogramSize);
	FRDGBufferRef Histogram = GraphBuilder.CreateBuffer(HistogramDesc, TEXT("RadixSort.Histogram"));

	// Create transient BucketOffsets buffer [RADIX_SIZE]
	FRDGBufferDesc BucketOffsetsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE);
	FRDGBufferRef BucketOffsets = GraphBuilder.CreateBuffer(BucketOffsetsDesc, TEXT("RadixSort.BucketOffsets"));

	// Ping-pong buffers using array + index pattern
	// Pass 0: Read Keys[0]=InOutMortonCodes, Write Keys[1]=KeysTemp
	// Pass 1: Read Keys[1]=KeysTemp, Write Keys[0]=InOutMortonCodes
	// Pass 2: Read Keys[0]=InOutMortonCodes, Write Keys[1]=KeysTemp
	// Pass 3: Read Keys[1]=KeysTemp, Write Keys[0]=InOutMortonCodes
	// After 4 passes, BufferIndex=0, so final result is in Keys[0]=InOutMortonCodes
	FRDGBufferRef Keys[2] = { InOutMortonCodes, KeysTemp };
	FRDGBufferRef Values[2] = { InOutParticleIndices, ValuesTemp };
	int32 BufferIndex = 0;

	// Auto-calculated passes for Morton code coverage
	// Morton code = GPU_MORTON_CODE_BITS (GridAxisBits × 3)
	// Passes = ceil(MortonCodeBits / RadixBits) = GPU_RADIX_SORT_PASSES
	// Ping-pong buffer: even passes → result in original buffer, odd → result in temp buffer
	// Current: GPU_RADIX_SORT_PASSES is even, so result is in InOutMortonCodes
	static_assert(GPU_RADIX_SORT_PASSES % 2 == 0, "GPU_RADIX_SORT_PASSES must be even for ping-pong buffer to return result in original buffer");
	for (int32 Pass = 0; Pass < GPU_RADIX_SORT_PASSES; ++Pass)
	{
		const int32 BitOffset = Pass * GPU_RADIX_BITS;
		const int32 SrcIndex = BufferIndex;
		const int32 DstIndex = BufferIndex ^ 1;

		RDG_EVENT_SCOPE(GraphBuilder, "RadixSort Pass %d (bits %d-%d)", Pass, BitOffset, BitOffset + 3);

		// Pass 1: Histogram
		{
			TShaderMapRef<FRadixSortHistogramCS> HistogramShader(ShaderMap);
			FRadixSortHistogramCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortHistogramCS::FParameters>();
			Params->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
			Params->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
			Params->Histogram = GraphBuilder.CreateUAV(Histogram);
			Params->ElementCount = ParticleCount;
			Params->BitOffset = BitOffset;
			Params->NumGroups = NumBlocks;

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Histogram"),
				HistogramShader,
				Params,
				FIntVector(NumBlocks, 1, 1)
			);
		}

		// Pass 2a: Global Prefix Sum (within each bucket)
		{
			TShaderMapRef<FRadixSortGlobalPrefixSumCS> PrefixSumShader(ShaderMap);
			FRadixSortGlobalPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortGlobalPrefixSumCS::FParameters>();
			Params->Histogram = GraphBuilder.CreateUAV(Histogram);
			Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);
			Params->NumGroups = NumBlocks;

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GlobalPrefixSum"),
				PrefixSumShader,
				Params,
				FIntVector(1, 1, 1)  // Single group, 16 threads
			);
		}

		// Pass 2b: Bucket Prefix Sum (across buckets)
		{
			TShaderMapRef<FRadixSortBucketPrefixSumCS> BucketSumShader(ShaderMap);
			FRadixSortBucketPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortBucketPrefixSumCS::FParameters>();
			Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("BucketPrefixSum"),
				BucketSumShader,
				Params,
				FIntVector(1, 1, 1)
			);
		}

		// Pass 3: Scatter
		{
			TShaderMapRef<FRadixSortScatterCS> ScatterShader(ShaderMap);
			FRadixSortScatterCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortScatterCS::FParameters>();
			Params->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
			Params->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
			Params->KeysOut = GraphBuilder.CreateUAV(Keys[DstIndex]);
			Params->ValuesOut = GraphBuilder.CreateUAV(Values[DstIndex]);
			Params->HistogramSRV = GraphBuilder.CreateSRV(Histogram);
			Params->GlobalOffsetsSRV = GraphBuilder.CreateSRV(BucketOffsets);
			Params->ElementCount = ParticleCount;
			Params->BitOffset = BitOffset;

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Scatter"),
				ScatterShader,
				Params,
				FIntVector(NumBlocks, 1, 1)
			);
		}

		// Ping-pong: toggle buffer index for next pass
		BufferIndex ^= 1;
	}

	// After GPU_RADIX_SORT_PASSES passes, the final sorted data is in Keys[BufferIndex]/Values[BufferIndex]
	// BufferIndex alternates each pass. With even passes, ends at 0 (original buffer).
	// static_assert above ensures GPU_RADIX_SORT_PASSES is even.
	InOutMortonCodes = Keys[BufferIndex];
	InOutParticleIndices = Values[BufferIndex];
}

void FGPUFluidSimulator::AddReorderParticlesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef OldParticlesSRV,
	FRDGBufferSRVRef SortedIndicesSRV,
	FRDGBufferUAVRef SortedParticlesUAV)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FReorderParticlesCS> ComputeShader(ShaderMap);

	FReorderParticlesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReorderParticlesCS::FParameters>();
	PassParameters->OldParticles = OldParticlesSRV;
	PassParameters->SortedIndices = SortedIndicesSRV;
	PassParameters->SortedParticles = SortedParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;

	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FReorderParticlesCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ReorderParticles(%d)", CurrentParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddComputeCellStartEndPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef SortedMortonCodesSRV,
	FRDGBufferUAVRef CellStartUAV,
	FRDGBufferUAVRef CellEndUAV)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const int32 CellCount = GPU_MAX_CELLS;

	// Step 1: Clear cell indices to invalid (0xFFFFFFFF)
	{
		TShaderMapRef<FClearCellIndicesCS> ClearShader(ShaderMap);
		FClearCellIndicesCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellIndicesCS::FParameters>();
		ClearParams->CellStart = CellStartUAV;
		ClearParams->CellEnd = CellEndUAV;

		const int32 NumGroups = FMath::DivideAndRoundUp(CellCount, FClearCellIndicesCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ClearCellIndices(%d)", CellCount),
			ClearShader,
			ClearParams,
			FIntVector(NumGroups, 1, 1)
		);
	}

	// Step 2: Compute cell start/end from sorted Morton codes
	{
		TShaderMapRef<FComputeCellStartEndCS> ComputeShader(ShaderMap);
		FComputeCellStartEndCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeCellStartEndCS::FParameters>();
		PassParameters->SortedMortonCodes = SortedMortonCodesSRV;
		PassParameters->CellStart = CellStartUAV;
		PassParameters->CellEnd = CellEndUAV;
		PassParameters->ParticleCount = CurrentParticleCount;

		const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FComputeCellStartEndCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ComputeCellStartEnd(%d)", CurrentParticleCount),
			ComputeShader,
			PassParameters,
			FIntVector(NumGroups, 1, 1)
		);
	}

}
