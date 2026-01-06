// Copyright KawaiiFluid Team. All Rights Reserved.
// Fluid Metrics Collector Utility
// Gathers simulation metrics for testing and validation

#pragma once

#include "CoreMinimal.h"
#include "Tests/FluidTestMetrics.h"
#include "Core/FluidParticle.h"

class UKawaiiFluidComponent;
class UKawaiiFluidSimulationModule;

/**
 * FFluidMetricsCollector
 *
 * Utility class for collecting simulation metrics from particle data.
 * Used for automated testing and runtime diagnostics.
 */
class KAWAIIFLUIDRUNTIME_API FFluidMetricsCollector
{
public:
	/**
	 * Collect metrics from an array of particles
	 *
	 * @param Particles       Array of fluid particles
	 * @param RestDensity     Rest density for ratio calculation (kg/m³)
	 * @param SimulationBounds Optional bounds to check for out-of-bounds particles
	 * @return Collected metrics
	 */
	static FFluidTestMetrics CollectFromParticles(
		const TArray<FFluidParticle>& Particles,
		float RestDensity,
		const FBox& SimulationBounds = FBox(FVector(-1e10f), FVector(1e10f)))
	{
		FFluidTestMetrics Metrics;
		const int32 NumParticles = Particles.Num();

		if (NumParticles == 0)
		{
			return Metrics;
		}

		Metrics.ParticleCount = NumParticles;

		// Initialize accumulators
		double DensitySum = 0.0;
		double DensitySqSum = 0.0;
		double LambdaSum = 0.0;
		double VelocitySum = 0.0;
		FVector COMSum = FVector::ZeroVector;
		float TotalMassSum = 0.0f;
		int64 NeighborCountSum = 0;

		Metrics.MinDensity = TNumericLimits<float>::Max();
		Metrics.MaxDensity = TNumericLimits<float>::Lowest();
		Metrics.MaxVelocity = 0.0f;
		Metrics.MaxLambda = 0.0f;
		Metrics.MinNeighborCount = TNumericLimits<int32>::Max();
		Metrics.MaxNeighborCount = 0;

		// Initialize bounds
		FVector BoundsMin = FVector(TNumericLimits<float>::Max());
		FVector BoundsMax = FVector(TNumericLimits<float>::Lowest());

		// Single pass collection
		for (int32 i = 0; i < NumParticles; ++i)
		{
			const FFluidParticle& P = Particles[i];

			// Density metrics
			DensitySum += P.Density;
			DensitySqSum += P.Density * P.Density;
			Metrics.MinDensity = FMath::Min(Metrics.MinDensity, P.Density);
			Metrics.MaxDensity = FMath::Max(Metrics.MaxDensity, P.Density);

			// Lambda metrics
			LambdaSum += P.Lambda;
			Metrics.MaxLambda = FMath::Max(Metrics.MaxLambda, FMath::Abs(P.Lambda));

			// Velocity metrics
			const float VelMag = P.Velocity.Size();
			VelocitySum += VelMag;
			Metrics.MaxVelocity = FMath::Max(Metrics.MaxVelocity, VelMag);

			// Center of mass
			COMSum += P.Position * P.Mass;
			TotalMassSum += P.Mass;

			// Bounds
			BoundsMin = BoundsMin.ComponentMin(P.Position);
			BoundsMax = BoundsMax.ComponentMax(P.Position);

			// Neighbor count
			const int32 NeighborCount = P.NeighborIndices.Num();
			NeighborCountSum += NeighborCount;
			Metrics.MaxNeighborCount = FMath::Max(Metrics.MaxNeighborCount, NeighborCount);

			// Isolated particles have 0 or 1 neighbor (self only)
			if (NeighborCount <= 1)
			{
				Metrics.IsolatedParticleCount++;
			}
			else
			{
				Metrics.MinNeighborCount = FMath::Min(Metrics.MinNeighborCount, NeighborCount);
			}

			// Stability checks
			if (!FMath::IsFinite(P.Position.X) || !FMath::IsFinite(P.Position.Y) || !FMath::IsFinite(P.Position.Z) ||
			    !FMath::IsFinite(P.Velocity.X) || !FMath::IsFinite(P.Velocity.Y) || !FMath::IsFinite(P.Velocity.Z))
			{
				Metrics.InvalidParticles++;
			}

			if (!SimulationBounds.IsInside(P.Position))
			{
				Metrics.ParticlesOutOfBounds++;
			}
		}

		// Calculate averages
		Metrics.AverageDensity = static_cast<float>(DensitySum / NumParticles);
		Metrics.AverageLambda = static_cast<float>(LambdaSum / NumParticles);
		Metrics.AverageVelocity = static_cast<float>(VelocitySum / NumParticles);
		Metrics.AverageNeighborCount = static_cast<float>(NeighborCountSum) / NumParticles;

		// Calculate variance and standard deviation
		const double MeanDensity = DensitySum / NumParticles;
		Metrics.DensityVariance = static_cast<float>((DensitySqSum / NumParticles) - (MeanDensity * MeanDensity));
		Metrics.DensityStdDev = FMath::Sqrt(FMath::Max(0.0f, Metrics.DensityVariance));

		// Density ratio
		if (RestDensity > 0.0f)
		{
			Metrics.DensityRatio = Metrics.AverageDensity / RestDensity;
		}

		// Center of mass
		if (TotalMassSum > 0.0f)
		{
			Metrics.CenterOfMass = COMSum / TotalMassSum;
		}

		// Total mass and volume
		Metrics.TotalMass = TotalMassSum;
		if (RestDensity > 0.0f)
		{
			// Volume in cm³ (mass in kg, density in kg/m³)
			// V = m/ρ, convert m³ to cm³ (*1e6)
			Metrics.TotalVolume = (TotalMassSum / RestDensity) * 1e6f;
		}

		// Particle bounds
		Metrics.ParticleBounds = FBox(BoundsMin, BoundsMax);

		// Fix min neighbor count if no non-isolated particles
		if (Metrics.MinNeighborCount == TNumericLimits<int32>::Max())
		{
			Metrics.MinNeighborCount = 0;
		}

		return Metrics;
	}

