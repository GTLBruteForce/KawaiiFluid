// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/SpatialHash.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Physics/DensityConstraint.h"
#include "Physics/ViscositySolver.h"
#include "Physics/AdhesionSolver.h"
#include "Collision/FluidCollider.h"
#include "Collision/MeshFluidCollider.h"
#include "Collision/PerPolygonCollisionProcessor.h"
#include "Components/FluidInteractionComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Async/Async.h"
#include "RenderingThread.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidParticle.h"

// Profiling
DECLARE_STATS_GROUP(TEXT("KawaiiFluidContext"), STATGROUP_KawaiiFluidContext, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Context Simulate"), STAT_ContextSimulate, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context PredictPositions"), STAT_ContextPredictPositions, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context UpdateNeighbors"), STAT_ContextUpdateNeighbors, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context SolveDensity"), STAT_ContextSolveDensity, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context HandleCollisions"), STAT_ContextHandleCollisions, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context WorldCollision"), STAT_ContextWorldCollision, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context FinalizePositions"), STAT_ContextFinalizePositions, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context ApplyViscosity"), STAT_ContextApplyViscosity, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context ApplyAdhesion"), STAT_ContextApplyAdhesion, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context ApplyCohesion"), STAT_ContextApplyCohesion, STATGROUP_KawaiiFluidContext);

UKawaiiFluidSimulationContext::UKawaiiFluidSimulationContext()
{
}

UKawaiiFluidSimulationContext::~UKawaiiFluidSimulationContext()
{
	// Explicitly reset to ensure complete type is available for destruction
	PerPolygonProcessor.Reset();
	ReleaseGPUSimulator();
}

void UKawaiiFluidSimulationContext::InitializeSolvers(const UKawaiiFluidPresetDataAsset* Preset)
{
	if (!Preset)
	{
		return;
	}

	DensityConstraint = MakeShared<FDensityConstraint>(
		Preset->RestDensity,
		Preset->SmoothingRadius,
		Preset->Compliance
	);
	ViscositySolver = MakeShared<FViscositySolver>();
	AdhesionSolver = MakeShared<FAdhesionSolver>();

	bSolversInitialized = true;
}

void UKawaiiFluidSimulationContext::EnsureSolversInitialized(const UKawaiiFluidPresetDataAsset* Preset)
{
	if (!bSolversInitialized && Preset)
	{
		InitializeSolvers(Preset);
	}
}

//=============================================================================
// GPU Simulation Methods
//=============================================================================

void UKawaiiFluidSimulationContext::InitializeGPUSimulator(int32 MaxParticleCount)
{
	if (GPUSimulator.IsValid())
	{
		// Already initialized - resize if needed
		if (GPUSimulator->GetMaxParticleCount() < MaxParticleCount)
		{
			GPUSimulator->Release();
			GPUSimulator->Initialize(MaxParticleCount);
		}
		return;
	}

	GPUSimulator = MakeShared<FGPUFluidSimulator>();
	GPUSimulator->Initialize(MaxParticleCount);

	UE_LOG(LogTemp, Log, TEXT("GPU Fluid Simulator initialized with capacity: %d"), MaxParticleCount);
}

void UKawaiiFluidSimulationContext::ReleaseGPUSimulator()
{
	if (GPUSimulator.IsValid())
	{
		GPUSimulator->Release();
		GPUSimulator.Reset();
	}
}

bool UKawaiiFluidSimulationContext::IsGPUSimulatorReady() const
{
	return GPUSimulator.IsValid() && GPUSimulator->IsReady();
}

FGPUFluidSimulationParams UKawaiiFluidSimulationContext::BuildGPUSimParams(
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	float SubstepDT) const
{
	FGPUFluidSimulationParams GPUParams;

	// Physics parameters from preset
	GPUParams.RestDensity = Preset->RestDensity;
	GPUParams.SmoothingRadius = Preset->SmoothingRadius;
	GPUParams.Compliance = Preset->Compliance;
	GPUParams.ParticleRadius = Preset->ParticleRadius;
	GPUParams.ViscosityCoefficient = Preset->ViscosityCoefficient;

	// Gravity from preset
	GPUParams.Gravity = FVector3f(Preset->Gravity);

	// Time
	GPUParams.DeltaTime = SubstepDT;

	// Spatial hash
	GPUParams.CellSize = Preset->SmoothingRadius;  // Cell size = smoothing radius

	// Bounds collision (use world bounds from params or default)
	if (Params.WorldBounds.IsValid)
	{
		GPUParams.BoundsMin = FVector3f(Params.WorldBounds.Min);
		GPUParams.BoundsMax = FVector3f(Params.WorldBounds.Max);
	}
	else
	{
		// Default large bounds (effectively no bounds collision)
		GPUParams.BoundsMin = FVector3f(-1000000.0f, -1000000.0f, -1000000.0f);
		GPUParams.BoundsMax = FVector3f(1000000.0f, 1000000.0f, 1000000.0f);
	}

	GPUParams.BoundsRestitution = Preset->Restitution;
	GPUParams.BoundsFriction = Preset->Friction;

	// Pressure iterations (typically 1-4)
	GPUParams.PressureIterations = 1;

	// Precompute kernel coefficients
	GPUParams.PrecomputeKernelCoefficients();

	// Configure Distance Field collision on GPU simulator (if enabled)
	if (GPUSimulator.IsValid() && Preset->bUseDistanceFieldCollision)
	{
		FGPUDistanceFieldCollisionParams DFParams;
		DFParams.bEnabled = 1;
		DFParams.ParticleRadius = Preset->ParticleRadius;
		DFParams.Restitution = Preset->DFCollisionRestitution;
		DFParams.Friction = Preset->DFCollisionFriction;
		DFParams.CollisionThreshold = Preset->DFCollisionThreshold;

		// Volume parameters will be set by scene renderer
		// when Global Distance Field is available
		DFParams.VolumeCenter = FVector3f::ZeroVector;
		DFParams.VolumeExtent = FVector3f(10000.0f);  // Large default extent
		DFParams.VoxelSize = 10.0f;  // Default voxel size
		DFParams.MaxDistance = 1000.0f;

		GPUSimulator->SetDistanceFieldCollisionParams(DFParams);
	}
	else if (GPUSimulator.IsValid())
	{
		GPUSimulator->SetDistanceFieldCollisionEnabled(false);
	}

	return GPUParams;
}

