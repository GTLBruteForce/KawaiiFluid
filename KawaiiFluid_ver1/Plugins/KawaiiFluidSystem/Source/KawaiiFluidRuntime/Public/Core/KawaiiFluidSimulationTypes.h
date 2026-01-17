// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include <atomic>
#include "KawaiiFluidSimulationTypes.generated.h"

class UFluidCollider;
class UFluidInteractionComponent;
class UKawaiiFluidComponent;
class UKawaiiFluidSimulationModule;

/**
 * World collision detection method
 */
UENUM(BlueprintType)
enum class EWorldCollisionMethod : uint8
{
	/** Legacy sweep-based collision (SweepSingleByChannel) */
	Sweep UMETA(DisplayName = "Sweep (Legacy)"),

	/** SDF-based collision using Overlap + ClosestPoint */
	SDF UMETA(DisplayName = "SDF (Distance-based)")
};

/**
 * Grid resolution preset for Z-Order sorting
 *
 * Controls the number of bits per axis for Morton code encoding.
 * Higher bits = larger simulation bounds but more memory/computation.
 *
 * Bounds extent formula: GridResolution × CellSize
 * - Small (6 bits):  64³ = 262,144 cells,  64 × CellSize bounds
 * - Medium (7 bits): 128³ = 2,097,152 cells, 128 × CellSize bounds
 * - Large (8 bits):  256³ = 16,777,216 cells, 256 × CellSize bounds
 *
 * For 100k particle simulations, Medium (7 bits) is recommended.
 */
UENUM(BlueprintType)
enum class EGridResolutionPreset : uint8
{
	/** 6 bits per axis = 64³ cells (262,144 total)
	 * Smallest bounds, fastest computation
	 * Good for: Small-scale simulations, local effects */
	Small UMETA(DisplayName = "Small"),

	/** 7 bits per axis = 128³ cells (2,097,152 total)
	 * Balanced bounds and performance
	 * Good for: Medium-scale simulations, 100k particles */
	Medium UMETA(DisplayName = "Medium"),

	/** 8 bits per axis = 256³ cells (16,777,216 total)
	 * Largest bounds, more computation
	 * Good for: Large-scale simulations, environmental effects */
	Large UMETA(DisplayName = "Large")
};

/** Helper functions for EGridResolutionPreset */
namespace GridResolutionPresetHelper
{
	/** Get bits per axis from preset */
	inline int32 GetAxisBits(EGridResolutionPreset Preset)
	{
		switch (Preset)
		{
		case EGridResolutionPreset::Small:  return 6;
		case EGridResolutionPreset::Medium: return 7;
		case EGridResolutionPreset::Large:  return 8;
		default: return 7;
		}
	}

	/** Get grid resolution per axis (2^bits) */
	inline int32 GetGridResolution(EGridResolutionPreset Preset)
	{
		return 1 << GetAxisBits(Preset);
	}

	/** Get total cell count (resolution³) */
	inline int32 GetMaxCells(EGridResolutionPreset Preset)
	{
		const int32 Res = GetGridResolution(Preset);
		return Res * Res * Res;
	}

	/** Get display name for UI */
	inline FString GetDisplayName(EGridResolutionPreset Preset)
	{
		switch (Preset)
		{
		case EGridResolutionPreset::Small:  return TEXT("Small");
		case EGridResolutionPreset::Medium: return TEXT("Medium");
		case EGridResolutionPreset::Large:  return TEXT("Large");
		default: return TEXT("Unknown");
		}
	}
}

/**
 * Debug visualization modes for fluid particles
 */
UENUM(BlueprintType)
enum class EFluidDebugVisualization : uint8
{
	/** Normal rendering (no debug) */
	None			UMETA(DisplayName = "None"),

	//--- Z-Order Sorting Verification ---
	// Use these modes to verify Z-Order (Morton code) sorting is working correctly.
	// If sorting works, spatially close particles will have similar array indices.

	/** [Z-Order] Color by array index - spatially close particles should have similar colors if sorted */
	ZOrderArrayIndex	UMETA(DisplayName = "Z-Order: Array Index"),
	/** [Z-Order] Color by Morton code computed from position */
	ZOrderMortonCode	UMETA(DisplayName = "Z-Order: Morton Code"),

	//--- General Debug Visualization ---

	/** Color by X position (Red gradient) */
	PositionX		UMETA(DisplayName = "Position X"),
	/** Color by Y position (Green gradient) */
	PositionY		UMETA(DisplayName = "Position Y"),
	/** Color by Z position (Blue gradient) */
	PositionZ		UMETA(DisplayName = "Position Z"),
	/** Color by density value */
	Density			UMETA(DisplayName = "Density"),

	//--- Legacy (deprecated, use ZOrderArrayIndex/ZOrderMortonCode instead) ---
	ArrayIndex		UMETA(DisplayName = "Array Index (Legacy)", Hidden),
	MortonCode		UMETA(DisplayName = "Morton Code (Legacy)", Hidden),
};

