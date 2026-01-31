// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/EngineTypes.h"
#include "Rendering/FluidRenderingParameters.h"
#include "KawaiiFluidPresetDataAsset.generated.h"

class UKawaiiFluidPresetDataAsset;

/** Delegate broadcast when preset properties change (SmoothingRadius, etc.) */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnPresetPropertyChanged, UKawaiiFluidPresetDataAsset*);

/**
 * Fluid Preset Data Asset
 * Stores all physics and rendering parameters for a fluid type
 * Editor-friendly configuration
 */
UCLASS(BlueprintType)
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidPresetDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UKawaiiFluidPresetDataAsset();

	//========================================
	// Rendering Parameters
	//========================================

	/**
	 * Metaball rendering parameters for this fluid preset
	 * All KawaiiFluidComponents sharing this preset will use these settings
	 * Note: ISM settings remain per-Component (debug/preview purpose, no batching needed)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (ShowOnlyInnerProperties))
	FFluidRenderingParameters RenderingParameters;

	//========================================
	// Physics | Material
	//========================================

	/** XSPH viscosity (0=water, 0.5=slime, 0.8=honey) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Viscosity = 0.5f;

	/** Density (kg/m³) - Water: 1000, Slime: 1200, Honey: 1400 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "500.0", ClampMax = "3000.0"))
	float Density = 1200.0f;

	/** Compressibility - lower = more incompressible, higher = softer */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "0.0", ClampMax = "0.001"))
	float Compressibility = 0.00001f;

	/**
	 * Surface Tension (Position-Based)
	 * Minimizes surface area - creates rounded water droplets
	 * Higher values = particles pull together more at the surface
	 * 0 = dispersed, 1 = tight spherical blobs
	 * Note: Uses Position-Based constraint (stable at low viscosity)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SurfaceTension = 0.3f;

	/**
	 * Cohesion (Position-Based)
	 * Keeps particles connected at rest distance (ParticleSpacing)
	 * Higher values = water streams stay connected, don't scatter like sand
	 * 0 = no cohesion (particles scatter freely)
	 * 0.3~0.5 = water-like (streams stay connected)
	 * Note: Different from Surface Tension - this maintains connections, not shape
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Cohesion = 0.0f;

	/**
	 * Adhesion (Akinci 2013)
	 * Force pulling fluid toward boundary surfaces
	 * Higher values = stronger "sticking" to walls/characters
	 * 0 = no adhesion force, 10+ = very sticky
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "0.0", ClampMax = "50.0"))
	float Adhesion = 5.0f;

	/** Bounciness (0=no bounce, 1=full bounce) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Bounciness = 0.0f;

	/** Friction */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Friction = 0.5f;

	/**
	 * Air resistance (0=no resistance, 1=maximum resistance)
	 * Controls velocity damping per substep
	 * 0 = no energy loss, 1 = 10% energy loss per substep
	 * Useful for stabilizing simulation and preventing perpetual motion
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AirResistance = 0.05f;

	/** Convert AirResistance to internal Damping value (0.9~1.0) */
	UFUNCTION(BlueprintPure, Category = "Physics|Material")
	float GetDamping() const { return 1.0f - (AirResistance * 0.1f); }

	//========================================
	// Physics | Simulation | Resolution
	//========================================

	/** Particle render radius (cm) - primary resolution input */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Resolution", meta = (ClampMin = "0.1"))
	float ParticleRadius = 5.0f;

	/**
	 * Spacing ratio (d/h) - particle density relative to smoothing radius
	 * Recommended: 0.5 (yields ~33 neighbors in 3D)
	 * Range: 0.3 (dense, ~124 neighbors) to 0.7 (sparse, ~15 neighbors)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Resolution",
		meta = (ClampMin = "0.1", ClampMax = "0.7"))
	float SpacingRatio = 0.5f;

	/**
	 * Smoothing radius (kernel radius h, cm)
	 * Auto-calculated: ParticleRadius * 2 / SpacingRatio
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Physics|Simulation|Resolution")
	float SmoothingRadius = 20.0f;

	/**
	 * Particle spacing (cm) - distance between particles when spawned
	 * Auto-calculated: ParticleRadius * 2
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Physics|Simulation|Resolution")
	float ParticleSpacing = 10.0f;

	/**
	 * Particle mass (kg)
	 * Auto-calculated: Density * (ParticleSpacing_m)³
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Physics|Simulation|Resolution")
	float ParticleMass = 1.0f;

	/** Estimated neighbor count based on spacing ratio */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Physics|Simulation|Resolution")
	int32 EstimatedNeighborCount = 33;

	//========================================
	// Physics | Simulation | Solver
	//========================================

	/** Substep target dt (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Solver", meta = (ClampMin = "0.001", ClampMax = "0.05"))
	float SubstepDeltaTime = 1.0f / 120.0f;

	/** Maximum substep count */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Solver", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxSubsteps = 8;

	/** XPBD constraint solver iterations (viscous fluid: 2-3, water: 4-6) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Solver", meta = (ClampMin = "1", ClampMax = "10"))
	int32 SolverIterations = 3;

	/**
	 * Compliance scaling exponent for SmoothingRadius independence
	 * When SmoothingRadius differs from 20cm, Compressibility is auto-scaled:
	 * ScaledCompressibility = Compressibility * (20/h)^Exponent
	 * Higher values = stronger scaling for stability at smaller radii
	 * Recommended: 4-6 (theoretical: 10, practical: 4-6)
	 * 0 = disable auto-scaling
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Solver", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float ComplianceExponent = 4.0f;

	/** Gravity */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Solver")
	FVector Gravity = FVector(0.0f, 0.0f, -980.0f);

	//========================================
	// Physics | Simulation | Collision
	//========================================

	/**
	 * Fluid name for collision event identification
	 * Used to identify fluid type on collision (e.g., Switch on Name in BP)
	 * Example: "Lava", "Water", "Slime"
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Collision")
	FName FluidName = NAME_None;

	/** Fluid name getter */
	UFUNCTION(BlueprintPure, Category = "Physics|Simulation|Collision")
	FName GetFluidName() const { return FluidName; }

	/**
	 * Collision threshold (cm)
	 * Particle center within (ParticleRadius + this value) from collider surface triggers collision
	 * Higher = earlier detection, Lower = closer approach before collision
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Collision", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float CollisionThreshold = 1.0f;

	//========================================
	// Physics | Simulation | Adhesion
	//========================================

	/**
	 * Adhesion Velocity Transfer Strength
	 * How much fluid velocity follows moving boundary velocity
	 * Higher values = fluid follows moving characters better
	 * 0 = no following, 1 = full velocity match
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Adhesion", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float AdhesionVelocityStrength = 0.5f;

	/** Adhesion radius (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Adhesion", meta = (ClampMin = "0.1"))
	float AdhesionRadius = 25.0f;

	/**
	 * Additional contact offset (cm) applied to adhesion/collision checks.
	 * Positive values allow particles to overlap deeper into bone colliders.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Adhesion", meta = (ClampMin = "-50.0", ClampMax = "50.0"))
	float AdhesionContactOffset = 0.0f;

	/**
	 * Scale applied to bone velocity when updating attached particle velocity.
	 * 1 = inherit full bone motion, 0 = keep previous particle velocity.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Adhesion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AdhesionBoneVelocityScale = 1.0f;

	/**
	 * Enable Boundary Attachment (BoneDeltaAttachment)
	 * When enabled, particles near boundary will attach and follow bone movement.
	 * Disable to use only adhesion forces without position constraint.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Adhesion")
	bool bEnableBoundaryAttachment = true;

	//========================================
	// Physics | Simulation | Boundary | Interaction
	//========================================

	/**
	 * Enable relative velocity based pressure damping
	 * When enabled, reduces pressure repulsion when boundary is approaching fluid
	 * This prevents fluid from "flying away" when characters move fast
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Boundary|Interaction")
	bool bEnableRelativeVelocityDamping = true;

	/**
	 * Strength of relative velocity damping for pressure
	 * 0 = no damping (original behavior), 1 = full damping (fluid sticks to boundary)
	 * Recommended: 0.5 ~ 0.8
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Boundary|Interaction",
		meta = (EditCondition = "bEnableRelativeVelocityDamping", ClampMin = "0.0", ClampMax = "1.0"))
	float RelativeVelocityDampingStrength = 0.6f;

	/**
	 * Strength of boundary velocity transfer to fluid
	 * Higher = fluid follows boundary more closely
	 * 1.0 = full velocity inheritance
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Boundary|Interaction",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BoundaryVelocityTransferStrength = 0.8f;

	/**
	 * Relative speed threshold (cm/s) where detachment begins
	 * When fluid-boundary relative speed exceeds this, velocity transfer reduces
	 * Typical walking: 300~500 cm/s, Running: 600~1000 cm/s
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Boundary|Interaction",
		meta = (ClampMin = "0.0", ClampMax = "5000.0"))
	float BoundaryDetachSpeedThreshold = 500.0f;

	/**
	 * Relative speed (cm/s) for full detachment
	 * Above this speed, no velocity transfer from boundary
	 * Should be > BoundaryDetachSpeedThreshold
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Boundary|Interaction",
		meta = (ClampMin = "0.0", ClampMax = "10000.0"))
	float BoundaryMaxDetachSpeed = 1500.0f;

	//========================================
	// Physics | Simulation | Stability
	//========================================

	/**
	 * Enable artificial pressure (scorr) for surface tension
	 * Prevents particle clustering at low-density regions (splash, surface)
	 * Based on PBF paper Section 4 (Macklin & Muller, 2013)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability")
	bool bEnableTensileCorrection = true;

	/**
	 * Artificial pressure strength (k in paper)
	 * Higher values create stronger repulsion at surface
	 * Typical: 0.1 for water-like, 0.01 for viscous fluids
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability",
		meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bEnableTensileCorrection"))
	float TensileInstabilityK = 0.1f;

	/**
	 * Artificial pressure exponent (n in paper)
	 * Higher values make the correction more localized
	 * Typical: 4
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability",
		meta = (ClampMin = "1", ClampMax = "8", EditCondition = "bEnableTensileCorrection"))
	int32 TensileInstabilityN = 4;

	/**
	 * Reference distance as fraction of smoothing radius (dq/h)
	 * scorr uses W(r)/W(dq) ratio
	 * Typical: 0.2 (20% of h), NVIDIA Flex uses 0.0
	 * When 0.0: W(dq) = W(0) = max kernel value, pure anti-clustering
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability",
		meta = (ClampMin = "0.0", ClampMax = "0.5", EditCondition = "bEnableTensileCorrection"))
	float TensileInstabilityDeltaQ = 0.2f;

	/**
	 * Enable particle sleeping for stability
	 * Sleeping particles are excluded from constraint solving, reducing micro-jitter
	 * Reference: NVIDIA Flex stabilization technique
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability")
	bool bEnableParticleSleeping = false;

	/**
	 * Velocity threshold for sleep (cm/s)
	 * Particles moving slower than this may enter sleep state
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability",
		meta = (EditCondition = "bEnableParticleSleeping", ClampMin = "0.1", ClampMax = "100.0"))
	float SleepVelocityThreshold = 5.0f;

	/**
	 * Number of consecutive low-velocity frames before sleeping
	 * Higher values = more stable but slower sleep response
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability",
		meta = (EditCondition = "bEnableParticleSleeping", ClampMin = "1", ClampMax = "120"))
	int32 SleepFrameThreshold = 30;

	/**
	 * Velocity threshold for wake-up (cm/s)
	 * Sleeping particles wake up when nearby particles or external forces exceed this
	 * Should be higher than SleepVelocityThreshold to prevent oscillation
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability",
		meta = (EditCondition = "bEnableParticleSleeping", ClampMin = "1.0", ClampMax = "200.0"))
	float WakeVelocityThreshold = 20.0f;

	/**
	 * Enable stack pressure for attached particles
	 * When enabled, particles stacked on top transfer their weight to particles below,
	 * causing lower particles to slide down faster - creating realistic dripping behavior
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability")
	bool bEnableStackPressure = true;

	/**
	 * Stack pressure strength multiplier
	 * Higher values = faster dripping, more separation between particles
	 * Recommended: 50 ~ 200 (due to SPH kernel normalization)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability",
		meta = (EditCondition = "bEnableStackPressure", ClampMin = "0.0", ClampMax = "1000.0"))
	float StackPressureScale = 100.0f;

	/**
	 * Stack pressure neighbor search radius (cm)
	 * 0 = use SmoothingRadius
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Stability",
		meta = (EditCondition = "bEnableStackPressure", ClampMin = "0.0"))
	float StackPressureRadius = 0.0f;

	//========================================
	// Physics | Simulation | Surface Tension (Position-Based, NVIDIA Flex style)
	//========================================

	/**
	 * Enable Position-Based Surface Tension (NVIDIA Flex style)
	 * When enabled: Uses position-based constraint for surface tension (experimental)
	 * When disabled: Uses traditional Akinci force-based surface tension (default, stable)
	 * Position-Based creates smoother droplets but may need parameter tuning.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|SurfaceTension")
	bool bEnablePositionBasedSurfaceTension = false;

	/**
	 * Surface Tension activation distance as ratio of SmoothingRadius
	 * Surface tension is applied when particle distance exceeds this.
	 * Lower values = tighter surface (more spherical droplets)
	 * Typical: 0.3 ~ 0.5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|SurfaceTension",
		meta = (ClampMin = "0.1", ClampMax = "0.9", EditCondition = "bEnablePositionBasedSurfaceTension"))
	float SurfaceTensionActivationRatio = 0.4f;

	/**
	 * Surface Tension falloff distance as ratio of SmoothingRadius
	 * Beyond this, strength decreases linearly to zero at SmoothingRadius.
	 * Must be > SurfaceTensionActivationRatio
	 * Typical: 0.6 ~ 0.9
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|SurfaceTension",
		meta = (ClampMin = "0.2", ClampMax = "1.0", EditCondition = "bEnablePositionBasedSurfaceTension"))
	float SurfaceTensionFalloffRatio = 0.7f;

	/**
	 * Surface particles neighbor threshold
	 * Particles with fewer neighbors get stronger surface tension (actual surface particles).
	 * This creates proper droplet formation at fluid boundaries.
	 * 0 = uniform strength for all particles
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|SurfaceTension",
		meta = (ClampMin = "0", ClampMax = "30", EditCondition = "bEnablePositionBasedSurfaceTension"))
	int32 SurfaceTensionSurfaceThreshold = 15;

	/**
	 * Maximum correction per iteration (cm)
	 * Limits position change from surface tension/cohesion per solver iteration.
	 * Prevents instability from large corrections.
	 * 0 = no limit (not recommended)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|SurfaceTension",
		meta = (ClampMin = "0.0", ClampMax = "50.0", EditCondition = "bEnablePositionBasedSurfaceTension"))
	float MaxSurfaceTensionCorrectionPerIteration = 5.0f;

	//========================================
	// Physics | Simulation | Cohesion (Position-Based, NVIDIA Flex style)
	//========================================

	/**
	 * Cohesion activation distance as ratio of SmoothingRadius
	 * Cohesion pulls particles together when distance exceeds this.
	 * Works with density constraint (which pushes apart) to maintain rest distance.
	 * Lower values = particles start attracting closer to rest distance
	 * Typical: 0.4 ~ 0.6 (similar to SurfaceTensionActivationRatio)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Cohesion",
		meta = (ClampMin = "0.1", ClampMax = "0.9"))
	float CohesionActivationRatio = 0.5f;

	/**
	 * Cohesion falloff distance as ratio of SmoothingRadius
	 * Beyond this, cohesion strength decreases linearly to zero at SmoothingRadius.
	 * Must be > CohesionActivationRatio
	 * Typical: 0.7 ~ 0.9
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Cohesion",
		meta = (ClampMin = "0.2", ClampMax = "1.0"))
	float CohesionFalloffRatio = 0.8f;

	/**
	 * Surface particles neighbor threshold for cohesion
	 * Particles with fewer neighbors (surface particles) get stronger cohesion.
	 * Creates gooey/stringy fluid behavior where streams stay connected.
	 * 0 = uniform cohesion strength for all particles
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics|Simulation|Cohesion",
		meta = (ClampMin = "0", ClampMax = "30"))
	int32 CohesionSurfaceThreshold = 15;

	//========================================
	// Utility Functions
	//========================================

	/** Recalculate derived parameters (SmoothingRadius, ParticleSpacing, ParticleMass) based on ParticleRadius and SpacingRatio */
	UFUNCTION(BlueprintCallable, Category = "Physics")
	void RecalculateDerivedParameters();

#if WITH_EDITORONLY_DATA
	/** Thumbnail rendering info (camera angle, distance, etc.) */
	UPROPERTY(VisibleAnywhere, Instanced, Category = "Thumbnail")
	TObjectPtr<class UThumbnailInfo> ThumbnailInfo;
#endif

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Delegate broadcast when preset properties change in editor */
	FOnPresetPropertyChanged OnPropertyChanged;
#endif
};
