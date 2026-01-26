// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Core/KawaiiFluidRenderingTypes.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "KawaiiFluidComponent.generated.h"

class UKawaiiFluidRenderingModule;
class UKawaiiFluidComponent;
class UKawaiiFluidPresetDataAsset;
class UKawaiiFluidVolumeComponent;
class AKawaiiFluidVolume;
class UNiagaraSystem;

/**
 * Instance data for preserving particle data during re-construction
 */
USTRUCT()
struct FKawaiiFluidComponentInstanceData : public FActorComponentInstanceData
{
	GENERATED_BODY()

public:
	FKawaiiFluidComponentInstanceData() = default;
	FKawaiiFluidComponentInstanceData(const UKawaiiFluidComponent* SourceComponent);

	virtual void ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase) override;

	// Particle data to preserve
	TArray<FFluidParticle> SavedParticles;
};

// EFluidBrushMode and FFluidBrushSettings are now defined in KawaiiFluidSimulationTypes.h

/**
 * Top-level spawn type
 */
UENUM(BlueprintType)
enum class EFluidSpawnType : uint8
{
	ShapeVolume UMETA(DisplayName = "Shape Volume", ToolTip = "Spawn particles filling a shape volume at BeginPlay"),
	Emitter     UMETA(DisplayName = "Emitter", ToolTip = "Continuously emit particles from a point"),
};

/**
 * Shape type for Shape Volume spawn mode
 */
UENUM(BlueprintType)
enum class EFluidShapeType : uint8
{
	Sphere      UMETA(DisplayName = "Sphere", ToolTip = "Spherical volume distribution"),
	Box         UMETA(DisplayName = "Box", ToolTip = "Box volume distribution"),
	Cylinder    UMETA(DisplayName = "Cylinder", ToolTip = "Cylindrical volume distribution"),
};

/**
 * Grid pattern for Shape Volume spawn mode
 */
UENUM(BlueprintType)
enum class ESpawnGridPattern : uint8
{
	Cubic       UMETA(DisplayName = "Cubic", ToolTip = "Regular cubic grid - simple but can have density fluctuations"),
	Hexagonal   UMETA(DisplayName = "Hexagonal", ToolTip = "Hexagonal close packing - more stable initial state for calm fluid"),
};

/**
 * Emitter type for Emitter spawn mode
 */
UENUM(BlueprintType)
enum class EFluidEmitterType : uint8
{
	Stream            UMETA(DisplayName = "Stream", ToolTip = "Random spawn within circle, controlled by ParticlesPerSecond"),
	HexagonalStream   UMETA(DisplayName = "Hexagonal Stream", ToolTip = "Hexagonal-packed layers for dense continuous stream (like a faucet)"),
	Spray             UMETA(DisplayName = "Spray", ToolTip = "Cone-shaped spray emission"),
};

/**
 * Layer spawn rate mode for Hexagonal Stream
 */
UENUM(BlueprintType)
enum class EStreamLayerMode : uint8
{
	VelocityBased     UMETA(DisplayName = "Velocity Based", ToolTip = "Spawn rate automatically adjusts to velocity for continuous stream"),
	FixedRate         UMETA(DisplayName = "Fixed Rate", ToolTip = "Spawn fixed number of layers per second"),
};


// ESplashConditionMode and EKawaiiFluidDebugDrawMode are now defined in KawaiiFluidRenderingTypes.h

/**
 * Fluid spawn settings
 */
USTRUCT(BlueprintType)
struct FFluidSpawnSettings
{
	GENERATED_BODY()

