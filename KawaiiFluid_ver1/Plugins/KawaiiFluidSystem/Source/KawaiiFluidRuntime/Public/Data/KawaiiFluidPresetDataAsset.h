// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
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

	/** Particle mass */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "0.01"))
	float ParticleMass = 1.0f;

	/** Smoothing radius (kernel radius h, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "1.0"))
	float SmoothingRadius = 20.0f;

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
	float Compliance = 0.01f;

	/** XPBD constraint solver iterations (viscous fluid: 2-3, water: 4-6) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Physics", meta = (ClampMin = "1", ClampMax = "10"))
	int32 SolverIterations = 3;

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "1.0"))
	float AdhesionRadius = 25.0f;

	/** Detach threshold (force above this causes detachment) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Adhesion", meta = (ClampMin = "0.0"))
	float DetachThreshold = 500.0f;

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
	// Rendering Parameters
	//========================================

	/** Particle render radius (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering", meta = (ClampMin = "0.1", ClampMax = "50.0"))
	float ParticleRadius = 5.0f;

	/** Fluid color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering")
	FLinearColor Color = FLinearColor(0.2f, 0.5f, 1.0f, 0.8f);

	//========================================
	// Limits
	//========================================

	/** Maximum particle count */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Limits", meta = (ClampMin = "1"))
	int32 MaxParticles = 10000;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
