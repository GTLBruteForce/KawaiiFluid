// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "Preview/FluidPreviewSettings.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"

class UKawaiiFluidPresetDataAsset;
class UKawaiiFluidSimulationContext;
class UKawaiiFluidSimulationModule;
class UKawaiiFluidRenderingModule;
class FSpatialHash;
struct FFluidParticle;

/**
 * Preview scene for fluid simulation in asset editor
 * Contains preview world, simulation logic, and visualization
 *
 * Implements IKawaiiFluidDataProvider to share data with RenderingModule
 * (same architecture as runtime for consistent preview)
 */
class KAWAIIFLUIDEDITOR_API FFluidPreviewScene : public FAdvancedPreviewScene,
                                                  public IKawaiiFluidDataProvider
{
public:
	FFluidPreviewScene(FPreviewScene::ConstructionValues CVS);
	virtual ~FFluidPreviewScene() override;

	/** GC 방지 */
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

	//========================================
	// IKawaiiFluidDataProvider Interface
	//========================================

	virtual const TArray<FFluidParticle>& GetParticles() const override;
	virtual int32 GetParticleCount() const override;
	virtual float GetParticleRadius() const override;
	virtual bool IsDataValid() const override;
	virtual FString GetDebugName() const override { return TEXT("FluidPreviewScene"); }

	//========================================
	// Render Mode Control
	//========================================

	/** Set render mode (ISM or SSFR) */
	void SetRenderMode(EFluidPreviewRenderMode NewMode);

	/** Get current render mode */
	EFluidPreviewRenderMode GetRenderMode() const { return CurrentRenderMode; }

	/** Get rendering module */
	UKawaiiFluidRenderingModule* GetRenderingModule() const { return RenderingModule; }

	//========================================
	// Preset Management
	//========================================

	/** Set the preset to preview */
	void SetPreset(UKawaiiFluidPresetDataAsset* InPreset);

	/** Get current preset */
	UKawaiiFluidPresetDataAsset* GetPreset() const { return CurrentPreset; }

	/** Refresh simulation from preset changes */
	void RefreshFromPreset();

	//========================================
	// Simulation Control
	//========================================

	/** Start simulation playback */
	void StartSimulation();

	/** Stop/pause simulation */
	void StopSimulation();

	/** Reset simulation to initial state */
	void ResetSimulation();

	/** Tick simulation forward */
	void TickSimulation(float DeltaTime);

	/** Is simulation currently running */
	bool IsSimulationActive() const { return bSimulationActive; }

	//========================================
	// Preview Settings
	//========================================

	/** Get preview settings struct */
	FFluidPreviewSettings& GetPreviewSettings();

	/** Get preview settings object for Details Panel */
	UFluidPreviewSettingsObject* GetPreviewSettingsObject() const { return PreviewSettingsObject; }

	/** Apply preview settings changes */
	void ApplyPreviewSettings();

	//========================================
	// Environment
	//========================================

	/** Setup floor mesh */
	void SetupFloor();

	/** Update environment from settings */
	void UpdateEnvironment();

	//========================================
	// Particle Access (via SimulationModule)
	//========================================

	/** Get mutable particles array (for simulation) */
	TArray<FFluidParticle>& GetParticlesMutable();

	/** Get average density */
	float GetAverageDensity() const;

	/** Get simulation time */
	float GetSimulationTime() const;

	/** Get simulation module */
	UKawaiiFluidSimulationModule* GetSimulationModule() const { return SimulationModule; }

private:
	/** Continuous spawn particles */
	void ContinuousSpawn(float DeltaTime);

	/** Create visualization components */
	void CreateVisualizationComponents();

	/** Update instanced mesh transforms */
	void UpdateInstancedMesh();

	/** Simple floor collision */
	void HandleFloorCollision();

private:
	//========================================
	// Preset & Settings
	//========================================

	/** Current preset being previewed */
	TObjectPtr<UKawaiiFluidPresetDataAsset> CurrentPreset;

	/** Preview settings object (for Details Panel) */
	TObjectPtr<UFluidPreviewSettingsObject> PreviewSettingsObject;

	//========================================
	// Simulation Data (Module-based)
	//========================================

	/** Simulation module - owns particles, SpatialHash, and provides API */
	TObjectPtr<UKawaiiFluidSimulationModule> SimulationModule;

	/** Simulation context - physics solver (shared with runtime) */
	TObjectPtr<UKawaiiFluidSimulationContext> SimulationContext;

	/** Spawn accumulator for fractional particles (continuous spawn) */
	float SpawnAccumulator;

	/** Is simulation running */
	bool bSimulationActive;

	//========================================
	// Rendering (same as runtime)
	//========================================

	/** Rendering module - same as runtime! (ISM + SSFR) */
	TObjectPtr<UKawaiiFluidRenderingModule> RenderingModule;

	/** Current render mode */
	EFluidPreviewRenderMode CurrentRenderMode;

	//========================================
	// Visualization Components
	//========================================

	/** Preview actor to hold components */
	TObjectPtr<AActor> PreviewActor;

	/** Floor mesh */
	TObjectPtr<UStaticMeshComponent> FloorMeshComponent;

	/** Wall meshes */
	TArray<TObjectPtr<UStaticMeshComponent>> WallMeshComponents;

	/** Cached particle radius for visualization */
	float CachedParticleRadius;
};