TArray<int32> UKawaiiFluidSimulationContext::ExtractAttachedParticleIndices(const TArray<FFluidParticle>& Particles) const
{
	TArray<int32> AttachedIndices;
	AttachedIndices.Reserve(Particles.Num() / 10);  // Estimate ~10% attached

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		if (Particles[i].bIsAttached)
		{
			AttachedIndices.Add(i);
		}
	}

	return AttachedIndices;
}

void UKawaiiFluidSimulationContext::HandleAttachedParticlesCPU(
	TArray<FFluidParticle>& Particles,
	const TArray<int32>& AttachedIndices,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	float SubstepDT)
{
	if (AttachedIndices.Num() == 0)
	{
		return;
	}

	// Update attached particle positions (bone tracking)
	// This is already done in the main Simulate loop before substeps

	// Apply adhesion for attached particles
	if (AdhesionSolver.IsValid() && Preset->AdhesionStrength > 0.0f)
	{
		// Only apply to attached particles
		for (int32 Idx : AttachedIndices)
		{
			FFluidParticle& Particle = Particles[Idx];

			// Apply sliding gravity (tangent component)
			const FVector& Normal = Particle.AttachedSurfaceNormal;
			float NormalComponent = FVector::DotProduct(Preset->Gravity, Normal);
			FVector TangentGravity = Preset->Gravity - NormalComponent * Normal;
			Particle.Velocity += TangentGravity * SubstepDT;

			// Apply velocity damping for attached particles (they move slower)
			Particle.Velocity *= 0.95f;

			// Update predicted position
			Particle.PredictedPosition = Particle.Position + Particle.Velocity * SubstepDT;
		}
	}

	// CPU handles world collision for attached particles
	// (Per-polygon collision will be added in Phase 2)
}

