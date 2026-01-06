// Copyright KawaiiFluid Team. All Rights Reserved.
// Fluid Simulation Test Metrics Collection
// Used for automated testing and validation of PBF/XPBD implementation

#pragma once

#include "CoreMinimal.h"
#include "FluidTestMetrics.generated.h"

/**
 * FFluidTestMetrics
 *
 * Collects simulation metrics for automated testing and validation.
 * Based on Position Based Fluids (Macklin & Müller, 2013) expected behaviors.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFluidTestMetrics
{
	GENERATED_BODY()

	//=========================================================================
	// Density Metrics (PBF Core Validation)
	//=========================================================================

	/** Average density of all particles (kg/m³) - Should be near RestDensity */
	UPROPERTY(BlueprintReadOnly, Category = "Density")
	float AverageDensity = 0.0f;

	/** Maximum density among all particles (kg/m³) - Should not exceed 200% of RestDensity */
	UPROPERTY(BlueprintReadOnly, Category = "Density")
	float MaxDensity = 0.0f;

	/** Minimum density among all particles (kg/m³) - Low values indicate neighbor deficiency */
	UPROPERTY(BlueprintReadOnly, Category = "Density")
	float MinDensity = 0.0f;

	/** Standard deviation of density (kg/m³) - Lower is more uniform */
	UPROPERTY(BlueprintReadOnly, Category = "Density")
	float DensityStdDev = 0.0f;

	/** Variance of density (kg²/m⁶) */
	UPROPERTY(BlueprintReadOnly, Category = "Density")
	float DensityVariance = 0.0f;

	/** Density relative to RestDensity (1.0 = exactly at rest density) */
	UPROPERTY(BlueprintReadOnly, Category = "Density")
	float DensityRatio = 0.0f;

	//=========================================================================
	// Volume & Mass Conservation Metrics
	//=========================================================================

	/** Center of mass position (cm) */
	UPROPERTY(BlueprintReadOnly, Category = "Conservation")
	FVector CenterOfMass = FVector::ZeroVector;

	/** Estimated total volume based on particle count and rest density (cm³) */
	UPROPERTY(BlueprintReadOnly, Category = "Conservation")
	float TotalVolume = 0.0f;

	/** Axis-Aligned Bounding Box of all particles */
	UPROPERTY(BlueprintReadOnly, Category = "Conservation")
	FBox ParticleBounds = FBox(EForceInit::ForceInit);

	/** Total mass of simulation (kg) */
	UPROPERTY(BlueprintReadOnly, Category = "Conservation")
	float TotalMass = 0.0f;

	//=========================================================================
	// Stability Metrics
	//=========================================================================

	/** Number of particles that escaped simulation bounds */
	UPROPERTY(BlueprintReadOnly, Category = "Stability")
	int32 ParticlesOutOfBounds = 0;

	/** Number of particles with NaN or Infinite position/velocity */
	UPROPERTY(BlueprintReadOnly, Category = "Stability")
	int32 InvalidParticles = 0;

	/** Maximum velocity magnitude (cm/s) - High values may indicate instability */
	UPROPERTY(BlueprintReadOnly, Category = "Stability")
	float MaxVelocity = 0.0f;

	/** Average velocity magnitude (cm/s) */
	UPROPERTY(BlueprintReadOnly, Category = "Stability")
	float AverageVelocity = 0.0f;

	/** Maximum acceleration experienced (cm/s²) */
	UPROPERTY(BlueprintReadOnly, Category = "Stability")
	float MaxAcceleration = 0.0f;

	//=========================================================================
	// XPBD Solver Metrics
	//=========================================================================

	/** Average Lambda value across all particles */
	UPROPERTY(BlueprintReadOnly, Category = "Solver")
	float AverageLambda = 0.0f;

	/** Maximum absolute Lambda value */
	UPROPERTY(BlueprintReadOnly, Category = "Solver")
	float MaxLambda = 0.0f;

	/** Average constraint error |C_i| after solving */
	UPROPERTY(BlueprintReadOnly, Category = "Solver")
	float AverageConstraintError = 0.0f;

	/** Maximum constraint error after solving */
	UPROPERTY(BlueprintReadOnly, Category = "Solver")
	float MaxConstraintError = 0.0f;

	/** Number of solver iterations performed */
	UPROPERTY(BlueprintReadOnly, Category = "Solver")
	int32 SolverIterations = 0;

	//=========================================================================
	// Neighbor Statistics
	//=========================================================================

	/** Average number of neighbors per particle */
	UPROPERTY(BlueprintReadOnly, Category = "Neighbors")
	float AverageNeighborCount = 0.0f;

	/** Maximum neighbor count */
	UPROPERTY(BlueprintReadOnly, Category = "Neighbors")
	int32 MaxNeighborCount = 0;

	/** Minimum neighbor count (excluding isolated particles) */
	UPROPERTY(BlueprintReadOnly, Category = "Neighbors")
	int32 MinNeighborCount = 0;

	/** Number of isolated particles (0 neighbors excluding self) */
	UPROPERTY(BlueprintReadOnly, Category = "Neighbors")
	int32 IsolatedParticleCount = 0;

	//=========================================================================
	// Performance Metrics
	//=========================================================================

	/** Total simulation time for this frame (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "Performance")
	float SimulationTimeMs = 0.0f;

	/** Time spent on neighbor search (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "Performance")
	float NeighborSearchTimeMs = 0.0f;

	/** Time spent on density constraint solving (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "Performance")
	float DensitySolveTimeMs = 0.0f;

	/** Total particle count */
	UPROPERTY(BlueprintReadOnly, Category = "Performance")
	int32 ParticleCount = 0;

	/** Current frame number */
	UPROPERTY(BlueprintReadOnly, Category = "Performance")
	int32 FrameNumber = 0;

	/** Simulation time elapsed (s) */
	UPROPERTY(BlueprintReadOnly, Category = "Performance")
	float SimulationElapsedTime = 0.0f;

	//=========================================================================
	// Helper Methods
	//=========================================================================

	/** Reset all metrics to default values */
	void Reset()
	{
		*this = FFluidTestMetrics();
	}

	/** Check if density is within acceptable range of RestDensity */
	bool IsDensityStable(float RestDensity, float TolerancePercent = 10.0f) const
	{
		const float LowerBound = RestDensity * (1.0f - TolerancePercent / 100.0f);
		const float UpperBound = RestDensity * (1.0f + TolerancePercent / 100.0f);
		return AverageDensity >= LowerBound && AverageDensity <= UpperBound;
	}

	/** Check if simulation is numerically stable */
	bool IsNumericallyStable() const
	{
		return InvalidParticles == 0 &&
		       FMath::IsFinite(MaxVelocity) &&
		       MaxVelocity < 100000.0f;  // 1000 m/s threshold
	}

	/** Check if volume is conserved within tolerance */
	bool IsVolumeConserved(float InitialVolume, float TolerancePercent = 20.0f) const
	{
		if (InitialVolume <= 0.0f) return false;
		const float Ratio = TotalVolume / InitialVolume;
		return Ratio >= (1.0f - TolerancePercent / 100.0f) &&
		       Ratio <= (1.0f + TolerancePercent / 100.0f);
	}

	/** Get a summary string for logging */
	FString GetSummary() const
	{
		return FString::Printf(
			TEXT("Particles: %d | Density: %.1f (±%.1f) | MaxVel: %.1f cm/s | Lambda: %.4f | Time: %.2fms"),
			ParticleCount,
			AverageDensity,
			DensityStdDev,
			MaxVelocity,
			AverageLambda,
			SimulationTimeMs
		);
	}
};

