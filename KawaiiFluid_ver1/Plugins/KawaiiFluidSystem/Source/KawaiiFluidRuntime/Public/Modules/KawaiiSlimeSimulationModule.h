// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "KawaiiSlimeSimulationModule.generated.h"

/**
 * Slime Interaction Delegates (Section 11 of SlimeImplementationInsights.md)
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSlimeGroundContactModule, FVector, Location, FVector, Normal);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSlimeObjectEnteredModule, AActor*, Object);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSlimeObjectExitedModule, AActor*, Object);

/**
 * Kawaii Slime Simulation Module
 *
 * Inherits from UKawaiiFluidSimulationModule and adds slime-specific features.
 *
 * Core Features (Section 3):
 * 1. Shape Matching - Form restoration (most important!)
 * 2. Clustering - Split/merge handling (Union-Find)
 * 3. Nucleus Control - Player movement (core particles only)
 *
 * Additional Features:
 * - Surface Tension (Section 7)
 * - Anti-Gravity (Section 13.1)
 * - Decompose Mode (fluid-like behavior)
 * - Interaction Events (Section 11)
 */
UCLASS(DefaultToInstanced, EditInlineNew, BlueprintType)
class KAWAIIFLUIDRUNTIME_API UKawaiiSlimeSimulationModule : public UKawaiiFluidSimulationModule
{
	GENERATED_BODY()

public:
	UKawaiiSlimeSimulationModule();

	//========================================
	// Initialization
	//========================================

	/** Initialize module (parent + slime-specific) */
	virtual void Initialize(UKawaiiFluidPresetDataAsset* InPreset) override;

	//========================================
	// Tick (Slime-specific logic)
	//========================================

	/** Slime-specific Tick - called from Component */
	UFUNCTION(BlueprintCallable, Category = "Slime|Module")
	void TickSlime(float DeltaTime);

	//========================================
	// Shape Matching (Section 4)
	// Most important for slime form maintenance!
	//========================================

	/** Enable shape matching constraint */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|ShapeMatching")
	bool bEnableShapeMatching = true;

	/** Shape matching stiffness (0 = soft slime, 1 = rigid jelly) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|ShapeMatching", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ShapeMatchingStiffness = 0.3f;

	/** Core particle stiffness multiplier (anchor particles get stronger restoration) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|ShapeMatching", meta = (ClampMin = "1.0", ClampMax = "5.0"))
	float CoreStiffnessMultiplier = 2.5f;

	/** Core radius ratio (particles within this ratio from center are "core") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|ShapeMatching", meta = (ClampMin = "0.1", ClampMax = "0.5"))
	float CoreRadiusRatio = 0.3f;

	//========================================
	// Nucleus Control (Section 6)
	// Player input goes to nucleus -> particles follow
	//========================================

	/** Nucleus position (center of slime, camera follows this) */
	UPROPERTY(BlueprintReadOnly, Category = "Slime|Nucleus")
	FVector NucleusPosition;

	/** Nucleus velocity */
	UPROPERTY(BlueprintReadOnly, Category = "Slime|Nucleus")
	FVector NucleusVelocity;

	/** Nucleus attraction strength - how strongly particles are pulled toward center */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Nucleus", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float NucleusAttractionStrength = 15.0f;

	/** Attraction falloff - outer particles get weaker attraction (0 = uniform, 1 = strong falloff) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Nucleus", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AttractionFalloff = 0.3f;

	/** How strongly nucleus follows particle center (0 = fixed, 1 = instant follow) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Nucleus", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float NucleusFollowStrength = 0.1f;

	//========================================
	// Movement (Player Input - Section 6.2)
	//========================================

	/** Movement force applied to core particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Movement")
	float MoveForce = 500.0f;

	/** Jump impulse strength */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Movement")
	float JumpStrength = 800.0f;

	/** Max move speed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Movement")
	float MaxMoveSpeed = 600.0f;

	//========================================
	// Clustering (Section 5)
	// Split/merge detection using Union-Find
	//========================================

	/** Enable clustering (split/merge detection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Cluster")
	bool bEnableClustering = true;

	/** Main cluster ID (player controlled) */
	UPROPERTY(BlueprintReadOnly, Category = "Slime|Cluster")
	int32 MainSourceID = 0;

	/** Number of clusters */
	UPROPERTY(BlueprintReadOnly, Category = "Slime|Cluster")
	int32 ClusterCount = 1;

	//========================================
	// Surface Tension (Section 7)
	//========================================

	/** Enable surface tension for slime */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Surface")
	bool bEnableSurfaceTension = true;

	/** Surface tension coefficient */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Surface", meta = (ClampMin = "0.0"))
	float SurfaceTensionCoefficient = 0.5f;

	/** Surface detection threshold (neighbor count - lower = more surface particles) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Surface", meta = (ClampMin = "1", ClampMax = "50"))
	int32 SurfaceNeighborThreshold = 25;

	//========================================
	// Decompose Mode (Fluid-like behavior)
	//========================================

	/** Enable decompose mode - particles behave like fluid (no shape matching/nucleus attraction) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Decompose")
	bool bDecomposeMode = false;

	/** Auto-recompose after time (0 = never) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Decompose", meta = (ClampMin = "0.0"))
	float RecomposeDelay = 3.0f;

	//========================================
	// Anti-Gravity (Section 13.1 - Jump Form Preservation)
	//========================================

	/** Anti-gravity strength during jump (counters gravity to maintain form) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|AntiGravity", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AntiGravityStrength = 0.5f;

	/** Is slime currently in air (jumping) */
	UPROPERTY(BlueprintReadOnly, Category = "Slime|AntiGravity")
	bool bIsInAir = false;

