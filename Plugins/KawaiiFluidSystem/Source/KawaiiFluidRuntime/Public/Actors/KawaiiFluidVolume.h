// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GPU/GPUFluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Core/KawaiiFluidRenderingTypes.h"
#include "KawaiiFluidVolume.generated.h"

class UKawaiiFluidVolumeComponent;
class UNiagaraSystem;
class UKawaiiFluidSimulationModule;
class UKawaiiFluidSimulationContext;
class UKawaiiFluidPresetDataAsset;
class UKawaiiFluidRenderingModule;
class AKawaiiFluidEmitter;

/**
 * Kawaii Fluid Volume
 *
 * The main simulation domain for KawaiiFluid particles.
 * This actor serves as the "solver unit" - it owns the simulation module,
 * context, and rendering for all particles within its bounds.
 *
 * All configuration properties are stored in the VolumeComponent.
 * This actor provides the simulation and rendering infrastructure.
 *
 * Usage:
 * 1. Place AKawaiiFluidVolume in the level
 * 2. Configure the Preset (water, lava, etc.) via VolumeComponent
 * 3. Adjust volume size as needed
 * 4. Place AKawaiiFluidEmitter actors inside and assign them to this volume
 *
 * All emitters targeting the same Volume will:
 * - Share the same physics preset
 * - Share the same Z-Order space for neighbor search
 * - Be simulated together in one GPU dispatch
 * - Be rendered together for optimal batching
 */
UCLASS(Blueprintable, meta = (DisplayName = "Kawaii Fluid Volume"))
class KAWAIIFLUIDRUNTIME_API AKawaiiFluidVolume : public AActor
{
	GENERATED_BODY()

public:
	AKawaiiFluidVolume();

	//========================================
	// AActor Interface
	//========================================

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	//========================================
	// Components
	//========================================

	/** Get the volume component (contains all configuration) */
	UFUNCTION(BlueprintPure, Category = "Volume")
	UKawaiiFluidVolumeComponent* GetVolumeComponent() const { return VolumeComponent; }

	//========================================
	// Delegate Getters (from VolumeComponent)
	//========================================

	/** Get the preset (delegates to VolumeComponent) */
	UFUNCTION(BlueprintPure, Category = "Preset")
	UKawaiiFluidPresetDataAsset* GetPreset() const;

	/** Get particle spacing from preset (delegates to VolumeComponent) */
	UFUNCTION(BlueprintPure, Category = "Preset")
	float GetParticleSpacing() const;

	/** Get volume size (delegates to VolumeComponent) */
	UFUNCTION(BlueprintPure, Category = "Volume")
	FVector GetVolumeSize() const;

	//========================================
	// Debug Methods (Delegate to VolumeComponent)
	//========================================

	/** Set debug draw mode */
	UFUNCTION(BlueprintCallable, Category = "Debug")
	void SetDebugDrawMode(EKawaiiFluidDebugDrawMode Mode);

	/** Get current debug draw mode */
	UFUNCTION(BlueprintPure, Category = "Debug")
	EKawaiiFluidDebugDrawMode GetDebugDrawMode() const;

	/** Disable debug drawing (sets mode to None) */
	UFUNCTION(BlueprintCallable, Category = "Debug")
	void DisableDebugDraw();

	//========================================
	// Rendering
	//========================================

	/** Get the rendering module */
	UFUNCTION(BlueprintPure, Category = "Rendering")
	UKawaiiFluidRenderingModule* GetRenderingModule() const { return RenderingModule; }

	//========================================
	// Brush API (Editor/Runtime shared)
	//========================================

	/** Add particles within radius (hemisphere distribution - spawns above surface only) */
	UFUNCTION(BlueprintCallable, Category = "Brush")
	void AddParticlesInRadius(const FVector& WorldCenter, float Radius, int32 Count,
	                          const FVector& Velocity, float Randomness = 0.8f,
	                          const FVector& SurfaceNormal = FVector::UpVector);

	/** Remove particles within radius (GPU-driven, no readback dependency) */
	UFUNCTION(BlueprintCallable, Category = "Brush")
	void RemoveParticlesInRadiusGPU(const FVector& WorldCenter, float Radius);