void UKawaiiFluidSimulationContext::SimulateGPU(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float DeltaTime,
	float& AccumulatedTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_SimulateGPU);

	if (!Preset || Particles.Num() == 0)
	{
		return;
	}

	// Ensure GPU simulator is ready
	if (!IsGPUSimulatorReady())
	{
		InitializeGPUSimulator(Preset->MaxParticles);
		if (!IsGPUSimulatorReady())
		{
			// Fall back to CPU simulation with modified params to avoid recursion
			UE_LOG(LogTemp, Warning, TEXT("GPU Simulator not ready, falling back to CPU simulation"));
			//FKawaiiFluidSimulationParams CPUParams = Params;
			//CPUParams.bUseGPUSimulation = false;
			//Simulate(Particles, Preset, CPUParams, SpatialHash, DeltaTime, AccumulatedTime);
			return;
		}
	}

	EnsureSolversInitialized(Preset);

	// =====================================================
	// Phase 2: No CPU readback - GPU buffer is source of truth
	// Rendering should read directly from GPU buffer
	//
	// OLD (Phase 1): Download → Upload → Simulate → Readback
	// NEW (Phase 2): Upload → Simulate (GPU keeps results)
	// =====================================================

	// Phase 2: Skip readback to avoid CPU-GPU sync stall
	// Particles array on CPU may be outdated, but GPU has correct data
	// Renderer should use GPU buffer directly via DataProvider::IsGPUSimulationActive()

	// TODO: Only readback occasionally if CPU needs to know positions
	// (e.g., for spawning near existing particles)
	/*
	if (GPUSimulator->GetParticleCount() > 0)
	{
		GPUSimulator->DownloadParticles(Particles);
	}
	*/

	// Cache collider shapes once per frame (required for IsCacheValid() to return true)
	CacheColliderShapes(Params.Colliders);

	// Collect and upload collision primitives to GPU
	{
		FGPUCollisionPrimitives CollisionPrimitives;
		const float DefaultFriction = Preset->Friction;
		const float DefaultRestitution = Preset->Restitution;

		// Build set of actors using Per-Polygon collision (to skip their primitive colliders)
		TSet<AActor*> PerPolygonActors;
		for (UFluidInteractionComponent* Interaction : Params.InteractionComponents)
		{
			if (Interaction && Interaction->IsPerPolygonCollisionEnabled())
			{
				if (AActor* Owner = Interaction->GetOwner())
				{
					PerPolygonActors.Add(Owner);
				}
			}
		}

		for (UFluidCollider* Collider : Params.Colliders)
		{
			if (!Collider || !Collider->IsColliderEnabled())
			{
				continue;
			}

			// Skip colliders on actors that use Per-Polygon collision
			AActor* ColliderOwner = Collider->GetOwner();
			if (ColliderOwner && PerPolygonActors.Contains(ColliderOwner))
			{
				continue;
			}

			// Check if this is a MeshFluidCollider (has ExportToGPUPrimitives)
			UMeshFluidCollider* MeshCollider = Cast<UMeshFluidCollider>(Collider);
			if (MeshCollider && MeshCollider->IsCacheValid())
			{
				MeshCollider->ExportToGPUPrimitives(
					CollisionPrimitives.Spheres,
					CollisionPrimitives.Capsules,
					CollisionPrimitives.Boxes,
					CollisionPrimitives.Convexes,
					CollisionPrimitives.ConvexPlanes,
					DefaultFriction,
					DefaultRestitution
				);
			}
		}

		// Upload to GPU
		if (!CollisionPrimitives.IsEmpty())
		{
			GPUSimulator->UploadCollisionPrimitives(CollisionPrimitives);
		}
	}

	// Update attached particle positions (bone tracking - before physics)
	UpdateAttachedParticlePositions(Particles, Params.InteractionComponents);

	// Step 2: Build GPU simulation parameters for this frame
	// Use fixed DeltaTime for stability (GPU handles its own substeps internally)
	const float SubstepDT = Preset->SubstepDeltaTime;
	FGPUFluidSimulationParams GPUParams = BuildGPUSimParams(Preset, Params, SubstepDT);
	GPUParams.ParticleCount = Particles.Num();

	// =====================================================
	// Phase 3: GPU-based particle spawning (eliminates race condition)
	// - New particles: Use AddSpawnRequests (GPU creates particles atomically)
	// - Particle removal: Use UploadParticles (fallback - rare case)
	// - Same count: No action needed (GPU buffer persists)
	// =====================================================
	const int32 CurrentCPUCount = Particles.Num();
	const int32 CurrentGPUCount = GPUSimulator->GetParticleCount();

	if (CurrentCPUCount > CurrentGPUCount)
	{
		// New particles spawned - use GPU spawn system (no race condition!)
		const int32 NewParticleCount = CurrentCPUCount - CurrentGPUCount;
		TArray<FGPUSpawnRequest> SpawnRequests;
		SpawnRequests.Reserve(NewParticleCount);

		for (int32 i = CurrentGPUCount; i < CurrentCPUCount; ++i)
		{
			const FFluidParticle& Particle = Particles[i];
			FGPUSpawnRequest Request;
			Request.Position = FVector3f(Particle.Position);
			Request.Velocity = FVector3f(Particle.Velocity);
			Request.Mass = Particle.Mass;
			Request.Radius = Preset->ParticleRadius;
			SpawnRequests.Add(Request);
		}

		// Debug: Log spawn positions
		if (SpawnRequests.Num() > 0)
		{
			const FGPUSpawnRequest& FirstReq = SpawnRequests[0];
			UE_LOG(LogTemp, Warning, TEXT("GPU Spawn: First particle position = (%.1f, %.1f, %.1f)"),
				FirstReq.Position.X, FirstReq.Position.Y, FirstReq.Position.Z);
		}

		GPUSimulator->AddSpawnRequests(SpawnRequests);

		UE_LOG(LogTemp, Log, TEXT("GPU Spawn: Adding %d new particles via GPU spawn system (total: %d -> %d)"),
			NewParticleCount, CurrentGPUCount, CurrentCPUCount);
	}
	else if (CurrentCPUCount < CurrentGPUCount)
	{
		// Particles removed - fallback to full upload (rare case)
		// TODO: Implement GPU-based particle removal
		GPUSimulator->UploadParticles(Particles);

		UE_LOG(LogTemp, Warning, TEXT("GPU Upload: Particle count reduced %d -> %d (using fallback upload)"),
			CurrentGPUCount, CurrentCPUCount);
	}
	// else: Particle count same - GPU buffer already has simulation results, no upload needed

	// Step 4: Run GPU simulation (async - results available next frame)
	GPUSimulator->SimulateSubstep(GPUParams);

	// =====================================================
	// Phase 2: AABB Filtering for Per-Polygon Collision
	// Filter particles inside Per-Polygon Collision enabled AABBs
	// =====================================================
	{
		TArray<FGPUFilterAABB> FilterAABBs;

		for (int32 i = 0; i < Params.InteractionComponents.Num(); ++i)
		{
			UFluidInteractionComponent* Interaction = Params.InteractionComponents[i];
			if (Interaction && Interaction->IsPerPolygonCollisionEnabled())
			{
				FBox AABB = Interaction->GetPerPolygonFilterAABB();
				if (AABB.IsValid)
				{
					FGPUFilterAABB FilterAABB;
					FilterAABB.Min = FVector3f(AABB.Min);
					FilterAABB.Max = FVector3f(AABB.Max);
					FilterAABB.InteractionIndex = i;
					FilterAABBs.Add(FilterAABB);
				}
			}
		}

		if (FilterAABBs.Num() > 0)
		{
			// Debug: Log each AABB coordinates (only occasionally to reduce spam)
			static int32 DebugFrameCounter = 0;
			if (++DebugFrameCounter % 60 == 0)  // Every 60 frames
			{
				for (int32 j = 0; j < FilterAABBs.Num(); ++j)
				{
					const FGPUFilterAABB& FAABB = FilterAABBs[j];
					UE_LOG(LogTemp, Log, TEXT("AABB[%d]: Min=(%.1f, %.1f, %.1f) Max=(%.1f, %.1f, %.1f)"),
						j,
						FAABB.Min.X, FAABB.Min.Y, FAABB.Min.Z,
						FAABB.Max.X, FAABB.Max.Y, FAABB.Max.Z);
				}
			}

			GPUSimulator->ExecuteAABBFiltering(FilterAABBs);

			// Note: Results are available on NEXT frame due to async GPU execution
			// The actual count is logged by GPUFluidSimulator itself after GPU completes
		}

		// =====================================================
		// Phase 2.5: Per-Polygon Collision Processing
		// CPU processes filtered candidates against skeletal mesh triangles
		// Results are applied back to GPU via ApplyCorrections
		// =====================================================

		// DEBUG: Check Per-Polygon status
		static int32 PerPolygonDebugCounter = 0;
		const bool bDebugLog = (++PerPolygonDebugCounter % 60 == 0);

		// Collect Per-Polygon enabled interaction components FIRST
		TArray<UFluidInteractionComponent*> PerPolygonInteractions;
		for (UFluidInteractionComponent* Interaction : Params.InteractionComponents)
		{
			if (Interaction && Interaction->IsPerPolygonCollisionEnabled())
			{
				PerPolygonInteractions.Add(Interaction);
			}
		}

		if (bDebugLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("Per-Polygon DEBUG: InteractionComponents=%d, PerPolygonEnabled=%d, HasFilteredCandidates=%d"),
				Params.InteractionComponents.Num(),
				PerPolygonInteractions.Num(),
				GPUSimulator->HasFilteredCandidates() ? 1 : 0);
		}

		if (PerPolygonInteractions.Num() > 0)
		{
			// Initialize Per-Polygon processor if needed
			if (!PerPolygonProcessor)
			{
				PerPolygonProcessor = MakeUnique<FPerPolygonCollisionProcessor>();
				PerPolygonProcessor->SetCollisionMargin(1.0f);
				PerPolygonProcessor->SetFriction(Preset->Friction);
				PerPolygonProcessor->SetRestitution(Preset->Restitution);
				UE_LOG(LogTemp, Warning, TEXT("Per-Polygon: Processor initialized"));
			}

			// Update BVH cache (skinned mesh vertex positions)
			PerPolygonProcessor->UpdateBVHCache(PerPolygonInteractions);

			if (bDebugLog)
			{
				UE_LOG(LogTemp, Warning, TEXT("Per-Polygon DEBUG: BVH updated, time=%.2fms"),
					PerPolygonProcessor->GetLastBVHUpdateTimeMs());
			}

			// Check if we have filtered candidates from PREVIOUS frame
			if (GPUSimulator->HasFilteredCandidates())
			{
				// Get filtered candidates from GPU (this may block for readback)
				TArray<FGPUCandidateParticle> Candidates;
				if (GPUSimulator->GetFilteredCandidates(Candidates))
				{
					if (bDebugLog)
					{
						UE_LOG(LogTemp, Warning, TEXT("Per-Polygon DEBUG: Got %d candidates from GPU"), Candidates.Num());
					}

					if (Candidates.Num() > 0)
					{
						// Process collisions on CPU (parallel)
						// NOTE: Use original InteractionComponents array, not PerPolygonInteractions
						// because Candidate.InteractionIndex is based on original array indices
						TArray<FParticleCorrection> Corrections;
						PerPolygonProcessor->ProcessCollisions(
							Candidates,
							Params.InteractionComponents,
							Preset->ParticleRadius,
							Corrections
						);

						if (bDebugLog)
						{
							UE_LOG(LogTemp, Warning, TEXT("Per-Polygon DEBUG: Processed=%d, Collisions=%d, Corrections=%d"),
								PerPolygonProcessor->GetLastProcessedCount(),
								PerPolygonProcessor->GetLastCollisionCount(),
								Corrections.Num());
						}

						// Apply corrections to GPU particles
						if (Corrections.Num() > 0)
						{
							GPUSimulator->ApplyCorrections(Corrections);
							UE_LOG(LogTemp, Warning, TEXT("Per-Polygon: Applied %d corrections to GPU"), Corrections.Num());

							// DEBUG: Log first correction details
							if (Corrections.Num() > 0)
							{
								const FParticleCorrection& First = Corrections[0];
								UE_LOG(LogTemp, Warning, TEXT("  First correction: ParticleIdx=%d, Delta=(%.2f,%.2f,%.2f)"),
									First.ParticleIndex,
									First.PositionDelta.X, First.PositionDelta.Y, First.PositionDelta.Z);
							}
						}
					}
				}
			}
		}
	}

	// Note: Attached particles, adhesion, cohesion are handled on CPU
	// but with 1-frame delay, these would need special handling
	// For now, skip CPU-only effects in GPU mode
}