	/** Spawn type (Shape Volume or Emitter) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn")
	EFluidSpawnType SpawnType = EFluidSpawnType::ShapeVolume;

	/** Shape type (when SpawnType is ShapeVolume) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume", EditConditionHides))
	EFluidShapeType ShapeType = EFluidShapeType::Sphere;

	/** Emitter type (when SpawnType is Emitter) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter", EditConditionHides))
	EFluidEmitterType EmitterType = EFluidEmitterType::Stream;

	// === Shape Settings (ShapeVolume mode) ===
	/** Sphere radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Shape",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume && ShapeType == EFluidShapeType::Sphere", EditConditionHides, ClampMin = "1.0"))
	float SphereRadius = 50.0f;

	/** Box size (Half Extent) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Shape",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume && ShapeType == EFluidShapeType::Box", EditConditionHides))
	FVector BoxExtent = FVector(50, 50, 50);

	/** Cylinder radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Shape",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume && ShapeType == EFluidShapeType::Cylinder", EditConditionHides, ClampMin = "1.0"))
	float CylinderRadius = 30.0f;

	/** Cylinder height (Half Height) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Shape",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume && ShapeType == EFluidShapeType::Cylinder", EditConditionHides, ClampMin = "1.0"))
	float CylinderHalfHeight = 50.0f;

	// === Distribution Settings (ShapeVolume mode) ===
	/** Grid pattern for particle distribution */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Distribution",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume", EditConditionHides))
	ESpawnGridPattern GridPattern = ESpawnGridPattern::Hexagonal;

	/** Apply random offset to grid positions */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Distribution",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume", EditConditionHides))
	bool bUseJitter = true;

	/** Random offset ratio (0~0.5) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Distribution",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume && bUseJitter", EditConditionHides, ClampMin = "0.0", ClampMax = "0.5"))
	float JitterAmount = 0.2f;

	// === Flow Settings (Emitter mode) ===
	/** Emission direction (normalized) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter", EditConditionHides))
	FVector SpawnDirection = FVector(0, 0, -1);

	/** Initial speed (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter", EditConditionHides, ClampMin = "0.0"))
	float SpawnSpeed = 100.0f;

	/** Stream radius (spread range) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter", EditConditionHides, ClampMin = "0.0"))
	float StreamRadius = 5.0f;

	/** Spacing between particles in hexagonal cross-section (0 = auto from SmoothingRadius * 0.5) - Hexagonal Stream only */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter && EmitterType == EFluidEmitterType::HexagonalStream", EditConditionHides, ClampMin = "0.0"))
	float StreamParticleSpacing = 0.0f;

	/** Layer spawn rate mode - Hexagonal Stream only */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter && EmitterType == EFluidEmitterType::HexagonalStream", EditConditionHides))
	EStreamLayerMode StreamLayerMode = EStreamLayerMode::VelocityBased;

	/** Number of hexagonal layers spawned per second - Fixed Rate mode only */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter && EmitterType == EFluidEmitterType::HexagonalStream && StreamLayerMode == EStreamLayerMode::FixedRate", EditConditionHides, ClampMin = "1.0", ClampMax = "500.0"))
	float StreamLayersPerSecond = 60.0f;

	/** Random position jitter for hexagonal packing (0 = perfect grid, 0.5 = max natural look) - Hexagonal Stream only */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter && EmitterType == EFluidEmitterType::HexagonalStream", EditConditionHides, ClampMin = "0.0", ClampMax = "0.5"))
	float StreamJitter = 0.15f;

	/**
	 * Layer-to-layer spacing ratio relative to particle spacing - Hexagonal Stream only
	 * Controls how tightly packed layers are along the flow direction.
	 * 1.0 = layers are particle-spacing apart (may look disconnected)
	 * 0.816 = ideal 3D hexagonal close packing (HCP) - default
	 * 0.5 = layers overlap significantly (very dense stream)
	 * Lower values = denser, more continuous stream
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter && EmitterType == EFluidEmitterType::HexagonalStream", EditConditionHides, ClampMin = "0.2", ClampMax = "1.0"))
	float StreamLayerSpacingRatio = 0.816f;

	/** Particles spawned per second - Stream and Spray modes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter && (EmitterType == EFluidEmitterType::Stream || EmitterType == EFluidEmitterType::Spray)", EditConditionHides, ClampMin = "1.0", ClampMax = "1000.0"))
	float ParticlesPerSecond = 30.0f;

	/** Cone angle (degrees) - Spray emitter only */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter && EmitterType == EFluidEmitterType::Spray", EditConditionHides, ClampMin = "0.0", ClampMax = "90.0"))
	float ConeAngle = 15.0f;

	// === Count Settings ===
	/** Auto-calculate particle count from shape size and spacing (ShapeVolume only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Count",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume", EditConditionHides))
	bool bAutoCalculateParticleCount = true;

	/** Explicit particle count (ShapeVolume only, when bAutoCalculateParticleCount is false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Count",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume && !bAutoCalculateParticleCount", EditConditionHides, ClampMin = "1", ClampMax = "100000"))
	int32 ParticleCount = 500;

	/** Maximum particle count - Emitter mode only (0 = unlimited) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Count",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter", EditConditionHides, ClampMin = "0"))
	int32 MaxParticleCount = 1000;

	/** Recycle oldest particles when MaxParticleCount is reached
	 * When true: removes oldest particles to make room for new spawns (FIFO queue behavior)
	 * When false: stops spawning when MaxParticleCount is reached
	 * Only effective when MaxParticleCount > 0
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Count",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter && MaxParticleCount > 0"))
	bool bContinuousSpawn = false;

	// === Common Settings ===
	/** Initial velocity (ShapeVolume mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume", EditConditionHides))
	FVector InitialVelocity = FVector::ZeroVector;

	/** Spawn position offset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn")
	FVector SpawnOffset = FVector::ZeroVector;

	/** Calculate expected particle count (for editor preview)
	 * @param InParticleSpacing Particle spacing from Preset (auto-calculated)
	 */
	int32 CalculateExpectedParticleCount(float InParticleSpacing) const;

	/** Check if this is a ShapeVolume mode (batch spawn at BeginPlay) */
	bool IsShapeVolumeMode() const
	{
		return SpawnType == EFluidSpawnType::ShapeVolume;
	}

	/** Check if this is an Emitter mode (continuous spawn) */
	bool IsEmitterMode() const
	{
		return SpawnType == EFluidSpawnType::Emitter;
	}
};