	/** Remove all particles by SourceID (GPU-driven) */
	UFUNCTION(BlueprintCallable, Category = "Brush")
	void RemoveParticlesBySourceGPU(int32 SourceID);

	/** Remove N oldest particles (GPU-driven) */
	UFUNCTION(BlueprintCallable, Category = "Brush")
	void RemoveOldestParticlesGPU(int32 Count);

	/** Remove all particles + clear rendering */
	UFUNCTION(BlueprintCallable, Category = "Brush")
	void ClearAllParticles();

	//========================================
	// Simulation
	//========================================

	/** Get the simulation module (owns particle data) */
	UFUNCTION(BlueprintPure, Category = "Simulation")
	UKawaiiFluidSimulationModule* GetSimulationModule() const { return SimulationModule; }

	/** Get the simulation context (owns solver) */
	UFUNCTION(BlueprintPure, Category = "Simulation")
	UKawaiiFluidSimulationContext* GetSimulationContext() const { return SimulationContext; }

	/** Run simulation for this volume */
	UFUNCTION(BlueprintCallable, Category = "Simulation")
	void Simulate(float DeltaTime);

	//========================================
	// Emitter Management
	//========================================

	/** Register an emitter to this volume */
	UFUNCTION(BlueprintCallable, Category = "Emitter")
	void RegisterEmitter(AKawaiiFluidEmitter* Emitter);

	/** Unregister an emitter from this volume */
	UFUNCTION(BlueprintCallable, Category = "Emitter")
	void UnregisterEmitter(AKawaiiFluidEmitter* Emitter);

	/** Get the number of registered emitters */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	int32 GetEmitterCount() const { return RegisteredEmitters.Num(); }

	/** Get all registered emitters */
	const TArray<TWeakObjectPtr<AKawaiiFluidEmitter>>& GetRegisteredEmitters() const { return RegisteredEmitters; }

	//========================================
	// Spawn Request API (for Emitters)
	//========================================

	/** Queue a single spawn request (will be processed in next simulation tick) */
	UFUNCTION(BlueprintCallable, Category = "Spawn")
	void QueueSpawnRequest(FVector Position, FVector Velocity, int32 SourceID = -1);

	/** Queue multiple spawn requests at once (batch version for efficiency) */
	void QueueSpawnRequests(const TArray<FVector>& Positions, const TArray<FVector>& Velocities, int32 SourceID = -1);

	/** Get number of pending spawn requests */
	UFUNCTION(BlueprintPure, Category = "Spawn")
	int32 GetPendingSpawnCount() const { return PendingSpawnRequests.Num(); }

	/** Clear all pending spawn requests */
	UFUNCTION(BlueprintCallable, Category = "Spawn")
	void ClearPendingSpawnRequests() { PendingSpawnRequests.Empty(); }

	/** Clear pending spawn requests for a specific SourceID */
	UFUNCTION(BlueprintCallable, Category = "Spawn")
	void ClearPendingSpawnRequestsForSource(int32 SourceID)
	{
		PendingSpawnRequests.RemoveAll([SourceID](const FGPUSpawnRequest& Request)
		{
			return Request.SourceID == SourceID;
		});
	}

	/** Process pending spawn requests (called automatically during simulation) */
	void ProcessPendingSpawnRequests();

protected:
	//========================================
	// Components
	//========================================

	/** The fluid volume component that defines the simulation bounds and all configuration */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume")
	TObjectPtr<UKawaiiFluidVolumeComponent> VolumeComponent;

	//========================================
	// Simulation
	//========================================

	/** Simulation module - owns particle data */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidSimulationModule> SimulationModule;

	/** Simulation context - owns solver and GPU simulator
	 *  Transient: This is created at runtime by the Subsystem and is world-specific.
	 *  Must not be serialized or copied between editor and PIE worlds.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UKawaiiFluidSimulationContext> SimulationContext;

	/** Rendering module - manages ISM and Metaball renderers */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidRenderingModule> RenderingModule;

	//========================================
	// Emitter Management
	//========================================

