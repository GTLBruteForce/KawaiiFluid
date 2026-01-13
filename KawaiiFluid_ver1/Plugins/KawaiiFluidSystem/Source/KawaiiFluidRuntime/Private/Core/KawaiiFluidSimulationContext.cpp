// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/SpatialHash.h"
#include "Core/KawaiiFluidSimulationStats.h"
#include "Components/KawaiiFluidSimulationVolumeComponent.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Physics/DensityConstraint.h"
#include "Physics/ViscositySolver.h"
#include "Physics/AdhesionSolver.h"
#include "Physics/StackPressureSolver.h"
#include "Collision/FluidCollider.h"
#include "Collision/MeshFluidCollider.h"
#include "Collision/PerPolygonCollisionProcessor.h"
#include "Components/FluidInteractionComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Async/Async.h"
#include "RenderingThread.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidParticle.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"

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

//========================================
// Auto-Scaling for SmoothingRadius Independence
// SPH stability depends on h (smoothing radius). When h changes, several parameters
// need to scale accordingly to maintain consistent behavior.
//========================================
namespace SPHScaling
{
	// Reference radius where the preset's parameters are tuned (cm)
	constexpr float ReferenceRadius = 20.0f;

	/**
	 * Calculate scaled Compliance for a given SmoothingRadius
	 * Kernel gradients scale as 1/h^5, so sumGradC2 scales as 1/h^10
	 * @param BaseCompliance - The Compliance value tuned for ReferenceRadius
	 * @param SmoothingRadius - Current smoothing radius (cm)
	 * @param Exponent - Scaling exponent (0 = no scaling, 4-6 typical, 10 theoretical)
	 * @return Scaled Compliance value
	 */
	FORCEINLINE float GetScaledCompliance(float BaseCompliance, float SmoothingRadius, float Exponent)
	{
		if (Exponent <= 0.0f)
		{
			return BaseCompliance;  // No scaling
		}
		const float Ratio = ReferenceRadius / FMath::Max(SmoothingRadius, 1.0f);
		return BaseCompliance * FMath::Pow(Ratio, Exponent);
	}
}

UKawaiiFluidSimulationContext::UKawaiiFluidSimulationContext()
{
}

UKawaiiFluidSimulationContext::~UKawaiiFluidSimulationContext()
{
	// Explicitly reset to ensure complete type is available for destruction
	PerPolygonProcessor.Reset();
	ReleaseRenderResource();
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
		SPHScaling::GetScaledCompliance(Preset->Compliance, Preset->SmoothingRadius, Preset->ComplianceScalingExponent)
	);
	ViscositySolver = MakeShared<FViscositySolver>();
	AdhesionSolver = MakeShared<FAdhesionSolver>();
	StackPressureSolver = MakeShared<FStackPressureSolver>();

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

//=============================================================================
// Render Resource Methods (배치 렌더링용)
//=============================================================================

void UKawaiiFluidSimulationContext::InitializeRenderResource()
{
	if (RenderResource.IsValid())
	{
		return; // Already initialized
	}

	RenderResource = MakeShared<FKawaiiFluidRenderResource>();

	// Initialize on render thread
	ENQUEUE_RENDER_COMMAND(InitContextRenderResource)(
		[RenderResourcePtr = RenderResource.Get()](FRHICommandListImmediate& RHICmdList)
		{
			RenderResourcePtr->InitResource(RHICmdList);
		}
	);

	UE_LOG(LogTemp, Log, TEXT("SimulationContext: RenderResource initialized"));
}

