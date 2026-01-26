// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/FluidParticle.h"

/**
 * @brief Viscosity solver.
 *
 * XSPH-based viscosity implementation.
 * Represents viscosity effects by averaging particle velocities with their neighbors.
 *
 * High viscosity coefficient = viscous fluids like honey, slime
 * Low viscosity coefficient = flowing fluids like water
 */
class KAWAIIFLUIDRUNTIME_API FViscositySolver
{
public:
	FViscositySolver();

	/**
	 * @brief Apply XSPH viscosity.
	 *
	 * v_i = v_i + c * Σ(v_j - v_i) * W(r_ij, h)
	 *
	 * @param Particles Particle array
	 * @param ViscosityCoeff Viscosity coefficient (0.0 ~ 1.0)
	 * @param SmoothingRadius Kernel radius
	 */
	void ApplyXSPH(TArray<FFluidParticle>& Particles, float ViscosityCoeff, float SmoothingRadius);

	/**
	 * @brief Apply viscoelastic springs (optional - for slime).
	 * Maintains spring connections between particles for stretch-and-return effects.
	 *
	 * @param Particles Particle array
	 * @param SpringStiffness Spring stiffness
	 * @param DeltaTime Time step
	 */
	void ApplyViscoelasticSprings(TArray<FFluidParticle>& Particles, float SpringStiffness, float DeltaTime);

private:
	/** Viscoelastic spring connection */
	struct FSpringConnection
	{
		int32 ParticleA;
		int32 ParticleB;
		float RestLength;

		FSpringConnection() : ParticleA(-1), ParticleB(-1), RestLength(0.0f) {}
		FSpringConnection(int32 A, int32 B, float Length) : ParticleA(A), ParticleB(B), RestLength(Length) {}
	};

	/** Spring connection list */
	TArray<FSpringConnection> Springs;

	/** Spring creation distance threshold */
	float SpringThreshold;

public:
	/** Update spring connections (neighbor-based) */
	void UpdateSprings(const TArray<FFluidParticle>& Particles, float SmoothingRadius);

	/** Remove all springs */
	void ClearSprings();
};
