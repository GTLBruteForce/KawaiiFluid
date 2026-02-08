// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineBaseTypes.h"  // For ELevelTick
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Components/KawaiiFluidInteractionComponent.h"
#include "GPU/GPUFluidParticle.h"
#include "KawaiiFluidSimulatorSubsystem.generated.h"

class UKawaiiFluidSimulationModule;
class UKawaiiFluidSimulationContext;
class UKawaiiFluidPresetDataAsset;
class UKawaiiFluidVolumeComponent;
class AKawaiiFluidVolume;
class AKawaiiFluidEmitter;
class UKawaiiFluidCollider;
class UKawaiiFluidInteractionComponent;
class AActor;
class ULevel;
class FSpatialHash;
struct FFluidParticle;
class FGPUFluidSimulator;

/**
 * Cache key for Context lookup
 * Different VolumeComponents use different Z-Order spaces
 */
USTRUCT()
struct FContextCacheKey
{
	GENERATED_BODY()

	/** Target Volume for Z-Order space bounds (nullptr = component-relative bounds) */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidVolumeComponent> VolumeComponent = nullptr;

	UPROPERTY()
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset = nullptr;

	FContextCacheKey() = default;
	FContextCacheKey(UKawaiiFluidVolumeComponent* InVolumeComponent, UKawaiiFluidPresetDataAsset* InPreset)
		: VolumeComponent(InVolumeComponent), Preset(InPreset) {}

	// Legacy constructor for backward compatibility
	explicit FContextCacheKey(UKawaiiFluidPresetDataAsset* InPreset)
		: VolumeComponent(nullptr), Preset(InPreset) {}

	bool operator==(const FContextCacheKey& Other) const
	{
		return VolumeComponent == Other.VolumeComponent && Preset == Other.Preset;
	}

	friend uint32 GetTypeHash(const FContextCacheKey& Key)
	{
		uint32 Hash = GetTypeHash(Key.VolumeComponent);
		Hash = HashCombine(Hash, GetTypeHash(Key.Preset));
		return Hash;
	}
};

/**
 * Kawaii Fluid Simulator Subsystem
 *
 * Orchestration (Conductor) - manages all fluid simulations in the world
 *
 * Responsibilities:
 * - Manages all SimulationComponents
 * - Batching: Same preset components are merged -> simulated -> split
 * - Global collider management
 * - Query API
 */