void UKawaiiFluidSimulationContext::ReleaseRenderResource()
{
	if (RenderResource.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(ReleaseContextRenderResource)(
			[RenderResource = MoveTemp(RenderResource)](FRHICommandListImmediate& RHICmdList) mutable
			{
				if (RenderResource.IsValid())
				{
					RenderResource->ReleaseResource();
					RenderResource.Reset();
				}
			}
		);
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
	// Scale Compliance for SmoothingRadius-independent stability
	GPUParams.Compliance = SPHScaling::GetScaledCompliance(
		Preset->Compliance, Preset->SmoothingRadius, Preset->ComplianceScalingExponent);
	GPUParams.ParticleRadius = Preset->ParticleRadius;
	GPUParams.ViscosityCoefficient = Preset->ViscosityCoefficient;
	GPUParams.CohesionStrength = Preset->CohesionStrength;
	GPUParams.GlobalDamping = Preset->GlobalDamping;

	// Stack Pressure (weight transfer from stacked attached particles)
	GPUParams.StackPressureScale = Preset->bEnableStackPressure ? Preset->StackPressureScale : 0.0f;

	// Gravity from preset
	GPUParams.Gravity = FVector3f(Preset->Gravity);

	// Time
	GPUParams.DeltaTime = SubstepDT;
	GPUParams.CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	// Spatial hash cell size - must match the Volume's CellSize for Morton code consistency
	// Priority: TargetVolumeComponent's CellSize > Fallback to SmoothingRadius
	if (UKawaiiFluidSimulationVolumeComponent* Volume = TargetVolumeComponent.Get())
	{
		GPUParams.CellSize = Volume->CellSize;
	}
	else
	{
		GPUParams.CellSize = Preset->SmoothingRadius;
	}

	// Bounds collision (use world bounds from params if containment is enabled)
	if (Params.WorldBounds.IsValid)
	{
		// Containment enabled - check if OBB or AABB mode
		const bool bHasRotation = !Params.BoundsRotation.Equals(FQuat::Identity, 0.0001f);

		if (bHasRotation)
		{
			// OBB mode - use Center, Extent, Rotation
			GPUParams.bUseOBB = 1;
			GPUParams.BoundsCenter = FVector3f(Params.BoundsCenter);
			GPUParams.BoundsExtent = FVector3f(Params.BoundsExtent);
			GPUParams.BoundsRotation = FVector4f(
				Params.BoundsRotation.X,
				Params.BoundsRotation.Y,
				Params.BoundsRotation.Z,
				Params.BoundsRotation.W
			);

			// Also set AABB for fallback (compute AABB from OBB)
			GPUParams.BoundsMin = FVector3f(Params.WorldBounds.Min);
			GPUParams.BoundsMax = FVector3f(Params.WorldBounds.Max);
		}
		else
		{
			// AABB mode - use Min/Max
			GPUParams.bUseOBB = 0;
			GPUParams.BoundsMin = FVector3f(Params.WorldBounds.Min);
			GPUParams.BoundsMax = FVector3f(Params.WorldBounds.Max);
			GPUParams.BoundsCenter = FVector3f(Params.BoundsCenter);
			GPUParams.BoundsExtent = FVector3f(Params.BoundsExtent);
			GPUParams.BoundsRotation = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);  // Identity
		}

		GPUParams.BoundsRestitution = Params.BoundsRestitution;
		GPUParams.BoundsFriction = Params.BoundsFriction;
	}
	else
	{
		// Default large bounds (effectively no bounds collision)
		GPUParams.bUseOBB = 0;
		GPUParams.BoundsMin = FVector3f(-1000000.0f, -1000000.0f, -1000000.0f);
		GPUParams.BoundsMax = FVector3f(1000000.0f, 1000000.0f, 1000000.0f);
		GPUParams.BoundsCenter = FVector3f::ZeroVector;
		GPUParams.BoundsExtent = FVector3f(1000000.0f);
		GPUParams.BoundsRotation = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);  // Identity
		GPUParams.BoundsRestitution = Preset->Restitution;
		GPUParams.BoundsFriction = Preset->Friction;
	}

	// Solver iterations (typically 1-4 for density constraint)
	GPUParams.SolverIterations = Preset->SolverIterations;

	// Tensile Instability Correction (PBF Eq.13-14)
	// Must be set before PrecomputeKernelCoefficients() to compute InvW_DeltaQ
	GPUParams.bEnableTensileInstability = Preset->bEnableTensileInstabilityCorrection ? 1 : 0;
	GPUParams.TensileK = Preset->TensileInstabilityK;
	GPUParams.TensileN = Preset->TensileInstabilityN;
	GPUParams.TensileDeltaQ = Preset->TensileInstabilityDeltaQ;

	// Precompute kernel coefficients (including InvW_DeltaQ for tensile instability)
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

	if (!Preset)
	{
		return;
	}

	// Ensure GPU simulator is ready
	if (!IsGPUSimulatorReady())
	{
		InitializeGPUSimulator(Preset->MaxParticles);
		if (!IsGPUSimulatorReady())
		{
			UE_LOG(LogTemp, Warning, TEXT("GPU Simulator not ready"));
			return;
		}
	}

	// Set default spawn parameters from Preset (for fallback when spawn request values are 0)
	GPUSimulator->SetDefaultSpawnRadius(Preset->ParticleRadius);
	GPUSimulator->SetDefaultSpawnMass(Preset->ParticleMass);

	// Set external force from simulation params (wind, player force, etc.)
	GPUSimulator->SetExternalForce(FVector3f(Params.ExternalForce));

	// Set primitive collision threshold from Preset
	GPUSimulator->SetPrimitiveCollisionThreshold(Preset->PrimitiveCollisionThreshold);

	// Set simulation bounds for Z-Order sorting (Morton code)
	// Priority: TargetVolumeComponent bounds > Preset bounds + SimulationOrigin
	FVector3f WorldBoundsMin, WorldBoundsMax;

	if (UKawaiiFluidSimulationVolumeComponent* Volume = TargetVolumeComponent.Get())
	{
		// Use Volume's world-space bounds directly
		WorldBoundsMin = FVector3f(Volume->GetWorldBoundsMin());
		WorldBoundsMax = FVector3f(Volume->GetWorldBoundsMax());
	}
	else
	{
		// Fallback: Calculate bounds from SmoothingRadius
		// GridAxisBits=7 → GridResolution=128, BoundsExtent = 128 * SmoothingRadius
		constexpr int32 GridResolution = 128;
		const float BoundsExtent = GridResolution * Preset->SmoothingRadius;
		const float HalfExtent = BoundsExtent * 0.5f;
		WorldBoundsMin = FVector3f(-HalfExtent) + FVector3f(Params.SimulationOrigin);
		WorldBoundsMax = FVector3f(HalfExtent) + FVector3f(Params.SimulationOrigin);
	}

	GPUSimulator->SetSimulationBounds(WorldBoundsMin, WorldBoundsMax);

	// =====================================================
	// GPU-Only Mode: No CPU Particles array dependency
	// - Spawning: Handled directly by SpawnParticle() → GPUSimulator->AddSpawnRequest()
	// - Physics: GPU handles everything
	// - Rendering: GPU buffer is source of truth
	// =====================================================

	// Check if we have any particles (either existing GPU particles or pending spawns)
	const int32 CurrentGPUCount = GPUSimulator->GetParticleCount();
	const int32 PendingSpawnCount = GPUSimulator->GetPendingSpawnCount();

	static int32 SimGPULogCounter = 0;
	if (++SimGPULogCounter % 60 == 1)
	{
		UE_LOG(LogTemp, Log, TEXT("SimulateGPU: GPUCount=%d, PendingSpawn=%d, GPUSimulator=%p"),
			CurrentGPUCount, PendingSpawnCount, GPUSimulator.Get());
	}

	if (CurrentGPUCount == 0 && PendingSpawnCount == 0)
	{
		return;  // Nothing to simulate
	}

	// Build GPU simulation parameters
	const float SubstepDT = Preset->SubstepDeltaTime;
	FGPUFluidSimulationParams GPUParams = BuildGPUSimParams(Preset, Params, SubstepDT);

	// ParticleCount will be updated by GPU after spawn processing
	// Use current GPU count + pending spawns as estimate
	GPUParams.ParticleCount = CurrentGPUCount + PendingSpawnCount;

	// =====================================================
	// Collision Primitives Upload (optimized - upload when dirty)
	// TODO: Add dirty flag check for further optimization
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

	
	// Collect and upload collision primitives to GPU (with bone tracking for adhesion)
	{
		FGPUCollisionPrimitives CollisionPrimitives;
		// Use persistent bone data for velocity calculation across frames
		CollisionPrimitives.BoneTransforms = MoveTemp(PersistentBoneTransforms);
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

		// Check if adhesion is enabled (Per-Polygon disabled actors can use GPU adhesion)
		const bool bUseGPUAdhesion = Preset->AdhesionStrength > 0.0f;

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
			if (MeshCollider)
			{
				// Cache collider shape if needed
				MeshCollider->CacheCollisionShapes();

				if (MeshCollider->IsCacheValid())
				{
					// Get OwnerID for collision feedback filtering
					// Use the owner actor's UniqueID to identify which actor owns these primitives
					int32 OwnerID = ColliderOwner ? ColliderOwner->GetUniqueID() : 0;

					// Use bone-aware export for GPU adhesion
					if (bUseGPUAdhesion)
					{
						MeshCollider->ExportToGPUPrimitivesWithBones(
							CollisionPrimitives.Spheres,
							CollisionPrimitives.Capsules,
							CollisionPrimitives.Boxes,
							CollisionPrimitives.Convexes,
							CollisionPrimitives.ConvexPlanes,
							CollisionPrimitives.BoneTransforms,
							PersistentBoneNameToIndex,  // Use persistent mapping
							DefaultFriction,
							DefaultRestitution,
							OwnerID
						);
					}
					else
					{
						// Legacy path without bone tracking
						MeshCollider->ExportToGPUPrimitives(
							CollisionPrimitives.Spheres,
							CollisionPrimitives.Capsules,
							CollisionPrimitives.Boxes,
							CollisionPrimitives.Convexes,
							CollisionPrimitives.ConvexPlanes,
							DefaultFriction,
							DefaultRestitution,
							OwnerID
						);
					}
				}
			}
		}

		// Upload to GPU only if we have primitives
		if (!CollisionPrimitives.IsEmpty())
		{
			GPUSimulator->UploadCollisionPrimitives(CollisionPrimitives);

			// Set adhesion parameters if enabled
			if (bUseGPUAdhesion && CollisionPrimitives.BoneTransforms.Num() > 0)
			{
				FGPUAdhesionParams AdhesionParams;
				AdhesionParams.bEnableAdhesion = 1;
				AdhesionParams.AdhesionStrength = Preset->AdhesionStrength;
				AdhesionParams.AdhesionRadius = Preset->AdhesionRadius;
				AdhesionParams.ColliderContactOffset = Preset->AdhesionContactOffset;
				AdhesionParams.BoneVelocityScale = Preset->AdhesionBoneVelocityScale;
				AdhesionParams.DetachAccelThreshold = Preset->AdhesionDetachAcceleration;  // Detach on fast acceleration
				AdhesionParams.DetachDistanceThreshold = Preset->AdhesionDetachDistance; // Detach if too far from surface
				AdhesionParams.SlidingFriction = DefaultFriction;
				AdhesionParams.CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

				// Gravity sliding parameters - use ExternalForce as gravity
				AdhesionParams.Gravity = FVector3f(Params.ExternalForce);
				AdhesionParams.GravitySlidingScale = 1.0f;  // Full gravity sliding effect

				GPUSimulator->SetAdhesionParams(AdhesionParams);
			}
			else
			{
				FGPUAdhesionParams AdhesionParams;
				AdhesionParams.bEnableAdhesion = 0;
				GPUSimulator->SetAdhesionParams(AdhesionParams);
			}
		}

		// Save bone transforms back to persistent storage for next frame
		PersistentBoneTransforms = MoveTemp(CollisionPrimitives.BoneTransforms);
	}

	// =====================================================
	// GPU Boundary Skinning (Flex-style Adhesion)
	// Upload local particles once, bone transforms each frame
	// GPU transforms local → world (much faster than CPU)
	// =====================================================
	{
		int32 TotalBoundaryParticles = 0;

		for (UFluidInteractionComponent* Interaction : Params.InteractionComponents)
		{
			if (Interaction && Interaction->HasLocalBoundaryParticles())
			{
				const int32 OwnerID = Interaction->GetBoundaryOwnerID();

				// Upload local particles only once (first time or after regeneration)
				if (!GPUSimulator->IsGPUBoundarySkinningEnabled() ||
				    GPUSimulator->GetTotalLocalBoundaryParticleCount() == 0)
				{
					TArray<FGPUBoundaryParticleLocal> LocalParticles;
					Interaction->CollectLocalBoundaryParticles(LocalParticles);

					if (LocalParticles.Num() > 0)
					{
						GPUSimulator->UploadLocalBoundaryParticles(OwnerID, LocalParticles);
					}
				}

				// Upload bone transforms each frame
				TArray<FMatrix> BoneTransforms;
				FMatrix ComponentTransform;
				Interaction->CollectBoneTransformsForBoundary(BoneTransforms, ComponentTransform);

				// Convert to FMatrix44f
				TArray<FMatrix44f> BoneTransforms44f;
				BoneTransforms44f.SetNum(BoneTransforms.Num());
				for (int32 i = 0; i < BoneTransforms.Num(); ++i)
				{
					BoneTransforms44f[i] = FMatrix44f(BoneTransforms[i]);
				}

				GPUSimulator->UploadBoneTransformsForBoundary(OwnerID, BoneTransforms44f, FMatrix44f(ComponentTransform));
			}
		}

		TotalBoundaryParticles = GPUSimulator->GetTotalLocalBoundaryParticleCount();

		// Set boundary adhesion parameters
		if (TotalBoundaryParticles > 0)
		{
			FGPUBoundaryAdhesionParams BoundaryAdhesionParams;
			BoundaryAdhesionParams.bEnabled = 1;
			BoundaryAdhesionParams.AdhesionStrength = Preset->AdhesionStrength;
			BoundaryAdhesionParams.AdhesionRadius = Preset->AdhesionRadius;
			BoundaryAdhesionParams.CohesionStrength = Preset->CohesionStrength;
			BoundaryAdhesionParams.SmoothingRadius = Preset->SmoothingRadius;
			BoundaryAdhesionParams.BoundaryParticleCount = TotalBoundaryParticles;
			BoundaryAdhesionParams.FluidParticleCount = GPUSimulator->GetParticleCount();
			BoundaryAdhesionParams.DeltaTime = Preset->SubstepDeltaTime;

			GPUSimulator->SetBoundaryAdhesionParams(BoundaryAdhesionParams);
		}
		else
		{
			FGPUBoundaryAdhesionParams BoundaryAdhesionParams;
			BoundaryAdhesionParams.bEnabled = 0;
			GPUSimulator->SetBoundaryAdhesionParams(BoundaryAdhesionParams);
		}
	}

	// =====================================================
	// Run GPU simulation with Accumulator method
	// Simulate with fixed dt substeps for frame-rate independence
	// =====================================================
	const int32 MaxSubstepsPerFrame = Preset->MaxSubsteps;
	const float MaxAllowedTime = Preset->SubstepDeltaTime * MaxSubstepsPerFrame;
	AccumulatedTime += FMath::Min(DeltaTime, MaxAllowedTime);

	int32 SubstepCount = 0;
	while (AccumulatedTime >= Preset->SubstepDeltaTime && SubstepCount < MaxSubstepsPerFrame)
	{
		GPUSimulator->SimulateSubstep(GPUParams);
		AccumulatedTime -= Preset->SubstepDeltaTime;
		++SubstepCount;
	}

	
	
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
					//UE_LOG(LogTemp, Log, TEXT("AABB[%d]: Min=(%.1f, %.1f, %.1f) Max=(%.1f, %.1f, %.1f)"),j,FAABB.Min.X, FAABB.Min.Y, FAABB.Min.Z,FAABB.Max.X, FAABB.Max.Y, FAABB.Max.Z);
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
							Preset->AdhesionStrength,  // 유체의 접착력
							Preset->AdhesionContactOffset,
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

	//========================================
	// Phase 2.6: Update Attached Particles
	// Update positions for particles attached to skeletal mesh surfaces
	// Check for detachment based on surface acceleration
	//========================================
	if (PerPolygonProcessor && PerPolygonProcessor->GetAttachedParticleCount() > 0)
	{
		TArray<FAttachedParticleUpdate> AttachmentUpdates;
		PerPolygonProcessor->UpdateAttachedParticles(
			Params.InteractionComponents,
			DeltaTime,
			AttachmentUpdates
		);

		if (AttachmentUpdates.Num() > 0)
		{
			GPUSimulator->ApplyAttachmentUpdates(AttachmentUpdates);

			static int32 AttachmentDebugCounter = 0;
			if (++AttachmentDebugCounter % 60 == 1)
			{
				UE_LOG(LogTemp, Log, TEXT("Attachment: Updated %d particles (%d attached)"),
					AttachmentUpdates.Num(),
					PerPolygonProcessor->GetAttachedParticleCount());
			}
		}
	}

	//========================================
	// GPU Statistics Collection
	// For GPU comparison, collect basic stats without particle readback
	//========================================
	CollectGPUSimulationStats(Preset, GPUParams.ParticleCount, SubstepCount);
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

	if (!Preset)
	{
		return;
	}

	// Debug: Check GPU simulation flag
	static int32 SimulateLogCounter = 0;
	if (++SimulateLogCounter % 60 == 1)
	{
		UE_LOG(LogTemp, Log, TEXT("Context::Simulate: bUseGPUSimulation=%d, Particles.Num=%d"),
			Params.bUseGPUSimulation ? 1 : 0, Particles.Num());
	}

	// GPU mode: dispatch early (GPU has its own particle count check via pending spawns)
	// Do NOT check CPU Particles.Num() here - GPU mode doesn't use CPU array
	if (Params.bUseGPUSimulation)
	{
		SimulateGPU(Particles, Preset, Params, SpatialHash, DeltaTime, AccumulatedTime);
		return;
	}

	// CPU mode: require particles in CPU array
	if (Particles.Num() == 0)
	{
		return;
	}

	EnsureSolversInitialized(Preset);

	// Accumulator method: simulate with fixed dt
	const int32 MaxSubstepsPerFrame = Preset->MaxSubsteps;
	const float MaxAllowedTime = Preset->SubstepDeltaTime * MaxSubstepsPerFrame;
	AccumulatedTime += FMath::Min(DeltaTime, MaxAllowedTime);

	// Cache collider shapes once per frame
	CacheColliderShapes(Params.Colliders);

	// Update attached particle positions (bone tracking - before physics)
	UpdateAttachedParticlePositions(Particles, Params.InteractionComponents);

	// Substep loop
	int32 SubstepCount = 0;
	while (AccumulatedTime >= Preset->SubstepDeltaTime && SubstepCount < MaxSubstepsPerFrame)
	{
		SimulateSubstep(Particles, Preset, Params, SpatialHash, Preset->SubstepDeltaTime);
		AccumulatedTime -= Preset->SubstepDeltaTime;
		++SubstepCount;
	}

	// Collect simulation statistics
	CollectSimulationStats(Particles, Preset, SubstepCount, false);
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
		HandleCollisions(Particles, Params.Colliders, SubstepDT);
	}

	// 5. World collision
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextWorldCollision);
		if (Params.bUseWorldCollision && Params.World)
		{
			HandleWorldCollision(Particles, Params, SpatialHash, Params.ParticleRadius, SubstepDT, Preset->Friction, Preset->Restitution);
		}
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

	// 10. Apply stack pressure (weight transfer from stacked attached particles)
	if (Preset->bEnableStackPressure && StackPressureSolver.IsValid())
	{
		float SearchRadius = Preset->StackPressureRadius > 0.0f
			? Preset->StackPressureRadius
			: Preset->SmoothingRadius;

		StackPressureSolver->Apply(
			Particles,
			Preset->Gravity,
			Preset->StackPressureScale,
			SearchRadius,
			SubstepDT
		);
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
	if (!DensityConstraint.IsValid())
	{
		return;
	}

	// XPBD: Lambda 초기화 (매 타임스텝 시작 시 0으로 리셋)
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		Particles[i].Lambda = 0.0f;
	});

	// Tensile Instability 파라미터 설정 (PBF Eq.13-14)
	const bool bUseTensileCorrection = Preset->bEnableTensileInstabilityCorrection;

	// Scale Compliance based on SmoothingRadius for stability across different particle sizes
	const float ScaledCompliance = SPHScaling::GetScaledCompliance(
		Preset->Compliance, Preset->SmoothingRadius, Preset->ComplianceScalingExponent);

	// XPBD 반복 솔버 (점성 유체: 2-3회, 물: 4-6회)
	const int32 SolverIterations = Preset->SolverIterations;
	for (int32 Iter = 0; Iter < SolverIterations; ++Iter)
	{
		if (bUseTensileCorrection)
		{
			// Tensile Instability 보정 포함 솔버
			FTensileInstabilityParams TensileParams;
			TensileParams.bEnabled = true;
			TensileParams.K = Preset->TensileInstabilityK;
			TensileParams.N = Preset->TensileInstabilityN;
			TensileParams.DeltaQ = Preset->TensileInstabilityDeltaQ;

			DensityConstraint->SolveWithTensileCorrection(
				Particles,
				Preset->SmoothingRadius,
				Preset->RestDensity,
				ScaledCompliance,
				DeltaTime,
				TensileParams
			);
		}
		else
		{
			// 기본 솔버
			DensityConstraint->Solve(
				Particles,
				Preset->SmoothingRadius,
				Preset->RestDensity,
				ScaledCompliance,
				DeltaTime
			);
		}
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
	const TArray<UFluidCollider*>& Colliders,
	float SubstepDT)
{
	for (UFluidCollider* Collider : Colliders)
	{
		if (Collider && Collider->IsColliderEnabled())
		{
			Collider->ResolveCollisions(Particles, SubstepDT);
		}
	}
}