	/** Grounded threshold (ratio of particles touching ground) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|AntiGravity", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GroundedThreshold = 0.2f;

	//========================================
	// Interaction Events (Section 11)
	//========================================

	/** Fired when slime contacts ground */
	UPROPERTY(BlueprintAssignable, Category = "Slime|Events")
	FOnSlimeGroundContactModule OnGroundContact;

	/** Fired when object enters slime */
	UPROPERTY(BlueprintAssignable, Category = "Slime|Events")
	FOnSlimeObjectEnteredModule OnObjectEntered;

	/** Fired when object exits slime */
	UPROPERTY(BlueprintAssignable, Category = "Slime|Events")
	FOnSlimeObjectExitedModule OnObjectExited;

	/** Threshold for "inside slime" detection (nearby particle count) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Interaction", meta = (ClampMin = "1", ClampMax = "20"))
	int32 InsideThreshold = 5;

	//========================================
	// Blueprint API
	//========================================

	//========================================
	// Uniform Spawn (for stable surface)
	//========================================

	/** Spawn particles in uniform distribution (Fibonacci sphere + concentric layers)
	 * This creates stable, evenly distributed particles that ShapeMatching can maintain
	 */
	UFUNCTION(BlueprintCallable, Category = "Slime|Spawn")
	void SpawnParticlesUniform(FVector Location, int32 Count, float Radius);

	/** Apply movement input (call from Pawn) - Section 6.2/10.3 */
	UFUNCTION(BlueprintCallable, Category = "Slime|Input")
	void ApplyMovementInput(FVector Input);

	/** Apply jump impulse */
	UFUNCTION(BlueprintCallable, Category = "Slime|Input")
	void ApplyJumpImpulse();

	/** Toggle decompose mode */
	UFUNCTION(BlueprintCallable, Category = "Slime|Input")
	void SetDecomposeMode(bool bEnable);

	/** Get center of mass for main cluster */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	FVector GetMainClusterCenter() const;

	/** Get particle count for main cluster */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	int32 GetMainClusterParticleCount() const;

	/** Check if actor is inside slime (Section 11.2) */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	bool IsActorInsideSlime(AActor* Actor) const;

	/** Check if slime is grounded */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	bool IsGrounded() const;

	//========================================
	// Simulation Params Override
	//========================================

	virtual FKawaiiFluidSimulationParams BuildSimulationParams() const override;

	//========================================
	// Owner Actor Setup (for Nucleus update)
	//========================================

	/** Set owner actor reference */
	void SetOwnerActor(AActor* InOwner) { OwnerActorWeak = InOwner; }

protected:
	//========================================
	// Core Slime Logic (Section 3)
	//========================================

	/** Initialize rest shape for shape matching (called when particles spawn) */
	void InitializeRestShape();

	/** Update core particle flags based on distance from center */
	void UpdateCoreParticles();

	/** Apply nucleus attraction - pull particles toward center (Section 6.2 Method 1) */
	void ApplyNucleusAttraction(float DeltaTime);

	/** Update nucleus position and velocity */
	void UpdateNucleus(float DeltaTime);

	/** Apply anti-gravity during jump */
	void ApplyAntiGravity(float DeltaTime);

	/** Update cluster assignments using Union-Find (Section 5.3) */
	void UpdateClusters();

	/** Detect surface particles and compute normals (Section 7.3) */
	void UpdateSurfaceParticles();

	/** Apply surface tension to surface particles (Section 7.2) */
	void ApplySurfaceTension();

	/** Check ground contact and update grounded state */
	void UpdateGroundedState();

	/** Check for ground contact and fire events (Section 11.3) */
	void CheckGroundContact();

	/** Track objects inside slime for enter/exit events */
	void UpdateObjectTracking();

private:
	//========================================
	// Union-Find Helpers (Section 5.3)
	//========================================

	int32 FindRoot(TArray<int32>& Parent, int32 Index);
	void UnionSets(TArray<int32>& Parent, TArray<int32>& Rank, int32 A, int32 B);

	//========================================
	// Internal State
	//========================================

	/** Owner actor weak reference */
	TWeakObjectPtr<AActor> OwnerActorWeak;

	/** Timer for auto-recompose */
	float DecomposeTimer = 0.0f;

	/** Flag to initialize rest shape once */
	bool bRestShapeInitialized = false;

	/** Cached max distance from center (for core particle calculation) */
	float CachedMaxDistanceFromCenter = 0.0f;

	/** Tracked actors currently inside slime (for enter/exit events) */
	UPROPERTY()
	TSet<TWeakObjectPtr<AActor>> ActorsInsideSlime;

	/** Actors to check for interaction (registered externally or found via overlap) */
	UPROPERTY()
	TArray<TWeakObjectPtr<AActor>> TrackedActors;
};
