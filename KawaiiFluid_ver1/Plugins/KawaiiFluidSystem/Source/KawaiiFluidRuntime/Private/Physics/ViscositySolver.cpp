// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Physics/ViscositySolver.h"
#include "Physics/SPHKernels.h"
#include "Async/ParallelFor.h"

namespace ViscosityConstants
{
	constexpr float CM_TO_M = 0.01f;
	constexpr float CM_TO_M_SQ = CM_TO_M * CM_TO_M;
}

FViscositySolver::FViscositySolver()
	: SpringThreshold(0.8f)
{
}

void FViscositySolver::ApplyXSPH(TArray<FFluidParticle>& Particles, float ViscosityCoeff, float SmoothingRadius)
{
	if (ViscosityCoeff <= 0.0f)
	{
		return;
	}

	const int32 ParticleCount = Particles.Num();
	if (ParticleCount == 0)
	{
		return;
	}

	// [Optimization 1] Cache kernel coefficients - compute once per frame
	SPHKernels::FKernelCoefficients KernelCoeffs;
	KernelCoeffs.Precompute(SmoothingRadius);

	// Radius squared (to avoid sqrt calls)
	const float RadiusSquared = SmoothingRadius * SmoothingRadius;

	// Store new velocities in temporary array
	TArray<FVector> NewVelocities;
	NewVelocities.SetNum(ParticleCount);

	// [Optimization 4] Load balancing - use Unbalanced flag to handle varying neighbor counts
	ParallelFor(ParticleCount, [&](int32 i)
	{
		const FFluidParticle& Particle = Particles[i];
		FVector VelocityCorrection = FVector::ZeroVector;
		float WeightSum = 0.0f;

		for (int32 NeighborIdx : Particle.NeighborIndices)
		{
			if (NeighborIdx == i)
			{
				continue;
			}

			const FFluidParticle& Neighbor = Particles[NeighborIdx];
			const FVector r = Particle.Position - Neighbor.Position;

			// [Optimization 2] Radius-based filtering - early skip if r² > h² (avoid sqrt)
			const float rSquared = r.SizeSquared();
			if (rSquared > RadiusSquared)
			{
				continue;
			}

			// [Optimization 1] Directly compute Poly6 with cached coefficients
			// W(r, h) = Poly6Coeff * (h² - r²)³
			// Unit conversion: cm -> m (coefficients already computed in m units)
			const float h2_m = KernelCoeffs.h2;
			const float r2_m = rSquared * ViscosityConstants::CM_TO_M_SQ;
			const float diff = h2_m - r2_m;
			const float Weight = (diff > 0.0f) ? KernelCoeffs.Poly6Coeff * diff * diff * diff : 0.0f;

			// Velocity difference
			const FVector VelocityDiff = Neighbor.Velocity - Particle.Velocity;

			VelocityCorrection += VelocityDiff * Weight;
			WeightSum += Weight;
		}

		// Normalization (optional)
		if (WeightSum > 0.0f)
		{
			VelocityCorrection /= WeightSum;
		}

		// Apply XSPH viscosity: v_new = v + c * Σ(v_j - v_i) * W
		NewVelocities[i] = Particle.Velocity + ViscosityCoeff * VelocityCorrection;

	}, EParallelForFlags::Unbalanced);

	// [Optimization 3] Simplify velocity application loop - use simple for loop instead of ParallelFor
	// Simple copy operations have more scheduler overhead than benefit
	for (int32 i = 0; i < ParticleCount; ++i)
	{
		Particles[i].Velocity = NewVelocities[i];
	}
}

void FViscositySolver::ApplyViscoelasticSprings(TArray<FFluidParticle>& Particles, float SpringStiffness, float DeltaTime)
{
	if (SpringStiffness <= 0.0f || Springs.Num() == 0)
	{
		return;
	}

	for (const FSpringConnection& Spring : Springs)
	{
		if (!Particles.IsValidIndex(Spring.ParticleA) || !Particles.IsValidIndex(Spring.ParticleB))
		{
			continue;
		}

		FFluidParticle& ParticleA = Particles[Spring.ParticleA];
		FFluidParticle& ParticleB = Particles[Spring.ParticleB];

		FVector Delta = ParticleA.Position - ParticleB.Position;
		float CurrentLength = Delta.Size();

		if (CurrentLength < KINDA_SMALL_NUMBER)
		{
			continue;
		}

		// Displacement
		float Displacement = CurrentLength - Spring.RestLength;

		// Spring force: F = -k * x
		FVector Force = SpringStiffness * Displacement * (Delta / CurrentLength);

		// Apply force to velocity (divide by mass)
		ParticleA.Velocity -= Force * DeltaTime / ParticleA.Mass;
		ParticleB.Velocity += Force * DeltaTime / ParticleB.Mass;
	}
}

void FViscositySolver::UpdateSprings(const TArray<FFluidParticle>& Particles, float SmoothingRadius)
{
	// Keep only valid springs
	Springs.RemoveAll([&](const FSpringConnection& Spring)
	{
		if (!Particles.IsValidIndex(Spring.ParticleA) || !Particles.IsValidIndex(Spring.ParticleB))
		{
			return true;
		}

		float Distance = FVector::Dist(
			Particles[Spring.ParticleA].Position,
			Particles[Spring.ParticleB].Position
		);

		// Break spring if too far apart
		return Distance > SmoothingRadius * 2.0f;
	});

	// Add new springs (between close neighbors)
	TSet<uint64> ExistingPairs;
	for (const FSpringConnection& Spring : Springs)
	{
		int32 MinIdx = FMath::Min(Spring.ParticleA, Spring.ParticleB);
		int32 MaxIdx = FMath::Max(Spring.ParticleA, Spring.ParticleB);
		ExistingPairs.Add((uint64)MinIdx << 32 | (uint64)MaxIdx);
	}

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		const FFluidParticle& Particle = Particles[i];

		for (int32 NeighborIdx : Particle.NeighborIndices)
		{
			if (NeighborIdx <= i)
			{
				continue;
			}

			const FFluidParticle& Neighbor = Particles[NeighborIdx];
			float Distance = FVector::Dist(Particle.Position, Neighbor.Position);

			// Spring creation condition
			if (Distance < SmoothingRadius * SpringThreshold)
			{
				int32 MinIdx = FMath::Min(i, NeighborIdx);
				int32 MaxIdx = FMath::Max(i, NeighborIdx);
				uint64 PairKey = (uint64)MinIdx << 32 | (uint64)MaxIdx;

				if (!ExistingPairs.Contains(PairKey))
				{
					Springs.Add(FSpringConnection(i, NeighborIdx, Distance));
					ExistingPairs.Add(PairKey);
				}
			}
		}
	}
}

void FViscositySolver::ClearSprings()
{
	Springs.Empty();
}