void UKawaiiFluidSimulationContext::Simulate(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float DeltaTime,
	float& AccumulatedTime)
{
	SCOPE_CYCLE_COUNTER(STAT_ContextSimulate);
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_Simulate);

	if (!Preset || Particles.Num() == 0)
	{
		return;
	}

	// Dispatch to GPU or CPU simulation based on Params (from Component)
	if (Params.bUseGPUSimulation)
	{
		SimulateGPU(Particles, Preset, Params, SpatialHash, DeltaTime, AccumulatedTime);
		return;
	}

	EnsureSolversInitialized(Preset);

	// Accumulator method: simulate with fixed dt
	constexpr int32 MaxSubstepsPerFrame = 4;
	const float MaxAllowedTime = Preset->SubstepDeltaTime * FMath::Min(Preset->MaxSubsteps, MaxSubstepsPerFrame);
	AccumulatedTime += FMath::Min(DeltaTime, MaxAllowedTime);

	// Cache collider shapes once per frame
	CacheColliderShapes(Params.Colliders);

	// Update attached particle positions (bone tracking - before physics)
	UpdateAttachedParticlePositions(Particles, Params.InteractionComponents);

	// Substep loop (hard limit: 4 substeps per frame)
	int32 SubstepCount = 0;
	while (AccumulatedTime >= Preset->SubstepDeltaTime && SubstepCount < MaxSubstepsPerFrame)
	{
		SimulateSubstep(Particles, Preset, Params, SpatialHash, Preset->SubstepDeltaTime);
		AccumulatedTime -= Preset->SubstepDeltaTime;
		++SubstepCount;
	}
}

