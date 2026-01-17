// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Components/FluidInteractionComponent.h"
#include "KawaiiFluidSimulatorSubsystem.generated.h"

class UKawaiiFluidComponent;
class UKawaiiFluidSimulationModule;
class UKawaiiFluidSimulationContext;
class UKawaiiFluidPresetDataAsset;
class UKawaiiFluidSimulationVolumeComponent;
class UFluidCollider;
class UFluidInteractionComponent;
class AActor;
class ULevel;
enum class EFluidType : uint8;
class FSpatialHash;
struct FFluidParticle;

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
	TObjectPtr<UKawaiiFluidSimulationVolumeComponent> VolumeComponent = nullptr;

	UPROPERTY()
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset = nullptr;

	FContextCacheKey() = default;
	FContextCacheKey(UKawaiiFluidSimulationVolumeComponent* InVolumeComponent, UKawaiiFluidPresetDataAsset* InPreset)
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
	// Volume Registration
	//========================================

	/** Register a simulation volume component (defines Z-Order space bounds) */
	void RegisterVolumeComponent(UKawaiiFluidSimulationVolumeComponent* VolumeComponent);

	/** Unregister a simulation volume component */
	void UnregisterVolumeComponent(UKawaiiFluidSimulationVolumeComponent* VolumeComponent);

	/** Get all registered volume components */
	const TArray<TObjectPtr<UKawaiiFluidSimulationVolumeComponent>>& GetAllVolumeComponents() const { return AllVolumeComponents; }

	//========================================
	// Component Registration (for backward compatibility)
	//========================================

	/** Register fluid component (deprecated - use RegisterModule) */
	void RegisterComponent(UKawaiiFluidComponent* Component);

	/** Unregister fluid component (deprecated - use UnregisterModule) */
	void UnregisterComponent(UKawaiiFluidComponent* Component);

	/** Get all registered components (deprecated) */
	const TArray<TObjectPtr<UKawaiiFluidComponent>>& GetAllFluidComponents() const { return AllFluidComponents; }

	//========================================
	// Global Colliders
	//========================================

	/** Register global collider (affects all fluids) */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid")
	void RegisterGlobalCollider(UFluidCollider* Collider);

	/** Unregister global collider */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid")
	void UnregisterGlobalCollider(UFluidCollider* Collider);

	/** Get all global colliders */
	const TArray<TObjectPtr<UFluidCollider>>& GetGlobalColliders() const { return GlobalColliders; }

	//========================================
	// Global Interaction Components
	//========================================

	/** Register global interaction component (for bone tracking) */
	void RegisterGlobalInteractionComponent(UFluidInteractionComponent* Component);

	/** Unregister global interaction component */
	void UnregisterGlobalInteractionComponent(UFluidInteractionComponent* Component);

	/** Get all global interaction components */
	const TArray<TObjectPtr<UFluidInteractionComponent>>& GetGlobalInteractionComponents() const { return GlobalInteractionComponents; }

	//========================================
	// Query API
	//========================================

	/** Get all particles within radius (across all components) */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid|Query")
	TArray<FFluidParticle> GetAllParticlesInRadius(FVector Location, float Radius) const;

	/** Get total particle count */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid|Query")
	int32 GetTotalParticleCount() const;

	/** Get component count */
	UFUNCTION(BlueprintCallable, Category = "KawaiiFluid|Query")
	int32 GetComponentCount() const { return AllFluidComponents.Num(); }

	/**
	 * Get Preset by SourceID (for collision event filtering)
	 * @param SourceID Particle source ID (from FGPUCollisionFeedback::ParticleSourceID)
	 * @return Preset of the module that owns this source, or nullptr if not found
	 */
	UFUNCTION(BlueprintPure, Category = "KawaiiFluid|Query")
	UKawaiiFluidPresetDataAsset* GetPresetBySourceID(int32 SourceID) const;

	/**
	 * Get Module by SourceID
	 * @param SourceID Particle source ID
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
	UKawaiiFluidSimulationContext* GetOrCreateContext(UKawaiiFluidSimulationVolumeComponent* VolumeComponent, UKawaiiFluidPresetDataAsset* Preset);

	/** Legacy: Get or create context without volume (uses component-relative bounds) */
	UKawaiiFluidSimulationContext* GetOrCreateContext(UKawaiiFluidPresetDataAsset* Preset)
	{
		return GetOrCreateContext(nullptr, Preset);
	}

private:
	//========================================
	// Module Management (New)
	//========================================

	/** All registered simulation modules */
	UPROPERTY()
	TArray<TObjectPtr<UKawaiiFluidSimulationModule>> AllModules;

	//========================================
	// Volume Component Management
	//========================================

	/** All registered simulation volume components */
	UPROPERTY()
	TArray<TObjectPtr<UKawaiiFluidSimulationVolumeComponent>> AllVolumeComponents;

	//========================================
	// Component Management (Deprecated)
	//========================================

	/** All registered modular components (deprecated - kept for backward compatibility) */
	UPROPERTY()
	TArray<TObjectPtr<UKawaiiFluidComponent>> AllFluidComponents;

	/** Global colliders */
	UPROPERTY()
	TArray<TObjectPtr<UFluidCollider>> GlobalColliders;

	/** Global interaction components */
	UPROPERTY()
	TArray<TObjectPtr<UFluidInteractionComponent>> GlobalInteractionComponents;

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

	/** CPU 충돌 피드백 버퍼 (Context에서 추가, 시뮬레이션 후 처리) */
	TArray<FKawaiiFluidCollisionEvent> CPUCollisionFeedbackBuffer;

	/** CPU 충돌 피드백 버퍼 락 (ParallelFor 안전) */
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

	FDelegateHandle OnActorSpawnedHandle;
	FDelegateHandle OnLevelAddedHandle;
	FDelegateHandle OnLevelRemovedHandle;
};
