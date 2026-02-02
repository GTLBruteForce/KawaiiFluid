// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "KawaiiFluidEmitterComponent.generated.h"

class AKawaiiFluidEmitter;
class AKawaiiFluidVolume;
class UKawaiiFluidSimulationModule;
class UBillboardComponent;
class APawn;

/**
 * Emitter type for KawaiiFluidEmitterComponent
 */
UENUM(BlueprintType)
enum class EKawaiiFluidEmitterMode : uint8
{
	/** One-time fill of a shape volume with particles (hexagonal pattern) */
	Fill UMETA(DisplayName = "Fill"),
	
	/** Continuous hexagonal stream emission (like a faucet) */
	Stream UMETA(DisplayName = "Stream")
};

/**
 * Shape type for Shape emitter mode
 */
UENUM(BlueprintType)
enum class EKawaiiFluidEmitterShapeType : uint8
{
	/** Spherical volume */
	Sphere UMETA(DisplayName = "Sphere"),
	
	/** Cube volume */
	Cube UMETA(DisplayName = "Cube"),
	
	/** Cylindrical volume */
	Cylinder UMETA(DisplayName = "Cylinder")
};

/**
 * Kawaii Fluid Emitter Component
 *
 * Component that handles particle spawning logic for AKawaiiFluidEmitter.
 * Supports two modes: Fill (one-time hexagonal fill) and Stream (continuous hexagonal stream).
 *
 * All configuration properties are stored in this component.
 * The AKawaiiFluidEmitter actor provides the world presence.
 *
 * Responsibilities:
 * - Manage target volume reference and registration
 * - Execute spawn logic based on EmitterMode
 * - Spawn particles in various shapes (sphere, cube, cylinder) with hexagonal packing
 * - Handle continuous stream spawning with hexagonal layers
 * - Track spawn state and accumulated time
 */