UCLASS()
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidSimulatorSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UKawaiiFluidSimulatorSubsystem();
	virtual ~UKawaiiFluidSimulatorSubsystem() override;

	//========================================
	// USubsystem Interface
	//========================================

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return true; }
	virtual bool IsTickableInEditor() const override { return false; }

	//========================================
	// Module Registration (New)
	//========================================

	/** Register simulation module */
	void RegisterModule(UKawaiiFluidSimulationModule* Module);

	/** Unregister simulation module */
	void UnregisterModule(UKawaiiFluidSimulationModule* Module);

	/** Get all registered modules */
	const TArray<TObjectPtr<UKawaiiFluidSimulationModule>>& GetAllModules() const { return AllModules; }

	//========================================
	// SourceID Allocation (Per-Component GPU Counter)
	//========================================

	/** Allocate a unique SourceID (0 ~ MaxSourceCount-1) for GPU counter tracking */
	int32 AllocateSourceID();

	/** Release a SourceID back to the pool */
	void ReleaseSourceID(int32 SourceID);

	//========================================
	// Volume Actor Registration (New Architecture)
	//========================================

	/** Register a fluid volume actor (new architecture - solver unit) */
	void RegisterVolume(AKawaiiFluidVolume* Volume);

	/** Unregister a fluid volume actor */
	void UnregisterVolume(AKawaiiFluidVolume* Volume);

	/** Get all registered volume actors */
	const TArray<TObjectPtr<AKawaiiFluidVolume>>& GetAllVolumes() const { return AllVolumes; }

	//========================================
	// Volume Component Registration (Legacy)
	//========================================

	/** Register a simulation volume component (defines Z-Order space bounds) */
	void RegisterVolumeComponent(UKawaiiFluidVolumeComponent* VolumeComponent);

	/** Unregister a simulation volume component */
	void UnregisterVolumeComponent(UKawaiiFluidVolumeComponent* VolumeComponent);

	/** Get all registered volume components */
	const TArray<TObjectPtr<UKawaiiFluidVolumeComponent>>& GetAllVolumeComponents() const { return AllVolumeComponents; }

	//========================================
	// Global Colliders
	//========================================

	/** Register global collider (affects all fluids) */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid")
	void RegisterGlobalCollider(UKawaiiFluidCollider* Collider);

	/** Unregister global collider */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid")
	void UnregisterGlobalCollider(UKawaiiFluidCollider* Collider);

	/** Get all global colliders */
	const TArray<TObjectPtr<UKawaiiFluidCollider>>& GetGlobalColliders() const { return GlobalColliders; }

	//========================================
	// Global Interaction Components
	//========================================

	/** Register global interaction component (for bone tracking) */
	void RegisterGlobalInteractionComponent(UKawaiiFluidInteractionComponent* Component);

	/** Unregister global interaction component */
	void UnregisterGlobalInteractionComponent(UKawaiiFluidInteractionComponent* Component);

	/** Get all global interaction components */
	const TArray<TObjectPtr<UKawaiiFluidInteractionComponent>>& GetGlobalInteractionComponents() const { return GlobalInteractionComponents; }

	//========================================
	// Query API
	//========================================

	/** Get all particles within radius (across all components) */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid|Query")
	TArray<FFluidParticle> GetAllParticlesInRadius(FVector Location, float Radius) const;

	/** Get total particle count */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid|Query")
	int32 GetTotalParticleCount() const;

	/** Get module count */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid|Query")
	int32 GetModuleCount() const { return AllModules.Num(); }

	/**
	 * Get Preset by SourceID (for collision event filtering)
	 * @param SourceID Particle source slot ID (AllocateSourceID, 0~MaxSourceCount-1)
	 * @return Preset of the module that owns this source, or nullptr if not found
	 */
	UFUNCTION(BlueprintPure, Category = "KawaiiFluid|Query")
	UKawaiiFluidPresetDataAsset* GetPresetBySourceID(int32 SourceID) const;

	/**
	 * Get Module by SourceID
	 * @param SourceID Particle source slot ID (AllocateSourceID, 0~MaxSourceCount-1)
	 * @return Module that owns this source, or nullptr if not found
	 */
	UKawaiiFluidSimulationModule* GetModuleBySourceID(int32 SourceID) const;

	//========================================
	// Context Management
	//========================================

	/** Get or create context for volume component and preset
	 *  Same VolumeComponent = same Z-Order space = particles can interact
	 *  Always uses GPU simulation
	 */
	UKawaiiFluidSimulationContext* GetOrCreateContext(UKawaiiFluidVolumeComponent* VolumeComponent, UKawaiiFluidPresetDataAsset* Preset);

	/** Legacy: Get or create context without volume (uses component-relative bounds) */
	UKawaiiFluidSimulationContext* GetOrCreateContext(UKawaiiFluidPresetDataAsset* Preset)
	{
		return GetOrCreateContext(nullptr, Preset);
	}

	/**
	 * Get all active GPUSimulators (for ViewExtension deferred simulation execution)
	 * @param OutSimulators Array to fill with GPUSimulator pointers
	 */
	void GetAllGPUSimulators(TArray<FGPUFluidSimulator*>& OutSimulators) const;

