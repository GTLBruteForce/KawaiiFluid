// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Components/KawaiiFluidComponent.h"  // For enums (EFluidType, EFluidDebugVisualization, etc.)
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Volume", meta = (DisplayName = "Uniform Size"))
	bool bUniformSize = true;

	/**
	 * Simulation volume size (cm) - cube dimensions when Uniform Size is checked
	 * Particles are confined within this box. Enter the full box size (not half).
	 * Maximum size is automatically limited by Internal Grid (Large preset) capacity.
	 *
	 * Example: 400 cm means a 400x400x400 cm cube.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Volume",
		meta = (EditCondition = "bUniformSize", EditConditionHides, DisplayName = "Size", ClampMin = "10.0"))
	float UniformVolumeSize = 2560.0f;

	/**
	 * Simulation volume size (cm) - separate X/Y/Z dimensions
	 * Particles are confined within this box. Enter the full box size (not half).
	 * Each axis is automatically clamped to Internal Grid (Large preset) capacity.
	 *
	 * Example: (400, 300, 200) means a 400x300x200 cm box.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Volume",
		meta = (EditCondition = "bUniformSize == false", EditConditionHides, DisplayName = "Size"))
	FVector VolumeSize = FVector(2560.0f, 2560.0f, 2560.0f);

	/**
	 * Wall bounce (0 = no bounce, 1 = full bounce)
	 * How much particles bounce when hitting the volume walls.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Volume",
		meta = (DisplayName = "Wall Bounce", ClampMin = "0.0", ClampMax = "1.0"))
	float WallBounce = 0.3f;

	/**
	 * Wall friction (0 = slippery, 1 = sticky)
	 * How much particles slow down when sliding along walls.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Volume",
		meta = (DisplayName = "Wall Friction", ClampMin = "0.0", ClampMax = "1.0"))
	float WallFriction = 0.1f;

	//========================================
	// Static Boundary Particles (Akinci 2012)
	//========================================

	/**
	 * Enable static boundary particles for density contribution at walls/floors
	 * This helps prevent density deficit near boundaries which causes wall climbing artifacts.
	 *
	 * WARNING: Currently this feature may cause particles to fly around chaotically.
	 * Keep disabled until the underlying issue is resolved.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Volume|Boundary",
		meta = (DisplayName = "Enable Static Boundary Particles"))
	bool bEnableStaticBoundaryParticles = false;

	/**
	 * Static boundary particle spacing (cm)
	 * Lower values = denser boundary particles = better density coverage but more particles
	 * Higher values = sparser boundary particles = fewer particles but may have gaps
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Volume|Boundary",
		meta = (EditCondition = "bEnableStaticBoundaryParticles", ClampMin = "1.0", ClampMax = "50.0",
		        DisplayName = "Boundary Particle Spacing"))
	float StaticBoundaryParticleSpacing = 5.0f;

	//========================================
	// Preset Configuration
	//========================================

	/** The fluid preset that defines physics and rendering parameters */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset")
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset;

	//========================================
	// Simulation Settings
	//========================================

	/**
	 * Fluid type (Water, Lava, Slime, etc.)
	 * Used to identify which fluid in collision events.
	 * FluidInteractionComponent's OnBoneParticleCollision receives this type.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision",
	          meta = (ToolTip = "Fluid type for collision event identification."))
	EFluidType FluidType = EFluidType::None;

	/** Use world collision (floor, walls, static meshes) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision")
	bool bUseWorldCollision = true;

	//========================================
	// Collision Events
	//========================================

	/** Enable collision events (FluidInteractionComponent callbacks) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision|Events")
	bool bEnableCollisionEvents = false;

	/** Minimum velocity for event triggering (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision|Events",
	          meta = (ClampMin = "0.0", EditCondition = "bEnableCollisionEvents"))
	float MinVelocityForEvent = 50.0f;

	/** Maximum events per frame (0 = unlimited) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision|Events",
	          meta = (ClampMin = "0", EditCondition = "bEnableCollisionEvents"))
	int32 MaxEventsPerFrame = 10;

	/** Per-particle event cooldown (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision|Events",
	          meta = (ClampMin = "0.0", EditCondition = "bEnableCollisionEvents"))
	float EventCooldownPerParticle = 0.1f;

	/** Particle hit event (Blueprint bindable) */
	UPROPERTY(BlueprintAssignable, Category = "Collision|Events")
	FOnFluidParticleHitComponent OnParticleHit;

	//========================================
	// Rendering Configuration
	//========================================

	/** Enable rendering for this volume */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bEnableRendering = true;

	/** Enable shadow casting via instanced spheres */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bEnableShadow = false;

	//========================================
	// Splash VFX
	//========================================

	/** Niagara system to spawn for splash/spray effects on fast-moving particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|VFX")
	TObjectPtr<UNiagaraSystem> SplashVFX;

	/** Velocity threshold to trigger splash VFX (world units/sec) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|VFX",
	          meta = (ClampMin = "0", EditCondition = "SplashVFX != nullptr"))
	float SplashVelocityThreshold = 200.0f;

	/** Maximum splash VFX spawns per frame (performance limit) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|VFX",
	          meta = (ClampMin = "1", ClampMax = "50", EditCondition = "SplashVFX != nullptr"))
	int32 MaxSplashVFXPerFrame = 10;

	/** Condition mode for splash VFX triggering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|VFX",
	          meta = (EditCondition = "SplashVFX != nullptr"))
	ESplashConditionMode SplashConditionMode = ESplashConditionMode::VelocityAndIsolation;

	/** Neighbor count threshold for isolation (particles with this many or fewer neighbors are considered isolated) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|VFX",
	          meta = (ClampMin = "0", ClampMax = "10", EditCondition = "SplashVFX != nullptr && SplashConditionMode != ESplashConditionMode::VelocityOnly"))
	int32 IsolationNeighborThreshold = 2;

	//========================================
	// Debug Particle Visualization
	//========================================

	/** Enable debug drawing using DrawDebugPoint (works with GPU simulation) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Particles")
	bool bEnableDebugDraw = false;

	/** Debug draw visualization mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Particles",
	          meta = (EditCondition = "bEnableDebugDraw"))
	EFluidDebugVisualization DebugDrawMode = EFluidDebugVisualization::ZOrderArrayIndex;

	/** Debug point size */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Particles",
	          meta = (EditCondition = "bEnableDebugDraw", ClampMin = "1.0", ClampMax = "50.0"))
	float DebugPointSize = 8.0f;

	//========================================
	// Static Boundary Debug Visualization
	//========================================

	/** Show static boundary particles (walls, floors) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Boundary")
	bool bShowStaticBoundaryParticles = false;

	/** Static boundary particle debug point size */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Boundary",
	          meta = (EditCondition = "bShowStaticBoundaryParticles", ClampMin = "1.0", ClampMax = "50.0"))
	float StaticBoundaryPointSize = 4.0f;

	/** Static boundary particle debug color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Boundary",
	          meta = (EditCondition = "bShowStaticBoundaryParticles"))
	FColor StaticBoundaryColor = FColor::Cyan;

	/** Show surface normals for static boundary particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Boundary",
	          meta = (EditCondition = "bShowStaticBoundaryParticles"))
	bool bShowStaticBoundaryNormals = false;

	/** Length of normal debug lines */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Boundary",
	          meta = (EditCondition = "bShowStaticBoundaryParticles && bShowStaticBoundaryNormals", ClampMin = "1.0", ClampMax = "100.0"))
	float StaticBoundaryNormalLength = 10.0f;

	//========================================
	// Editor Brush (Editor only)
	//========================================