void UKawaiiFluidSimulationContext::HandleWorldCollision(
	TArray<FFluidParticle>& Particles,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float ParticleRadius,
	float SubstepDT,
	float Friction,
	float Restitution)
{
	// Dispatch to appropriate method based on WorldCollisionMethod
	switch (Params.WorldCollisionMethod)
	{
	case EWorldCollisionMethod::SDF:
		HandleWorldCollision_SDF(Particles, Params, SpatialHash, ParticleRadius, SubstepDT, Friction, Restitution);
		break;

	case EWorldCollisionMethod::Sweep:
	default:
		HandleWorldCollision_Sweep(Particles, Params, SpatialHash, ParticleRadius, SubstepDT, Friction, Restitution);
		break;
	}
}

//========================================
// Legacy Sweep-based World Collision
//========================================

void UKawaiiFluidSimulationContext::HandleWorldCollision_Sweep(
	TArray<FFluidParticle>& Particles,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float ParticleRadius,
	float SubstepDT,
	float Friction,
	float Restitution)
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

			// Sweep from Position to PredictedPosition
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
				const FVector& Normal = HitResult.ImpactNormal;

				// Only modify PredictedPosition
				Particle.PredictedPosition = CollisionPos;

				// Calculate desired velocity after collision response
				// Initialize to zero - particle stops on surface by default
				FVector DesiredVelocity = FVector::ZeroVector;
				float VelDotNormal = FVector::DotProduct(Particle.Velocity, Normal);

				// Minimum velocity threshold for applying restitution bounce
				// Prevents "popcorn" oscillation for particles resting on surfaces
				const float MinBounceVelocity = 50.0f;  // cm/s

				if (VelDotNormal < 0.0f)
				{
					// Particle moving INTO surface - apply collision response
					FVector VelNormal = Normal * VelDotNormal;
					FVector VelTangent = Particle.Velocity - VelNormal;

					if (VelDotNormal < -MinBounceVelocity)
					{
						// Significant impact - apply full collision response
						// Normal: reflect with Restitution (0 = stick, 1 = perfect bounce)
						// Tangent: dampen with Friction (0 = slide, 1 = stop)
						DesiredVelocity = VelTangent * (1.0f - Friction) - VelNormal * Restitution;
					}
					else
					{
						// Low velocity contact (resting on surface) - no bounce, just slide
						DesiredVelocity = VelTangent * (1.0f - Friction);
					}
				}
				// else: VelDotNormal >= 0 means particle moving AWAY from surface
				// DesiredVelocity stays zero - particle stops on surface (same as OLD behavior)

				// Back-calculate Position so FinalizePositions derives DesiredVelocity
				// FinalizePositions: Velocity = (PredictedPosition - Position) / dt
				// Therefore: Position = PredictedPosition - DesiredVelocity * dt
				Particle.Position = Particle.PredictedPosition - DesiredVelocity * SubstepDT;

				// 충돌 이벤트 버퍼에 추가 (나중에 ProcessCollisionFeedback에서 처리)
				if (Params.bEnableCollisionEvents && Params.CPUCollisionFeedbackBufferPtr && Params.CPUCollisionFeedbackLockPtr)
				{
					const float Speed = Particle.Velocity.Size();
					if (Speed >= Params.MinVelocityForEvent)
					{
						FKawaiiFluidCollisionEvent Event;
						Event.ParticleIndex = Particle.ParticleID;
						Event.SourceID = Particle.SourceID;
						Event.ColliderOwnerID = HitResult.GetActor() ? HitResult.GetActor()->GetUniqueID() : -1;
						Event.BoneIndex = -1;  // CPU path doesn't have bone info
						Event.HitActor = HitResult.GetActor();
						Event.HitLocation = HitResult.Location;
						Event.HitNormal = HitResult.ImpactNormal;
						Event.HitSpeed = Speed;
						// HitInteractionComponent는 ProcessCollisionFeedback에서 조회

						// 버퍼에 추가 (thread-safe)
						FScopeLock Lock(Params.CPUCollisionFeedbackLockPtr);
						Params.CPUCollisionFeedbackBufferPtr->Add(Event);
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

//========================================
// SDF-based World Collision (Overlap + ClosestPoint)
//========================================

void UKawaiiFluidSimulationContext::HandleWorldCollision_SDF(
	TArray<FFluidParticle>& Particles,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float ParticleRadius,
	float SubstepDT,
	float Friction,
	float Restitution)
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

	// Cell-based broad-phase (same as Sweep method)
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

	// Collision margin (particle radius + safety margin)
	const float CollisionMargin = ParticleRadius * 1.1f;

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

			// SDF approach: Overlap + ClosestPoint
			TArray<FOverlapResult> Overlaps;
			bool bOverlapped = World->OverlapMultiByChannel(
				Overlaps,
				Particle.PredictedPosition,
				FQuat::Identity,
				Params.CollisionChannel,
				FCollisionShape::MakeSphere(CollisionMargin),
				LocalParams
			);

			if (!bOverlapped || Overlaps.Num() == 0)
			{
				return;
			}

			// Find closest collision among all overlapping primitives
			float MinSignedDistance = MAX_FLT;
			FVector BestNormal = FVector::UpVector;
			FVector BestClosestPoint = Particle.PredictedPosition;
			AActor* HitActor = nullptr;

			for (const FOverlapResult& Overlap : Overlaps)
			{
				UPrimitiveComponent* Comp = Overlap.GetComponent();
				if (!Comp)
				{
					continue;
				}

				FVector ClosestPoint;
				float DistToSurface = Comp->GetClosestPointOnCollision(
					Particle.PredictedPosition, ClosestPoint);

				if (DistToSurface < 0.0f)
				{
					// GetClosestPointOnCollision returns -1 on failure
					continue;
				}

				// Calculate signed distance and normal
				FVector ToParticle = Particle.PredictedPosition - ClosestPoint;
				float Dist = ToParticle.Size();
				FVector Normal;

				if (Dist > KINDA_SMALL_NUMBER)
				{
					Normal = ToParticle / Dist;
				}
				else
				{
					// Particle is exactly on or inside surface
					// Use velocity direction to determine push direction
					Normal = -Particle.Velocity.GetSafeNormal();
					if (Normal.IsNearlyZero())
					{
						Normal = FVector::UpVector;
					}
					Dist = 0.0f;
				}

				// Convert to signed distance (negative if inside)
				// Since we're overlapping, if Dist is very small, we're likely inside
				float SignedDist = Dist;
				if (Dist < CollisionMargin * 0.5f)
				{
					// Treat as being inside or very close to surface
					SignedDist = Dist - CollisionMargin;
				}

				if (SignedDist < MinSignedDistance)
				{
					MinSignedDistance = SignedDist;
					BestNormal = Normal;
					BestClosestPoint = ClosestPoint;
					HitActor = Overlap.GetActor();
				}
			}

			// Apply collision response if within margin
			if (MinSignedDistance < CollisionMargin)
			{
				// Push particle to surface + margin
				float Penetration = CollisionMargin - MinSignedDistance;
				FVector CollisionPos = Particle.PredictedPosition + BestNormal * Penetration;

				// Only modify PredictedPosition
				Particle.PredictedPosition = CollisionPos;

				// Calculate desired velocity after collision response
				// Initialize to zero - particle stops on surface by default
				FVector DesiredVelocity = FVector::ZeroVector;
				float VelDotNormal = FVector::DotProduct(Particle.Velocity, BestNormal);

				// Minimum velocity threshold for applying restitution bounce
				// Prevents "popcorn" oscillation for particles resting on surfaces
				const float MinBounceVelocity = 50.0f;  // cm/s

				if (VelDotNormal < 0.0f)
				{
					// Particle moving INTO surface - apply collision response
					FVector VelNormal = BestNormal * VelDotNormal;
					FVector VelTangent = Particle.Velocity - VelNormal;

					if (VelDotNormal < -MinBounceVelocity)
					{
						// Significant impact - apply full collision response
						// Normal: reflect with Restitution (0 = stick, 1 = perfect bounce)
						// Tangent: dampen with Friction (0 = slide, 1 = stop)
						DesiredVelocity = VelTangent * (1.0f - Friction) - VelNormal * Restitution;
					}
					else
					{
						// Low velocity contact (resting on surface) - no bounce, just slide
						DesiredVelocity = VelTangent * (1.0f - Friction);
					}
				}
				// else: VelDotNormal >= 0 means particle moving AWAY from surface
				// DesiredVelocity stays zero - particle stops on surface (same as OLD behavior)

				// Back-calculate Position so FinalizePositions derives DesiredVelocity
				Particle.Position = Particle.PredictedPosition - DesiredVelocity * SubstepDT;

				// 충돌 이벤트 버퍼에 추가 (나중에 ProcessCollisionFeedback에서 처리)
				if (Params.bEnableCollisionEvents && Params.CPUCollisionFeedbackBufferPtr && Params.CPUCollisionFeedbackLockPtr)
				{
					const float Speed = Particle.Velocity.Size();
					if (Speed >= Params.MinVelocityForEvent)
					{
						FKawaiiFluidCollisionEvent Event;
						Event.ParticleIndex = Particle.ParticleID;
						Event.SourceID = Particle.SourceID;
						Event.ColliderOwnerID = HitActor ? HitActor->GetUniqueID() : -1;
						Event.BoneIndex = -1;  // CPU path doesn't have bone info
						Event.HitActor = HitActor;
						Event.HitLocation = BestClosestPoint;
						Event.HitNormal = BestNormal;
						Event.HitSpeed = Speed;
						// HitInteractionComponent는 ProcessCollisionFeedback에서 조회

						// 버퍼에 추가 (thread-safe)
						FScopeLock Lock(Params.CPUCollisionFeedbackLockPtr);
						Params.CPUCollisionFeedbackBufferPtr->Add(Event);
					}
				}

				// Detach from character if hitting different surface
				if (Particle.bIsAttached && HitActor != Particle.AttachedActor.Get())
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

	// Floor detachment check (same as Sweep method)
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
			Preset->DetachThreshold,
			Preset->AdhesionContactOffset
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
		ClusterParticleMap.FindOrAdd(Particles[i].SourceID).Add(i);
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

//=============================================================================
// Statistics Collection
//=============================================================================

void UKawaiiFluidSimulationContext::CollectSimulationStats(
	const TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	int32 SubstepCount,
	bool bIsGPU)
{
	FKawaiiFluidSimulationStatsCollector& Stats = GetFluidStatsCollector();

	if (!Stats.IsEnabled())
	{
		return;
	}

	// Begin new frame of stats collection
	Stats.BeginFrame();
	Stats.SetGPUSimulation(bIsGPU);
	Stats.SetSubstepCount(SubstepCount);

	if (Preset)
	{
		Stats.SetRestDensity(Preset->RestDensity);
		Stats.SetSolverIterations(Preset->SolverIterations);
	}

	// Count particle types
	int32 TotalCount = Particles.Num();
	int32 AttachedCount = 0;
	int32 GroundCount = 0;

	for (const FFluidParticle& Particle : Particles)
	{
		// Velocity sample
		float VelMag = static_cast<float>(Particle.Velocity.Size());
		Stats.AddVelocitySample(VelMag);

		// Density sample
		Stats.AddDensitySample(Particle.Density);

		// Neighbor count sample
		Stats.AddNeighborCountSample(Particle.NeighborIndices.Num());

		// Count attached particles
		if (Particle.bIsAttached)
		{
			AttachedCount++;
		}

		// Count ground contact
		if (Particle.bNearGround)
		{
			GroundCount++;
		}
	}

	// Set particle counts
	int32 ActiveCount = TotalCount - AttachedCount;
	Stats.SetParticleCounts(TotalCount, ActiveCount, AttachedCount);

	// Set ground contact count as collision stat
	for (int32 i = 0; i < GroundCount; ++i)
	{
		Stats.AddGroundContact();
	}

	// End frame and finalize statistics
	Stats.EndFrame();
}

void UKawaiiFluidSimulationContext::CollectGPUSimulationStats(
	const UKawaiiFluidPresetDataAsset* Preset,
	int32 ParticleCount,
	int32 SubstepCount)
{
	FKawaiiFluidSimulationStatsCollector& Stats = GetFluidStatsCollector();

	if (!Stats.IsEnabled())
	{
		return;
	}

	// Begin new frame of stats collection
	Stats.BeginFrame();
	Stats.SetGPUSimulation(true);

	if (Preset)
	{
		Stats.SetRestDensity(Preset->RestDensity);
		Stats.SetSolverIterations(Preset->SolverIterations);

		// Use actual substep count from simulation
		Stats.SetSubstepCount(SubstepCount);
	}

	// Detailed GPU stats: download particles and collect full statistics
	if (Stats.IsDetailedGPUEnabled() && GPUSimulator.IsValid() && ParticleCount > 0)
	{
		TArray<FFluidParticle> GPUParticles;
		// Use GetAllGPUParticles instead of DownloadParticles
		// DownloadParticles requires existing CPU particles for ParticleID matching
		// GetAllGPUParticles creates new particles directly from GPU readback
		bool bSuccess = GPUSimulator->GetAllGPUParticles(GPUParticles);

		if (bSuccess && GPUParticles.Num() > 0)
		{
			// Count particle types
			int32 TotalCount = GPUParticles.Num();
			int32 AttachedCount = 0;
			int32 GroundCount = 0;

			// Debug log for detailed GPU stats
			static int32 DetailedStatsLogCounter = 0;
			if (++DetailedStatsLogCounter % 60 == 1)
			{
				float FirstVel = GPUParticles.Num() > 0 ? GPUParticles[0].Velocity.Size() : 0.0f;
				float FirstDensity = GPUParticles.Num() > 0 ? GPUParticles[0].Density : 0.0f;
				UE_LOG(LogTemp, Log, TEXT("GPU Detailed Stats: Retrieved %d particles, First.Vel=%.1f, First.Density=%.1f"),
					TotalCount, FirstVel, FirstDensity);
			}

			for (const FFluidParticle& Particle : GPUParticles)
			{
				// Velocity sample
				float VelMag = static_cast<float>(Particle.Velocity.Size());
				Stats.AddVelocitySample(VelMag);

				// Density sample
				Stats.AddDensitySample(Particle.Density);

				// Neighbor count sample (GPU doesn't track neighbors the same way, use 0)
				Stats.AddNeighborCountSample(Particle.NeighborIndices.Num());

				// Count attached particles
				if (Particle.bIsAttached)
				{
					AttachedCount++;
				}

				// Count ground contact
				if (Particle.bNearGround)
				{
					GroundCount++;
				}
			}

			// Set particle counts
			int32 ActiveCount = TotalCount - AttachedCount;
			Stats.SetParticleCounts(TotalCount, ActiveCount, AttachedCount);

			// Set ground contact count
			for (int32 i = 0; i < GroundCount; ++i)
			{
				Stats.AddGroundContact();
			}

			// Calculate stability metrics from GPU particle data
			if (TotalCount > 0 && Preset)
			{
				// Extract density and velocity arrays for stability calculation
				TArray<float> Densities;
				TArray<float> Velocities;
				TArray<float> Masses;
				Densities.Reserve(TotalCount);
				Velocities.Reserve(TotalCount);
				Masses.Reserve(TotalCount);

				for (const FFluidParticle& Particle : GPUParticles)
				{
					Densities.Add(Particle.Density);
					Velocities.Add(static_cast<float>(Particle.Velocity.Size()));
					Masses.Add(Particle.Mass);
				}

				Stats.CalculateStabilityMetrics(
					Densities.GetData(),
					Velocities.GetData(),
					Masses.GetData(),
					TotalCount,
					Preset->RestDensity);
			}
		}
		else
		{
			// Readback returned empty - use basic counts
			static int32 ReadbackFailLogCounter = 0;
			if (++ReadbackFailLogCounter % 60 == 1)
			{
				UE_LOG(LogTemp, Warning, TEXT("GPU Detailed Stats: GetAllGPUParticles failed (bSuccess=%d, Count=%d)"),
					bSuccess ? 1 : 0, GPUParticles.Num());
			}
			Stats.SetParticleCounts(ParticleCount, ParticleCount, 0);
		}
	}
	else
	{
		// Basic mode: no readback, just set particle count
		Stats.SetParticleCounts(ParticleCount, ParticleCount, 0);
	}

	// End frame and finalize statistics
	Stats.EndFrame();
}