	/**
	 * Collect metrics from a fluid component
	 *
	 * @param Component The fluid component
	 * @return Collected metrics
	 */
	static FFluidTestMetrics CollectFromComponent(const UKawaiiFluidComponent* Component);

	/**
	 * Collect metrics from a simulation module
	 *
	 * @param Module The simulation module
	 * @return Collected metrics
	 */
	static FFluidTestMetrics CollectFromModule(const UKawaiiFluidSimulationModule* Module);

	/**
	 * Calculate constraint error for a single particle
	 * C_i = (ρ_i / ρ_0) - 1
	 *
	 * @param Density       Current density
	 * @param RestDensity   Target density
	 * @return Constraint value (0 = satisfied)
	 */
	static float CalculateConstraintError(float Density, float RestDensity)
	{
		if (RestDensity <= 0.0f) return 0.0f;
		return (Density / RestDensity) - 1.0f;
	}

	/**
	 * Calculate average constraint error across all particles
	 *
	 * @param Particles     Array of particles
	 * @param RestDensity   Target density
	 * @return Average |C_i| value
	 */
	static float CalculateAverageConstraintError(
		const TArray<FFluidParticle>& Particles,
		float RestDensity)
	{
		if (Particles.Num() == 0 || RestDensity <= 0.0f) return 0.0f;

		double ErrorSum = 0.0;
		for (const FFluidParticle& P : Particles)
		{
			ErrorSum += FMath::Abs(CalculateConstraintError(P.Density, RestDensity));
		}
		return static_cast<float>(ErrorSum / Particles.Num());
	}

	/**
	 * Calculate maximum constraint error
	 *
	 * @param Particles     Array of particles
	 * @param RestDensity   Target density
	 * @return Maximum |C_i| value
	 */
	static float CalculateMaxConstraintError(
		const TArray<FFluidParticle>& Particles,
		float RestDensity)
	{
		if (Particles.Num() == 0 || RestDensity <= 0.0f) return 0.0f;

		float MaxError = 0.0f;
		for (const FFluidParticle& P : Particles)
		{
			MaxError = FMath::Max(MaxError, FMath::Abs(CalculateConstraintError(P.Density, RestDensity)));
		}
		return MaxError;
	}

	/**
	 * Check if the simulation has reached equilibrium
	 *
	 * @param History       Metrics history
	 * @param VelocityThreshold Maximum average velocity to consider stable (cm/s)
	 * @param DensityVarianceThreshold Maximum density variance over time
	 * @param MinSamples    Minimum samples required for check
	 * @return True if simulation is in equilibrium
	 */
	static bool IsInEquilibrium(
		const FFluidTestMetricsHistory& History,
		float VelocityThreshold = 10.0f,
		float DensityVarianceThreshold = 5.0f,
		int32 MinSamples = 60)
	{
		if (History.Samples.Num() < MinSamples) return false;

		// Check recent samples for low velocity
		const int32 StartIdx = History.Samples.Num() - MinSamples;
		for (int32 i = StartIdx; i < History.Samples.Num(); ++i)
		{
			if (History.Samples[i].AverageVelocity > VelocityThreshold)
			{
				return false;
			}
		}

		return History.HasDensityStabilized(MinSamples, DensityVarianceThreshold);
	}
};