void UKawaiiFluidSimulationContext::SimulateSubstep(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float SubstepDT)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_SimulateSubstep);

	// 1. Predict positions
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextPredictPositions);
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_PredictPositions);
		PredictPositions(Particles, Preset, Params.ExternalForce, SubstepDT);
	}

	// 2. Update neighbors
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextUpdateNeighbors);
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_UpdateNeighbors);
		UpdateNeighbors(Particles, SpatialHash, Preset->SmoothingRadius);
	}

	// 3. Solve density constraints
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextSolveDensity);
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_SolveDensity);

		// Determine if we need to store original positions for core particle reduction
		const bool bHasCoreReduction = Params.CoreDensityConstraintReduction > 0.0f;

		// Store original predicted positions (before density constraint)
		TArray<FVector> OriginalPositions;
		if (bHasCoreReduction)
		{
			OriginalPositions.SetNum(Particles.Num());
			for (int32 i = 0; i < Particles.Num(); ++i)
			{
				OriginalPositions[i] = Particles[i].PredictedPosition;
			}
		}

		SolveDensityConstraints(Particles, Preset, SubstepDT);

		// Apply density constraint reduction for core particles
		if (bHasCoreReduction)
		{
			ParallelFor(Particles.Num(), [&](int32 i)
			{
				FFluidParticle& P = Particles[i];

				// Core particles have reduced density constraint effect
				if (P.bIsCoreParticle)
				{
					// Blend between density-corrected position and original position
					// Higher reduction = closer to original position (less density constraint effect)
					P.PredictedPosition = FMath::Lerp(
						P.PredictedPosition,
						OriginalPositions[i],
						Params.CoreDensityConstraintReduction
					);
				}
			});
		}
	}

	// 3.5. Apply shape matching (for slime - after density, before collision)
	if (Params.bEnableShapeMatching)
	{
		ApplyShapeMatchingConstraint(Particles, Params);
	}

	// 4. Handle collisions
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextHandleCollisions);
		HandleCollisions(Particles, Params.Colliders);
	}

	// 5. World collision
	if (Params.bUseWorldCollision && Params.World)
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextWorldCollision);
		HandleWorldCollision(Particles, Params, SpatialHash, Params.ParticleRadius);
	}

	// 6. Finalize positions
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextFinalizePositions);
		FinalizePositions(Particles, SubstepDT);
	}

	// 7. Apply viscosity
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextApplyViscosity);
		ApplyViscosity(Particles, Preset);
	}

	// 8. Apply adhesion
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextApplyAdhesion);
		ApplyAdhesion(Particles, Preset, Params.Colliders);
	}

	// 9. Apply cohesion (surface tension between particles)
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextApplyCohesion);
		ApplyCohesion(Particles, Preset);
	}
}

void UKawaiiFluidSimulationContext::PredictPositions(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FVector& ExternalForce,
	float DeltaTime)
{
	const FVector TotalForce = Preset->Gravity + ExternalForce;

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		FFluidParticle& Particle = Particles[i];

		FVector AppliedForce = TotalForce;

		// Attached particles: apply only tangent gravity (sliding effect)
		if (Particle.bIsAttached)
		{
			const FVector& Normal = Particle.AttachedSurfaceNormal;
			float NormalComponent = FVector::DotProduct(Preset->Gravity, Normal);
			FVector TangentGravity = Preset->Gravity - NormalComponent * Normal;
			AppliedForce = TangentGravity + ExternalForce;
		}

		Particle.Velocity += AppliedForce * DeltaTime;
		Particle.PredictedPosition = Particle.Position + Particle.Velocity * DeltaTime;
	});
}

void UKawaiiFluidSimulationContext::UpdateNeighbors(
	TArray<FFluidParticle>& Particles,
	FSpatialHash& SpatialHash,
	float SmoothingRadius)
{
	// Rebuild spatial hash (sequential - hashmap write)
	TArray<FVector> Positions;
	Positions.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Positions.Add(Particle.PredictedPosition);
	}

	SpatialHash.BuildFromPositions(Positions);

	// Cache neighbors for each particle (parallel - read only)
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		SpatialHash.GetNeighbors(
			Particles[i].PredictedPosition,
			SmoothingRadius,
			Particles[i].NeighborIndices
		);
	});
}

void UKawaiiFluidSimulationContext::SolveDensityConstraints(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	float DeltaTime)
{
	if (DensityConstraint.IsValid())
	{
		DensityConstraint->Solve(
			Particles,
			Preset->SmoothingRadius,
			Preset->RestDensity,
			Preset->Compliance,
			DeltaTime
		);
	}
}

