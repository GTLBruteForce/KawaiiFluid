// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "KawaiiFluidSimulationContext.generated.h"

class FSpatialHash;
class FDensityConstraint;
class FViscositySolver;
class FAdhesionSolver;
class UKawaiiFluidPresetDataAsset;

/**
 * Stateless Simulation Context
 *
 * Contains pure simulation logic with no internal state.
 * All state is passed in as parameters, making it thread-safe and reusable.
 *
 * Inherit from this class to create custom fluid behaviors:
 * - ULavaSimulationContext: Temperature-based viscosity, solidification
 * - USandSimulationContext: Friction-based rest, granular behavior
 * - USnowSimulationContext: Compression-based density changes
 */
UCLASS(BlueprintType, Blueprintable)
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidSimulationContext : public UObject
{
	GENERATED_BODY()

public:
	UKawaiiFluidSimulationContext();
	virtual ~UKawaiiFluidSimulationContext() override;

	/**
	 * Initialize solvers (called once when context is created)
	 */
	virtual void InitializeSolvers(const UKawaiiFluidPresetDataAsset* Preset);

	/**
	 * Main simulation entry point (Stateless)
	 *
	 * @param Particles - In/Out particle array
	 * @param Preset - Read-only preset parameters
	 * @param Params - Per-frame simulation parameters
	 * @param SpatialHash - In/Out spatial hash (rebuilt each frame)
	 * @param DeltaTime - Frame delta time
	 * @param AccumulatedTime - In/Out accumulated time for substeps
	 */
	virtual void Simulate(
		TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset,
		const FKawaiiFluidSimulationParams& Params,
		FSpatialHash& SpatialHash,
		float DeltaTime,
		float& AccumulatedTime
	);

	/**
	 * Single substep simulation
	 */
	virtual void SimulateSubstep(
		TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset,
		const FKawaiiFluidSimulationParams& Params,
		FSpatialHash& SpatialHash,
		float SubstepDT
	);

protected:
	//========================================
	// Simulation Steps (Virtual - Override for custom behaviors)
	//========================================

	/** 1. Apply external forces and predict positions */
	virtual void PredictPositions(
		TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset,
		const FVector& ExternalForce,
		float DeltaTime
	);

	/** 2. Update neighbor cache using spatial hash */
	virtual void UpdateNeighbors(
		TArray<FFluidParticle>& Particles,
		FSpatialHash& SpatialHash,
		float SmoothingRadius
	);

	/** 3. Solve density constraints (XPBD) */
	virtual void SolveDensityConstraints(
		TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset,
		float DeltaTime
	);

	/** 4. Handle collisions with registered colliders */
	virtual void HandleCollisions(
		TArray<FFluidParticle>& Particles,
		const TArray<UFluidCollider*>& Colliders
	);

	/** 5. Handle world geometry collision */
	virtual void HandleWorldCollision(
		TArray<FFluidParticle>& Particles,
		const FKawaiiFluidSimulationParams& Params,
		FSpatialHash& SpatialHash,
		float ParticleRadius
	);

	/** 6. Finalize positions and update velocities */
	virtual void FinalizePositions(
		TArray<FFluidParticle>& Particles,
		float DeltaTime
	);

	/** 7. Apply viscosity (XSPH) */
	virtual void ApplyViscosity(
		TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset
	);

	/** 8. Apply adhesion to surfaces */
	virtual void ApplyAdhesion(
		TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset,
		const TArray<UFluidCollider*>& Colliders
	);

	/** 3.5. Apply shape matching constraint (for slime) */
	virtual void ApplyShapeMatchingConstraint(
		TArray<FFluidParticle>& Particles,
		const FKawaiiFluidSimulationParams& Params
	);

	/** Update attached particle positions (bone tracking) */
	virtual void UpdateAttachedParticlePositions(
		TArray<FFluidParticle>& Particles,
		const TArray<UFluidInteractionComponent*>& InteractionComponents
	);

	/** Cache collider shapes (once per frame) */
	virtual void CacheColliderShapes(const TArray<UFluidCollider*>& Colliders);

protected:
	//========================================
	// Cached Solvers (Lazy initialization)
	//========================================

	/** Density constraint solver */
	TSharedPtr<FDensityConstraint> DensityConstraint;

	/** Viscosity solver */
	TSharedPtr<FViscositySolver> ViscositySolver;

	/** Adhesion solver */
	TSharedPtr<FAdhesionSolver> AdhesionSolver;

	/** Flag to check if solvers are initialized */
	bool bSolversInitialized = false;

	/** Ensure solvers are initialized */
	void EnsureSolversInitialized(const UKawaiiFluidPresetDataAsset* Preset);
};
