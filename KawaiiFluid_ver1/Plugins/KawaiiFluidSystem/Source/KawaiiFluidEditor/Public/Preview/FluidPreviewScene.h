// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "Preview/FluidPreviewSettings.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"

class UKawaiiFluidPresetDataAsset;
class UKawaiiFluidSimulationContext;
class UKawaiiFluidSimulationModule;
class UKawaiiFluidRenderingModule;
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

	// GPU Simulation Interface (required for Metaball rendering)
	virtual bool IsGPUSimulationActive() const override;
	virtual int32 GetGPUParticleCount() const override;
	virtual FGPUFluidSimulator* GetGPUSimulator() const override;

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
	// Particle Access (GPU mode - limited)
	//========================================

	/** Get mutable particles array (not available in GPU mode) */
	TArray<FFluidParticle>& GetParticlesMutable();

	/** Get average density (not available in GPU mode) */
	float GetAverageDensity() const;

	/** Get simulation time (not available in GPU mode) */
	float GetSimulationTime() const;

private:
	/** Continuous spawn particles */
	void SpawnParticles(float DeltaTime);

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
	// Simulation Data (GPU-based)
	//========================================

	/** Simulation context - physics solver with GPU simulator */
	TObjectPtr<UKawaiiFluidSimulationContext> SimulationContext;

	/** Simulation module - uses same spawn logic as runtime */
	TObjectPtr<UKawaiiFluidSimulationModule> SimulationModule;

	/** Spawn accumulated time for continuous spawn */
	float SpawnAccumulatedTime;

	/** Total simulation time (accumulated) */
	float TotalSimulationTime;

	/** Is simulation running */
	bool bSimulationActive;

	//========================================
	// Rendering (same as runtime)
	//========================================

	/** Rendering module - same as runtime! (ISM + SSFR) */
	TObjectPtr<UKawaiiFluidRenderingModule> RenderingModule;

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
