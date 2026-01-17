// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Collision/PerPolygonCollisionProcessor.h"
#include "GPU/GPUFluidParticle.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Components/KawaiiFluidSimulationVolumeComponent.h"
#include "KawaiiFluidSimulationContext.generated.h"

// Forward declarations
class FSpatialHash;
class FDensityConstraint;
class FViscositySolver;
class FAdhesionSolver;
class FStackPressureSolver;
class FGPUFluidSimulator;
class FKawaiiFluidRenderResource;
struct FGPUFluidSimulationParams;

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

	//========================================
	// GPU Simulation Mode
	//========================================

	/**
	 * Initialize GPU simulator (call before using GPU mode)
	 * GPU simulation is always used
	 */
	virtual void InitializeGPUSimulator(int32 MaxParticleCount);

	/**
	 * Release GPU simulator resources
	 */
	virtual void ReleaseGPUSimulator();

	/**
	 * Check if GPU simulator is ready
	 */
	bool IsGPUSimulatorReady() const;

	/**
	 * Get GPU simulator pointer (for Phase 2 GPU→GPU rendering)
	 */
	FGPUFluidSimulator* GetGPUSimulator() const { return GPUSimulator.Get(); }

	/**
	 * Get the cached preset associated with this context
	 * Returns nullptr if preset has been garbage collected
	 */
	UKawaiiFluidPresetDataAsset* GetCachedPreset() const { return CachedPreset.Get(); }

	/**
	 * Set the preset associated with this context
	 */
	void SetCachedPreset(UKawaiiFluidPresetDataAsset* InPreset) { CachedPreset = InPreset; }

	/** Mark GPU world-collision cache dirty (rebuild on next GPU sim) */
	void MarkGPUWorldCollisionCacheDirty() { bGPUWorldCollisionCacheDirty = true; }

	//========================================
	// Target Volume Component (Z-Order Space Bounds)
	//========================================

	/**
	 * Get the target volume component for Z-Order space bounds
	 * When set, this context uses the Volume's bounds instead of Preset bounds
	 */
	UKawaiiFluidSimulationVolumeComponent* GetTargetVolumeComponent() const { return TargetVolumeComponent.Get(); }

	/**
	 * Set the target volume component for Z-Order space bounds
	 * @param InVolumeComponent The volume component to use (nullptr = use Preset bounds)
	 */
	void SetTargetVolumeComponent(UKawaiiFluidSimulationVolumeComponent* InVolumeComponent) { TargetVolumeComponent = InVolumeComponent; }

	//========================================
	// Render Resource (배치 렌더링용)
	// Context가 소유하여 같은 Context를 공유하는 렌더러들이
	// 동일한 RenderResource를 사용 → Draw Call 감소
	//========================================

	/**
	 * Initialize render resource for this context
	 * Called when context is first used for rendering
	 */
	void InitializeRenderResource();

	/**
	 * Release render resource
	 */
	void ReleaseRenderResource();

	/**
	 * Get shared render resource raw pointer (for quick access)
	 */
	FKawaiiFluidRenderResource* GetRenderResource() const { return RenderResource.Get(); }

	/**
	 * Get shared render resource as TSharedPtr (for sharing ownership)
	 */
	TSharedPtr<FKawaiiFluidRenderResource> GetRenderResourceShared() const { return RenderResource; }

	/**
	 * Check if render resource is valid
	 */
	bool HasValidRenderResource() const { return RenderResource.IsValid(); }

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
		const TArray<TObjectPtr<UFluidCollider>>& Colliders,
		float SubstepDT
	);

	/** 5. Handle world geometry collision (dispatches to Sweep or SDF based on Params) */
	virtual void HandleWorldCollision(
		TArray<FFluidParticle>& Particles,
		const FKawaiiFluidSimulationParams& Params,
		FSpatialHash& SpatialHash,
		float ParticleRadius,
		float SubstepDT,
		float Friction = 0.5f,
		float Restitution = 0.0f
	);

	/** 5a. Legacy sweep-based world collision (SweepSingleByChannel) */
	virtual void HandleWorldCollision_Sweep(
		TArray<FFluidParticle>& Particles,
		const FKawaiiFluidSimulationParams& Params,
		FSpatialHash& SpatialHash,
		float ParticleRadius,
		float SubstepDT,
		float Friction,
		float Restitution
	);

	/** 5b. SDF-based world collision (Overlap + ClosestPoint) */
	virtual void HandleWorldCollision_SDF(
		TArray<FFluidParticle>& Particles,
		const FKawaiiFluidSimulationParams& Params,
		FSpatialHash& SpatialHash,
		float ParticleRadius,
		float SubstepDT,
		float Friction,
		float Restitution
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
		const TArray<TObjectPtr<UFluidCollider>>& Colliders
	);

	/** 9. Apply cohesion (surface tension between particles) */
	virtual void ApplyCohesion(
		TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset
	);

	/** Collect simulation statistics for debugging/comparison (CPU) */
	virtual void CollectSimulationStats(
		const TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset,
		int32 SubstepCount,
		bool bIsGPU
	);

	/** Collect GPU simulation statistics (without particle readback) */
	virtual void CollectGPUSimulationStats(
		const UKawaiiFluidPresetDataAsset* Preset,
		int32 ParticleCount,
		int32 SubstepCount
	);

	/** 3.5. Apply shape matching constraint (for slime) */
	virtual void ApplyShapeMatchingConstraint(
		TArray<FFluidParticle>& Particles,
		const FKawaiiFluidSimulationParams& Params
	);

	/** Update attached particle positions (bone tracking) */
	virtual void UpdateAttachedParticlePositions(
		TArray<FFluidParticle>& Particles,
		const TArray<TObjectPtr<UFluidInteractionComponent>>& InteractionComponents
	);

	/** Cache collider shapes (once per frame) */
	virtual void CacheColliderShapes(const TArray<TObjectPtr<UFluidCollider>>& Colliders);

	/** Append cached world-collision primitives (GPU) using channel-filtered world query */
	void AppendGPUWorldCollisionPrimitives(
		FGPUCollisionPrimitives& OutPrimitives,
		const FKawaiiFluidSimulationParams& Params,
		const FBox& QueryBounds,
		float DefaultFriction,
		float DefaultRestitution,
		const TSet<AActor*>& PerPolygonActors
	);

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

	/** Stack pressure solver (weight transfer from stacked attached particles) */
	TSharedPtr<FStackPressureSolver> StackPressureSolver;

	/** Flag to check if solvers are initialized */
	bool bSolversInitialized = false;

	/** Ensure solvers are initialized */
	void EnsureSolversInitialized(const UKawaiiFluidPresetDataAsset* Preset);

	//========================================
	// GPU Simulation
	//========================================

	/** GPU fluid simulator instance */
	TSharedPtr<FGPUFluidSimulator> GPUSimulator;

	//========================================
	// Render Resource (배치 렌더링용)
	//========================================

	/** Shared render resource for all renderers using this context */
	TSharedPtr<FKawaiiFluidRenderResource> RenderResource;

	/** Per-Polygon collision processor for CPU-based skeletal mesh triangle collision */
	TUniquePtr<FPerPolygonCollisionProcessor> PerPolygonProcessor;

	/**
	 * Simulate using GPU compute shaders
	 * Always uses GPU simulation
	 */
	virtual void SimulateGPU(
		TArray<FFluidParticle>& Particles,
		const UKawaiiFluidPresetDataAsset* Preset,
		const FKawaiiFluidSimulationParams& Params,
		FSpatialHash& SpatialHash,
		float DeltaTime,
		float& AccumulatedTime
	);

	/**
	 * Build GPU simulation parameters from preset and frame params
	 */
	FGPUFluidSimulationParams BuildGPUSimParams(
		const UKawaiiFluidPresetDataAsset* Preset,
		const FKawaiiFluidSimulationParams& Params,
		float SubstepDT
	) const;

	/**
	 * Separate attached particles for CPU handling
	 * @return Indices of attached particles
	 */
	TArray<int32> ExtractAttachedParticleIndices(const TArray<FFluidParticle>& Particles) const;

	/**
	 * Handle attached particles on CPU (bone tracking, adhesion, per-polygon collision)
	 */
	virtual void HandleAttachedParticlesCPU(
		TArray<FFluidParticle>& Particles,
		const TArray<int32>& AttachedIndices,
		const UKawaiiFluidPresetDataAsset* Preset,
		const FKawaiiFluidSimulationParams& Params,
		float SubstepDT
	);

	//========================================
	// Persistent Bone Transform Data (for GPU adhesion)
	//========================================

	/** Bone transforms persisted across frames for velocity calculation */
	TArray<FGPUBoneTransform> PersistentBoneTransforms;

	/** Bone name to index mapping persisted across frames */
	TMap<FName, int32> PersistentBoneNameToIndex;

	/** Weak reference to the preset associated with this context */
	TWeakObjectPtr<UKawaiiFluidPresetDataAsset> CachedPreset;

	/** Weak reference to the target volume component for Z-Order space bounds */
	TWeakObjectPtr<UKawaiiFluidSimulationVolumeComponent> TargetVolumeComponent;

	//========================================
	// GPU World Collision Cache (channel-based)
	//========================================

	/** Cached primitives built from world collision channel */
	FGPUCollisionPrimitives CachedGPUWorldCollisionPrimitives;

	/** Cached query bounds for world collision */
	FBox CachedGPUWorldCollisionBounds = FBox(EForceInit::ForceInit);

	/** Cached world pointer for world collision */
	TWeakObjectPtr<UWorld> CachedGPUWorldCollisionWorld;

	/** Cached collision channel */
	TEnumAsByte<ECollisionChannel> CachedGPUWorldCollisionChannel = ECC_MAX;

	/** Cached world collision primitives dirty flag */
	bool bGPUWorldCollisionCacheDirty = true;

	/** Static boundary particles need regeneration flag */
	bool bStaticBoundaryParticlesDirty = true;
};