void UKawaiiFluidSimulationContext::CacheColliderShapes(const TArray<UFluidCollider*>& Colliders)
{
	for (UFluidCollider* Collider : Colliders)
	{
		if (Collider && Collider->IsColliderEnabled())
		{
			Collider->CacheCollisionShapes();
		}
	}
}

void UKawaiiFluidSimulationContext::HandleCollisions(
	TArray<FFluidParticle>& Particles,
	const TArray<UFluidCollider*>& Colliders)
{
	for (UFluidCollider* Collider : Colliders)
	{
		if (Collider && Collider->IsColliderEnabled())
		{
			Collider->ResolveCollisions(Particles);
		}
	}
}

void UKawaiiFluidSimulationContext::HandleWorldCollision(
	TArray<FFluidParticle>& Particles,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float ParticleRadius)
{
	UWorld* World = Params.World;
	if (!World || Particles.Num() == 0)
	{
		return;
	}

	const float CellSize = SpatialHash.GetCellSize();
	const auto& Grid = SpatialHash.GetGrid();

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;
	if (Params.IgnoreActor.IsValid())
	{
		QueryParams.AddIgnoredActor(Params.IgnoreActor.Get());
	}

	// Cell-based broad-phase
	struct FCellQueryData
	{
		FVector CellCenter;
		FVector CellExtent;
		TArray<int32> ParticleIndices;
	};

	TArray<FCellQueryData> CellQueries;
	CellQueries.Reserve(Grid.Num());

	for (const auto& Pair : Grid)
	{
		FCellQueryData CellData;
		CellData.CellCenter = FVector(Pair.Key) * CellSize + FVector(CellSize * 0.5f);
		CellData.CellExtent = FVector(CellSize * 0.5f);
		CellData.ParticleIndices = Pair.Value;
		CellQueries.Add(MoveTemp(CellData));
	}

	// Cell overlap check - parallel
	TArray<uint8> CellCollisionResults;
	CellCollisionResults.SetNumZeroed(CellQueries.Num());

	ParallelFor(CellQueries.Num(), [&](int32 CellIdx)
	{
		const FCellQueryData& CellData = CellQueries[CellIdx];
		if (World->OverlapBlockingTestByChannel(
			CellData.CellCenter, FQuat::Identity, Params.CollisionChannel,
			FCollisionShape::MakeBox(CellData.CellExtent), QueryParams))
		{
			CellCollisionResults[CellIdx] = 1;
		}
	});

	// Collect collision candidates
	TArray<int32> CollisionParticleIndices;
	CollisionParticleIndices.Reserve(Particles.Num());

	for (int32 CellIdx = 0; CellIdx < CellQueries.Num(); ++CellIdx)
	{
		if (CellCollisionResults[CellIdx])
		{
			CollisionParticleIndices.Append(CellQueries[CellIdx].ParticleIndices);
		}
	}

	if (CollisionParticleIndices.Num() == 0)
	{
		return;
	}

	// Physics scene read lock
	FPhysScene* PhysScene = World->GetPhysicsScene();
	if (!PhysScene)
	{
		return;
	}

	FPhysicsCommand::ExecuteRead(PhysScene, [&]()
	{
		ParallelFor(CollisionParticleIndices.Num(), [&](int32 j)
		{
			const int32 i = CollisionParticleIndices[j];
			FFluidParticle& Particle = Particles[i];

			FCollisionQueryParams LocalParams;
			LocalParams.bTraceComplex = false;
			LocalParams.bReturnPhysicalMaterial = false;
			if (Params.IgnoreActor.IsValid())
			{
				LocalParams.AddIgnoredActor(Params.IgnoreActor.Get());
			}

			FHitResult HitResult;
			bool bHit = World->SweepSingleByChannel(
				HitResult,
				Particle.Position,
				Particle.PredictedPosition,
				FQuat::Identity,
				Params.CollisionChannel,
				FCollisionShape::MakeSphere(ParticleRadius),
				LocalParams
			);

			if (bHit && HitResult.bBlockingHit)
			{
				FVector CollisionPos = HitResult.Location + HitResult.ImpactNormal * 0.01f;

				Particle.PredictedPosition = CollisionPos;
				Particle.Position = CollisionPos;

				float VelDotNormal = FVector::DotProduct(Particle.Velocity, HitResult.ImpactNormal);
				if (VelDotNormal < 0.0f)
				{
					Particle.Velocity -= VelDotNormal * HitResult.ImpactNormal;
				}

				// Fire collision event if enabled
				if (Params.bEnableCollisionEvents && Params.OnCollisionEvent.IsBound() && Params.EventCountPtr)
				{
					const float Speed = Particle.Velocity.Size();
					const int32 CurrentEventCount = Params.EventCountPtr->load(std::memory_order_relaxed);
					if (Speed >= Params.MinVelocityForEvent &&
					    CurrentEventCount < Params.MaxEventsPerFrame)
					{
						// Check cooldown (read-only during ParallelFor - safe since writes happen on game thread)
						bool bCanEmitEvent = true;
						if (Params.EventCooldownPerParticle > 0.0f && Params.ParticleLastEventTimePtr)
						{
							const float* LastEventTime = Params.ParticleLastEventTimePtr->Find(Particle.ParticleID);
							if (LastEventTime && (Params.CurrentGameTime - *LastEventTime) < Params.EventCooldownPerParticle)
							{
								bCanEmitEvent = false;
							}
						}

						if (bCanEmitEvent)
						{
							FKawaiiFluidCollisionEvent Event(
								Particle.ParticleID,
								HitResult.GetActor(),
								HitResult.Location,
								HitResult.ImpactNormal,
								Speed
							);

							// Execute on game thread with safety checks
							TWeakObjectPtr<UWorld> WeakWorld(Params.World);
							FOnFluidCollisionEvent Callback = Params.OnCollisionEvent;
							TMap<int32, float>* CooldownMapPtr = Params.ParticleLastEventTimePtr;
							const int32 ParticleID = Particle.ParticleID;
							const float CooldownValue = Params.EventCooldownPerParticle;
							AsyncTask(ENamedThreads::GameThread, [WeakWorld, Callback, Event, CooldownMapPtr, ParticleID, CooldownValue]()
							{
								if (!WeakWorld.IsValid())
								{
									return;
								}
								if (Callback.IsBound())
								{
									Callback.Execute(Event);
								}
								// Update cooldown map on game thread (safe - single thread write)
								if (CooldownMapPtr && CooldownValue > 0.0f)
								{
									if (UWorld* World = WeakWorld.Get())
									{
										CooldownMapPtr->Add(ParticleID, World->GetTimeSeconds());
									}
								}
							});

							Params.EventCountPtr->fetch_add(1, std::memory_order_relaxed);
						}
					}
				}

				// Detach from character if hitting different surface
				if (Particle.bIsAttached)
				{
					AActor* HitActor = HitResult.GetActor();
					if (HitActor != Particle.AttachedActor.Get())
					{
						Particle.bIsAttached = false;
						Particle.AttachedActor.Reset();
						Particle.AttachedBoneName = NAME_None;
						Particle.AttachedLocalOffset = FVector::ZeroVector;
						Particle.AttachedSurfaceNormal = FVector::UpVector;
					}
				}
			}
			else if (Particle.bIsAttached)
			{
				// Floor detection for attached particles
				const float FloorCheckDistance = 3.0f;
				FHitResult FloorHit;
				bool bNearFloor = World->LineTraceSingleByChannel(
					FloorHit,
					Particle.Position,
					Particle.Position - FVector(0, 0, FloorCheckDistance),
					Params.CollisionChannel,
					LocalParams
				);

				if (bNearFloor && FloorHit.GetActor() != Particle.AttachedActor.Get())
				{
					Particle.bIsAttached = false;
					Particle.AttachedActor.Reset();
					Particle.AttachedBoneName = NAME_None;
					Particle.AttachedLocalOffset = FVector::ZeroVector;
					Particle.AttachedSurfaceNormal = FVector::UpVector;
				}
			}
		});
	});

	// Floor detachment check
	const float FloorDetachDistance = 5.0f;
	const float FloorNearDistance = 20.0f;

	for (FFluidParticle& Particle : Particles)
	{
		if (!Particle.bIsAttached)
		{
			Particle.bNearGround = false;
			continue;
		}

		FCollisionQueryParams FloorQueryParams;
		FloorQueryParams.bTraceComplex = false;
		if (Params.IgnoreActor.IsValid())
		{
			FloorQueryParams.AddIgnoredActor(Params.IgnoreActor.Get());
		}
		if (Particle.AttachedActor.IsValid())
		{
			FloorQueryParams.AddIgnoredActor(Particle.AttachedActor.Get());
		}

		FHitResult FloorHit;
		bool bNearFloor = World->LineTraceSingleByChannel(
			FloorHit,
			Particle.Position,
			Particle.Position - FVector(0, 0, FloorNearDistance),
			Params.CollisionChannel,
			FloorQueryParams
		);

		Particle.bNearGround = bNearFloor;

		if (bNearFloor && FloorHit.Distance <= FloorDetachDistance)
		{
			Particle.bIsAttached = false;
			Particle.AttachedActor.Reset();
			Particle.AttachedBoneName = NAME_None;
			Particle.AttachedLocalOffset = FVector::ZeroVector;
			Particle.AttachedSurfaceNormal = FVector::UpVector;
			Particle.bJustDetached = true;
		}
	}
}

