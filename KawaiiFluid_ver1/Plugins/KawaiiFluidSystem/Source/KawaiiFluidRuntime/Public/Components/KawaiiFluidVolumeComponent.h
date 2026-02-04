// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Core/KawaiiFluidRenderingTypes.h"
#include "KawaiiFluidVolumeComponent.generated.h"

class UKawaiiFluidSimulationModule;
class UKawaiiFluidPresetDataAsset;
class UNiagaraSystem;

/**
 * Kawaii Fluid Volume Component
 *
 * Defines the simulation bounds and Z-Order space for fluid particles.
 * This component is the spatial definition part of AKawaiiFluidVolume.
 *
 * Features:
 * - User-friendly size configuration (uniform or per-axis)
 * - Wall collision parameters (bounce, friction)
 * - Auto-calculated Z-Order grid settings
 * - Debug visualization
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent, DisplayName="Kawaii Fluid Volume"))
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidVolumeComponent : public UBoxComponent
{
	GENERATED_BODY()

public:
	UKawaiiFluidVolumeComponent();

	//========================================
	// UActorComponent Interface
	//========================================

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	//========================================
	// Volume Size Configuration (User-Friendly)
	//========================================

	/**
	 * Use uniform (cube) size for simulation volume
	 * When checked, enter a single size value. When unchecked, enter separate X/Y/Z values.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume",
		meta = (DisplayName = "Uniform Size", EditCondition = "!bUseUnlimitedSize", EditConditionHides))
	bool bUniformSize = true;

	/**
	 * Simulation volume size (cm) - cube dimensions when Uniform Size is checked
	 * Particles are confined within this box and collide with its boundaries.
	 *
	 * When "Enable Unlimited Simulation Range" is OFF: Size is clamped to grid capacity.
	 * When "Enable Unlimited Simulation Range" is ON: No size limit - can be any size.
	 *
	 * Example: 400 cm means a 400x400x400 cm cube.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume",
		meta = (EditCondition = "bUniformSize && !bUseUnlimitedSize", EditConditionHides, DisplayName = "Size", ClampMin = "10.0"))
	float UniformVolumeSize = 2560.0f;

	/**
	 * Simulation volume size (cm) - separate X/Y/Z dimensions
	 * Particles are confined within this box and collide with its boundaries.
	 *
	 * When "Enable Unlimited Simulation Range" is OFF: Size is clamped to grid capacity.
	 * When "Enable Unlimited Simulation Range" is ON: No size limit - can be any size.
	 *
	 * Example: (400, 300, 200) means a 400x300x200 cm box.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume",
		meta = (EditCondition = "!bUniformSize && !bUseUnlimitedSize", EditConditionHides, DisplayName = "Size"))
	FVector VolumeSize = FVector(2560.0f, 2560.0f, 2560.0f);

	/**
	 * Disable the volume box entirely - no collision boundaries, no wireframe
	 * Particles can move freely without any box constraints.
	 * Only available when "Enable Unlimited Simulation Range" is enabled.
	 *
	 * When enabled: Size properties are hidden, no bounds collision, no wireframe visualization
	 * When disabled: Volume box acts as collision boundary with the specified size
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume",
		meta = (DisplayName = "Use Unlimited Size", EditCondition = "bUseHybridTiledZOrder", EditConditionHides))
	bool bUseUnlimitedSize = false;

	//========================================
	// Preset Configuration
	//========================================

	/** The fluid preset that defines physics and rendering parameters */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume")
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset;

	/** Maximum particle count for this volume's GPU buffer */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume", meta = (ClampMin = "1"))
	int32 MaxParticleCount = 200000;

	//========================================
	// Collision Settings
	//========================================

	/** Use world collision (floor, walls, static meshes) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Collision")
	bool bUseWorldCollision = true;

	/**
	 * Enable static boundary particles for density contribution at walls/floors (Akinci 2012)
	 * This helps prevent density deficit near boundaries which causes wall climbing artifacts.
	 *
	 * PERFORMANCE WARNING:
	 * - CPU cost: Boundary particles are generated on CPU for each collision primitive
	 * - GPU memory: Additional particle buffer allocation (proportional to surface area)
	 * - Neighbor search: O(N × M) where N=fluid particles, M=boundary particles
	 * - Density calculation: Extra kernel evaluations per fluid particle
	 *
	 * Recommended for small enclosed spaces where wall behavior is critical.
	 * Not recommended for large open areas or performance-sensitive scenarios.
	 *
	 * NOTE: Only available when World Collision is enabled and "Use Unlimited Size" is disabled.
	 * In Unlimited Size mode, static boundary particles are always disabled for performance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Collision",
		meta = (DisplayName = "Enable Static Boundary Particles",
		        EditCondition = "bUseWorldCollision && !bUseUnlimitedSize", EditConditionHides))
	bool bEnableStaticBoundaryParticles = false;

	/**
	 * Static boundary particle spacing (cm)
	 * Lower values = denser boundary particles = better density coverage but more particles
	 * Higher values = sparser boundary particles = fewer particles but may have gaps
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Collision",
		meta = (EditCondition = "bUseWorldCollision && bEnableStaticBoundaryParticles && !bUseUnlimitedSize", EditConditionHides,
		        ClampMin = "1.0", ClampMax = "50.0", DisplayName = "Boundary Particle Spacing"))
	float StaticBoundaryParticleSpacing = 5.0f;

	//========================================
	// Collision Events
	//========================================

	/** Enable collision events (FluidInteractionComponent callbacks) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Collision|Events")
	bool bEnableCollisionEvents = false;

	/** Minimum velocity for event triggering (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Collision|Events",
	          meta = (ClampMin = "0.0", EditCondition = "bEnableCollisionEvents"))
	float MinVelocityForEvent = 50.0f;

	/** Maximum events per frame (0 = unlimited) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Collision|Events",
	          meta = (ClampMin = "0", EditCondition = "bEnableCollisionEvents"))
	int32 MaxEventsPerFrame = 10;

	/** Per-particle event cooldown (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Collision|Events",
	          meta = (ClampMin = "0.0", EditCondition = "bEnableCollisionEvents"))
	float EventCooldownPerParticle = 0.1f;

	/** Particle hit event (Blueprint bindable) */
	UPROPERTY(BlueprintAssignable, Category = "Fluid Volume|Collision|Events")
	FOnFluidParticleHitComponent OnParticleHit;

	//========================================
	// Rendering Configuration
	//========================================

	/** Enable shadow casting via instanced spheres */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Rendering")
	bool bEnableShadow = false;

	/** Shadow mesh quality - controls polygon count of shadow spheres */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Rendering", meta = (EditCondition = "bEnableShadow"))
	EFluidShadowMeshQuality ShadowMeshQuality = EFluidShadowMeshQuality::Medium;

	/** Maximum distance from camera for shadow rendering (cm).
	 * Particles beyond this distance will skip GPU readback for shadow ISM.
	 * Set to 0 to use DirectionalLight's DynamicShadowDistance automatically. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Rendering", meta = (EditCondition = "bEnableShadow", ClampMin = "0"))
	float ShadowCullDistance = 0.0f;

	//========================================
	// Splash VFX
	//========================================

	/** Niagara system to spawn for splash/spray effects on fast-moving particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|VFX")
	TObjectPtr<UNiagaraSystem> SplashVFX;

	/** Velocity threshold to trigger splash VFX (world units/sec) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|VFX",
	          meta = (ClampMin = "0", EditCondition = "SplashVFX != nullptr"))
	float SplashVelocityThreshold = 200.0f;

	/** Maximum splash VFX spawns per frame (performance limit) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|VFX",
	          meta = (ClampMin = "1", ClampMax = "50", EditCondition = "SplashVFX != nullptr"))
	int32 MaxSplashVFXPerFrame = 10;

	/** Condition mode for splash VFX triggering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|VFX",
	          meta = (EditCondition = "SplashVFX != nullptr"))
	ESplashConditionMode SplashConditionMode = ESplashConditionMode::VelocityAndIsolation;

	/** Neighbor count threshold for isolation (particles with this many or fewer neighbors are considered isolated) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|VFX",
	          meta = (ClampMin = "0", ClampMax = "10", EditCondition = "SplashVFX != nullptr && SplashConditionMode != ESplashConditionMode::VelocityOnly"))
	int32 IsolationNeighborThreshold = 2;

	//========================================
	// Debug Draw Mode
	//========================================

	/** Debug draw mode for particle visualization.
	 *  Uses simulation ParticleRadius for accurate size representation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Draw Mode")
	EKawaiiFluidDebugDrawMode DebugDrawMode = EKawaiiFluidDebugDrawMode::None;

	/** Debug particle color (ISM mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Draw Mode",
	          meta = (EditCondition = "DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM", EditConditionHides))
	FLinearColor ISMDebugColor = FLinearColor(0.2f, 0.5f, 1.0f, 0.8f);

	//========================================
	// Static Boundary Debug Visualization
	//========================================

	/** Show static boundary particles (walls, floors) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Boundary")
	bool bShowStaticBoundaryParticles = false;

	/** Static boundary particle debug point size */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Boundary",
	          meta = (EditCondition = "bShowStaticBoundaryParticles", ClampMin = "1.0", ClampMax = "50.0"))
	float StaticBoundaryPointSize = 4.0f;

	/** Static boundary particle debug color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Boundary",
	          meta = (EditCondition = "bShowStaticBoundaryParticles"))
	FColor StaticBoundaryColor = FColor::Cyan;

	/** Show surface normals for static boundary particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Boundary",
	          meta = (EditCondition = "bShowStaticBoundaryParticles"))
	bool bShowStaticBoundaryNormals = false;

	/** Length of normal debug lines */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Boundary",
	          meta = (EditCondition = "bShowStaticBoundaryParticles && bShowStaticBoundaryNormals", ClampMin = "1.0", ClampMax = "100.0"))
	float StaticBoundaryNormalLength = 10.0f;

	//========================================
	// Editor Brush (Editor only)
	//========================================