private:
	//========================================
	// Module Management (New)
	//========================================

	/** All registered simulation modules */
	UPROPERTY()
	TArray<TObjectPtr<UKawaiiFluidSimulationModule>> AllModules;

	//========================================
	// SourceID Allocation State
	//========================================

	/** Bitfield tracking used SourceIDs (index = SourceID, true = in use) */
	TBitArray<> UsedSourceIDs;

	/** Next SourceID hint for faster allocation */
	int32 NextSourceIDHint = 0;

	//========================================
	// Volume Actor Management (New Architecture)
	//========================================

	/** All registered fluid volume actors (new architecture) */
	UPROPERTY()
	TArray<TObjectPtr<AKawaiiFluidVolume>> AllVolumes;

	//========================================
	// Volume Component Management (Legacy)
	//========================================

	/** All registered simulation volume components (legacy) */
	UPROPERTY()
	TArray<TObjectPtr<UKawaiiFluidVolumeComponent>> AllVolumeComponents;

	//========================================
	// Component Management (Deprecated)
	//========================================

	/** Global colliders */
	UPROPERTY()
	TArray<TObjectPtr<UKawaiiFluidCollider>> GlobalColliders;

	/** Global interaction components */
	UPROPERTY()
	TArray<TObjectPtr<UKawaiiFluidInteractionComponent>> GlobalInteractionComponents;

	/** Context cache (Preset + VolumeComponent -> Instance) */
	UPROPERTY()
	TMap<FContextCacheKey, TObjectPtr<UKawaiiFluidSimulationContext>> ContextCache;

	/** Default context for presets without custom context */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidSimulationContext> DefaultContext;

	//========================================
	// Batching Resources (Module-based)
	//========================================

	/** Shared spatial hash for batching */
	TSharedPtr<FSpatialHash> SharedSpatialHash;

	/** Batch info array (Module-based) */
	TArray<FKawaiiFluidModuleBatchInfo> ModuleBatchInfos;

	/** Merged particle buffer for module batching */
	TArray<FFluidParticle> MergedFluidParticleBuffer;

	/** Atomic event counter for thread-safe collision event tracking */
	std::atomic<int32> EventCountThisFrame{0};

	//========================================
	// CPU Collision Feedback Buffer
	//========================================

	/** CPU collision feedback buffer (written by Context, processed after simulation) */
	TArray<FKawaiiFluidCollisionEvent> CPUCollisionFeedbackBuffer;

	/** CPU collision feedback buffer lock (ParallelFor safe) */
	FCriticalSection CPUCollisionFeedbackLock;

	//========================================
	// Simulation Methods
	//========================================

	/** Simulate independent modules */
	void SimulateIndependentFluidComponents(float DeltaTime);

	/** Simulate batched modules */
	void SimulateBatchedFluidComponents(float DeltaTime);

	/** Group modules by Preset + VolumeComponent */
	TMap<FContextCacheKey, TArray<TObjectPtr<UKawaiiFluidSimulationModule>>> GroupModulesByContext() const;

	/** Merge particles from modules */
	void MergeModuleParticles(const TArray<TObjectPtr<UKawaiiFluidSimulationModule>>& Modules);

	/** Split particles back to modules */
	void SplitModuleParticles(const TArray<TObjectPtr<UKawaiiFluidSimulationModule>>& Modules);

	/** Build merged params from modules */
	FKawaiiFluidSimulationParams BuildMergedModuleSimulationParams(const TArray<TObjectPtr<UKawaiiFluidSimulationModule>>& Modules);

	//========================================
	// World Change Tracking (GPU world collision cache)
	//========================================

	void HandleActorSpawned(AActor* Actor);
	void HandleActorDestroyed(AActor* Actor);
	void HandleLevelAdded(ULevel* InLevel, UWorld* InWorld);
	void HandleLevelRemoved(ULevel* InLevel, UWorld* InWorld);
	void MarkAllContextsWorldCollisionDirty();

	/**
	 * Post-actor tick handler - runs simulation AFTER animation evaluation
	 * This fixes 1-frame delay in bone attachment following
	 * @param World - The world being ticked
	 * @param TickType - Type of tick (viewports only, time only, etc.)
	 * @param DeltaTime - Frame delta time
	 */
	void HandlePostActorTick(UWorld* World, ELevelTick TickType, float DeltaTime);

	FDelegateHandle OnActorSpawnedHandle;
	FDelegateHandle OnLevelAddedHandle;
	FDelegateHandle OnLevelRemovedHandle;
	FDelegateHandle OnPostActorTickHandle;
};
