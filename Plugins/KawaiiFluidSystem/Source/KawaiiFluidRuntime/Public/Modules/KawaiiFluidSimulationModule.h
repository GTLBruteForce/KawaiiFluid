// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "GPU/GPUFluidSimulator.h"
#include "Components/KawaiiFluidInteractionComponent.h"
#include "KawaiiFluidSimulationModule.generated.h"

/** Collision event callback type */
DECLARE_DELEGATE_OneParam(FOnModuleCollisionEvent, const FKawaiiFluidCollisionEvent&);

class FSpatialHash;
class UKawaiiFluidPresetDataAsset;
class UFluidCollider;
class UKawaiiFluidInteractionComponent;
class UKawaiiFluidSimulationContext;
class UKawaiiFluidVolumeComponent;
class AKawaiiFluidVolume;

/**
 * Fluid simulation data module (UObject-based)
 *
 * Owns data required for simulation and provides Blueprint API.
 * Actual simulation logic is handled by UKawaiiFluidSimulationContext,
 * orchestration is managed by UKawaiiFluidSimulatorSubsystem.
 *
 * Responsibilities:
 * - Owns particle array
 * - Owns SpatialHash (for Independent mode)
 * - Manages collider/interaction component references
 * - Manages Preset reference
 * - Accumulates external forces
 * - Particle spawn/despawn API
 *
 * Usage:
 * - Included as Instanced in UKawaiiFluidComponent
 * - Blueprint functions directly callable
 */
