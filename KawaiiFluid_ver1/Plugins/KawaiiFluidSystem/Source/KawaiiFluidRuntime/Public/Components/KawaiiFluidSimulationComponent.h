// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiRenderParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Rendering/IKawaiiFluidRenderable.h"
#include "Rendering/KawaiiFluidRenderingMode.h"
#include "KawaiiFluidSimulationComponent.generated.h"

class UKawaiiFluidPresetDataAsset;
class UFluidCollider;
class UFluidInteractionComponent;
class FKawaiiFluidRenderResource;
class FSpatialHash;

/**
 * Particle collision event delegate (Legacy)
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(
	FOnFluidParticleHitLegacy,
	int32, ParticleIndex,
	AActor*, HitActor,
	FVector, HitLocation,
	FVector, HitNormal,
	float, HitSpeed
);

/**
 * Kawaii Fluid Simulation Component
 *
 * Data owner + API provider
 * Can be attached to any Actor for fluid simulation
 *
 * Responsibilities:
 * - Owns particle array
 * - Manages render resources
 * - Provides gameplay API (SpawnParticles, ApplyForce, etc.)
 * - Implements IKawaiiFluidRenderable
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidSimulationComponent : public UActorComponent, public IKawaiiFluidRenderable
{
	GENERATED_BODY()

public:
	UKawaiiFluidSimulationComponent();
	virtual ~UKawaiiFluidSimulationComponent() override;

	//========================================
	// UActorComponent Interface
	//========================================

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void BeginDestroy() override;
	
	//========================================
	// IKawaiiFluidRenderable Interface
	//========================================

	virtual FKawaiiFluidRenderResource* GetFluidRenderResource() const override;
	virtual bool IsFluidRenderResourceValid() const override;
	virtual float GetParticleRenderRadius() const override;
	virtual FString GetDebugName() const override;
	virtual bool ShouldUseSSFR() const override;
	virtual bool ShouldUseDebugMesh() const override;
	virtual UInstancedStaticMeshComponent* GetDebugMeshComponent() const override;
	virtual int32 GetParticleCount() const override;

	//========================================
	// Configuration
	//========================================

	/** Fluid preset data asset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Configuration")
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset;

	/** Simulation enabled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Configuration")
	bool bSimulationEnabled = true;

	/**
	 * Independent simulation mode
	 * true: This component runs its own simulation (not batched)
	 * false: Subsystem batches with same-preset components
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Configuration")
	bool bIndependentSimulation = false;

	/** Use world collision */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision")
	bool bUseWorldCollision = true;

	//========================================
	// Preset Overrides (Per-Instance)
	//========================================

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Override RestDensity from preset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (InlineEditConditionToggle))
	bool bOverride_RestDensity = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (EditCondition = "bOverride_RestDensity", ClampMin = "0.1"))
	float Override_RestDensity = 1200.0f;

	/** Override Compliance from preset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (InlineEditConditionToggle))
	bool bOverride_Compliance = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (EditCondition = "bOverride_Compliance", ClampMin = "0.0"))
	float Override_Compliance = 0.01f;

	/** Override SmoothingRadius from preset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (InlineEditConditionToggle))
	bool bOverride_SmoothingRadius = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (EditCondition = "bOverride_SmoothingRadius", ClampMin = "1.0"))
	float Override_SmoothingRadius = 20.0f;

	/** Override ViscosityCoefficient from preset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (InlineEditConditionToggle))
	bool bOverride_ViscosityCoefficient = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (EditCondition = "bOverride_ViscosityCoefficient", ClampMin = "0.0", ClampMax = "1.0"))
	float Override_ViscosityCoefficient = 0.5f;

	/** Override Gravity from preset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (InlineEditConditionToggle))
	bool bOverride_Gravity = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (EditCondition = "bOverride_Gravity"))
	FVector Override_Gravity = FVector(0.0f, 0.0f, -980.0f);

	/** Override AdhesionStrength from preset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (InlineEditConditionToggle))
	bool bOverride_AdhesionStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Override", meta = (EditCondition = "bOverride_AdhesionStrength", ClampMin = "0.0", ClampMax = "1.0"))
	float Override_AdhesionStrength = 0.5f;

	//========================================
	// Rendering
	//========================================

	/** Rendering mode selection */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering")
	EKawaiiFluidRenderingMode RenderingMode = EKawaiiFluidRenderingMode::ISM;

	/** Enable debug rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug")
	bool bEnableDebugRendering = true;

	/** Debug mesh sphere radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug", meta = (ClampMin = "0.1", ClampMax = "50.0", EditCondition = "bEnableDebugRendering"))
	float DebugMeshRadius = 3.0f;

	//========================================
	// Events
	//========================================

	/** Particle collision event (Blueprint bindable) */
	UPROPERTY(BlueprintAssignable, Category = "Fluid|Events")
	FOnFluidParticleHitLegacy OnParticleHit;

	/** Enable particle hit events (performance consideration - default off) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events")
	bool bEnableParticleHitEvents = false;

	/** Minimum velocity for event (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "0.0", EditCondition = "bEnableParticleHitEvents"))
	float MinVelocityForEvent = 50.0f;

	/** Max events per frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "1", ClampMax = "100", EditCondition = "bEnableParticleHitEvents"))
	int32 MaxEventsPerFrame = 10;

	/** Per-particle event cooldown in seconds (prevents same particle from spamming events) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Events", meta = (ClampMin = "0.0", EditCondition = "bEnableParticleHitEvents"))
	float EventCooldownPerParticle = 0.1f;

	//========================================
	// Auto Spawn (Initial)
	//========================================

	/** Spawn on begin play */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn")
	bool bSpawnOnBeginPlay = true;

	/** Auto spawn count */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "1", ClampMax = "5000", EditCondition = "bSpawnOnBeginPlay"))
	int32 AutoSpawnCount = 100;

	/** Auto spawn radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "1.0", ClampMax = "500.0", EditCondition = "bSpawnOnBeginPlay"))
	float AutoSpawnRadius = 50.0f;

	//========================================
	// Continuous Spawn
	//========================================

	/** Enable continuous particle spawning */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn")
	bool bContinuousSpawn = false;

	/** Particles to spawn per second */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "0.1", ClampMax = "1000.0", EditCondition = "bContinuousSpawn"))
	float ParticlesPerSecond = 10.0f;

	/** Maximum particle count (0 = unlimited) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "0", EditCondition = "bContinuousSpawn"))
	int32 MaxParticleCount = 500;

	/** Spawn radius for continuous spawn */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "0.0", ClampMax = "100.0", EditCondition = "bContinuousSpawn"))
	float ContinuousSpawnRadius = 10.0f;

	/** Spawn offset from component location */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (EditCondition = "bContinuousSpawn"))
	FVector SpawnOffset = FVector::ZeroVector;

	/** Initial velocity for spawned particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (EditCondition = "bContinuousSpawn"))
	FVector SpawnVelocity = FVector::ZeroVector;

	//========================================
	// Blueprint API
	//========================================

	/** Spawn particles at location */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void SpawnParticles(FVector Location, int32 Count, float SpawnRadius = 50.0f);

	/** Spawn single particle */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	int32 SpawnParticle(FVector Position, FVector Velocity = FVector::ZeroVector);

	/** Apply force to all particles */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyExternalForce(FVector Force);

	/** Apply force to specific particle */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ApplyForceToParticle(int32 ParticleIndex, FVector Force);

	/** Register collider */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void RegisterCollider(UFluidCollider* Collider);

	/** Unregister collider */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void UnregisterCollider(UFluidCollider* Collider);

	/** Register interaction component */
	void RegisterInteractionComponent(UFluidInteractionComponent* Component);

	/** Unregister interaction component */
	void UnregisterInteractionComponent(UFluidInteractionComponent* Component);

	/** Get particle positions */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	TArray<FVector> GetParticlePositions() const;

	/** Get particle velocities */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	TArray<FVector> GetParticleVelocities() const;

	/** Clear all particles */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void ClearAllParticles();

	/** Set continuous spawn enabled */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Spawn")
	void SetContinuousSpawnEnabled(bool bEnabled);

	/** Set particles per second */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Spawn")
	void SetParticlesPerSecond(float NewRate);

	/** Set spawn velocity */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Spawn")
	void SetSpawnVelocity(FVector NewVelocity);

	//========================================
	// Query Functions
	//========================================

	/** Get particles in radius */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	TArray<int32> GetParticlesInRadius(FVector Location, float Radius) const;

	/** Get particles in box */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	TArray<int32> GetParticlesInBox(FVector Center, FVector Extent) const;

	/** Get particles near actor */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	TArray<int32> GetParticlesNearActor(AActor* Actor, float Radius) const;

	/** Get particle info */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Query")
	bool GetParticleInfo(int32 ParticleIndex, FVector& OutPosition, FVector& OutVelocity, float& OutDensity) const;

	//========================================
	// Direct Access (Advanced)
	//========================================

	/** Read-only particle array access */
	const TArray<FFluidParticle>& GetParticles() const { return Particles; }

	/** Writable particle array access (caution: affects simulation) */
	TArray<FFluidParticle>& GetParticlesMutable() { return Particles; }

	/** Get accumulated external force */
	FVector GetAccumulatedExternalForce() const { return AccumulatedExternalForce; }

	/** Reset accumulated external force */
	void ResetExternalForce() { AccumulatedExternalForce = FVector::ZeroVector; }

	/** Get registered colliders */
	const TArray<UFluidCollider*>& GetColliders() const { return Colliders; }

	/** Get registered interaction components */
	const TArray<UFluidInteractionComponent*>& GetInteractionComponents() const { return InteractionComponents; }

	/** Get spatial hash (for batching) */
	FSpatialHash* GetSpatialHash() const { return SpatialHash.Get(); }

	/** Get accumulated time (for batching) */
	float GetAccumulatedTime() const { return AccumulatedTime; }
	void SetAccumulatedTime(float Time) { AccumulatedTime = Time; }

	/** Build simulation params for context */
	FKawaiiFluidSimulationParams BuildSimulationParams() const;

	//========================================
	// Override System
	//========================================

	/** Returns true if any override is enabled */
	bool HasAnyOverride() const
	{
		return bOverride_RestDensity || bOverride_Compliance || bOverride_SmoothingRadius ||
		       bOverride_ViscosityCoefficient || bOverride_Gravity || bOverride_AdhesionStrength;
	}

	/** Should this component use independent simulation? (explicit flag OR has overrides) */
	bool ShouldSimulateIndependently() const
	{
		return bIndependentSimulation || HasAnyOverride();
	}

	/**
	 * Get effective preset for simulation
	 * Returns RuntimePreset if overrides exist, otherwise returns original Preset
	 * Called by Subsystem before passing to Context
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	UKawaiiFluidPresetDataAsset* GetEffectivePreset();

	/** Update RuntimePreset with current override values */
	void UpdateRuntimePreset();

	/** Mark RuntimePreset as needing update */
	void MarkRuntimePresetDirty() { bRuntimePresetDirty = true; }

	/** Update render data (call after simulation) */
	void UpdateRenderData();

	/** Update debug mesh instances */
	void UpdateDebugInstances();

