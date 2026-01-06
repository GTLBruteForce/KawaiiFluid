// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "KawaiiFluidSimulatorSubsystem.generated.h"

class UKawaiiFluidComponent;
class UKawaiiFluidSimulationModule;
class UKawaiiFluidSimulationContext;
class UKawaiiFluidPresetDataAsset;
class UFluidCollider;
class UFluidInteractionComponent;
class FSpatialHash;
struct FFluidParticle;

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
	const TArray<UKawaiiFluidSimulationModule*>& GetAllModules() const { return AllModules; }

	//========================================
	// Component Registration (for backward compatibility)
	//========================================

	/** Register fluid component (deprecated - use RegisterModule) */
	void RegisterComponent(UKawaiiFluidComponent* Component);

	/** Unregister fluid component (deprecated - use UnregisterModule) */
	void UnregisterComponent(UKawaiiFluidComponent* Component);

	/** Get all registered components (deprecated) */
	const TArray<UKawaiiFluidComponent*>& GetAllFluidComponents() const { return AllFluidComponents; }

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
	const TArray<UFluidCollider*>& GetGlobalColliders() const { return GlobalColliders; }

	//========================================
	// Global Interaction Components
	//========================================

	/** Register global interaction component (for bone tracking) */
	void RegisterGlobalInteractionComponent(UFluidInteractionComponent* Component);

	/** Unregister global interaction component */
	void UnregisterGlobalInteractionComponent(UFluidInteractionComponent* Component);

	/** Get all global interaction components */
	const TArray<UFluidInteractionComponent*>& GetGlobalInteractionComponents() const { return GlobalInteractionComponents; }

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

	//========================================
	// Context Management
	//========================================

	/** Get or create context for preset */
	UKawaiiFluidSimulationContext* GetOrCreateContext(const UKawaiiFluidPresetDataAsset* Preset);

private:
	//========================================
	// Module Management (New)
	//========================================

	/** All registered simulation modules */
	UPROPERTY()
	TArray<UKawaiiFluidSimulationModule*> AllModules;

	//========================================
	// Component Management (Deprecated)
	//========================================

	/** All registered modular components (deprecated - kept for backward compatibility) */
	UPROPERTY()
	TArray<UKawaiiFluidComponent*> AllFluidComponents;

	/** Global colliders */
	UPROPERTY()
	TArray<UFluidCollider*> GlobalColliders;

	/** Global interaction components */
	UPROPERTY()
	TArray<UFluidInteractionComponent*> GlobalInteractionComponents;

	/** Context cache (Preset -> Instance) - each Preset gets its own Context/GPUSimulator */
	UPROPERTY()
	TMap<TObjectPtr<const UKawaiiFluidPresetDataAsset>, TObjectPtr<UKawaiiFluidSimulationContext>> ContextCache;

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
	// Simulation Methods
	//========================================

	/** Simulate independent modules */
	void SimulateIndependentFluidComponents(float DeltaTime);

	/** Simulate batched modules */
	void SimulateBatchedFluidComponents(float DeltaTime);

	/** Group modules by preset */
	TMap<UKawaiiFluidPresetDataAsset*, TArray<UKawaiiFluidSimulationModule*>> GroupModulesByPreset() const;

	/** Merge particles from modules */
	void MergeModuleParticles(const TArray<UKawaiiFluidSimulationModule*>& Modules);

	/** Split particles back to modules */
	void SplitModuleParticles(const TArray<UKawaiiFluidSimulationModule*>& Modules);

	/** Build merged params from modules */
	FKawaiiFluidSimulationParams BuildMergedModuleSimulationParams(const TArray<UKawaiiFluidSimulationModule*>& Modules);
};
