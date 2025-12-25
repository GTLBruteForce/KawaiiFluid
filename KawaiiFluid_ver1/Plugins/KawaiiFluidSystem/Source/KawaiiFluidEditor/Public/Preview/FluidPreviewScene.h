// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "Preview/FluidPreviewSettings.h"

class UKawaiiFluidPresetDataAsset;
class UKawaiiFluidSimulationContext;
class FSpatialHash;
struct FFluidParticle;

/**
 * Preview scene for fluid simulation in asset editor
 * Contains preview world, simulation logic, and visualization
 */
class KAWAIIFLUIDEDITOR_API FFluidPreviewScene : public FAdvancedPreviewScene
{
public:
	FFluidPreviewScene(FPreviewScene::ConstructionValues CVS);
	virtual ~FFluidPreviewScene() override;

	/** GC 방지 */
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

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

	/** Setup boundary walls */
	void SetupWalls();

	/** Update environment from settings */
	void UpdateEnvironment();

	//========================================
	// Particle Access
	//========================================

	/** Get particles array */
	TArray<FFluidParticle>& GetParticles() { return Particles; }
	const TArray<FFluidParticle>& GetParticles() const { return Particles; }

	/** Get current particle count */
	int32 GetParticleCount() const { return Particles.Num(); }

	/** Get average density */
	float GetAverageDensity() const;

	/** Get simulation time */
	float GetSimulationTime() const { return AccumulatedTime; }

	//========================================
	// Visualization
	//========================================

	/** Update particle mesh instances */
	void UpdateParticleVisuals();

	/** Get particle mesh component for debug drawing */
	UInstancedStaticMeshComponent* GetParticleMeshComponent() const { return ParticleMeshComponent; }

private:
	/** Continuous spawn particles */
	void ContinuousSpawn(float DeltaTime);

	/** Create visualization components */
	void CreateVisualizationComponents();

	/** Update instanced mesh transforms */
	void UpdateInstancedMesh();

	/** Simple floor collision */
	void HandleFloorCollision();

	/** Simple wall collision */
	void HandleWallCollision();

private:
	//========================================
	// Preset & Settings
	//========================================

	/** Current preset being previewed */
	TObjectPtr<UKawaiiFluidPresetDataAsset> CurrentPreset;

	/** Preview settings object (for Details Panel) */
	TObjectPtr<UFluidPreviewSettingsObject> PreviewSettingsObject;

	//========================================
	// Simulation Data
	//========================================

	/** Particle array */
	TArray<FFluidParticle> Particles;

	/** Spatial hash for neighbor search */
	TSharedPtr<FSpatialHash> SpatialHash;

	/** Simulation context */
	TObjectPtr<UKawaiiFluidSimulationContext> SimulationContext;

	/** Accumulated simulation time */
	float AccumulatedTime;

	/** Spawn accumulator for fractional particles */
	float SpawnAccumulator;

	/** Is simulation running */
	bool bSimulationActive;

	//========================================
	// Visualization Components
	//========================================

	/** Preview actor to hold components */
	TObjectPtr<AActor> PreviewActor;

	/** Instanced mesh for particles */
	TObjectPtr<UInstancedStaticMeshComponent> ParticleMeshComponent;

	/** Floor mesh */
	TObjectPtr<UStaticMeshComponent> FloorMeshComponent;

	/** Wall meshes */
	TArray<TObjectPtr<UStaticMeshComponent>> WallMeshComponents;

	/** Cached particle radius for visualization */
	float CachedParticleRadius;
};
