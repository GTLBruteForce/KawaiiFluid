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
	Stream      UMETA(DisplayName = "Stream", ToolTip = "Straight stream emission (like a faucet)"),
	Spray       UMETA(DisplayName = "Spray", ToolTip = "Cone-shaped spray emission"),
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
	/** Spacing between particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Distribution",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::ShapeVolume", EditConditionHides, ClampMin = "1.0"))
	float ParticleSpacing = 10.0f;

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

	/** Particles spawned per second */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle Spawn|Flow",
	          meta = (EditCondition = "SpawnType == EFluidSpawnType::Emitter", EditConditionHides, ClampMin = "1.0", ClampMax = "1000.0"))
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

	/** Calculate expected particle count (for editor preview) */
	int32 CalculateExpectedParticleCount() const;

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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(
	FOnFluidParticleHitComponent,
	int32, ParticleIndex,
	AActor*, HitActor,
	FVector, HitLocation,
	FVector, HitNormal,
	float, HitSpeed
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

	/** ISM Renderer Settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "ISM Settings"))
	FKawaiiFluidISMRendererSettings ISMSettings;

	/** Metaball Renderer Settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "Metaball Settings"))
	FKawaiiFluidMetaballRendererSettings MetaballSettings;

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

	/** Spawn single particle for Stream/Jet mode */
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
};