// FFluidBrushSettings and FOnFluidParticleHitComponent are now defined in KawaiiFluidSimulationTypes.h

/**
 * Kawaii Fluid Component (Unified Component)
 *
 * Unified component for fluid simulation.
 * Uses modular design to separate simulation/rendering/collision management.
 *
 * Access simulation API through SimulationModule:
 * - Component->SimulationModule->SpawnParticles(...)
 * - Component->SimulationModule->ApplyExternalForce(...)
 *
 * Usage:
 * @code
 * FluidComponent = CreateDefaultSubobject<UKawaiiFluidComponent>(TEXT("FluidComponent"));
 * FluidComponent->Preset = MyPreset;
 *
 * // Direct module access from Blueprint/C++
 * FluidComponent->SimulationModule->SpawnParticles(Location, 100, 50.0f);
 * @endcode
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent, DisplayName="Kawaii Fluid"))
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UKawaiiFluidComponent();

	//========================================
	// UActorComponent Interface
	//========================================

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnRegister() override;
	virtual void OnComponentDestroyed(bool bDestroyingOK) override;
	virtual TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	
	//========================================
	// Module Accessors
	//========================================

	/** Returns simulation module - provides all APIs for particles/colliders/external forces */
	UFUNCTION(BlueprintPure, Category = "Fluid")
	UKawaiiFluidSimulationModule* GetSimulationModule() const { return SimulationModule; }

	/** Returns rendering module - provides access to renderers */
	UKawaiiFluidRenderingModule* GetRenderingModule() const { return RenderingModule; }

	//========================================
	// Preset Configuration
	//========================================

	/** Fluid preset data asset - contains physics and rendering parameters */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Configuration")
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset;

	//========================================
	// Simulation Volume Access (Delegated to SimulationModule)
	// Configure Simulation Volume settings in the "Fluid|Simulation Volume" category
	// of the SimulationModule property below.
	//========================================

	/** Get the target simulation volume actor (from SimulationModule) */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	AKawaiiFluidVolume* GetTargetSimulationVolume() const;

	/** Get the effective volume component (from SimulationModule) */
	UFUNCTION(BlueprintPure, Category = "Fluid|Simulation Volume")
	UKawaiiFluidVolumeComponent* GetTargetVolumeComponent() const;

	/** Set the target simulation volume at runtime (delegates to SimulationModule) */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Simulation Volume")
	void SetTargetSimulationVolume(AKawaiiFluidVolume* NewSimulationVolume);

	//========================================
	// Rendering Settings
	//========================================

	/** Enable rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering")
	bool bEnableRendering = true;

	/** Enable shadow casting via instanced spheres */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering")
	bool bEnableShadow = true;

	//========================================
	// Splash VFX
	//========================================

	/** Niagara system to spawn for splash/spray effects on fast-moving particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|VFX")
	TObjectPtr<UNiagaraSystem> SplashVFX;

	/** Velocity threshold to trigger splash VFX (world units/sec) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|VFX", meta = (ClampMin = "0", EditCondition = "SplashVFX != nullptr"))
	float SplashVelocityThreshold = 200.0f;

	/** Maximum splash VFX spawns per frame (performance limit) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|VFX", meta = (ClampMin = "1", ClampMax = "50", EditCondition = "SplashVFX != nullptr"))
	int32 MaxSplashVFXPerFrame = 10;

	/** Condition mode for splash VFX triggering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|VFX", meta = (EditCondition = "SplashVFX != nullptr"))
	ESplashConditionMode SplashConditionMode = ESplashConditionMode::VelocityAndIsolation;

	/** Neighbor count threshold for isolation (particles with this many or fewer neighbors are considered isolated) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|VFX", meta = (ClampMin = "0", ClampMax = "10", EditCondition = "SplashVFX != nullptr && SplashConditionMode != ESplashConditionMode::VelocityOnly"))
	int32 IsolationNeighborThreshold = 2;

	//========================================
	// Debug Visualization
	//========================================

	/** Debug visualization mode (None, ISM Sphere, or Debug Point) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug")
	EKawaiiFluidDebugDrawMode DebugDrawMode = EKawaiiFluidDebugDrawMode::None;

	/** Debug particle color (ISM mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug", meta = (EditCondition = "DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM", EditConditionHides))
	FLinearColor ISMDebugColor = FLinearColor(0.2f, 0.5f, 1.0f, 0.8f);

	/** Maximum particles to render in ISM mode (performance limit) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug",
		meta = (EditCondition = "DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM", EditConditionHides,
		        ClampMin = "1000", ClampMax = "500000"))
	int32 ISMMaxRenderParticles = 100000;

	/** Debug visualization type (DebugDraw mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug", meta = (EditCondition = "DebugDrawMode == EKawaiiFluidDebugDrawMode::DebugDraw", EditConditionHides))
	EFluidDebugVisualization DebugVisualizationType = EFluidDebugVisualization::ZOrderArrayIndex;

	//========================================
	// Spawn Settings
	//========================================

	/** Spawn settings (mode, shape, direction, etc.) - displayed expanded via ShowOnlyInnerProperties */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn", meta = (ShowOnlyInnerProperties))
	FFluidSpawnSettings SpawnSettings;

	//========================================
	// Events
	//========================================

	/** Particle hit event (Blueprint bindable) */
	UPROPERTY(BlueprintAssignable, Category = "Fluid|Events")
	FOnFluidParticleHitComponent OnParticleHit;

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
	// Brush API (Editor/Runtime shared)
	//========================================

	/** Add particles within radius (hemisphere distribution - spawns above surface only) */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Brush")
	void AddParticlesInRadius(const FVector& WorldCenter, float Radius, int32 Count,
	                          const FVector& Velocity, float Randomness = 0.8f,
	                          const FVector& SurfaceNormal = FVector::UpVector);

	/** Remove particles within radius */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Brush")
	int32 RemoveParticlesInRadius(const FVector& WorldCenter, float Radius);

	/** Remove all particles + clear rendering */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Brush")
	void ClearAllParticles();

	//========================================
	// Debug Visualization API (Blueprint)
	//========================================

	/**
	 * Set debug visualization mode (None, ISM, or DebugDraw)
	 * @param Mode Debug draw mode to use
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Debug")
	void SetDebugDrawMode(EKawaiiFluidDebugDrawMode Mode);

	/** Get current debug draw mode */
	UFUNCTION(BlueprintPure, Category = "Fluid|Debug")
	EKawaiiFluidDebugDrawMode GetDebugDrawMode() const { return DebugDrawMode; }

	/**
	 * Set debug visualization type (for DebugDraw mode)
	 * @param Type Visualization type (ZOrderArrayIndex, Density, etc.)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Debug")
	void SetDebugVisualizationType(EFluidDebugVisualization Type);

	/** Get current debug visualization type */
	UFUNCTION(BlueprintPure, Category = "Fluid|Debug")
	EFluidDebugVisualization GetDebugVisualizationType() const { return DebugVisualizationType; }

	//========================================
	// Static Boundary Debug Visualization
	// (Shows boundary particles on walls/floors for density contribution)
	//========================================

	//========================================
	// Static Boundary Particles (Akinci 2012)
	// Enables density contribution from walls/floors
	//========================================

	/** Enable static boundary particles for density contribution at walls/floors (Akinci 2012)
	 * This helps prevent density deficit near boundaries which causes wall climbing artifacts */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Boundary")
	bool bEnableStaticBoundaryParticles = true;

	/**
	 * Static boundary particle spacing (cm)
	 * Lower values = denser boundary particles = better density coverage but more particles
	 * Higher values = sparser boundary particles = fewer particles but may have gaps
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Boundary",
		meta = (EditCondition = "bEnableStaticBoundaryParticles", ClampMin = "1.0", ClampMax = "50.0",
		        ToolTip = "경계 입자 간격 (cm).\n작을수록 정밀하지만 입자 수 증가.\nParticleRadius의 0.5~1.0배 권장."))
	float StaticBoundaryParticleSpacing = 5.0f;

	/** Show static boundary particles (walls, floors) generated for density contribution (Akinci 2012) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug|Boundary")
	bool bShowStaticBoundaryParticles = false;

	/** Static boundary particle debug point size */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug|Boundary", meta = (EditCondition = "bShowStaticBoundaryParticles", ClampMin = "1.0", ClampMax = "50.0"))
	float StaticBoundaryPointSize = 4.0f;

	/** Static boundary particle debug color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug|Boundary", meta = (EditCondition = "bShowStaticBoundaryParticles"))
	FColor StaticBoundaryColor = FColor::Cyan;

	/** Show surface normals for static boundary particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug|Boundary", meta = (EditCondition = "bShowStaticBoundaryParticles"))
	bool bShowStaticBoundaryNormals = false;

	/** Length of normal debug lines */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug|Boundary", meta = (EditCondition = "bShowStaticBoundaryParticles && bShowStaticBoundaryNormals", ClampMin = "1.0", ClampMax = "100.0"))
	float StaticBoundaryNormalLength = 10.0f;