UCLASS(ClassGroup = (KawaiiFluid), meta = (BlueprintSpawnableComponent, DisplayName = "Kawaii Fluid Emitter"))
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidEmitterComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UKawaiiFluidEmitterComponent();

	//========================================
	// UActorComponent Interface
	//========================================

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	//========================================
	// Emitter
	//========================================

	/** Enable or disable particle emission */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter")
	bool bEnabled = true;

	/** The target volume to emit particles into.
	 *  If not set, automatically finds the nearest volume at BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter")
	TObjectPtr<AKawaiiFluidVolume> TargetVolume;

	/** Emitter mode: Fill (one-time fill) or Stream (continuous emission) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter")
	EKawaiiFluidEmitterMode EmitterMode = EKawaiiFluidEmitterMode::Stream;

	/** Shape type for Fill mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Fill Shape",
		meta = (EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Fill", EditConditionHides))
	EKawaiiFluidEmitterShapeType ShapeType = EKawaiiFluidEmitterShapeType::Sphere;

	/** Sphere radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Fill Shape",
		meta = (EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Fill && ShapeType == EKawaiiFluidEmitterShapeType::Sphere", EditConditionHides, ClampMin = "1.0"))
	float SphereRadius = 50.0f;

	/** Cube half-size (size / 2) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Fill Shape",
		meta = (EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Fill && ShapeType == EKawaiiFluidEmitterShapeType::Cube", EditConditionHides))
	FVector CubeHalfSize = FVector(50.0f, 50.0f, 50.0f);

	/** Cylinder radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Fill Shape",
		meta = (EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Fill && ShapeType == EKawaiiFluidEmitterShapeType::Cylinder", EditConditionHides, ClampMin = "1.0"))
	float CylinderRadius = 30.0f;

	/** Cylinder half-height */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Fill Shape",
		meta = (EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Fill && ShapeType == EKawaiiFluidEmitterShapeType::Cylinder", EditConditionHides, ClampMin = "1.0"))
	float CylinderHalfHeight = 50.0f;

	/** Stream cross-sectional radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Stream",
		meta = (EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Stream", EditConditionHides, ClampMin = "1.0"))
	float StreamRadius = 25.0f;

	/** Layers per second (target spawn rate).
	 *  Higher values create denser streams.
	 *  Position spacing = InitialSpeed / LayersPerSecond.
	 *  Note: Limited to 1 layer per frame to prevent explosion during frame drops.
	 *  Effective rate may be lower than this value at low FPS. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Stream",
		meta = (DisplayName = "Layers Per Second",
		        EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Stream", 
		        EditConditionHides, ClampMin = "1.0", ClampMax = "300.0"))
	float LayersPerSecond = 60.0f;

	/** Enable jitter for stream particles.
	 *  Adds random offset to particle positions for natural appearance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Stream",
		meta = (EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Stream", EditConditionHides))
	bool bUseStreamJitter = false;

	/** Jitter amount as a fraction of particle spacing (0.0 ~ 0.5).
	 *  0.15 = 15% of spacing as max random offset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Stream",
		meta = (EditCondition = "EmitterMode == EKawaiiFluidEmitterMode::Stream && bUseStreamJitter", 
		        EditConditionHides, ClampMin = "0.0", ClampMax = "0.5"))
	float StreamJitterAmount = 0.15f;

	/** Use world space for velocity direction (if false, uses local space) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Velocity")
	bool bUseWorldSpaceVelocity = false;

	/** Initial velocity direction for spawned particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Velocity")
	FVector InitialVelocityDirection = FVector(0, 0, -1);

	/** Initial speed for spawned particles (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Velocity",
		meta = (ClampMin = "0.0"))
	float InitialSpeed = 250.0f;

	/** Maximum particles this emitter can spawn (0 = unlimited) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Limits",
		meta = (ClampMin = "0"))
	int32 MaxParticleCount = 100000;

	/** Enable continuous spawning by recycling oldest particles when MaxParticleCount is exceeded
	 *  Only applicable to Stream mode - Fill mode spawns once and doesn't need recycling */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Limits",
		meta = (DisplayName = "Continuous Spawn", EditCondition = "MaxParticleCount > 0 && EmitterMode == EKawaiiFluidEmitterMode::Stream", EditConditionHides))
	bool bRecycleOldestParticles = true;

	/** Whether to automatically start spawning on BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter")
	bool bAutoStartSpawning = true;

	//========================================
	// Optimization
	//========================================

	/** Enable distance-based optimization.
	 *  When enabled, this emitter only spawns when the reference actor is within ActivationDistance.
	 *  Particles are despawned when reference actor moves outside the distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Optimization")
	bool bUseDistanceOptimization = false;

	/** Custom actor to use as distance reference.
	 *  If not set, defaults to Player Pawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Optimization",
		meta = (EditCondition = "bUseDistanceOptimization", EditConditionHides))
	TObjectPtr<AActor> DistanceReferenceActor;

	/** Distance at which this emitter activates (cm).
	 *  Only used when bUseDistanceOptimization is true.
	 *  Hysteresis (10% of this value) is automatically applied to prevent toggling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Optimization",
		meta = (EditCondition = "bUseDistanceOptimization", EditConditionHides, ClampMin = "100.0"))
	float ActivationDistance = 2000.0f;

	/** For Fill mode: automatically re-spawn when re-entering activation distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Emitter|Optimization",
		meta = (EditCondition = "bUseDistanceOptimization && EmitterMode == EKawaiiFluidEmitterMode::Fill",
		        EditConditionHides))
	bool bAutoRespawnOnReentry = true;

	//========================================
	// Target Volume API
	//========================================

	/** Get the target volume */
	UFUNCTION(BlueprintPure, Category = "Target")
	AKawaiiFluidVolume* GetTargetVolume() const { return TargetVolume; }

	/** Set the target volume */
	UFUNCTION(BlueprintCallable, Category = "Target")
	void SetTargetVolume(AKawaiiFluidVolume* NewVolume);

	//========================================
	// API
	//========================================

	/** Get the owner emitter actor */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	AKawaiiFluidEmitter* GetOwnerEmitter() const;

	/** Get particle spacing from target volume's preset */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	float GetParticleSpacing() const;

	/** Execute fill spawn (Fill mode) */
	UFUNCTION(BlueprintCallable, Category = "Emitter")
	void SpawnFill();

	/** Spawn a burst of particles */
	UFUNCTION(BlueprintCallable, Category = "Emitter")
	void BurstSpawn(int32 Count);

	/** Get total particles spawned by this emitter */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	int32 GetSpawnedParticleCount() const { return SpawnedParticleCount; }

	/** Check if the emitter has reached its particle limit */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	bool HasReachedParticleLimit() const;

	/** Check if Fill mode */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	bool IsFillMode() const { return EmitterMode == EKawaiiFluidEmitterMode::Fill; }

	/** Check if Stream mode */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	bool IsStreamMode() const { return EmitterMode == EKawaiiFluidEmitterMode::Stream; }

	/** Start stream spawning (Stream mode only) */
	UFUNCTION(BlueprintCallable, Category = "Emitter")
	void StartStreamSpawn();

	/** Stop stream spawning (Stream mode only) */
	UFUNCTION(BlueprintCallable, Category = "Emitter")
	void StopStreamSpawn();

	/** Check if stream is currently spawning */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	bool IsStreamSpawning() const { return bStreamSpawning; }

