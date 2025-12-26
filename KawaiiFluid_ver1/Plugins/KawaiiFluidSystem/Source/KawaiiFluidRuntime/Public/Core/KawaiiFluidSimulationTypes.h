// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include <atomic>
#include "KawaiiFluidSimulationTypes.generated.h"

class UFluidCollider;
class UFluidInteractionComponent;
class UKawaiiFluidSimulationComponent;
class UKawaiiFluidComponent;

/**
 * Collision event data
 */
struct FKawaiiFluidCollisionEvent
{
	int32 ParticleIndex;
	TWeakObjectPtr<AActor> HitActor;
	FVector HitLocation;
	FVector HitNormal;
	float HitSpeed;

	FKawaiiFluidCollisionEvent() = default;
	FKawaiiFluidCollisionEvent(int32 InIndex, AActor* InActor, const FVector& InLocation, const FVector& InNormal, float InSpeed)
		: ParticleIndex(InIndex), HitActor(InActor), HitLocation(InLocation), HitNormal(InNormal), HitSpeed(InSpeed) {}
};

/** Collision event callback signature */
DECLARE_DELEGATE_OneParam(FOnFluidCollisionEvent, const FKawaiiFluidCollisionEvent&);

/**
 * Simulation parameters passed to Context
 * Contains external forces, colliders, and other per-frame data
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FKawaiiFluidSimulationParams
{
	GENERATED_BODY()

	/** External force accumulated this frame */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	FVector ExternalForce = FVector::ZeroVector;

	/** Registered colliders */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	TArray<UFluidCollider*> Colliders;

	/** Registered interaction components */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	TArray<UFluidInteractionComponent*> InteractionComponents;

	/** World reference for collision queries */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	TObjectPtr<UWorld> World = nullptr;

	/** Use world collision */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	bool bUseWorldCollision = true;

	/** Collision channel for world collision */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	TEnumAsByte<ECollisionChannel> CollisionChannel = ECC_GameTraceChannel1;

	/** Particle render radius (for collision detection) */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	float ParticleRadius = 5.0f;

	/** Actor to ignore in collision queries */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	TWeakObjectPtr<AActor> IgnoreActor;

	//========================================
	// Collision Event Settings
	//========================================

	/** Enable collision events */
	bool bEnableCollisionEvents = false;

	/** Minimum velocity for collision event (cm/s) */
	float MinVelocityForEvent = 50.0f;

	/** Max events per frame */
	int32 MaxEventsPerFrame = 10;

	/**
	 * Pointer to atomic event counter (thread-safe, managed externally)
	 * Must be set before simulation if collision events are enabled
	 */
	std::atomic<int32>* EventCountPtr = nullptr;

	/** Per-particle event cooldown in seconds (prevents same particle spamming events) */
	float EventCooldownPerParticle = 0.1f;

	/** Pointer to per-particle last event time map (managed by component) */
	TMap<int32, float>* ParticleLastEventTimePtr = nullptr;

	/** Current game time for cooldown calculation */
	float CurrentGameTime = 0.0f;

	/** Collision event callback (non-UPROPERTY, set by component) */
	FOnFluidCollisionEvent OnCollisionEvent;

	FKawaiiFluidSimulationParams() = default;
};

/**
 * Batching info for merge/split operations
 * Note: Uses raw pointer since this struct is only used within a single frame
 */
struct FKawaiiFluidBatchInfo
{
	/** Component that owns these particles (raw pointer - only valid during batch operation) */
	UKawaiiFluidSimulationComponent* Component = nullptr;

	/** Start index in merged buffer */
	int32 StartIndex = 0;

	/** Number of particles from this component */
	int32 ParticleCount = 0;

	FKawaiiFluidBatchInfo() = default;
	FKawaiiFluidBatchInfo(UKawaiiFluidSimulationComponent* InComponent, int32 InStart, int32 InCount)
		: Component(InComponent), StartIndex(InStart), ParticleCount(InCount) {}
};

/**
 * Batching info for new modular components (UKawaiiFluidComponent)
 */
struct FKawaiiFluidModularBatchInfo
{
	/** Component that owns these particles */
	UKawaiiFluidComponent* Component = nullptr;

	/** Start index in merged buffer */
	int32 StartIndex = 0;

	/** Number of particles from this component */
	int32 ParticleCount = 0;

	FKawaiiFluidModularBatchInfo() = default;
	FKawaiiFluidModularBatchInfo(UKawaiiFluidComponent* InComponent, int32 InStart, int32 InCount)
		: Component(InComponent), StartIndex(InStart), ParticleCount(InCount) {}
};