UCLASS(DefaultToInstanced, EditInlineNew, BlueprintType)
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidSimulationModule : public UObject, public IKawaiiFluidDataProvider
{
	GENERATED_BODY()

public:
	UKawaiiFluidSimulationModule();

	// UObject interface
	virtual void PostInitProperties() override;
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	//========================================
	// Initialization / Cleanup
	//========================================

	/** Initialize module */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	virtual void Initialize(UKawaiiFluidPresetDataAsset* InPreset);

	/** Cleanup module */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void Shutdown();

	/** Check initialization state */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	bool IsInitialized() const { return bIsInitialized; }

	//========================================
	// Preset / Parameters
	//========================================

	/** Set preset */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SetPreset(UKawaiiFluidPresetDataAsset* InPreset);

	/** Get preset */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	UKawaiiFluidPresetDataAsset* GetPreset() const { return Preset; }

	/** Build simulation parameters */
	virtual FKawaiiFluidSimulationParams BuildSimulationParams() const;

	//========================================
	// Simulation Volume (Unified Volume System)
	//========================================

	/**
	 * Target Simulation Volume Actor (for sharing simulation space between multiple fluid components)
	 *
	 * When set, this module will use the external Volume's bounds for simulation.
	 * Multiple modules sharing the same Volume can interact with each other.
	 *
	 * When nullptr, the module uses its own internal volume settings configured below.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume", meta = (DisplayName = "Target Volume (External)"))
	TObjectPtr<AKawaiiFluidVolume> TargetSimulationVolume = nullptr;

	/**
	 * Use uniform (cube) size for simulation volume
	 * When checked, enter a single size value. When unchecked, enter separate X/Y/Z values.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides, DisplayName = "Uniform Size"))
	bool bUniformSize = true;

	/**
	 * Simulation volume size (cm) - cube dimensions when Uniform Size is checked
	 * Particles are confined within this box. Enter the full box size (not half).
	 *
	 * Example: 400 cm means a 400×400×400 cm cube.
	 * Default: 2560 cm (Medium Z-Order preset with CellSize=20)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr && bUniformSize", EditConditionHides,
			DisplayName = "Size", ClampMin = "10.0", ClampMax = "5120.0"))
	float UniformVolumeSize = 2560.0f;

	/**
	 * Simulation volume size (cm) - separate X/Y/Z dimensions
	 * Particles are confined within this box. Enter the full box size (not half).
	 *
	 * Example: (400, 300, 200) means a 400×300×200 cm box.
	 * Default: 2560 cm per axis (Medium Z-Order preset with CellSize=20)
	 *
	 * Note: Values exceeding the system maximum will be automatically clamped.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr && bUniformSize == false", EditConditionHides,
			DisplayName = "Size"))
	FVector VolumeSize = FVector(2560.0f, 2560.0f, 2560.0f);

	/**
	 * Simulation volume rotation
	 * Default (0,0,0) = axis-aligned box. Rotating creates an oriented box.
	 *
	 * Note: Large rotations may reduce the effective volume size due to internal constraints.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides, DisplayName = "Rotation"))
	FRotator VolumeRotation = FRotator::ZeroRotator;

	/**
	 * Grid resolution preset (auto-selected based on volume size)
	 * Read-only - the system automatically selects the optimal preset.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced",
		meta = (DisplayName = "Internal Grid Preset (Auto)"))
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;

	/** Internal cell size (auto-derived from fluid preset) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced",
		meta = (DisplayName = "Cell Size (Auto)"))
	float CellSize = 20.0f;

	/** Grid bits per axis */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	int32 GridAxisBits = 7;

	/** Grid resolution per axis */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	int32 GridResolution = 128;

	/** Total cell count */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	int32 MaxCells = 2097152;

	/** Internal bounds extent */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	float BoundsExtent = 2560.0f;

	/** World-space minimum bounds */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	FVector WorldBoundsMin = FVector(-1280.0f, -1280.0f, -1280.0f);

	/** World-space maximum bounds */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Simulation Volume|Advanced")
	FVector WorldBoundsMax = FVector(1280.0f, 1280.0f, 1280.0f);

	/** Get the target simulation volume actor (can be nullptr) */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	AKawaiiFluidVolume* GetTargetSimulationVolume() const { return TargetSimulationVolume; }

	/** Set the target simulation volume at runtime */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Simulation Volume")
	void SetTargetSimulationVolume(AKawaiiFluidVolume* NewSimulationVolume);

	/** Get the effective volume size (full size, cm)
	 * Returns UniformVolumeSize as FVector if bUniformSize is true, otherwise VolumeSize
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	FVector GetEffectiveVolumeSize() const
	{
		return bUniformSize ? FVector(UniformVolumeSize) : VolumeSize;
	}

	/** Get the volume half-extent (for internal collision/rendering use)
	 * Returns GetEffectiveVolumeSize() * 0.5
	 */
	FVector GetVolumeHalfExtent() const
	{
		return GetEffectiveVolumeSize() * 0.5f;
	}

	/** Recalculate internal volume bounds (call after changing size or owner location)
	 * Called automatically when properties change in editor
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Simulation Volume")
	void RecalculateVolumeBounds();

	/** Update volume info display from current effective source
	 * When TargetSimulationVolume is set, reads from external volume
	 * Otherwise uses internal CellSize to calculate
	 */
	void UpdateVolumeInfoDisplay();

	/**
	 * Called when Preset reference is changed externally (e.g., from owning Component)
	 * Handles delegate rebinding and CellSize synchronization
	 */
	void OnPresetChangedExternal(UKawaiiFluidPresetDataAsset* NewPreset);

	//========================================
	// Particle Data Access (IKawaiiFluidDataProvider implementation)
	//========================================

	/** Particle array (read-only) - IKawaiiFluidDataProvider::GetParticles() */
	virtual const TArray<FFluidParticle>& GetParticles() const override { return Particles; }

	/** Particle array (mutable - for Subsystem/Context) */
	TArray<FFluidParticle>& GetParticlesMutable() { return Particles; }

	/** Particle count - IKawaiiFluidDataProvider::GetParticleCount() */
	/** GPU mode: returns GPU particle count, CPU mode: returns Particles.Num() */
	UFUNCTION(BlueprintPure, Category = "Fluid")
	virtual int32 GetParticleCount() const override
	{
		if (bGPUSimulationActive)
		{
			if (TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin())
			{
				// Include both existing GPU particles AND pending spawn requests
				return GPUSim->GetParticleCount() + GPUSim->GetPendingSpawnCount();
			}
		}
		return Particles.Num();
	}

	/**
	 * Get particle count for a specific source (component)
	 * Uses GPU source counters for per-component tracking (2-3 frame latency)
	 * @param SourceID - Source component ID (0 to MaxSourceCount-1)
	 * @return Particle count for the specified source, 0 if invalid or not found
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid")
	int32 GetParticleCountForSource(int32 SourceID) const;

	//========================================
	// Particle Spawn/Despawn
	//========================================

	/** Spawn single particle */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticle(FVector Position, FVector Velocity = FVector::ZeroVector);

	/** Spawn multiple particles with random distribution (for Point mode, legacy compatibility) */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void SpawnParticles(FVector Location, int32 Count, float SpawnRadius);

	/** Spawn particles in sphere with grid distribution (for Sphere mode)
	 * @param Center Sphere center
	 * @param Radius Sphere radius
	 * @param Spacing Particle spacing
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation (shape is spherical, so only applied to velocity)
	 * @return Number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesSphere(FVector Center, float Radius, float Spacing,
	                           bool bJitter = true, float JitterAmount = 0.2f,
	                           FVector Velocity = FVector::ZeroVector,
	                           FRotator Rotation = FRotator::ZeroRotator);

	/** Spawn particles in box with grid distribution (for Box mode)
	 * @param Center Box center
	 * @param Extent Box Half Extent (X, Y, Z)
	 * @param Spacing Particle spacing
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation
	 * @return Number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesBox(FVector Center, FVector Extent, float Spacing,
	                        bool bJitter = true, float JitterAmount = 0.2f,
	                        FVector Velocity = FVector::ZeroVector,
	                        FRotator Rotation = FRotator::ZeroRotator);

	/** Spawn particles in cylinder with grid distribution (for Cylinder mode)
	 * @param Center Cylinder center
	 * @param Radius Cylinder radius
	 * @param HalfHeight Cylinder half height (Z-axis)
	 * @param Spacing Particle spacing
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation
	 * @return Number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesCylinder(FVector Center, float Radius, float HalfHeight, float Spacing,
	                             bool bJitter = true, float JitterAmount = 0.2f,
	                             FVector Velocity = FVector::ZeroVector,
	                             FRotator Rotation = FRotator::ZeroRotator);

	//========================================
	// Hexagonal Grid Spawn Functions (stable initial state)
	//========================================

	/** Spawn particles in box with Hexagonal Close Packing
	 * Provides more stable initial density distribution than cubic grid
	 * @param Center Box center
	 * @param Extent Box Half Extent (X, Y, Z)
	 * @param Spacing Particle spacing
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation
	 * @return Number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesBoxHexagonal(FVector Center, FVector Extent, float Spacing,
	                                  bool bJitter = true, float JitterAmount = 0.2f,
	                                  FVector Velocity = FVector::ZeroVector,
	                                  FRotator Rotation = FRotator::ZeroRotator);

	/** Spawn particles in sphere with Hexagonal Close Packing
	 * @param Center Sphere center
	 * @param Radius Sphere radius
	 * @param Spacing Particle spacing
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation
	 * @return Number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesSphereHexagonal(FVector Center, float Radius, float Spacing,
	                                     bool bJitter = true, float JitterAmount = 0.2f,
	                                     FVector Velocity = FVector::ZeroVector,
	                                     FRotator Rotation = FRotator::ZeroRotator);

	/** Spawn particles in cylinder with Hexagonal Close Packing
	 * @param Center Cylinder center
	 * @param Radius Cylinder radius
	 * @param HalfHeight Cylinder half height
	 * @param Spacing Particle spacing
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation
	 * @return Number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesCylinderHexagonal(FVector Center, float Radius, float HalfHeight, float Spacing,
	                                       bool bJitter = true, float JitterAmount = 0.2f,
	                                       FVector Velocity = FVector::ZeroVector,
	                                       FRotator Rotation = FRotator::ZeroRotator);

	//========================================
	// Explicit Count Spawn Functions
	//========================================

	/** Spawn specified number of particles in sphere
	 * @param Center Sphere center
	 * @param Radius Sphere radius
	 * @param Count Number of particles to spawn
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation (shape is spherical, so only applied to velocity)
	 * @return Actual number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesSphereByCount(FVector Center, float Radius, int32 Count,
	                                  bool bJitter = true, float JitterAmount = 0.2f,
	                                  FVector Velocity = FVector::ZeroVector,
	                                  FRotator Rotation = FRotator::ZeroRotator);

	/** Spawn specified number of particles in box
	 * @param Center Box center
	 * @param Extent Box Half Extent (X, Y, Z)
	 * @param Count Number of particles to spawn
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation
	 * @return Actual number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesBoxByCount(FVector Center, FVector Extent, int32 Count,
	                               bool bJitter = true, float JitterAmount = 0.2f,
	                               FVector Velocity = FVector::ZeroVector,
	                               FRotator Rotation = FRotator::ZeroRotator);

	/** Spawn specified number of particles in cylinder
	 * @param Center Cylinder center
	 * @param Radius Cylinder radius
	 * @param HalfHeight Cylinder half height (Z-axis)
	 * @param Count Number of particles to spawn
	 * @param bJitter Apply random offset
	 * @param JitterAmount Random offset ratio (0~0.5)
	 * @param Velocity Initial velocity
	 * @param Rotation Local→World rotation
	 * @return Actual number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticlesCylinderByCount(FVector Center, float Radius, float HalfHeight, int32 Count,
	                                    bool bJitter = true, float JitterAmount = 0.2f,
	                                    FVector Velocity = FVector::ZeroVector,
	                                    FRotator Rotation = FRotator::ZeroRotator);

	/** Spawn directional particles (for Spout/Spray mode)
	 * @param Position Spawn position
	 * @param Direction Emission direction (normalized)
	 * @param Speed Initial velocity magnitude
	 * @param Radius Stream radius (dispersion range)
	 * @param ConeAngle Spray angle (degrees, 0 = straight line)
	 * @return Spawned particle ID
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticleDirectional(FVector Position, FVector Direction, float Speed,
	                               float Radius = 0.0f, float ConeAngle = 0.0f);

	/** Spawn circular cross-section layer with Hexagonal Packing (for Stream mode)
	 * @param Position Layer center position
	 * @param Direction Emission direction (normalized)
	 * @param Speed Initial velocity magnitude
	 * @param Radius Stream radius
	 * @param Spacing Particle spacing (0 = auto-calculate as SmoothingRadius * 0.5)
	 * @param Jitter Position random offset ratio (0~0.5, 0=perfect grid, 0.5=max natural)
	 * @return Number of spawned particles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticleDirectionalHexLayer(FVector Position, FVector Direction, float Speed,
	                                        float Radius, float Spacing = 0.0f, float Jitter = 0.15f);

	/** C++ only: Batch version that collects requests without sending. Caller must send batch manually. */
	int32 SpawnParticleDirectionalHexLayerBatch(FVector Position, FVector Direction, float Speed,
	                                             float Radius, float Spacing, float Jitter,
	                                             TArray<FGPUSpawnRequest>& OutBatch);

	/** Remove all particles for this module's SourceID (GPU-driven) */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ClearAllParticles();

	/** Remove particles within radius (GPU-driven, no readback dependency)
	 * @param Center World position of brush center
	 * @param Radius Brush radius
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void DespawnByBrushGPU(FVector Center, float Radius);

	/** Remove all particles with specific SourceID (GPU-driven)
	 * @param SourceID Source component ID to despawn
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void DespawnBySourceGPU(int32 SourceID);

	/** Remove N oldest particles (GPU-driven histogram, no readback dependency)
	 * @param IncomingSpawnCount Number of particles about to be spawned (GPU computes actual removal)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void DespawnOldestGPU(int32 IncomingSpawnCount);

	/** Get particle positions array */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	TArray<FVector> GetParticlePositions() const;

	/** Get particle velocities array */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	TArray<FVector> GetParticleVelocities() const;

	//========================================
	// External Forces
	//========================================

	/** Apply external force to all particles (accumulated) */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyExternalForce(FVector Force);

	/** Apply force to specific particle */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyForceToParticle(int32 ParticleIndex, FVector Force);

	/** Get accumulated external force */
	FVector GetAccumulatedExternalForce() const { return AccumulatedExternalForce; }

	/** Reset accumulated external force */
	void ResetExternalForce() { AccumulatedExternalForce = FVector::ZeroVector; }

	//========================================
	// Collider Management
	//========================================

	/** Register collider */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void RegisterCollider(UFluidCollider* Collider);

	/** Unregister collider */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void UnregisterCollider(UFluidCollider* Collider);

	/** Clear all colliders */
	void ClearColliders() { Colliders.Empty(); }

	/** Get registered collider list */
	const TArray<TObjectPtr<UFluidCollider>>& GetColliders() const { return Colliders; }

	//========================================
	// SpatialHash (for Independent mode)
	//========================================

	/** Get SpatialHash */
	FSpatialHash* GetSpatialHash() const { return SpatialHash.Get(); }

	/** Initialize SpatialHash */
	void InitializeSpatialHash(float InCellSize);

	//========================================
	// Time Management (for Substep)
	//========================================

	/** Accumulated time */
	float GetAccumulatedTime() const { return AccumulatedTime; }
	void SetAccumulatedTime(float Time) { AccumulatedTime = Time; }

	//========================================
	// Query
	//========================================

	/** Find particle indices within radius */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	TArray<int32> GetParticlesInRadius(FVector Location, float Radius) const;

	/** Find particle indices within box */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	TArray<int32> GetParticlesInBox(FVector Center, FVector Extent) const;

	/** Get particle information */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	bool GetParticleInfo(int32 ParticleIndex, FVector& OutPosition, FVector& OutVelocity, float& OutDensity) const;

	//========================================
	// Simulation Control
	//========================================

	/** Enable/disable simulation */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SetSimulationEnabled(bool bEnabled) { bSimulationEnabled = bEnabled; }

	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	bool IsSimulationEnabled() const { return bSimulationEnabled; }

	/** Independent mode (not batch processed by Subsystem) */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SetIndependentSimulation(bool bIndependent) { bIndependentSimulation = bIndependent; }

	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	bool IsIndependentSimulation() const { return bIndependentSimulation; }

	//========================================
	// Context (Outer chain cache)
	//========================================

	/** Get Owner Actor (cached) */
	UFUNCTION(BlueprintPure, Category = "Fluid|Module")
	AActor* GetOwnerActor() const;

	//========================================
	// Collision Settings
	//========================================

	/** Enable World Collision */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision")
	bool bUseWorldCollision = true;

	//========================================
	// Volume Visualization
	//========================================

	/** Bounds wireframe color (green box showing simulation bounds in editor) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides, DisplayName = "Wireframe Color"))
	FColor VolumeWireframeColor = FColor::Green;

	/** Show internal grid space wireframe (for debugging spatial partitioning) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume|Advanced",
		meta = (EditCondition = "TargetSimulationVolume == nullptr", EditConditionHides, DisplayName = "Show Internal Grid Wireframe"))
	bool bShowZOrderSpaceWireframe = false;

	/** Internal grid wireframe color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Simulation Volume|Advanced",
		meta = (EditCondition = "TargetSimulationVolume == nullptr && bShowZOrderSpaceWireframe", EditConditionHides, DisplayName = "Grid Wireframe Color"))
	FColor ZOrderSpaceWireframeColor = FColor::Red;

	//========================================
	// Simulation Bounds API
	//========================================

	/** Set simulation bounds at runtime
	 * @param Size Full size of the simulation volume (cm)
	 * @param Rotation World-space rotation of the volume
	 * @param Bounce Wall bounce coefficient (0-1)
	 * @param Friction Wall friction coefficient (0-1)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Simulation Volume")
	void SetSimulationVolume(const FVector& Size, const FRotator& Rotation, float Bounce, float Friction);

	/** Resolve volume boundary collisions - keeps particles inside */
	void ResolveVolumeBoundaryCollisions();

	//========================================
	// Legacy API (Deprecated)
	//========================================

	/** @deprecated Use VolumeSize/UniformVolumeSize. Bounce/Friction are now from Preset (Restitution/Friction) */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Containment", meta = (DeprecatedFunction, DeprecationMessage = "Use SetSimulationVolume instead"))
	void SetContainment(bool bEnabled, const FVector& Center, const FVector& Extent,
	                    const FQuat& Rotation, float Restitution, float Friction);

	/** @deprecated Use ResolveVolumeBoundaryCollisions instead */
	void ResolveContainmentCollisions();

	//========================================
	// Event Settings
	//========================================

	/** Enable collision events */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events")
	bool bEnableCollisionEvents = false;

	/** Minimum velocity for event triggering (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "0.0", EditCondition = "bEnableCollisionEvents"))
	float MinVelocityForEvent = 50.0f;

	/** Maximum events per frame (0 = unlimited) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "0", EditCondition = "bEnableCollisionEvents"))
	int32 MaxEventsPerFrame = 10;

	/** Event cooldown per particle (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "0.0", EditCondition = "bEnableCollisionEvents"))
	float EventCooldownPerParticle = 0.1f;

	/** Set collision event callback */
	void SetCollisionEventCallback(FOnModuleCollisionEvent InCallback) { OnCollisionEventCallback = InCallback; }

	/** Get collision event callback */
	const FOnModuleCollisionEvent& GetCollisionEventCallback() const { return OnCollisionEventCallback; }

	/** Process collision feedback (GPU + CPU unified)
	 * Called by Subsystem after simulation. Processes both GPU buffer + CPU buffer.
	 * @param OwnerIDToIC - OwnerID→IC map built by Subsystem (for O(1) lookup)
	 * @param CPUFeedbackBuffer - Subsystem's CPU collision feedback buffer (filtered by SourceID)
	 */
	void ProcessCollisionFeedback(
		const TMap<int32, UKawaiiFluidInteractionComponent*>& OwnerIDToIC,
		const TArray<FKawaiiFluidCollisionEvent>& CPUFeedbackBuffer);

	//========================================
	// Preset (internal cache - set by Component)
	//========================================

	/** Cached preset reference (set by owning Component via Initialize/SetPreset)
	 * Note: Component owns the Preset, Module just caches reference for simulation
	 */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset;

private:
	//========================================
	// Internal
	//========================================

	/** Collision event callback */
	FOnModuleCollisionEvent OnCollisionEventCallback;

	//========================================
	// Data
	//========================================

	/** Particle array - supports both editor serialization + SaveGame */
	UPROPERTY()
	TArray<FFluidParticle> Particles;

	/** Spatial hashing (for Independent mode) */
	TSharedPtr<FSpatialHash> SpatialHash;

	/** Registered colliders */
	UPROPERTY()
	TArray<TObjectPtr<UFluidCollider>> Colliders;

	/** Accumulated external force */
	FVector AccumulatedExternalForce = FVector::ZeroVector;

	/** Substep time accumulation */
	float AccumulatedTime = 0.0f;

	/** Volume center (world space, set dynamically from Component location) */
	FVector VolumeCenter = FVector::ZeroVector;

	/** Volume rotation as quaternion (computed from VolumeRotation) */
	FQuat VolumeRotationQuat = FQuat::Identity;

	/** Simulation enabled */
	bool bSimulationEnabled = true;

	/** Independent mode flag */
	bool bIndependentSimulation = true;

	/** Initialization state */
	bool bIsInitialized = false;

	/** Last event time per particle (for cooldown tracking) */
	TMap<int32, float> ParticleLastEventTime;

public:
	/** Get particle last event time map (for cooldown tracking) */
	TMap<int32, float>& GetParticleLastEventTimeMap() { return ParticleLastEventTime; }

	//========================================
	// IKawaiiFluidDataProvider Interface (remaining methods)
	//========================================

	/** Get particle radius (simulation actual radius) - IKawaiiFluidDataProvider */
	virtual float GetParticleRadius() const override;

	/** Data validity check - IKawaiiFluidDataProvider */
	/** GPU mode: checks GPU particle count, CPU mode: checks Particles.Num() */
	virtual bool IsDataValid() const override
	{
		if (bGPUSimulationActive)
		{
			if (TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin())
			{
				return GPUSim->GetParticleCount() > 0 || GPUSim->GetPendingSpawnCount() > 0;
			}
		}
		return Particles.Num() > 0;
	}

	/** Get debug name - IKawaiiFluidDataProvider */
	virtual FString GetDebugName() const override;

	//========================================
	// GPU Buffer Access (Phase 2) - IKawaiiFluidDataProvider
	//========================================

	/** Check if GPU simulation is active */
	virtual bool IsGPUSimulationActive() const override;

	/** Get GPU particle count */
	virtual int32 GetGPUParticleCount() const override;

	/** Get GPU simulator instance */
	virtual FGPUFluidSimulator* GetGPUSimulator() const override { return WeakGPUSimulator.Pin().Get(); }

	/** Set GPU simulator reference (called by Context when GPU mode is active) */
	void SetGPUSimulator(const TSharedPtr<FGPUFluidSimulator>& InSimulator) { WeakGPUSimulator = InSimulator; }

	/** Set GPU simulation active flag */
	void SetGPUSimulationActive(bool bActive) { bGPUSimulationActive = bActive; }

	//========================================
	// GPU ↔ CPU Particle Sync (PIE/Serialization)
	//========================================

	/**
	 * Sync GPU particles to CPU Particles array
	 * Called on save (PreSave) and PIE transition (PreBeginPIE)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Module")
	void SyncGPUParticlesToCPU();

	/**
	 * Upload CPU Particles array to GPU
	 * Called on load (PostLoad) and PIE start (BeginPlay)
	 */
	void UploadCPUParticlesToGPU();

	//========================================
	// Simulation Context Reference
	//========================================

	/** Get the simulation context associated with this module
	 * Context is owned by SimulatorSubsystem and shared by all modules with same preset
	 * Returns nullptr if module is not registered with subsystem
	 */
	UKawaiiFluidSimulationContext* GetSimulationContext() const { return CachedSimulationContext; }

	/** Set the simulation context reference (called by SimulatorSubsystem on registration) */
	void SetSimulationContext(UKawaiiFluidSimulationContext* InContext) { CachedSimulationContext = InContext; }

	//========================================
	// Volume Component Access (Internal)
	//========================================

	/**
	 * Get the effective volume component for Z-Order space bounds
	 * Returns external TargetSimulationVolume's component if set,
	 * otherwise returns internal OwnedVolumeComponent
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	UKawaiiFluidVolumeComponent* GetTargetVolumeComponent() const;

	/**
	 * Get the internally owned volume component (always valid after initialization)
	 * This is used when no external TargetSimulationVolume is set
	 */
	UKawaiiFluidVolumeComponent* GetOwnedVolumeComponent() const { return OwnedVolumeComponent; }

	/**
	 * Check if using external volume (TargetSimulationVolume is set)
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	bool IsUsingExternalVolume() const { return TargetSimulationVolume != nullptr; }

	//========================================
	// Source Identification
	//========================================

	/** Set source ID for spawned particles (Component's unique ID) */
	void SetSourceID(int32 InSourceID);

	/** Get cached source ID */
	int32 GetSourceID() const { return CachedSourceID; }

private:
	/** Weak reference to GPU simulator (owned by SimulationContext via TSharedPtr) */
	TWeakPtr<FGPUFluidSimulator> WeakGPUSimulator;

	/** GPU simulation active flag */
	bool bGPUSimulationActive = false;

	/** Cached simulation context pointer (owned by SimulatorSubsystem) */
	UKawaiiFluidSimulationContext* CachedSimulationContext = nullptr;

	/**
	 * Internally owned volume component for Z-Order space bounds
	 * Created automatically during Initialize(), used when TargetSimulationVolume is nullptr
	 */
	UPROPERTY(Transient)
	TObjectPtr<UKawaiiFluidVolumeComponent> OwnedVolumeComponent = nullptr;

	/**
	 * Previously registered volume component (for editor unregistration tracking)
	 * Used to properly unregister when TargetSimulationVolume changes in editor
	 */
	TWeakObjectPtr<UKawaiiFluidVolumeComponent> PreviousRegisteredVolume = nullptr;

	/** Called when the TargetSimulationVolume actor is destroyed */
	UFUNCTION()
	void OnTargetVolumeDestroyed(AActor* DestroyedActor);

	/** Bind/Unbind to TargetSimulationVolume's OnDestroyed delegate */
	void BindToVolumeDestroyedEvent();
	void UnbindFromVolumeDestroyedEvent();

	/** Whether we're currently bound to the volume's OnDestroyed event */
	bool bBoundToVolumeDestroyed = false;

#if WITH_EDITOR
	/** Callback when Preset's properties change (SmoothingRadius, etc.) */
	void OnPresetPropertyChanged(UKawaiiFluidPresetDataAsset* ChangedPreset);

	/** Bind/Unbind to Preset's OnPropertyChanged delegate */
	void BindToPresetPropertyChanged();
	void UnbindFromPresetPropertyChanged();

	/** Delegate handle for preset property changes */
	FDelegateHandle PresetPropertyChangedHandle;

	/** Callback when objects are replaced (e.g., asset reload) */
	void OnObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap);

	/** Bind/Unbind to FCoreUObjectDelegates::OnObjectsReplaced */
	void BindToObjectsReplaced();
	void UnbindFromObjectsReplaced();

	/** Delegate handle for objects replaced event */
	FDelegateHandle ObjectsReplacedHandle;

	/** Sync GPU particles to CPU before PIE starts */
	void OnPreBeginPIE(bool bIsSimulating);

	/** Delegate handle for PreBeginPIE */
	FDelegateHandle PreBeginPIEHandle;
#endif

	/** Cached source ID for spawned particles (Component's unique ID, -1 = invalid) */
	int32 CachedSourceID = -1;
};