	/** Registered emitters targeting this volume */
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<AKawaiiFluidEmitter>> RegisteredEmitters;

	//========================================
	// Spawn Request Queue
	//========================================

	/** Pending spawn requests to be processed in the next simulation tick */
	TArray<FGPUSpawnRequest> PendingSpawnRequests;

	//========================================
	// Internal
	//========================================

	/** Initialize simulation module (basic setup, called in PostInitializeComponents) */
	void InitializeSimulation();

	/** Register simulation module with Subsystem for GPU (called in BeginPlay) */
	void RegisterSimulationWithSubsystem();

	/** Cleanup simulation resources */
	void CleanupSimulation();

	/** Initialize rendering module */
	void InitializeRendering();

	/** Cleanup rendering resources */
	void CleanupRendering();

	/** Register this volume to the subsystem */
	void RegisterToSubsystem();

	/** Unregister this volume from the subsystem */
	void UnregisterFromSubsystem();

private:
	//========================================
	// Event System
	//========================================

	/** Handle collision event from Module callback */
	void HandleCollisionEvent(const FKawaiiFluidCollisionEvent& Event);

	//========================================
	// Debug Draw Helpers
	//========================================

	/** Draw debug particles using DrawDebugPoint */
	void DrawDebugParticles();

	/** Draw static boundary particles for debug visualization */
	void DrawDebugStaticBoundaryParticles();

	/** Cached debug draw mode (for change detection) */
	EKawaiiFluidDebugDrawMode CachedDebugDrawMode = EKawaiiFluidDebugDrawMode::None;

	/** Cached ISM debug color (for change detection) */
	FLinearColor CachedISMDebugColor = FLinearColor(0.2f, 0.5f, 1.0f, 0.8f);

#if WITH_EDITOR
	/** Initialize rendering for editor mode (ISM only for brush visualization) */
	void InitializeEditorRendering();

	/** Generate boundary particles for editor preview (without GPU simulation) */
	void GenerateEditorBoundaryParticlesPreview();

	/** Cached boundary particles for editor preview */
	TArray<FVector> EditorPreviewBoundaryPositions;
	TArray<FVector> EditorPreviewBoundaryNormals;

	/** Last frame editor preview was generated */
	uint64 LastEditorPreviewFrame = 0;

	/** Flag to track if editor rendering is initialized */
	bool bEditorRenderingInitialized = false;
#endif

	/** Compute debug color for a particle */
	FColor ComputeDebugDrawColor(int32 ParticleIndex, int32 TotalCount, const FVector& Position, float Density, bool bNearBoundary = false, int32 ZOrderArrayIndex = -1) const;

	/** Cached bounds for debug visualization (auto-computed) */
	FVector DebugDrawBoundsMin = FVector::ZeroVector;
	FVector DebugDrawBoundsMax = FVector::ZeroVector;

	//========================================
	// Shadow Readback Cache (GPU Mode)
	//========================================

	/** Cached shadow positions from last successful readback */
	TArray<FVector> CachedShadowPositions;

	/** Cached shadow velocities for prediction */
	TArray<FVector> CachedShadowVelocities;

	/** Cached neighbor counts for isolation detection */
	TArray<int32> CachedNeighborCounts;

	/** Previous frame neighbor counts for state change detection (non-isolated -> isolated) */
	TArray<int32> PrevNeighborCounts;

	/** Cached anisotropy axis 1 (xyz=direction, w=scale) for ellipsoid shadows */
	TArray<FVector4> CachedAnisotropyAxis1;

	/** Cached anisotropy axis 2 */
	TArray<FVector4> CachedAnisotropyAxis2;

	/** Cached anisotropy axis 3 */
	TArray<FVector4> CachedAnisotropyAxis3;

	/** Frame number of last successful shadow readback */
	uint64 LastShadowReadbackFrame = 0;

	/** Time of last shadow readback (for prediction delta calculation) */
	double LastShadowReadbackTime = 0.0;

	/** Buffer for shadow position prediction to avoid per-frame allocation */
	TArray<FVector> ShadowPredictionBuffer;
};
