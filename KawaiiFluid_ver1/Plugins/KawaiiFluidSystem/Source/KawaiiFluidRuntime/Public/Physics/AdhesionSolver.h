// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/FluidParticle.h"

class UFluidCollider;

/**
 * @brief Adhesion solver.
 *
 * Based on Akinci et al. 2013 "Versatile Surface Tension and Adhesion for SPH Fluids"
 * Implements fluid particle adhesion to surfaces (characters, walls, etc.)
 */
class KAWAIIFLUIDRUNTIME_API FAdhesionSolver
{
public:
	FAdhesionSolver();

	/**
	 * @brief Apply adhesion forces.
	 *
	 * @param Particles Particle array
	 * @param Colliders Collider list (adhesion targets)
	 * @param AdhesionStrength Adhesion strength (0.0 ~ 1.0)
	 * @param AdhesionRadius Adhesion range
	 * @param DetachThreshold Detachment threshold
	 * @param ColliderContactOffset Contact distance offset (positive allows deeper penetration)
	 */
	void Apply(
		TArray<FFluidParticle>& Particles,
		const TArray<TObjectPtr<UFluidCollider>>& Colliders,
		float AdhesionStrength,
		float AdhesionRadius,
		float DetachThreshold,
		float ColliderContactOffset
	);

	/**
	 * @brief Apply inter-particle cohesion (surface tension).
	 *
	 * @param Particles Particle array
	 * @param CohesionStrength Cohesion strength
	 * @param SmoothingRadius Kernel radius
	 */
	void ApplyCohesion(
		TArray<FFluidParticle>& Particles,
		float CohesionStrength,
		float SmoothingRadius
	);

private:
	/**
	 * @brief Compute adhesion force with boundary surface.
	 */
	FVector ComputeAdhesionForce(
		const FVector& ParticlePos,
		const FVector& SurfacePoint,
		const FVector& SurfaceNormal,
		float Distance,
		float AdhesionStrength,
		float AdhesionRadius
	);

	/**
	 * @brief Update attachment state.
	 */
	void UpdateAttachmentState(
		FFluidParticle& Particle,
		AActor* Collider,
		float Force,
		float DetachThreshold,
		FName BoneName,
		const FTransform& BoneTransform,
		const FVector& ParticlePosition,
		const FVector& SurfaceNormal
	);
};