#if WITH_EDITORONLY_DATA
	/** Brush settings for particle painting in editor */
	UPROPERTY(EditAnywhere, Category = "Brush Editor")
	FFluidBrushSettings BrushSettings;

	/** Brush mode active state (set by editor mode) */
	bool bBrushModeActive = false;
#endif

	//========================================
	// Debug Visualization (Internal)
	//========================================

	/** Show bounds wireframe in editor (internal fixed value) */
	bool bShowBoundsInEditor = true;

	/** Show bounds wireframe at runtime (internal fixed value) */
	bool bShowBoundsAtRuntime = false;

	/** Wireframe color (internal fixed value) */
	FColor BoundsColor = FColor::Cyan;

	/** Wireframe line thickness (internal fixed value) */
	float BoundsLineThickness = 2.0f;

	/** Show Z-Order space wireframe (for debugging spatial partitioning) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Z-Order Space", meta = (DisplayName = "Show Z-Order Space Wireframe"))
	bool bShowZOrderSpaceWireframe = false;

	/** Z-Order space wireframe color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Debug|Z-Order Space",
		meta = (EditCondition = "bShowZOrderSpaceWireframe", EditConditionHides, DisplayName = "Z-Order Space Wireframe Color"))
	FColor ZOrderSpaceWireframeColor = FColor::Red;

	//========================================
	// Hybrid Tiled Z-Order (Unlimited Simulation Range)
	//========================================

	/**
	 * Enable Hybrid Tiled Z-Order mode for UNLIMITED simulation range (default: enabled)
	 *
	 * When enabled:
	 *   - Particles can exist anywhere in the world (no bounds clipping)
	 *   - Characters with attached fluid can move freely across the entire map
	 *   - Uses 21-bit sort keys (TileHash 3 bits + LocalMorton 18 bits)
	 *   - Always uses Medium preset internally (2^21 = 2M cells)
	 *   - 8 tile hash buckets; collisions are filtered by distance check (r² < h²)
	 *   - Same radix sort passes as classic mode (no performance overhead)
	 *
	 * When disabled:
	 *   - Uses classic bounded Morton code (preset-dependent bit width)
	 *   - Particles outside bounds are clamped (may cause incorrect neighbor search)
	 *   - Simulation range limited by volume size and grid preset
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Volume|Z-Order Space",
		meta = (DisplayName = "Enable Unlimited Simulation Range"))
	bool bUseHybridTiledZOrder = true;

	//========================================
	// Internal Grid Data (Auto-Calculated, access via getter functions)
	//========================================

	/** Cell size for Z-Order grid (auto-derived from Preset->SmoothingRadius) */
	float CellSize = 20.0f;

	/** Grid resolution preset for Z-Order sorting (auto-selected based on volume size) */
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;

	/** Grid resolution bits per axis (derived from GridResolutionPreset) */
	int32 GridAxisBits = 7;

	/** Grid resolution per axis (2^GridAxisBits) */
	int32 GridResolution = 128;

	/** Total number of cells (GridResolution^3) */
	int32 MaxCells = 2097152;

	/** Actual simulation bounds extent (may differ from requested VolumeSize due to grid constraints) */
	float BoundsExtent = 2560.0f;

	/** World-space minimum bounds (Component location - Extent/2) */
	FVector WorldBoundsMin = FVector(-1280.0f, -1280.0f, -1280.0f);

	/** World-space maximum bounds (Component location + Extent/2) */
	FVector WorldBoundsMax = FVector(1280.0f, 1280.0f, 1280.0f);

	//========================================
	// Public Methods
	//========================================

	/** Recalculate bounds based on VolumeSize and component location */
	UFUNCTION(BlueprintCallable, Category = "Fluid Volume")
	void RecalculateBounds();

	/** Check if a world position is within this Volume's bounds */
	UFUNCTION(BlueprintCallable, Category = "Fluid Volume")
	bool IsPositionInBounds(const FVector& WorldPosition) const;

	/** Get simulation bounds (world-space) */
	UFUNCTION(BlueprintCallable, Category = "Fluid Volume")
	void GetSimulationBounds(FVector& OutMin, FVector& OutMax) const;

	/** Get simulation bounds min (world-space) */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	FVector GetWorldBoundsMin() const { return WorldBoundsMin; }

	/** Get simulation bounds max (world-space) */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	FVector GetWorldBoundsMax() const { return WorldBoundsMax; }

	/** Get the effective volume size (full size, cm) */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	FVector GetEffectiveVolumeSize() const
	{
		return bUniformSize ? FVector(UniformVolumeSize) : VolumeSize;
	}

	/** Get the volume half-extent (for internal collision/rendering use) */
	FVector GetVolumeHalfExtent() const
	{
		return GetEffectiveVolumeSize() * 0.5f;
	}

	/** Get wall bounce coefficient (from Preset's Restitution) */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	float GetWallBounce() const;

	/** Get wall friction coefficient (from Preset's Friction) */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	float GetWallFriction() const;

	/** Get cell size */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	float GetCellSize() const { return CellSize; }

	/** Get bounds extent */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	float GetBoundsExtent() const { return BoundsExtent; }

	/** Get grid resolution preset */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	EGridResolutionPreset GetGridResolutionPreset() const { return GridResolutionPreset; }

	/** Get grid axis bits */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	int32 GetGridAxisBits() const { return GridAxisBits; }

	/** Is static boundary particles enabled (always false when Use Unlimited Size is enabled) */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	bool IsStaticBoundaryParticlesEnabled() const { return !bUseUnlimitedSize && bEnableStaticBoundaryParticles; }

	/** Get static boundary particle spacing */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	float GetStaticBoundaryParticleSpacing() const { return StaticBoundaryParticleSpacing; }

	//========================================
	// Preset & Simulation Getters
	//========================================

	/** Get the preset */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	UKawaiiFluidPresetDataAsset* GetPreset() const { return Preset; }

	/** Get particle spacing from preset */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	float GetParticleSpacing() const;

	//========================================
	// Debug Methods
	//========================================

	/** Set debug draw mode */
	UFUNCTION(BlueprintCallable, Category = "Fluid Volume|Debug")
	void SetDebugDrawMode(EKawaiiFluidDebugDrawMode Mode);

	/** Get current debug draw mode */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume|Debug")
	EKawaiiFluidDebugDrawMode GetDebugDrawMode() const { return DebugDrawMode; }

	/** Disable debug drawing */
	UFUNCTION(BlueprintCallable, Category = "Fluid Volume|Debug")
	void DisableDebugDraw();

	//========================================
	// Module Registration (Legacy support)
	//========================================

	/** Get all registered fluid modules using this Volume */
	const TArray<TWeakObjectPtr<UKawaiiFluidSimulationModule>>& GetRegisteredModules() const { return RegisteredModules; }

	/** Register a fluid module to this Volume */
	void RegisterModule(UKawaiiFluidSimulationModule* Module);

	/** Unregister a fluid module from this Volume */
	void UnregisterModule(UKawaiiFluidSimulationModule* Module);

	/** Get the number of registered modules */
	UFUNCTION(BlueprintPure, Category = "Fluid Volume")
	int32 GetRegisteredModuleCount() const { return RegisteredModules.Num(); }

private:
	//========================================
	// Registered Modules (Legacy)
	//========================================

	/** Fluid modules using this Volume (for legacy KawaiiFluidComponent support) */
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UKawaiiFluidSimulationModule>> RegisteredModules;

	//========================================
	// Subsystem Registration
	//========================================

	void RegisterToSubsystem();
	void UnregisterFromSubsystem();

	//========================================
	// Debug Visualization
	//========================================

	void DrawBoundsVisualization();
};

// Legacy typedef for backward compatibility
// TODO: Remove after full migration
using UKawaiiFluidSimulationVolumeComponent = UKawaiiFluidVolumeComponent;
