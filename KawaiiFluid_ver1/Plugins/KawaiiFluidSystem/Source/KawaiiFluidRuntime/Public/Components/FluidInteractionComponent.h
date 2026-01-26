// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "FluidInteractionComponent.generated.h"

class UKawaiiFluidSimulatorSubsystem;
class UFluidCollider;
class UMeshFluidCollider;
class UKawaiiFluidPresetDataAsset;

// EFluidType is now defined in KawaiiFluidSimulationTypes.h

//========================================
// GPU Collision Feedback Delegates (Particle -> Player Interaction)
//========================================

/**
 * Fired when entering a specific fluid region.
 * @param FluidTag Fluid tag (e.g., "Water", "Lava")
 * @param ParticleCount Number of particles in contact
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnFluidEnter, FName, FluidTag, int32, ParticleCount);

/**
 * Fired when exiting a specific fluid region.
 * @param FluidTag Fluid tag (e.g., "Water", "Lava")
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFluidExit, FName, FluidTag);

/**
 * Fired when fluid force is updated (every tick).
 * @param Force Force vector from fluid (cm/s²)
 * @param Pressure Average pressure value
 * @param ContactCount Number of particles in contact
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnFluidForceUpdate, FVector, Force, float, Pressure, int32, ContactCount);

/**
 * Fired when particles collide with a specific bone.
 * Used for Niagara spawning (AttachToComponent follows the bone).
 * @param BoneIndex Bone index that received the collision
 * @param BoneName Bone name that received the collision
 * @param ContactCount Number of particles contacting that bone
 * @param AverageVelocity Average velocity of colliding particles (Niagara direction/intensity)
 * @param FluidName Name of the colliding fluid (Preset FluidName, use Switch on Name)
 * @param ImpactOffset Offset from the bone origin to the impact location (cm)
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_SixParams(FOnBoneParticleCollision, int32, BoneIndex, FName, BoneName, int32, ContactCount, FVector, AverageVelocity, FName, FluidName, FVector, ImpactOffset);

/**
 * Fired when a monitored bone receives an impact above the threshold.
 * Checks MonitoredBones each tick and triggers when speed exceeds BoneImpactSpeedThreshold.
 * @param BoneName Bone name that received the impact
 * @param ImpactSpeed Absolute fluid speed (cm/s)
 * @param ImpactForce Impact force (Newton)
 * @param ImpactDirection Impact direction (normalized)
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnBoneFluidImpact, FName, BoneName, float, ImpactSpeed, float, ImpactForce, FVector, ImpactDirection);

/**
 * Fluid interaction component.
 * Attach to characters/objects to enable fluid interaction.
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent))
class KAWAIIFLUIDRUNTIME_API UFluidInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFluidInteractionComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Cached subsystem reference */
	UPROPERTY(Transient)
	TObjectPtr<UKawaiiFluidSimulatorSubsystem> TargetSubsystem;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction",
		meta = (ToolTip = "Automatically creates and registers a MeshFluidCollider on begin play."))
	bool bAutoCreateCollider;

	//========================================
	// GPU Collision Feedback (Particle -> Player Interaction)
	// Computes forces and fires events using GPU collision feedback data.
	//========================================

	/**
	 * Enable GPU collision feedback.
	 * Reads back particle-collider contacts to compute forces and events.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Drag Force Feedback",
	          meta = (ToolTip = "Enables GPU collision feedback.\nWhen enabled, particle-collider contacts are read back to compute physical forces and trigger fluid events (OnFluidEnter/Exit).\nThere is a 2-3 frame latency with minimal FPS impact."))
	bool bEnableForceFeedback = false;

	/**
	 * Force smoothing speed (1/s).
	 * Higher values respond faster; lower values are smoother.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Drag Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback", ClampMin = "0.1", ClampMax = "50.0",
	                  ToolTip = "Force smoothing speed (1/s).\nHigher values react faster; lower values are smoother.\n5-15 is recommended to avoid sudden spikes."))
	float ForceSmoothingSpeed = 10.0f;

	/**
	 * Drag coefficient (C_d).
	 * Dimensionless coefficient used in the drag equation.
	 * Sphere: ~0.47, capsule/cylinder: ~1.0, human: ~1.0-1.3
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Drag Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback", ClampMin = "0.1", ClampMax = "3.0",
	                  ToolTip = "Drag coefficient (C_d).\nUsed in the drag equation F = 1/2 * rho * C_d * A * |v|^2.\nSphere: ~0.47, capsule/cylinder: ~1.0, human: ~1.0-1.3"))
	float DragCoefficient = 1.0f;

	/**
	 * Drag-to-force scale.
	 * Scales physical drag into gameplay force.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Drag Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback", ClampMin = "0.0001", ClampMax = "10.0",
	                  ToolTip = "Drag-to-force scale.\nScales physical drag into game force.\nAdjusts how strongly waves push characters."))
	float DragForceMultiplier = 0.01f;

	/**
	 * Use relative velocity for force calculation in OnFluidForceUpdate.
	 * true: relative velocity (v_fluid - v_body) for drag in still water
	 * false: absolute velocity (v_fluid) for waves/waterfalls pushing
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Drag Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback",
	                  ToolTip = "Use relative velocity in OnFluidForceUpdate.\ntrue: relative velocity (v_fluid - v_body) for resistance in still water\nfalse: absolute velocity (v_fluid) for waves/waterfalls pushing\nGenerally, true is recommended."))
	bool bUseRelativeVelocityForForce = true;

	/**
	 * Minimum particle count required to trigger per-tag events.
	 * OnFluidEnter/Exit fire only above this threshold.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Drag Force Feedback",
	          meta = (EditCondition = "bEnableForceFeedback", ClampMin = "1", ClampMax = "100",
	                  ToolTip = "Minimum particle count for OnFluidEnter/Exit.\nEvents fire only when colliding particles meet this threshold.\n5-20 is recommended to reduce noise."))
	int32 MinParticleCountForFluidEvent = 5;

	/** Current force from fluid (smoothed). */
	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Drag Force Feedback")
	FVector CurrentFluidForce;

	/** Current number of contact particles. */
	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Drag Force Feedback")
	int32 CurrentContactCount;

	/** Current average pressure. */
	UPROPERTY(BlueprintReadOnly, Category = "Fluid Interaction|Drag Force Feedback")
	float CurrentAveragePressure;

	/** Fluid area enter event (per fluid tag). */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidEnter OnFluidEnter;

	/** Fluid area exit event (per fluid tag). */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidExit OnFluidExit;

	/** Fluid force update event (every tick when force feedback is enabled). */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnFluidForceUpdate OnFluidForceUpdate;

	//========================================
	// Bone Impact Monitoring (Auto Events)
	//========================================

	/**
	 * Enable per-bone impact monitoring.
	 * Checks monitored bones every tick and fires OnBoneFluidImpact
	 * when BoneImpactSpeedThreshold is exceeded.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Bone Impact Monitoring",
	          meta = (ToolTip = "Enable automatic per-bone impact detection.\nMonitored bones are checked every tick and an event is fired when the threshold is exceeded."))
	bool bEnableBoneImpactMonitoring = false;

	/**
	 * List of bones to monitor.
	 * These bones are checked every tick.
	 * Example: "head", "spine_03", "thigh_l"
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Bone Impact Monitoring",
	          meta = (EditCondition = "bEnableBoneImpactMonitoring",
	                  ToolTip = "List of bone names to monitor.\nUse names from the skeleton asset (e.g., head, spine_03, thigh_l)."))
	TArray<FName> MonitoredBones;

	/**
	 * Impact speed threshold (cm/s).
	 * OnBoneFluidImpact fires when fluid speed exceeds this value.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Bone Impact Monitoring",
	          meta = (EditCondition = "bEnableBoneImpactMonitoring", ClampMin = "0.0", ClampMax = "5000.0",
	                  ToolTip = "Impact speed threshold (cm/s).\nThe event fires when fluid speed exceeds this value.\nRecommended: 500-1000"))
	float BoneImpactSpeedThreshold = 500.0f;

	/**
	 * Per-bone impact event.
	 * Fired when a monitored bone exceeds the impact threshold.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnBoneFluidImpact OnBoneFluidImpact;

	//========================================
	// Per-Bone Force Feedback (for Additive Animation / Spring)
	//========================================

	/**
	 * Enable per-bone force computation.
	 * When enabled, computes per-bone drag for additive animation or springs.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Bone Force",
	          meta = (EditCondition = "bEnableForceFeedback",
	                  ToolTip = "Enable per-bone force computation.\nWhen enabled, computes per-bone drag.\nUseful for Additive Animation or AnimDynamics springs."))
	bool bEnablePerBoneForce = false;

	/**
	 * Per-bone force smoothing speed (1/s).
	 * Higher responds faster, lower smooths changes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Bone Force",
	          meta = (EditCondition = "bEnablePerBoneForce", ClampMin = "0.1", ClampMax = "50.0",
	                  ToolTip = "Per-bone force smoothing speed (1/s).\nHigher responds faster, lower smooths changes."))
	float PerBoneForceSmoothingSpeed = 10.0f;

	/**
	 * Per-bone force multiplier.
	 * Scales the computed per-bone force.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Per-Bone Force",
	          meta = (EditCondition = "bEnablePerBoneForce", ClampMin = "0.0001", ClampMax = "100.0",
	                  ToolTip = "Per-bone force multiplier.\nScales the force applied to additive animation or springs."))
	float PerBoneForceMultiplier = 1.0f;

	/**
	 * Get current fluid force for a bone (by index).
	 * @param BoneIndex Bone index to query
	 * @return Fluid force vector for that bone (ZeroVector if not found)
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Per-Bone Force")
	FVector GetFluidForceForBone(int32 BoneIndex) const;

	/**
	 * Get current fluid force for a bone (by name).
	 * @param BoneName Bone name to query
	 * @return Fluid force vector for that bone (ZeroVector if not found)
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Per-Bone Force")
	FVector GetFluidForceForBoneByName(FName BoneName) const;

	/**
	 * Get a map of all bone forces (BoneIndex -> Force).
	 * @return Map of bone indices to force vectors
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Per-Bone Force")
	TMap<int32, FVector> GetAllBoneForces() const { return CurrentPerBoneForces; }

	/**
	 * Get bone indices with active fluid force.
	 * @param OutBoneIndices Output array of bone indices
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Per-Bone Force")
	void GetActiveBoneIndices(TArray<int32>& OutBoneIndices) const;

	/**
	 * Get the bone with the strongest force.
	 * @param OutBoneIndex Bone index with the strongest force (-1 if none)
	 * @param OutForce Force vector for that bone
	 * @return true if any bone has force
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Per-Bone Force")
	bool GetStrongestBoneForce(int32& OutBoneIndex, FVector& OutForce) const;

	//========================================
	// Bone Collision Events (for Niagara Spawning)
	//========================================

	/**
	 * Enable per-bone collision events.
	 * Fires OnBoneParticleCollision when particles hit bones.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Bone Collision Events",
	          meta = (EditCondition = "bEnablePerBoneForce",
	                  ToolTip = "Enable per-bone collision events.\nWhen enabled, OnBoneParticleCollision is fired on bone hits.\nUseful for Niagara effect spawning."))
	bool bEnableBoneCollisionEvents = false;

	/**
	 * Minimum particle count required to trigger OnBoneParticleCollision.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Bone Collision Events",
	          meta = (EditCondition = "bEnableBoneCollisionEvents", ClampMin = "1", ClampMax = "50",
	                  ToolTip = "Minimum particle count required to fire the event.\nEvents fire only when enough particles hit the bone."))
	int32 MinParticleCountForBoneEvent = 3;

	/**
	 * Collision event cooldown (seconds).
	 * Prevents firing too frequently on the same bone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Bone Collision Events",
	          meta = (EditCondition = "bEnableBoneCollisionEvents", ClampMin = "0.0", ClampMax = "2.0",
	                  ToolTip = "Collision event cooldown (seconds).\nPrevents repeated events on the same bone within this time window."))
	float BoneEventCooldown = 0.1f;

	/**
	 * Per-bone collision event.
	 * Useful for Niagara spawning (SpawnSystemAttached follows the bone).
	 */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Interaction|Events")
	FOnBoneParticleCollision OnBoneParticleCollision;

	/**
	 * Get current contact count for a bone.
	 * @param BoneIndex Bone index to query
	 * @return Number of particles in contact with that bone
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Bone Collision Events")
	int32 GetBoneContactCount(int32 BoneIndex) const;

	/**
	 * Get a map of all bone contact counts.
	 * @return Map of bone indices to contact counts
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Bone Collision Events")
	TMap<int32, int32> GetAllBoneContactCounts() const { return CurrentBoneContactCounts; }

	/**
	 * Get bone indices that are currently in contact with particles.
	 * @param OutBoneIndices Output array of bone indices
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Bone Collision Events")
	void GetBonesWithContacts(TArray<int32>& OutBoneIndices) const;

	/**
	 * Convert a bone index to a bone name.
	 * @param BoneIndex Bone index
	 * @return Bone name (NAME_None if invalid)
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Bone Collision Events")
	FName GetBoneNameFromIndex(int32 BoneIndex) const;

	/**
	 * Get the owner's SkeletalMeshComponent.
	 * Used by Niagara SpawnSystemAttached -> AttachToComponent.
	 * @return SkeletalMeshComponent (nullptr if none)
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Bone Collision Events")
	class USkeletalMeshComponent* GetOwnerSkeletalMesh() const;

	/**
	 * Get the bone with the most particle contacts.
	 * @param OutBoneIndex Bone index with most contacts (-1 if none)
	 * @param OutContactCount Contact count for that bone
	 * @return true if any bone is in contact
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Bone Collision Events")
	bool GetMostContactedBone(int32& OutBoneIndex, int32& OutContactCount) const;

	/** Get the current fluid force. */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	FVector GetCurrentFluidForce() const { return CurrentFluidForce; }

	/** Get the current average fluid pressure. */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	float GetCurrentFluidPressure() const { return CurrentAveragePressure; }

	/**
	 * Apply fluid force to CharacterMovement.
	 * Works only if the actor has a CharacterMovementComponent.
	 * @param ForceScale Force scale (default 1.0)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Drag Force Feedback")
	void ApplyFluidForceToCharacterMovement(float ForceScale = 1.0f);

	/**
	 * Check if currently colliding with a specific fluid tag.
	 * @param FluidTag Fluid tag to test (e.g., "Water", "Lava")
	 * @return true if colliding with that tag
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	bool IsCollidingWithFluidTag(FName FluidTag) const;

	/**
	 * Get average absolute speed of the currently colliding fluid (cm/s).
	 * Useful for knockdown checks; independent of character velocity.
	 * @return Average fluid speed (cm/s). 0 if not colliding.
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	float GetFluidImpactSpeed() const;

	/**
	 * Get average absolute speed of fluid colliding with a specific bone (cm/s).
	 * @param BoneName Bone name to filter (e.g., "head", "spine_01", "pelvis")
	 * @return Average fluid speed for that bone (cm/s). 0 if not colliding.
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	float GetFluidImpactSpeedForBone(FName BoneName) const;

	/**
	 * Get impact force magnitude of the currently colliding fluid (N).
	 * Based on F = 1/2 * rho * C_d * A * |v|^2 (v is absolute fluid speed).
	 * @return Total impact force (Newton). 0 if not colliding.
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	float GetFluidImpactForceMagnitude() const;

	/**
	 * Get impact force magnitude of fluid colliding with a specific bone (N).
	 * @param BoneName Bone name to filter (e.g., "head", "spine_01", "pelvis")
	 * @return Impact force for that bone (Newton). 0 if not colliding.
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	float GetFluidImpactForceMagnitudeForBone(FName BoneName) const;

	/**
	 * Get impact direction of the currently colliding fluid (normalized).
	 * Computed from the average particle velocity.
	 * @return Normalized impact direction. ZeroVector if not colliding.
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	FVector GetFluidImpactDirection() const;

	/**
	 * Get impact direction of fluid colliding with a specific bone (normalized).
	 * @param BoneName Bone name to filter (e.g., "head", "spine_01", "pelvis")
	 * @return Impact direction for that bone. ZeroVector if not colliding.
	 */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Drag Force Feedback")
	FVector GetFluidImpactDirectionForBone(FName BoneName) const;

	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	void DetachAllFluid();

	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	void PushFluid(FVector Direction, float Force);

	/** Check if subsystem is valid */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction")
	bool HasValidTarget() const { return TargetSubsystem != nullptr; }

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY()
	TObjectPtr<UMeshFluidCollider> AutoCollider;

	void CreateAutoCollider();
	void RegisterWithSimulator();
	void UnregisterFromSimulator();

	//========================================
	// Drag Force Feedback Internal State
	//========================================

	/** Accumulated force before smoothing (used for smoothing). */
	FVector SmoothedForce = FVector::ZeroVector;

	/** Previous-frame collision state per fluid tag (for enter/exit events). */
	TMap<FName, bool> PreviousFluidTagStates;

	/** Current-frame colliding particle counts per fluid tag. */
	TMap<FName, int32> CurrentFluidTagCounts;

	/** Collider index associated with this component (for GPU feedback filtering). */
	int32 ColliderIndex = -1;

	/** Whether GPU feedback is already enabled. */
	bool bGPUFeedbackEnabled = false;

	//========================================
	// Per-Bone Force Internal State
	//========================================

	/** Current fluid force per bone index (smoothed). */
	TMap<int32, FVector> CurrentPerBoneForces;

	/** Pre-smoothed force per bone index (used for smoothing). */
	TMap<int32, FVector> SmoothedPerBoneForces;

	/** Bone index -> bone name cache (from SkeletalMesh). */
	TMap<int32, FName> BoneIndexToNameCache;

	/** Whether the bone name cache is initialized. */
	bool bBoneNameCacheInitialized = false;

	/** Debug log timer (prints every 3 seconds). */
	float PerBoneForceDebugTimer = 0.0f;

	//========================================
	// Bone Collision Events Internal State
	//========================================

	/** Current contact particle count per bone index. */
	TMap<int32, int32> CurrentBoneContactCounts;

	/** Average collision velocity per bone index (for Niagara direction). */
	TMap<int32, FVector> CurrentBoneAverageVelocities;

	/** Per-bone event cooldown timers (bone index -> remaining time). */
	TMap<int32, float> BoneEventCooldownTimers;

	/** Bones that had contacts last frame (for new contact detection). */
	TSet<int32> PreviousContactBones;

	/** Process bone collision events (called inside ProcessPerBoneForces). */
	void ProcessBoneCollisionEvents(float DeltaTime, const TArray<struct FGPUCollisionFeedback>& AllFeedback, int32 FeedbackCount);

	/** Initialize bone name cache. */
	void InitializeBoneNameCache();

	/** Process per-bone forces (called inside ProcessCollisionFeedback). */
	void ProcessPerBoneForces(float DeltaTime, const TArray<struct FGPUCollisionFeedback>& AllFeedback, int32 FeedbackCount, float ParticleRadius);

	/** Process GPU feedback (called every tick). */
	void ProcessCollisionFeedback(float DeltaTime);

	/** Update fluid tag enter/exit events. */
	void UpdateFluidTagEvents();

	/** Monitor per-bone impacts and fire events. */
	void CheckBoneImpacts();

	/** Auto-enable GPU collision feedback. */
	void EnableGPUCollisionFeedbackIfNeeded();

	//========================================
	// Boundary Particles (Flex-style Adhesion)
	//========================================

