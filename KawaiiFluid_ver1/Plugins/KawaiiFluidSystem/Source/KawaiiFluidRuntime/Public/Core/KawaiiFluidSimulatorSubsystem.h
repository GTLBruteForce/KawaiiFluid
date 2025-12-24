// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "KawaiiFluidSimulatorSubsystem.generated.h"

class UKawaiiFluidSimulationComponent;
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
	// Component Registration
	//========================================

	/** Register simulation component */
	void RegisterComponent(UKawaiiFluidSimulationComponent* Component);

	/** Unregister simulation component */
	void UnregisterComponent(UKawaiiFluidSimulationComponent* Component);

	/** Get all registered components */
	const TArray<UKawaiiFluidSimulationComponent*>& GetAllComponents() const { return AllComponents; }

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
	int32 GetComponentCount() const { return AllComponents.Num(); }

	//========================================
	// Context Management
	//========================================

	/** Get or create context for preset */
	UKawaiiFluidSimulationContext* GetOrCreateContext(const UKawaiiFluidPresetDataAsset* Preset);

private:
	//========================================
	// Component Management
	//========================================

	/** All registered components */
	UPROPERTY()
	TArray<UKawaiiFluidSimulationComponent*> AllComponents;

	/** Global colliders */
	UPROPERTY()
	TArray<UFluidCollider*> GlobalColliders;

	/** Global interaction components */
	UPROPERTY()
	TArray<UFluidInteractionComponent*> GlobalInteractionComponents;

	/** Context cache (ContextClass -> Instance) */
	UPROPERTY()
	TMap<TSubclassOf<UKawaiiFluidSimulationContext>, UKawaiiFluidSimulationContext*> ContextCache;

	/** Default context for presets without custom context */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidSimulationContext> DefaultContext;

	//========================================
	// Batching Resources
	//========================================

	/** Shared spatial hash for batching */
	TSharedPtr<FSpatialHash> SharedSpatialHash;

	/** Merged particle buffer for batching */
	TArray<FFluidParticle> MergedParticleBuffer;

	/** Batch info array */
	TArray<FKawaiiFluidBatchInfo> BatchInfos;

	/** Atomic event counter for thread-safe collision event tracking */
	std::atomic<int32> EventCountThisFrame{0};

	//========================================
	// Simulation Methods
	//========================================

	/** Simulate independent components (each has own spatial hash) */
	void SimulateIndependentComponents(float DeltaTime);

	/** Simulate batched components (same preset merged) */
	void SimulateBatchedComponents(float DeltaTime);

	/** Group components by preset */
	TMap<UKawaiiFluidPresetDataAsset*, TArray<UKawaiiFluidSimulationComponent*>> GroupComponentsByPreset() const;

	/** Merge particles from components into single buffer */
	void MergeParticles(const TArray<UKawaiiFluidSimulationComponent*>& Components);

	/** Split merged buffer back to components */
	void SplitParticles(const TArray<UKawaiiFluidSimulationComponent*>& Components);

	/** Build merged simulation params */
	FKawaiiFluidSimulationParams BuildMergedSimulationParams(const TArray<UKawaiiFluidSimulationComponent*>& Components);
};