/**
 * FFluidTestMetricsHistory
 *
 * Stores time-series of metrics for trend analysis.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FFluidTestMetricsHistory
{
	GENERATED_BODY()

	/** Maximum number of samples to store */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "History")
	int32 MaxSamples = 300;  // 5 seconds at 60 FPS

	/** Recorded metrics samples */
	UPROPERTY(BlueprintReadOnly, Category = "History")
	TArray<FFluidTestMetrics> Samples;

	/** Add a new sample, removing oldest if at capacity */
	void AddSample(const FFluidTestMetrics& Metrics)
	{
		if (Samples.Num() >= MaxSamples)
		{
			Samples.RemoveAt(0);
		}
		Samples.Add(Metrics);
	}

	/** Get average density over all samples */
	float GetAverageDensityOverTime() const
	{
		if (Samples.Num() == 0) return 0.0f;
		float Sum = 0.0f;
		for (const auto& S : Samples) Sum += S.AverageDensity;
		return Sum / Samples.Num();
	}

	/** Get maximum velocity ever recorded */
	float GetMaxVelocityEver() const
	{
		float Max = 0.0f;
		for (const auto& S : Samples)
		{
			Max = FMath::Max(Max, S.MaxVelocity);
		}
		return Max;
	}

	/** Check if density has stabilized (low variance in recent samples) */
	bool HasDensityStabilized(int32 RecentSampleCount = 60, float VarianceThreshold = 10.0f) const
	{
		if (Samples.Num() < RecentSampleCount) return false;

		float Sum = 0.0f;
		const int32 StartIdx = Samples.Num() - RecentSampleCount;
		for (int32 i = StartIdx; i < Samples.Num(); ++i)
		{
			Sum += Samples[i].AverageDensity;
		}
		const float Mean = Sum / RecentSampleCount;

		float VarianceSum = 0.0f;
		for (int32 i = StartIdx; i < Samples.Num(); ++i)
		{
			const float Diff = Samples[i].AverageDensity - Mean;
			VarianceSum += Diff * Diff;
		}
		const float Variance = VarianceSum / RecentSampleCount;

		return Variance < VarianceThreshold;
	}

	/** Clear all samples */
	void Clear()
	{
		Samples.Empty();
	}
};