public:
	/**
	 * Enable boundary particle system.
	 * Generates particles on the mesh surface for Flex-style adhesion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (ToolTip = "Enable boundary particle system.\nCreates invisible particles on the mesh surface\nfor natural Flex-style adhesion/cohesion."))
	bool bEnableBoundaryParticles = false;

	/**
	 * Boundary particle spacing (cm).
	 * Recommended: 0.5-1.0x ParticleRadius.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bEnableBoundaryParticles", ClampMin = "1.0", ClampMax = "50.0",
	                  ToolTip = "Boundary particle spacing (cm).\nSmaller is more precise but increases particle count.\nRecommended: 0.5-1.0x ParticleRadius."))
	float BoundaryParticleSpacing = 5.0f;

	/**
	 * Coulomb friction coefficient for boundary particles.
	 * Controls how much fluid slows down when sliding on the surface.
	 * 0.0 = frictionless (ice), 0.6 = default, 1.0+ = sticky surface
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bEnableBoundaryParticles", ClampMin = "0.0", ClampMax = "2.0",
	                  ToolTip = "Coulomb friction coefficient.\n0.0 = frictionless (ice)\n0.6 = default\n1.0+ = sticky surface (honey, slime)"))
	float BoundaryFrictionCoefficient = 0.6f;

	/**
	 * Show boundary particles for debug.
	 * Visualizes boundary particles in the viewport.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bEnableBoundaryParticles",
	                  ToolTip = "Show boundary particles for debug.\nVisualizes boundary particle positions as small spheres."))
	bool bShowBoundaryParticles = false;

	/** Debug particle color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bShowBoundaryParticles"))
	FColor BoundaryParticleDebugColor = FColor::Cyan;

	/** Debug particle size (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bShowBoundaryParticles", ClampMin = "0.5", ClampMax = "10.0"))
	float BoundaryParticleDebugSize = 2.0f;

	/**
	 * Show boundary particle normals.
	 * Visualizes surface normals as arrows.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bShowBoundaryParticles",
	                  ToolTip = "Show boundary particle normals.\nVisualizes each boundary particle's surface normal as an arrow."))
	bool bShowBoundaryNormals = false;

	/** Normal arrow length (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Interaction|Boundary Particles",
	          meta = (EditCondition = "bShowBoundaryParticles && bShowBoundaryNormals", ClampMin = "1.0", ClampMax = "50.0"))
	float BoundaryNormalLength = 10.0f;

	/** Get boundary particle count. */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Boundary Particles")
	int32 GetBoundaryParticleCount() const { return BoundaryParticlePositions.Num(); }

	/** Get boundary particle positions (world space). */
	UFUNCTION(BlueprintPure, Category = "Fluid Interaction|Boundary Particles")
	const TArray<FVector>& GetBoundaryParticlePositions() const { return BoundaryParticlePositions; }

	/** Regenerate boundary particles manually. */
	UFUNCTION(BlueprintCallable, Category = "Fluid Interaction|Boundary Particles")
	void RegenerateBoundaryParticles();

	/** Collect boundary particle data for GPU (legacy - CPU world-space upload). */
	void CollectGPUBoundaryParticles(struct FGPUBoundaryParticles& OutBoundaryParticles) const;

	/** Collect local boundary particle data for GPU skinning (one-time upload).
	 * @param OutLocalParticles - Output array for local boundary particles
	 * @param Psi - Boundary particle volume contribution (calculated from Preset)
	 * @param Friction - Friction coefficient (from Preset)
	 */
	void CollectLocalBoundaryParticles(TArray<struct FGPUBoundaryParticleLocal>& OutLocalParticles, float Psi, float Friction) const;

	/** Collect bone transforms for GPU skinning (per-frame upload). */
	void CollectBoneTransformsForBoundary(TArray<FMatrix>& OutBoneTransforms, FMatrix& OutComponentTransform) const;

	/** Get component unique ID (for GPU skinning owner ID). */
	int32 GetBoundaryOwnerID() const { return GetUniqueID(); }

	/** Whether GPU skinning is enabled (true when boundary particles and local data exist). */
	bool HasLocalBoundaryParticles() const { return bEnableBoundaryParticles && bBoundaryParticlesInitialized && BoundaryParticleLocalPositions.Num() > 0; }

	/** Check if initialized local particles exist (independent of bEnableBoundaryParticles). */
	bool HasInitializedBoundaryParticles() const { return bBoundaryParticlesInitialized && BoundaryParticleLocalPositions.Num() > 0; }

	/** Whether boundary adhesion is enabled. */
	bool IsBoundaryAdhesionEnabled() const { return bEnableBoundaryParticles && bBoundaryParticlesInitialized && BoundaryParticlePositions.Num() > 0; }