/**
 * Collision event data
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FKawaiiFluidCollisionEvent
{
	GENERATED_BODY()

	// ID-based (from GPU)
	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	int32 ParticleIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	int32 SourceID = -1;              // Particle source Component ID

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	int32 ColliderOwnerID = -1;       // Hit target Actor ID

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	int32 BoneIndex = -1;             // Hit bone index (-1 = none)

	// Pointer-based (looked up)
	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	TObjectPtr<AActor> HitActor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	TObjectPtr<UKawaiiFluidComponent> SourceComponent = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	TObjectPtr<UFluidInteractionComponent> HitInteractionComponent = nullptr;

	// Collision data
	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	FVector HitLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	FVector HitNormal = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	float HitSpeed = 0.0f;
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
	TArray<TObjectPtr<UFluidCollider>> Colliders;

	/** Registered interaction components */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	TArray<TObjectPtr<UFluidInteractionComponent>> InteractionComponents;

	/** World reference for collision queries */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	TObjectPtr<UWorld> World = nullptr;

	/** Use world collision */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	bool bUseWorldCollision = true;

	/** World collision detection method */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	EWorldCollisionMethod WorldCollisionMethod = EWorldCollisionMethod::SDF;

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
	// GPU Simulation
	//========================================

	/** Grid resolution preset for Z-Order sorting (determines shader permutation) */
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;

	/**
	 * Simulation origin (component world location)
	 * Used to offset preset simulation bounds to world space
	 * Preset bounds are defined relative to component, not absolute world coordinates
	 */
	FVector SimulationOrigin = FVector::ZeroVector;

	/** World bounds for GPU AABB collision (optional) */
	UPROPERTY(BlueprintReadWrite, Category = "Simulation")
	FBox WorldBounds = FBox(EForceInit::ForceInit);

	/** Bounds center (world space) - for OBB collision */
	FVector BoundsCenter = FVector::ZeroVector;

	/** Bounds half-extent (local space) - for OBB collision */
	FVector BoundsExtent = FVector::ZeroVector;

	/** Bounds rotation - for OBB collision (identity = AABB mode) */
	FQuat BoundsRotation = FQuat::Identity;

	/** Bounds collision restitution (bounciness) - used for Containment on GPU */
	float BoundsRestitution = 0.3f;

	/** Bounds collision friction - used for Containment on GPU */
	float BoundsFriction = 0.1f;

	//========================================
	// Static Boundary Particles (Akinci 2012)
	//========================================

	/** Enable static boundary particles for density contribution at walls/floors
	 * This helps prevent density deficit near boundaries which causes wall climbing artifacts */
	bool bEnableStaticBoundaryParticles = true;

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

	/** Source ID for filtering collision events (only events from this source trigger callback) */
	int32 SourceID = -1;

	//========================================
	// Shape Matching (Slime)
	//========================================

	/** Enable shape matching constraint */
	bool bEnableShapeMatching = false;

	/** Shape matching stiffness (0 = no restoration, 1 = rigid) */
	float ShapeMatchingStiffness = 0.01f;

	/** Core particle stiffness multiplier */
	float ShapeMatchingCoreMultiplier = 1.0f;

	/** Core density constraint reduction (0 = full density effect, 1 = no density effect for core) */
	float CoreDensityConstraintReduction = 0.0f;

	//========================================
	// Surface Detection (Slime)
	//========================================

	/** Neighbor count threshold for surface detection (fewer neighbors = surface particle) */
	int32 SurfaceNeighborThreshold = 25;

	//========================================
	// CPU Collision Feedback Buffer (for deferred processing)
	//========================================

	/** CPU 충돌 피드백 버퍼 포인터 (Subsystem이 소유, Context가 추가) */
	TArray<FKawaiiFluidCollisionEvent>* CPUCollisionFeedbackBufferPtr = nullptr;

	/** CPU 충돌 피드백 버퍼 락 (ParallelFor 안전) */
	FCriticalSection* CPUCollisionFeedbackLockPtr = nullptr;

	FKawaiiFluidSimulationParams() = default;
};

/**
 * Batching info for Module-based simulation
 */
struct FKawaiiFluidModuleBatchInfo
{
	/** Module that owns these particles */
	TObjectPtr<UKawaiiFluidSimulationModule> Module = nullptr;

	/** Start index in merged buffer */
	int32 StartIndex = 0;

	/** Number of particles from this module */
	int32 ParticleCount = 0;

	FKawaiiFluidModuleBatchInfo() = default;
	FKawaiiFluidModuleBatchInfo(UKawaiiFluidSimulationModule* InModule, int32 InStart, int32 InCount)
		: Module(InModule), StartIndex(InStart), ParticleCount(InCount) {}
};
