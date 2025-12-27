// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/SpatialHash.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Physics/DensityConstraint.h"
#include "Physics/ViscositySolver.h"
#include "Physics/AdhesionSolver.h"
#include "Collision/FluidCollider.h"
#include "Components/FluidInteractionComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Async/Async.h"

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

UKawaiiFluidSimulationContext::UKawaiiFluidSimulationContext()
{
}

UKawaiiFluidSimulationContext::~UKawaiiFluidSimulationContext()
{
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

void UKawaiiFluidSimulationContext::Simulate(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float DeltaTime,
	float& AccumulatedTime)
{
	SCOPE_CYCLE_COUNTER(STAT_ContextSimulate);

	if (!Preset || Particles.Num() == 0)
	{
		return;
	}

	EnsureSolversInitialized(Preset);

	// Accumulator method: simulate with fixed dt
	const float MaxAllowedTime = Preset->SubstepDeltaTime * Preset->MaxSubsteps;
	AccumulatedTime += FMath::Min(DeltaTime, MaxAllowedTime);

	// Cache collider shapes once per frame
	CacheColliderShapes(Params.Colliders);

	// Update attached particle positions (bone tracking - before physics)
	UpdateAttachedParticlePositions(Particles, Params.InteractionComponents);

	// Substep loop
	while (AccumulatedTime >= Preset->SubstepDeltaTime)
	{
		SimulateSubstep(Particles, Preset, Params, SpatialHash, Preset->SubstepDeltaTime);
		AccumulatedTime -= Preset->SubstepDeltaTime;
	}
}

void UKawaiiFluidSimulationContext::SimulateSubstep(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float SubstepDT)
{
	// 1. Predict positions
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextPredictPositions);
		PredictPositions(Particles, Preset, Params.ExternalForce, SubstepDT);
	}

	// 2. Update neighbors
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextUpdateNeighbors);
		UpdateNeighbors(Particles, SpatialHash, Preset->SmoothingRadius);
	}

	// 3. Solve density constraints
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextSolveDensity);

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