private:
	/** Boundary particle world positions (updated every frame). */
	TArray<FVector> BoundaryParticlePositions;

	/** Boundary particle local positions (mesh surface, generated on init). */
	TArray<FVector> BoundaryParticleLocalPositions;

	/** Boundary particle surface normals (world space, updated every frame). */
	TArray<FVector> BoundaryParticleNormals;

	/** Boundary particle local normals (mesh surface, generated on init). */
	TArray<FVector> BoundaryParticleLocalNormals;

	/** Bone index that owns each boundary particle (-1 = static). */
	TArray<int32> BoundaryParticleBoneIndices;

	/** For skeletal meshes: vertex indices for GetSkinnedVertexPosition. */
	TArray<int32> BoundaryParticleVertexIndices;

	/** Whether this is a skeletal mesh. */
	bool bIsSkeletalMesh = false;

	/** Whether boundary particles are initialized. */
	bool bBoundaryParticlesInitialized = false;

	/** Generate boundary particles (sample mesh surface). */
	void GenerateBoundaryParticles();

	/** Update boundary particle positions (apply bone transforms for skeletal meshes). */
	void UpdateBoundaryParticlePositions();

	/** Draw debug boundary particles. */
	void DrawDebugBoundaryParticles();

	/** Sample triangle surface. */
	void SampleTriangleSurface(const FVector& V0, const FVector& V1, const FVector& V2,
	                           float Spacing, TArray<FVector>& OutPoints);

	//=============================================================================
	// Surface sampling based on Physics Asset / Simple Collision
	//=============================================================================

	/** Sample sphere collider surface. */
	void SampleSphereSurface(const struct FKSphereElem& Sphere, int32 BoneIndex, const FTransform& LocalTransform);

	/** Sample capsule (sphyl) collider surface. */
	void SampleCapsuleSurface(const struct FKSphylElem& Capsule, int32 BoneIndex);

	/** Sample box collider surface. */
	void SampleBoxSurface(const struct FKBoxElem& Box, int32 BoneIndex);

	/** Sample hemisphere surface (capsule top/bottom). */
	void SampleHemisphere(const FTransform& Transform, float Radius, float ZOffset,
	                      int32 ZDirection, int32 BoneIndex, int32 NumSamples);

	/** Sample all primitives from AggGeom. */
	void SampleAggGeomSurfaces(const struct FKAggregateGeom& AggGeom, int32 BoneIndex);
};