#if WITH_EDITORONLY_DATA
	/** Brush settings */
	UPROPERTY(EditAnywhere, Category = "Brush")
	FFluidBrushSettings BrushSettings;

	/** Brush mode active state (set in editor mode) */
	bool bBrushModeActive = false;
#endif

	//========================================
	// Z-Order Space Configuration (Advanced)
	//========================================

	/**
	 * Cell size for Z-Order grid (auto-derived from Preset->SmoothingRadius)
	 * Determines the grid cell size for neighbor search.
	 * This value is automatically set from the fluid preset's SmoothingRadius.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume|Advanced", meta = (DisplayName = "Cell Size (from Preset)"))
	float CellSize = 20.0f;

	/**
	 * Grid resolution preset for Z-Order sorting (auto-selected based on volume size)
	 * Read-only - the system automatically selects the optimal preset.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume|Advanced", meta = (DisplayName = "Internal Grid Preset (Auto)"))
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;

	//========================================
	// Internal Info (Read-Only, Auto-Calculated)
	//========================================

	/** Grid resolution bits per axis (derived from GridResolutionPreset) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume|Advanced")
	int32 GridAxisBits = 7;

	/** Grid resolution per axis (2^GridAxisBits) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume|Advanced")
	int32 GridResolution = 128;

	/** Total number of cells (GridResolution^3) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume|Advanced")
	int32 MaxCells = 2097152;

	/** Actual simulation bounds extent (may differ from requested VolumeSize due to grid constraints) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume|Advanced")
	float BoundsExtent = 2560.0f;

	/** World-space minimum bounds (Component location - Extent/2) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume|Advanced")
	FVector WorldBoundsMin = FVector(-1280.0f, -1280.0f, -1280.0f);

	/** World-space maximum bounds (Component location + Extent/2) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Volume|Advanced")
	FVector WorldBoundsMax = FVector(1280.0f, 1280.0f, 1280.0f);

	//========================================
	// Debug Visualization
	//========================================

	/** Show bounds wireframe in editor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowBoundsInEditor = true;

	/** Show bounds wireframe at runtime */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowBoundsAtRuntime = false;

	/** Wireframe color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FColor BoundsColor = FColor::Green;

	/** Wireframe line thickness */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.5", ClampMax = "10.0"))
	float BoundsLineThickness = 2.0f;

	/** Show internal Z-Order grid space wireframe (for debugging spatial partitioning) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Advanced", meta = (DisplayName = "Show Internal Grid Wireframe"))
	bool bShowZOrderSpaceWireframe = false;

	/** Internal grid wireframe color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Advanced",
		meta = (EditCondition = "bShowZOrderSpaceWireframe", EditConditionHides, DisplayName = "Grid Wireframe Color"))
	FColor ZOrderSpaceWireframeColor = FColor::Red;

	//========================================
	// Public Methods
	//========================================

	/** Recalculate bounds based on VolumeSize and component location */
	UFUNCTION(BlueprintCallable, Category = "Volume")
	void RecalculateBounds();

	/** Check if a world position is within this Volume's bounds */
	UFUNCTION(BlueprintCallable, Category = "Volume")
	bool IsPositionInBounds(const FVector& WorldPosition) const;

	/** Get simulation bounds (world-space) */
	UFUNCTION(BlueprintCallable, Category = "Volume")
	void GetSimulationBounds(FVector& OutMin, FVector& OutMax) const;

	/** Get simulation bounds min (world-space) */
	UFUNCTION(BlueprintPure, Category = "Volume")
	FVector GetWorldBoundsMin() const { return WorldBoundsMin; }

	/** Get simulation bounds max (world-space) */
	UFUNCTION(BlueprintPure, Category = "Volume")
	FVector GetWorldBoundsMax() const { return WorldBoundsMax; }

	/** Get the effective volume size (full size, cm) */
	UFUNCTION(BlueprintPure, Category = "Volume")
	FVector GetEffectiveVolumeSize() const
	{
		return bUniformSize ? FVector(UniformVolumeSize) : VolumeSize;
	}

	/** Get the volume half-extent (for internal collision/rendering use) */
	FVector GetVolumeHalfExtent() const
	{
		return GetEffectiveVolumeSize() * 0.5f;
	}

	/** Get wall bounce coefficient */
	UFUNCTION(BlueprintPure, Category = "Volume")
	float GetWallBounce() const { return WallBounce; }

	/** Get wall friction coefficient */
	UFUNCTION(BlueprintPure, Category = "Volume")
	float GetWallFriction() const { return WallFriction; }

	/** Get cell size */
	UFUNCTION(BlueprintPure, Category = "Volume")
	float GetCellSize() const { return CellSize; }

	/** Get bounds extent */
	UFUNCTION(BlueprintPure, Category = "Volume")
	float GetBoundsExtent() const { return BoundsExtent; }

	/** Get grid resolution preset */
	UFUNCTION(BlueprintPure, Category = "Volume")
	EGridResolutionPreset GetGridResolutionPreset() const { return GridResolutionPreset; }

	/** Get grid axis bits */
	UFUNCTION(BlueprintPure, Category = "Volume")
	int32 GetGridAxisBits() const { return GridAxisBits; }

	/** Is static boundary particles enabled */
	UFUNCTION(BlueprintPure, Category = "Volume")
	bool IsStaticBoundaryParticlesEnabled() const { return bEnableStaticBoundaryParticles; }

	/** Get static boundary particle spacing */
	UFUNCTION(BlueprintPure, Category = "Volume")
	float GetStaticBoundaryParticleSpacing() const { return StaticBoundaryParticleSpacing; }

	//========================================
	// Preset & Simulation Getters
	//========================================

	/** Get the preset */
	UFUNCTION(BlueprintPure, Category = "Preset")
	UKawaiiFluidPresetDataAsset* GetPreset() const { return Preset; }

	/** Get fluid type */
	UFUNCTION(BlueprintPure, Category = "Collision")
	EFluidType GetFluidType() const { return FluidType; }

	/** Set fluid type */
	UFUNCTION(BlueprintCallable, Category = "Collision")
	void SetFluidType(EFluidType InFluidType);

	/** Get particle spacing from preset */
	UFUNCTION(BlueprintPure, Category = "Preset")
	float GetParticleSpacing() const;

	//========================================
	// Debug Methods
	//========================================

	/** Set debug visualization mode */
	UFUNCTION(BlueprintCallable, Category = "Debug")
	void SetDebugVisualization(EFluidDebugVisualization Mode);

	/** Get current debug visualization mode */
	UFUNCTION(BlueprintPure, Category = "Debug")
	EFluidDebugVisualization GetDebugVisualization() const { return DebugDrawMode; }

	/** Enable debug drawing with specified mode */
	UFUNCTION(BlueprintCallable, Category = "Debug")
	void EnableDebugDraw(EFluidDebugVisualization Mode, float PointSize = 8.0f);

	/** Disable debug drawing */
	UFUNCTION(BlueprintCallable, Category = "Debug")
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
	UFUNCTION(BlueprintPure, Category = "Volume")
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
