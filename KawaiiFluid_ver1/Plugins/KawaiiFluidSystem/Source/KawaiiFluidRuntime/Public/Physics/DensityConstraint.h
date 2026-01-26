// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/FluidParticle.h"

/**
 * Tensile Instability Correction Parameters (PBF Eq.13-14)
 * s_corr = -k * (W(r)/W(Δq))^n
 * Prevents particle clustering at surface/splash regions
 */
struct FTensileInstabilityParams
{
	bool bEnabled = false;     // Enable scorr
	float K = 0.1f;            // Strength coefficient (default 0.1)
	int32 N = 4;               // Exponent (default 4)
	float DeltaQ = 0.2f;       // Reference distance ratio (Δq/h)
	float W_DeltaQ = 0.0f;     // Precomputed W(Δq, h) for efficiency
};

/**
 * SPH kernel coefficients (precomputed)
 * Precomputed values to eliminate Pow() calls
 */
struct FSPHKernelCoeffs
{
	float h;              // Kernel radius (m)
	float h2;             // h²
	float Poly6Coeff;     // 315 / (64πh⁹)
	float SpikyCoeff;     // -45 / (πh⁶)
	float InvRestDensity; // 1 / ρ₀
	float SmoothingRadiusSq; // h² (cm²)

	// Tensile Instability Correction (PBF Section 4)
	FTensileInstabilityParams TensileParams;
};

/**
 * @brief PBF density constraint solver.
 *
 * Constraint: C_i = (ρ_i / ρ_0) - 1 = 0
 * Corrects particle positions to maintain rest density (ρ_0)
 */
class KAWAIIFLUIDRUNTIME_API FDensityConstraint
{
public:
	FDensityConstraint();
	FDensityConstraint(float InRestDensity, float InSmoothingRadius, float InEpsilon);

	/** Solve density constraint (single iteration) - XPBD */
	void Solve(TArray<FFluidParticle>& Particles, float InSmoothingRadius, float InRestDensity, float InCompliance, float DeltaTime);

	/** Solve density constraint (with Tensile Instability correction) - XPBD + scorr */
	void SolveWithTensileCorrection(
		TArray<FFluidParticle>& Particles,
		float InSmoothingRadius,
		float InRestDensity,
		float InCompliance,
		float DeltaTime,
		const FTensileInstabilityParams& TensileParams);

	/** Set parameters */
	void SetRestDensity(float NewRestDensity);
	void SetEpsilon(float NewEpsilon);

private:
	float RestDensity;      // Rest density (kg/m³)
	float Epsilon;          // Stability constant
	float SmoothingRadius;  // Kernel radius (cm)

	//========================================
	// SoA Cache (Structure of Arrays)
	//========================================
	TArray<float> PosX, PosY, PosZ;  // Predicted positions
	TArray<float> Masses;             // Masses
	TArray<float> Densities;          // Densities
	TArray<float> Lambdas;            // Lambda values
	TArray<float> DeltaPX, DeltaPY, DeltaPZ;  // Position corrections

	//========================================
	// SoA Management Functions
	//========================================
	void ResizeSoAArrays(int32 NumParticles);
	void CopyToSoA(const TArray<FFluidParticle>& Particles);
	void ApplyFromSoA(TArray<FFluidParticle>& Particles);

	//========================================
	// SIMD Optimized Functions (used internally by Solve)
	//========================================

	/** Step 1: Compute density + Lambda simultaneously (SIMD) */
	void ComputeDensityAndLambda_SIMD(
		const TArray<FFluidParticle>& Particles,
		const FSPHKernelCoeffs& Coeffs);

	/** Step 2: Compute position corrections (SIMD) */
	void ComputeDeltaP_SIMD(
		const TArray<FFluidParticle>& Particles,
		const FSPHKernelCoeffs& Coeffs);

	//========================================
	// Legacy Functions (backward compatibility)
	//========================================
	void ComputeDensities(TArray<FFluidParticle>& Particles);
	void ComputeLambdas(TArray<FFluidParticle>& Particles);
	void ApplyPositionCorrection(TArray<FFluidParticle>& Particles);
	float ComputeParticleDensity(const FFluidParticle& Particle, const TArray<FFluidParticle>& Particles);
	float ComputeParticleLambda(const FFluidParticle& Particle, const TArray<FFluidParticle>& Particles);
	FVector ComputeDeltaPosition(int32 ParticleIndex, const TArray<FFluidParticle>& Particles);
};
