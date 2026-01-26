// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * @brief Collection of SPH kernel functions.
 *
 * Poly6: For density estimation (smooth decay)
 * Spiky: For gradient computation (strong repulsion at close range)
 * Viscosity: For viscosity computation
 * Adhesion: For adhesion forces
 * Cohesion: For cohesion forces (surface tension)
 */
namespace SPHKernels
{
	//========================================
	// Poly6 Kernel (density estimation)
	// W(r, h) = 315 / (64 * π * h^9) * (h² - r²)³
	//========================================

	/** Poly6 kernel coefficient */
	KAWAIIFLUIDRUNTIME_API float Poly6Coefficient(float h);

	/** Poly6 kernel value */
	KAWAIIFLUIDRUNTIME_API float Poly6(float r, float h);

	/** Poly6 kernel (vector input) */
	KAWAIIFLUIDRUNTIME_API float Poly6(const FVector& r, float h);

	//========================================
	// Spiky Kernel (pressure gradient)
	// ∇W(r, h) = -45 / (π * h^6) * (h - r)² * r̂
	//========================================

	/** Spiky kernel gradient coefficient */
	KAWAIIFLUIDRUNTIME_API float SpikyGradientCoefficient(float h);

	/** Spiky kernel gradient */
	KAWAIIFLUIDRUNTIME_API FVector SpikyGradient(const FVector& r, float h);

	//========================================
	// Viscosity Kernel Laplacian (viscosity)
	// ∇²W(r, h) = 45 / (π * h^6) * (h - r)
	//========================================

	/** Viscosity kernel Laplacian coefficient */
	KAWAIIFLUIDRUNTIME_API float ViscosityLaplacianCoefficient(float h);

	/** Viscosity kernel Laplacian */
	KAWAIIFLUIDRUNTIME_API float ViscosityLaplacian(float r, float h);

	//========================================
	// Adhesion Kernel (adhesion forces)
	// Akinci et al. 2013
	//========================================

	/** Adhesion kernel */
	KAWAIIFLUIDRUNTIME_API float Adhesion(float r, float h);

	//========================================
	// Cohesion Kernel (cohesion/surface tension)
	// Akinci et al. 2013
	//========================================

	/** Cohesion kernel */
	KAWAIIFLUIDRUNTIME_API float Cohesion(float r, float h);

	//========================================
	// Utilities
	//========================================

	/** Precompute kernel coefficients (for optimization) */
	struct FKernelCoefficients
	{
		float Poly6Coeff;
		float SpikyGradCoeff;
		float ViscosityLapCoeff;
		float h;
		float h2;  // h²
		float h6;  // h^6
		float h9;  // h^9

		FKernelCoefficients() : Poly6Coeff(0), SpikyGradCoeff(0), ViscosityLapCoeff(0), h(0), h2(0), h6(0), h9(0) {}
		void Precompute(float SmoothingRadius);
	};
}
