// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Rendering/FluidRenderingParameters.h"
#include "KawaiiFluidPresetDataAsset.generated.h"

class UKawaiiFluidSimulationContext;

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
	// Context Class (Extensibility)
	//========================================

	/**
	 * Custom simulation context class
	 * Override for custom fluid behaviors (lava, sand, etc.)
	 * If nullptr, uses default UKawaiiFluidSimulationContext
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Extension")
	TSubclassOf<UKawaiiFluidSimulationContext> ContextClass;

	//========================================
	// Physics Parameters
	//========================================

	/** Rest density (kg/m³) - Slime: 1200, Water: 1000, Honey: 1400 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.1"))
	float RestDensity = 1200.0f;

	/** Smoothing radius (kernel radius h, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "1.0"))
	float SmoothingRadius = 20.0f;

	//========================================
	// Particle Size Parameters (Auto-calculated)
	//========================================

	/**
	 * Spacing ratio relative to SmoothingRadius (d/h)
	 * Recommended: 0.5 (yields ~33 neighbors in 3D)
	 * Range: 0.3 (dense, ~124 neighbors) to 0.7 (sparse, ~15 neighbors)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Particle Size",
		meta = (ClampMin = "0.1", ClampMax = "0.7"))
	float SpacingRatio = 0.5f;

	/**
	 * Particle spacing (cm) - distance between particles when spawned
	 * Auto-calculated: SmoothingRadius * SpacingRatio
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Particle Size")
	float ParticleSpacing = 10.0f;

	/**
	 * Particle mass (kg)
	 * Auto-calculated: RestDensity * (Spacing_m)³
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Particle Size")
	float ParticleMass = 1.0f;

	/**
	 * Particle render radius (cm)
	 * Auto-calculated: ParticleSpacing * 0.5
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Particle Size")
	float ParticleRadius = 5.0f;

	/** Estimated neighbor count based on spacing ratio */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fluid|Particle Size")
	int32 EstimatedNeighborCount = 33;

	/** Substep target dt (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.001", ClampMax = "0.05"))
	float SubstepDeltaTime = 1.0f / 120.0f;

	/** Maximum substep count */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxSubsteps = 8;

	/** Gravity */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics")
	FVector Gravity = FVector(0.0f, 0.0f, -980.0f);

	/** XPBD Compliance - smaller = more incompressible (Slime: 0.01, Water: 0.0001) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.0"))
	float Compliance = 0.00001f;

	/**
	 * Compliance scaling exponent for SmoothingRadius independence
	 * When SmoothingRadius differs from 20cm, Compliance is auto-scaled:
	 * ScaledCompliance = Compliance * (20/h)^Exponent
	 * Higher values = stronger scaling for stability at smaller radii
	 * Recommended: 4-6 (theoretical: 10, practical: 4-6)
	 * 0 = disable auto-scaling
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float ComplianceScalingExponent = 4.0f;

	/** XPBD constraint solver iterations (viscous fluid: 2-3, water: 4-6) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "1", ClampMax = "10"))
	int32 SolverIterations = 3;

	/**
	 * Global velocity damping per substep
	 * Applied after FinalizePositions to dissipate energy
	 * 1.0 = no damping, 0.99 = 1% damping per substep, 0.95 = 5% damping
	 * Useful for stabilizing simulation and preventing perpetual motion
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.9", ClampMax = "1.0"))
	float GlobalDamping = 0.995f;

	//========================================
	// Tensile Instability Correction (PBF Eq.13-14)
	//========================================

	/**
	 * Enable artificial pressure (scorr) for surface tension
	 * Prevents particle clustering at low-density regions (splash, surface)
	 * Based on PBF paper Section 4 (Macklin & Müller, 2013)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Tensile Instability")
	bool bEnableTensileInstabilityCorrection = true;

	/**
	 * Artificial pressure strength (k in paper)
	 * Higher values create stronger repulsion at surface
	 * Typical: 0.1 for water-like, 0.01 for viscous fluids
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Tensile Instability",
		meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bEnableTensileInstabilityCorrection"))
	float TensileInstabilityK = 0.1f;

	/**
	 * Artificial pressure exponent (n in paper)
	 * Higher values make the correction more localized
	 * Typical: 4
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Tensile Instability",
		meta = (ClampMin = "1", ClampMax = "8", EditCondition = "bEnableTensileInstabilityCorrection"))
	int32 TensileInstabilityN = 4;

	/**
	 * Reference distance as fraction of smoothing radius (Δq/h)
	 * scorr uses W(r)/W(Δq) ratio
	 * Typical: 0.2 (20% of h)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Tensile Instability",
		meta = (ClampMin = "0.01", ClampMax = "0.5", EditCondition = "bEnableTensileInstabilityCorrection"))
	float TensileInstabilityDeltaQ = 0.2f;

	//========================================
	// Viscosity Parameters
	//========================================

	/** XSPH viscosity coefficient (0=water, 0.5=slime, 0.8=honey) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Viscosity", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ViscosityCoefficient = 0.5f;

	//========================================
	// Surface Tension Parameters
	//========================================

	/** Cohesion strength - surface tension between particles (0=dispersed, 1=tight blob like water droplet) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Surface Tension", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CohesionStrength = 0.3f;

	//========================================
	// Adhesion Parameters
	//========================================

	/** Adhesion strength - stickiness to surfaces (characters, walls) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AdhesionStrength = 0.5f;

	/** Adhesion radius (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.1"))
	float AdhesionRadius = 25.0f;

	/**
	 * Additional contact offset (cm) applied to adhesion/collision checks.
	 * Positive values allow particles to overlap deeper into bone colliders.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "-50.0", ClampMax = "50.0"))
	float AdhesionContactOffset = 0.0f;

	/**
	 * Scale applied to bone velocity when updating attached particle velocity.
	 * 1 = inherit full bone motion, 0 = keep previous particle velocity.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AdhesionBoneVelocityScale = 1.0f;

	/** Detach distance threshold (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.0"))
	float AdhesionDetachDistance = 15.0f;

	/** Detach acceleration threshold (cm/s^2) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.0"))
	float AdhesionDetachAcceleration = 1000.0f;

	/** Detach threshold (force above this causes detachment) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.0"))
	float DetachThreshold = 500.0f;

	//========================================
	// Stack Pressure Parameters
	//========================================

	/**
	 * Enable stack pressure for attached particles
	 * When enabled, particles stacked on top transfer their weight to particles below,
	 * causing lower particles to slide down faster - creating realistic dripping behavior
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Stack Pressure")
	bool bEnableStackPressure = true;

	/**
	 * Stack pressure strength multiplier
	 * Higher values = faster dripping, more separation between particles
	 * Recommended: 50 ~ 200 (due to SPH kernel normalization)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Stack Pressure",
		meta = (EditCondition = "bEnableStackPressure", ClampMin = "0.0", ClampMax = "1000.0"))
	float StackPressureScale = 100.0f;

	/**
	 * Stack pressure neighbor search radius (cm)
	 * 0 = use SmoothingRadius
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Stack Pressure",
		meta = (EditCondition = "bEnableStackPressure", ClampMin = "0.0"))
	float StackPressureRadius = 0.0f;

	//========================================
	// Collision Parameters
	//========================================

	/** Restitution (bounciness) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Restitution = 0.0f;

	/** Friction */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Friction = 0.5f;

	/** Collision channel */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision")
	TEnumAsByte<ECollisionChannel> CollisionChannel = ECC_GameTraceChannel1;

	/**
	 * Primitive collision threshold (cm)
	 * 입자 중심이 콜라이더 표면에서 (ParticleRadius + 이 값) 이내면 충돌 처리됨
	 * 값이 클수록 더 일찍 충돌 감지, 작을수록 더 가깝게 접근해야 충돌
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Collision", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float PrimitiveCollisionThreshold = 1.0f;

	//========================================
	// Distance Field Collision (GPU)
	//========================================

	/**
	 * Enable Distance Field collision
	 * Uses UE5 Global Distance Field for GPU-based static mesh collision
	 * Requires "Generate Mesh Distance Fields" enabled in Project Settings
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Distance Field Collision")
	bool bUseDistanceFieldCollision = false;

	/** Distance threshold for collision detection (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Distance Field Collision", meta = (ClampMin = "0.1", ClampMax = "50.0", EditCondition = "bUseDistanceFieldCollision"))
	float DFCollisionThreshold = 1.0f;

	/** Distance Field collision restitution (bounciness) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Distance Field Collision", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bUseDistanceFieldCollision"))
	float DFCollisionRestitution = 0.3f;

	/** Distance Field collision friction */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Distance Field Collision", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bUseDistanceFieldCollision"))
	float DFCollisionFriction = 0.1f;

	//========================================
	// Limits
	//========================================

	/** Maximum particle count */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Limits", meta = (ClampMin = "1"))
	int32 MaxParticles = 10000;

	//========================================
	// Rendering Parameters
	//========================================

	/**
	 * Metaball rendering parameters for this fluid preset
	 * All KawaiiFluidComponents sharing this preset will use these settings
	 * Note: ISM settings remain per-Component (debug/preview purpose, no batching needed)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering")
	FFluidRenderingParameters RenderingParameters;

	/** Recalculate derived particle parameters (mass, spacing, radius) based on SmoothingRadius and RestDensity */
	UFUNCTION(BlueprintCallable, Category = "Fluid")
	void RecalculateDerivedParameters();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
