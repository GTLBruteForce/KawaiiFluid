// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "KawaiiFluidComponent.generated.h"

class UKawaiiFluidRenderingModule;
class UKawaiiFluidComponent;
class UKawaiiFluidPresetDataAsset;

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
	int32 SavedNextParticleID = 0;
};

/**
 * Brush mode type
 */
UENUM(BlueprintType)
enum class EFluidBrushMode : uint8
{
	Add       UMETA(DisplayName = "Add Particles"),
	Remove    UMETA(DisplayName = "Remove Particles")
};

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

/**
 * Brush settings struct
 */
USTRUCT(BlueprintType)
struct FFluidBrushSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Brush")
	EFluidBrushMode Mode = EFluidBrushMode::Add;

	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "10.0", ClampMax = "500.0"))
	float Radius = 50.0f;

	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "1", ClampMax = "100"))
	int32 ParticlesPerStroke = 15;

	UPROPERTY(EditAnywhere, Category = "Brush")
	FVector InitialVelocity = FVector(0, 0, 0);

	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Randomness = 0.8f;

	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "0.01", ClampMax = "0.5"))
	float StrokeInterval = 0.03f;
};

/**
 * Particle hit event delegate
 * Called when a particle collides with an actor
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnFluidParticleHitComponent,
	const FKawaiiFluidCollisionEvent&, CollisionEvent
);

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
	virtual void OnUnregister() override;
	virtual TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;
	
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
	// GPU Simulation
	//========================================

	/** Enable GPU physics simulation (compute shaders)
	 * When enabled, SPH physics runs on GPU for better performance with large particle counts
	 * Attached particles are still handled by CPU for bone tracking
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|GPU")
	bool bUseGPUSimulation = false;

	//========================================
	// Rendering Settings
	//========================================

	/** Enable rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering")
	bool bEnableRendering = true;

	/** ISM Renderer Settings (per-Component, debug/preview purpose) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "ISM Settings"))
	FKawaiiFluidISMRendererSettings ISMSettings;

	// Note: Metaball rendering parameters are in Preset->RenderingParameters
	// This ensures same Preset = same Metaball rendering = proper batching

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
	// Debug Visualization (Z-Order Sorting)
	//========================================

	/**
	 * Set debug visualization mode for verifying Z-Order sorting
	 *
	 * Use ArrayIndex mode to check if particles are spatially sorted:
	 * - If Z-Order sorted correctly: particles close in space will have similar colors
	 * - If NOT sorted: colors will be random/scattered regardless of position
	 *
	 * @param Mode Debug visualization mode (None to disable)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Debug")
	void SetDebugVisualization(EFluidDebugVisualization Mode);

	/** Get current debug visualization mode */
	UFUNCTION(BlueprintPure, Category = "Fluid|Debug")
	EFluidDebugVisualization GetDebugVisualization() const;

	//========================================
	// DrawDebugPoint Visualization
	// (Works with GPU simulation, no material needed)
	//========================================

	/** Enable debug drawing using DrawDebugPoint (works with GPU simulation) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug")
	bool bEnableDebugDraw = false;

	/** Debug draw visualization mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug", meta = (EditCondition = "bEnableDebugDraw"))
	EFluidDebugVisualization DebugDrawMode = EFluidDebugVisualization::ZOrderArrayIndex;

	/** Debug point size */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Debug", meta = (EditCondition = "bEnableDebugDraw", ClampMin = "1.0", ClampMax = "50.0"))
	float DebugPointSize = 8.0f;

	/** Enable debug drawing with specified mode */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Debug")
	void EnableDebugDraw(EFluidDebugVisualization Mode, float PointSize = 8.0f);

	/** Disable debug drawing */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Debug")
	void DisableDebugDraw();

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

	/** Compute debug color for a particle */
	FColor ComputeDebugDrawColor(int32 ParticleIndex, int32 TotalCount, const FVector& Position, float Density) const;

	/** Cached bounds for debug visualization (auto-computed) */
	FVector DebugDrawBoundsMin = FVector::ZeroVector;
	FVector DebugDrawBoundsMax = FVector::ZeroVector;
};