void UKawaiiFluidSimulationContext::FinalizePositions(
	TArray<FFluidParticle>& Particles,
	float DeltaTime)
{
	const float InvDeltaTime = 1.0f / DeltaTime;

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		FFluidParticle& Particle = Particles[i];
		Particle.Velocity = (Particle.PredictedPosition - Particle.Position) * InvDeltaTime;
		Particle.Position = Particle.PredictedPosition;
	});
}

void UKawaiiFluidSimulationContext::ApplyViscosity(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset)
{
	if (ViscositySolver.IsValid() && Preset->ViscosityCoefficient > 0.0f)
	{
		ViscositySolver->ApplyXSPH(Particles, Preset->ViscosityCoefficient, Preset->SmoothingRadius);
	}
}

void UKawaiiFluidSimulationContext::ApplyAdhesion(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const TArray<UFluidCollider*>& Colliders)
{
	if (AdhesionSolver.IsValid() && Preset->AdhesionStrength > 0.0f)
	{
		AdhesionSolver->Apply(
			Particles,
			Colliders,
			Preset->AdhesionStrength,
			Preset->AdhesionRadius,
			Preset->DetachThreshold
		);
	}
}

void UKawaiiFluidSimulationContext::ApplyCohesion(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset)
{
	if (AdhesionSolver.IsValid() && Preset->CohesionStrength > 0.0f)
	{
		AdhesionSolver->ApplyCohesion(
			Particles,
			Preset->CohesionStrength,
			Preset->SmoothingRadius
		);
	}
}