protected:
	//========================================
	// Spawn State
	//========================================

	/** Accumulated time for rate-based spawning */
	float SpawnAccumulator = 0.0f;

	/** Total particles spawned by this emitter */
	int32 SpawnedParticleCount = 0;

	/** Whether auto spawn has been executed (Fill mode) */
	bool bAutoSpawnExecuted = false;

	/** Whether stream is currently spawning (Stream mode) */
	bool bStreamSpawning = false;

	/** Whether we need to search for volume in next tick (deferred search for BeginPlay order issues) */
	bool bPendingVolumeSearch = false;

	//========================================
	// Distance Optimization State
	//========================================

	/** Current activation state based on player distance */
	bool bDistanceActivated = true;

	/** Cached player pawn reference */
	TWeakObjectPtr<APawn> CachedPlayerPawn;

	/** Timer for distance check interval */
	float DistanceCheckAccumulator = 0.0f;

	/** Distance check interval (10 Hz) */
	static constexpr float DistanceCheckInterval = 0.1f;

	/** Track if Fill mode needs re-spawn on reentry */
	bool bNeedsRespawnOnReentry = false;

	//========================================
	// Internal Spawn Methods
	//========================================

	/** Process continuous spawning for Stream mode */
	void ProcessContinuousSpawn(float DeltaTime);

	/** Process Stream emitter mode */
	void ProcessStreamEmitter(float DeltaTime);

	//========================================
	// Shape Spawning (Shape mode)
	//========================================

	/** Spawn particles in a sphere shape (hexagonal pattern) */
	int32 SpawnParticlesSphereHexagonal(FVector Center, FQuat Rotation, float Radius, float Spacing, FVector InInitialVelocity);

	/** Spawn particles in a cube shape (hexagonal pattern) */
	int32 SpawnParticlesCubeHexagonal(FVector Center, FQuat Rotation, FVector HalfSize, float Spacing, FVector InInitialVelocity);

	/** Spawn particles in a cylinder shape (hexagonal pattern) */
	int32 SpawnParticlesCylinderHexagonal(FVector Center, FQuat Rotation, float Radius, float HalfHeight, float Spacing, FVector InInitialVelocity);

	//========================================
	// Stream Spawning (Stream mode)
	//========================================

	/** Spawn a hexagonal layer of particles for stream */
	void SpawnStreamLayer(FVector Position, FVector LayerDirection, FVector VelocityDirection, float Speed, float Radius, float Spacing);

	/** Spawn a hexagonal layer of particles for stream - batch collection version (no immediate GPU send) */
	void SpawnStreamLayerBatch(FVector Position, FVector LayerDirection, FVector VelocityDirection, 
	                           float Speed, float Radius, float Spacing,
	                           TArray<FVector>& OutPositions, TArray<FVector>& OutVelocities);

	//========================================
	// Internal Helpers
	//========================================

	/** Queue spawn request to target volume's simulation module */
	void QueueSpawnRequest(const TArray<FVector>& Positions, const TArray<FVector>& Velocities);

	/** Get the simulation module from target volume */
	UKawaiiFluidSimulationModule* GetSimulationModule() const;

	/** Remove oldest particles if recycling is enabled */
	void RecycleOldestParticlesIfNeeded(int32 NewParticleCount);

	/** Update velocity arrow visualization */
	void UpdateVelocityArrowVisualization();

	//========================================
	// Distance Optimization Methods
	//========================================

	/** Update distance optimization state (called at 10Hz) */
	void UpdateDistanceOptimization(float DeltaTime);

	/** Handle activation state change */
	void OnDistanceActivationChanged(bool bNewState);

	/** Despawn all particles from this emitter */
	void DespawnAllParticles();

	/** Get player pawn (cached) */
	APawn* GetPlayerPawn();

	/** Get hysteresis distance (auto-calculated as 10% of ActivationDistance) */
	FORCEINLINE float GetHysteresisDistance() const { return ActivationDistance * 0.1f; }

	//========================================
	// Internal Constants & Configuration
	//========================================

#if WITH_EDITORONLY_DATA
	/** Billboard icon for editor visualization */
	UPROPERTY(Transient)
	TObjectPtr<UBillboardComponent> BillboardComponent;

	/** Velocity direction arrow (editor visualization only) */
	UPROPERTY(Transient)
	TObjectPtr<class UArrowComponent> VelocityArrow;
#endif

	// Debug visualization settings (internal)
	bool bShowSpawnVolumeWireframe = true;
	FColor SpawnVolumeWireframeColor = FColor::Cyan;
	float WireframeThickness = 2.0f;

	// Advanced spawn settings (internal)
	bool bAutoFindVolume = true;
	bool bAutoCalculateParticleCount = true;
	int32 ParticleCount = 500;
	bool bUseJitter = true;
	float JitterAmount = 0.2f;
	
	// Stream advanced settings (internal)
	FVector SpawnOffset = FVector::ZeroVector;
	FVector SpawnDirection = FVector(0, 0, -1);
	float StreamParticleSpacing = 0.0f;
	// Note: StreamJitter moved to UPROPERTY (bUseStreamJitter, StreamJitterAmount)
	float StreamLayerSpacingRatio = 0.816f;

	/** Cached SourceID for this emitter (allocated from Subsystem, 0~63 range) */
	int32 CachedSourceID = -1;

	//========================================
	// Volume Registration (Internal)
	//========================================

	/** Register this emitter to its target volume */
	void RegisterToVolume();

	/** Unregister this emitter from its target volume */
	void UnregisterFromVolume();

	/** Find the nearest volume automatically */
	AKawaiiFluidVolume* FindNearestVolume() const;

#if WITH_EDITOR
	//========================================
	// Debug Visualization
	//========================================

	/** Draw spawn volume/emitter wireframe visualization */
	void DrawSpawnVolumeVisualization();

	/** Draw distance optimization visualization (activation/deactivation spheres) */
	void DrawDistanceVisualization();
#endif
};
