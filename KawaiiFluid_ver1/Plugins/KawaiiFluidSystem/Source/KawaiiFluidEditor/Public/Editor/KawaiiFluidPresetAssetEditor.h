// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "EditorUndoClient.h"

class UKawaiiFluidPresetDataAsset;
class SFluidPresetEditorViewport;
class FFluidPreviewScene;
class UFluidPreviewSettingsObject;

/**
 * Main asset editor toolkit for KawaiiFluidPresetDataAsset
 * Provides 3D viewport preview with real-time simulation
 */
class KAWAIIFLUIDEDITOR_API FKawaiiFluidPresetAssetEditor : public FAssetEditorToolkit,
                                                            public FEditorUndoClient,
                                                            public FTickableEditorObject
{
public:
	FKawaiiFluidPresetAssetEditor();
	virtual ~FKawaiiFluidPresetAssetEditor() override;

	/**
	 * Initialize the editor with a preset asset
	 */
	void InitFluidPresetEditor(
		const EToolkitMode::Type Mode,
		const TSharedPtr<IToolkitHost>& InitToolkitHost,
		UKawaiiFluidPresetDataAsset* InPreset);

	//~ Begin FAssetEditorToolkit Interface
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;
	//~ End FAssetEditorToolkit Interface

	//~ Begin FEditorUndoClient Interface
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;
	//~ End FEditorUndoClient Interface

	//~ Begin FTickableEditorObject Interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return true; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
	//~ End FTickableEditorObject Interface

	//========================================
	// Playback Control
	//========================================

	/** Start simulation playback */
	void Play();

	/** Pause simulation */
	void Pause();

	/** Stop simulation and reset */
	void Stop();

	/** Reset particles only (keep playing state) */
	void Reset();

	/** Is simulation playing */
	bool IsPlaying() const { return bIsPlaying; }

	/** Set simulation speed multiplier */
	void SetSimulationSpeed(float Speed);

	/** Get current simulation speed */
	float GetSimulationSpeed() const { return SimulationSpeed; }

	//========================================
	// Accessors
	//========================================

	/** Get the preset being edited */
	UKawaiiFluidPresetDataAsset* GetEditingPreset() const { return EditingPreset; }

	/** Get preview scene */
	TSharedPtr<FFluidPreviewScene> GetPreviewScene() const { return PreviewScene; }

	//========================================
	// Property Change Handling
	//========================================

	/** Called when preset property changes */
	void OnPresetPropertyChanged(const FPropertyChangedEvent& PropertyChangedEvent);

	/** Called when preview settings property changes */
	void OnPreviewSettingsPropertyChanged(const FPropertyChangedEvent& PropertyChangedEvent);

private:
	//========================================
	// Tab Spawners
	//========================================

	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnTab_PreviewSettings(const FSpawnTabArgs& Args);

	//========================================
	// Toolbar
	//========================================

	void ExtendToolbar();

	//========================================
	// Internal Methods
	//========================================

	void RefreshPreview();
	void UpdateSimulation(float DeltaTime);
	void BindEditorDelegates();
	void UnbindEditorDelegates();

private:
	//========================================
	// Edited Asset
	//========================================

	/** Preset being edited */
	UKawaiiFluidPresetDataAsset* EditingPreset;

	//========================================
	// Preview System
	//========================================

	/** Preview scene */
	TSharedPtr<FFluidPreviewScene> PreviewScene;

	/** Viewport widget */
	TSharedPtr<SFluidPresetEditorViewport> ViewportWidget;

	//========================================
	// Details Views
	//========================================

	/** Details view for preset properties */
	TSharedPtr<IDetailsView> DetailsView;

	/** Details view for preview settings */
	TSharedPtr<IDetailsView> PreviewSettingsView;

	//========================================
	// Playback State
	//========================================

	/** Is simulation playing */
	bool bIsPlaying;

	/** Simulation speed multiplier */
	float SimulationSpeed;

	//========================================
	// Tab IDs
	//========================================

	static const FName ViewportTabId;
	static const FName DetailsTabId;
	static const FName PreviewSettingsTabId;
	static const FName AppIdentifier;
};