private:
	//========================================
	// Runtime Preset (Override System)
	//========================================

	/** Runtime preset with overrides applied (Transient - not saved) */
	UPROPERTY(Transient)
	TObjectPtr<UKawaiiFluidPresetDataAsset> RuntimePreset;

	/** Flag indicating RuntimePreset needs update */
	bool bRuntimePresetDirty = true;

	//========================================
	// Simulation Data
	//========================================

	/** Particle array */
	UPROPERTY()
	TArray<FFluidParticle> Particles;

	/** Accumulated external force */
	FVector AccumulatedExternalForce = FVector::ZeroVector;

	/** Atomic event counter for thread-safe collision event tracking */
	mutable std::atomic<int32> EventCountThisFrame{0};

	/** Per-particle last event time for cooldown (ParticleID -> LastEventTime) */
	mutable TMap<int32, float> ParticleLastEventTime;

	/** Next particle ID */
	int32 NextParticleID = 0;

	/** Substep time accumulator */
	float AccumulatedTime = 0.0f;

	/** Spawn time accumulator for continuous spawning */
	float SpawnAccumulatedTime = 0.0f;

	//========================================
	// Colliders and Interactions
	//========================================

	/** Registered colliders */
	UPROPERTY()
	TArray<UFluidCollider*> Colliders;

	/** Registered interaction components */
	UPROPERTY()
	TArray<UFluidInteractionComponent*> InteractionComponents;

	//========================================
	// Spatial Hash
	//========================================

	/** Spatial hash (owned by component for independent mode) */
	TSharedPtr<FSpatialHash> SpatialHash;

	//========================================
	// Rendering Resources
	//========================================

	/** GPU render resource */
	TSharedPtr<FKawaiiFluidRenderResource> RenderResource;

	/** Debug mesh component */
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> DebugMeshComponent;

	//========================================
	// Internal Methods
	//========================================

	/** Initialize spatial hash */
	void InitializeSpatialHash();

	/** Initialize render resource */
	void InitializeRenderResource();

	/** Initialize debug mesh */
	void InitializeDebugMesh();

	/** Convert to render particles */
	TArray<FKawaiiRenderParticle> ConvertToRenderParticles() const;
};