void UKawaiiFluidSimulationContext::ApplyShapeMatchingConstraint(
	TArray<FFluidParticle>& Particles,
	const FKawaiiFluidSimulationParams& Params)
{
	if (Particles.Num() < 2)
	{
		return;
	}

	// DEBUG: Log first few frames
	static int32 DebugFrameCount = 0;
	bool bDebugLog = (DebugFrameCount++ < 5);
	if (bDebugLog)
	{
		int32 ValidRestOffsetCount = 0;
		for (const FFluidParticle& P : Particles)
		{
			if (!P.RestOffset.IsNearlyZero())
			{
				ValidRestOffsetCount++;
			}
		}
		UE_LOG(LogTemp, Warning, TEXT("ShapeMatching: Particles=%d, ValidRestOffsets=%d, Stiffness=%.2f"),
			Particles.Num(), ValidRestOffsetCount, Params.ShapeMatchingStiffness);
	}

	// Group particles by cluster
	TMap<int32, TArray<int32>> ClusterParticleMap;
	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		ClusterParticleMap.FindOrAdd(Particles[i].ClusterID).Add(i);
	}

	// Apply shape matching per cluster
	for (auto& Pair : ClusterParticleMap)
	{
		const TArray<int32>& Indices = Pair.Value;

		if (Indices.Num() < 2)
		{
			continue;
		}

		// Compute current center of mass (using PredictedPosition)
		FVector xcm = FVector::ZeroVector;
		float TotalMass = 0.0f;

		for (int32 Idx : Indices)
		{
			const FFluidParticle& P = Particles[Idx];
			xcm += P.PredictedPosition * P.Mass;
			TotalMass += P.Mass;
		}

		if (TotalMass < KINDA_SMALL_NUMBER)
		{
			continue;
		}

		xcm /= TotalMass;

		// Apply shape matching constraint via Velocity
		for (int32 Idx : Indices)
		{
			FFluidParticle& P = Particles[Idx];

			if (P.RestOffset.IsNearlyZero())
			{
				continue;
			}

			// Goal position = current center + rest offset (no rotation for now)
			FVector GoalPosition = xcm + P.RestOffset;

			// Compute correction direction and magnitude
			FVector Correction = GoalPosition - P.PredictedPosition;

			// Apply stiffness (core particles get stronger correction)
			float EffectiveStiffness = Params.ShapeMatchingStiffness;
			if (P.bIsCoreParticle)
			{
				EffectiveStiffness *= Params.ShapeMatchingCoreMultiplier;
			}
			EffectiveStiffness = FMath::Clamp(EffectiveStiffness, 0.0f, 1.0f);

			// Apply correction to PredictedPosition (proper PBF approach)
			// FinalizePositions will derive Velocity from position change
			P.PredictedPosition += Correction * EffectiveStiffness;
		}
	}
}

void UKawaiiFluidSimulationContext::UpdateAttachedParticlePositions(
	TArray<FFluidParticle>& Particles,
	const TArray<UFluidInteractionComponent*>& InteractionComponents)
{
	if (InteractionComponents.Num() == 0 || Particles.Num() == 0)
	{
		return;
	}

	// Group particles by owner (O(P) single traversal)
	TMap<AActor*, TArray<int32>> OwnerToParticleIndices;

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		const FFluidParticle& Particle = Particles[i];
		if (Particle.bIsAttached && Particle.AttachedActor.IsValid() && Particle.AttachedBoneName != NAME_None)
		{
			OwnerToParticleIndices.FindOrAdd(Particle.AttachedActor.Get()).Add(i);
		}
	}

	// Process per InteractionComponent
	for (UFluidInteractionComponent* Interaction : InteractionComponents)
	{
		if (!Interaction)
		{
			continue;
		}

		AActor* InteractionOwner = Interaction->GetOwner();
		if (!InteractionOwner)
		{
			continue;
		}

		TArray<int32>* ParticleIndicesPtr = OwnerToParticleIndices.Find(InteractionOwner);
		if (!ParticleIndicesPtr || ParticleIndicesPtr->Num() == 0)
		{
			continue;
		}

		USkeletalMeshComponent* SkelMesh = InteractionOwner->FindComponentByClass<USkeletalMeshComponent>();
		if (!SkelMesh)
		{
			continue;
		}

		// Group by bone (optimization: minimize GetBoneTransform calls)
		TMap<FName, TArray<int32>> BoneToParticleIndices;
		for (int32 ParticleIdx : *ParticleIndicesPtr)
		{
			const FFluidParticle& Particle = Particles[ParticleIdx];
			BoneToParticleIndices.FindOrAdd(Particle.AttachedBoneName).Add(ParticleIdx);
		}

		// Update particle positions per bone
		for (auto& BonePair : BoneToParticleIndices)
		{
			const FName& BoneName = BonePair.Key;
			const TArray<int32>& BoneParticleIndices = BonePair.Value;

			int32 BoneIndex = SkelMesh->GetBoneIndex(BoneName);
			if (BoneIndex == INDEX_NONE)
			{
				continue;
			}

			FTransform CurrentBoneTransform = SkelMesh->GetBoneTransform(BoneIndex);

			for (int32 ParticleIdx : BoneParticleIndices)
			{
				FFluidParticle& Particle = Particles[ParticleIdx];
				FVector OldWorldPosition = CurrentBoneTransform.TransformPosition(Particle.AttachedLocalOffset);
				FVector BoneDelta = OldWorldPosition - Particle.Position;
				Particle.Position += BoneDelta;
			}
		}
	}
}