protected:
	//========================================
	// Modules
	//========================================

	/** Simulation module - access via GetSimulationModule() */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Instanced, Category = "Fluid")
	TObjectPtr<UKawaiiFluidSimulationModule> SimulationModule;

	
	/** Rendering module - renderer management */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidRenderingModule> RenderingModule;
private:
	//========================================
	// Continuous Spawn
	//========================================

	float SpawnAccumulatedTime = 0.0f;
	void ProcessContinuousSpawn(float DeltaTime);

	//========================================
	// Debug State Cache
	//========================================

	/** Cached debug draw mode (for change detection) */
	EKawaiiFluidDebugDrawMode CachedDebugDrawMode = EKawaiiFluidDebugDrawMode::None;

	/** Cached ISM debug color (for change detection) */
	FLinearColor CachedISMDebugColor = FLinearColor(0.2f, 0.5f, 1.0f, 0.8f);

	//========================================
	// Spawn Helpers
	//========================================

	/** Execute auto spawn (called from BeginPlay) */
	void ExecuteAutoSpawn();

	/** Spawn single particle for Stream/Jet mode (Spray mode) */
	void SpawnDirectionalParticle();

	//========================================
	// Editor Visualization
	//========================================

#if WITH_EDITOR
	/** Spawn area visualization (called in editor) */
	void DrawSpawnAreaVisualization();
#endif

	//========================================
	// Event System
	//========================================

	/** Handle collision event from Module callback */
	void HandleCollisionEvent(const FKawaiiFluidCollisionEvent& Event);

	//========================================
	// Subsystem Registration
	//========================================

	void RegisterToSubsystem();
	void UnregisterFromSubsystem();

	//========================================
	// Debug Draw Helpers
	//========================================

	/** Draw debug particles using DrawDebugPoint */
	void DrawDebugParticles();

	/** Draw static boundary particles for debug visualization */
	void DrawDebugStaticBoundaryParticles();

#if WITH_EDITOR
	/** Generate boundary particles for editor preview (without GPU simulation) */
	void GenerateEditorBoundaryParticlesPreview();

	/** Cached boundary particles for editor preview */
	TArray<FVector> EditorPreviewBoundaryPositions;
	TArray<FVector> EditorPreviewBoundaryNormals;

	/** Last frame editor preview was generated */
	uint64 LastEditorPreviewFrame = 0;
#endif

	/** Compute debug color for a particle */
	FColor ComputeDebugDrawColor(int32 ParticleIndex, int32 TotalCount, const FVector& Position, float Density) const;

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
