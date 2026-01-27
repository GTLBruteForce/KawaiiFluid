// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "HAL/IConsoleManager.h"

//=============================================================================
// Stat Group Declarations
//=============================================================================

DECLARE_STATS_GROUP(TEXT("KawaiiFluidSimulation"), STATGROUP_KawaiiFluidSimulation, STATCAT_Advanced);

// Performance timing stats
DECLARE_CYCLE_STAT_EXTERN(TEXT("Total Simulation"), STAT_FluidTotalSimulation, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Spatial Hash"), STAT_FluidSpatialHash, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Density Solve"), STAT_FluidDensitySolve, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Viscosity"), STAT_FluidViscosity, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Cohesion"), STAT_FluidCohesion, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Collision"), STAT_FluidCollision, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("GPU Simulation"), STAT_FluidGPUSimulation, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("GPU Readback"), STAT_FluidGPUReadback, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);

// Counter stats
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Particle Count"), STAT_FluidParticleCount, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Active Particles"), STAT_FluidActiveParticles, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Attached Particles"), STAT_FluidAttachedParticles, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Substep Count"), STAT_FluidSubstepCount, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);

// Float stats (displayed as integers with 2 decimal precision * 100)
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Avg Velocity (cm/s)"), STAT_FluidAvgVelocity, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Max Velocity (cm/s)"), STAT_FluidMaxVelocity, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Avg Density"), STAT_FluidAvgDensity, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Density Error %"), STAT_FluidDensityError, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Avg Neighbors"), STAT_FluidAvgNeighbors, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);

// Stability metrics (GPU detailed mode only)
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Density StdDev"), STAT_FluidDensityStdDev, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Velocity StdDev"), STAT_FluidVelocityStdDev, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Per-Particle Error %"), STAT_FluidPerParticleError, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Kinetic Energy"), STAT_FluidKineticEnergy, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Stability Score"), STAT_FluidStabilityScore, STATGROUP_KawaiiFluidSimulation, KAWAIIFLUIDRUNTIME_API);

//=============================================================================
// Fluid Simulation Statistics Collector
// Collects and aggregates statistics during simulation for debugging
//=============================================================================

/**
 * Statistics snapshot for a single frame
 * Used to compare CPU vs GPU simulation behavior
 */
struct KAWAIIFLUIDRUNTIME_API FKawaiiFluidSimulationStats
{
	//========================================
	// Particle Statistics
	//========================================

	int32 ParticleCount = 0;
	int32 ActiveParticleCount = 0;
	int32 AttachedParticleCount = 0;

	//========================================
	// Velocity Statistics (cm/s)
	//========================================

	float AvgVelocity = 0.0f;
	float MinVelocity = 0.0f;
	float MaxVelocity = 0.0f;

	//========================================
	// Density Statistics
	//========================================

	float AvgDensity = 0.0f;
	float MinDensity = 0.0f;
	float MaxDensity = 0.0f;
	float DensityError = 0.0f;  // Average |density - restDensity| / restDensity (%)
	float RestDensity = 1000.0f;  // Reference rest density

	//========================================
	// Stability Metrics (GPU Detailed Mode)
	//========================================

	float DensityStdDev = 0.0f;        // Standard deviation of density
	float VelocityStdDev = 0.0f;       // Standard deviation of velocity magnitude
	float PerParticleDensityError = 0.0f;  // Mean of |ρᵢ - ρ₀| / ρ₀ * 100 (%)
	float KineticEnergy = 0.0f;        // Average kinetic energy (0.5 * m * v²)
	float StabilityScore = 0.0f;       // 0-100 composite stability score (100 = perfectly stable)

	//========================================
	// Neighbor Statistics
	//========================================

	float AvgNeighborCount = 0.0f;
	int32 MinNeighborCount = 0;
	int32 MaxNeighborCount = 0;

	//========================================
	// Force Statistics (magnitude)
	//========================================

	float AvgPressureCorrection = 0.0f;  // Average position correction from density solve
	float AvgViscosityForce = 0.0f;
	float AvgCohesionForce = 0.0f;

	//========================================
	// Collision Statistics
	//========================================

	int32 BoundsCollisionCount = 0;
	int32 PrimitiveCollisionCount = 0;
	int32 GroundContactCount = 0;

	//========================================
	// Solver Statistics
	//========================================

	int32 SubstepCount = 0;
	int32 SolverIterations = 0;

	//========================================
	// Performance (milliseconds)
	//========================================

	double TotalSimulationTimeMs = 0.0;
	double SpatialHashTimeMs = 0.0;
	double DensitySolveTimeMs = 0.0;
	double ViscosityTimeMs = 0.0;
	double CohesionTimeMs = 0.0;
	double CollisionTimeMs = 0.0;
	double GPUSimulationTimeMs = 0.0;
	double GPUReadbackTimeMs = 0.0;

	//========================================
	// Simulation Mode
	//========================================

	bool bIsGPUSimulation = false;

	/** Reset all statistics to zero */
	void Reset()
	{
		*this = FKawaiiFluidSimulationStats();
	}

	/** Log statistics to output log */
	void LogStats(const FString& Label = TEXT("")) const;

	/** Get formatted string for display */
	FString ToString() const;

	/** Compare with another stats snapshot and return differences */
	FString CompareWith(const FKawaiiFluidSimulationStats& Other, const FString& OtherLabel = TEXT("Other")) const;
};

/**
 * Statistics collector that accumulates data during simulation
 * Call Begin() at start of frame, collect during simulation, call End() to finalize
 */
class KAWAIIFLUIDRUNTIME_API FKawaiiFluidSimulationStatsCollector
{
public:
	FKawaiiFluidSimulationStatsCollector() = default;

	/** Begin collecting stats for a new frame */
	void BeginFrame();

	/** End frame and finalize statistics */
	void EndFrame();

	/** Get the current frame's statistics */
	const FKawaiiFluidSimulationStats& GetStats() const { return CurrentStats; }

	/** Get previous frame's statistics for comparison */
	const FKawaiiFluidSimulationStats& GetPreviousStats() const { return PreviousStats; }

	/** Check if collection is enabled */
	bool IsEnabled() const { return bEnabled; }

	/** Enable/disable stat collection */
	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }

	/** Check if detailed GPU stats are enabled (requires readback) */
	bool IsDetailedGPUEnabled() const { return bDetailedGPU; }

	/** Enable/disable detailed GPU stats (causes GPU readback - expensive!) */
	void SetDetailedGPUEnabled(bool bInDetailedGPU) { bDetailedGPU = bInDetailedGPU; }

	/** Check if debug readback is requested (for debug visualization) */
	bool IsReadbackRequested() const { return bReadbackRequested; }

	/** Request debug readback (for debug draw/visualization) */
	void SetReadbackRequested(bool bInRequested) { bReadbackRequested = bInRequested; }

	/** Check if any readback is needed (detailed stats OR debug visualization) */
	bool IsAnyReadbackNeeded() const { return bDetailedGPU || bReadbackRequested; }

	//========================================
	// Particle Data Collection
	//========================================

	/** Set particle counts */
	void SetParticleCounts(int32 Total, int32 Active, int32 Attached);

	/** Add velocity sample */
	void AddVelocitySample(float VelocityMagnitude);

	/** Add density sample */
	void AddDensitySample(float Density);

	/** Add neighbor count sample */
	void AddNeighborCountSample(int32 NeighborCount);

	/** Set rest density for error calculation */
	void SetRestDensity(float InRestDensity) { CurrentStats.RestDensity = InRestDensity; }

	/**
	 * Calculate stability metrics from GPU readback data
	 * Call this after GPU readback is complete (in detailed mode)
	 * @param Densities - Array of particle densities
	 * @param Velocities - Array of particle velocity magnitudes
	 * @param Masses - Array of particle masses (can be nullptr if all masses are 1.0)
	 * @param Count - Number of particles
	 * @param RestDensity - Target rest density for error calculation
	 */
	void CalculateStabilityMetrics(
		const float* Densities,
		const float* Velocities,
		const float* Masses,
		int32 Count,
		float RestDensity);

	//========================================
	// Force Data Collection
	//========================================

	/** Add pressure correction sample */
	void AddPressureCorrectionSample(float CorrectionMagnitude);

	/** Add viscosity force sample */
	void AddViscosityForceSample(float ForceMagnitude);

	/** Add cohesion force sample */
	void AddCohesionForceSample(float ForceMagnitude);

	//========================================
	// Collision Data Collection
	//========================================

	/** Increment bounds collision count */
	void AddBoundsCollision() { CurrentStats.BoundsCollisionCount++; }

	/** Increment primitive collision count */
	void AddPrimitiveCollision() { CurrentStats.PrimitiveCollisionCount++; }

	/** Increment ground contact count */
	void AddGroundContact() { CurrentStats.GroundContactCount++; }

	//========================================
	// Solver Settings
	//========================================

	/** Set substep count */
	void SetSubstepCount(int32 Count) { CurrentStats.SubstepCount = Count; }

	/** Set solver iterations */
	void SetSolverIterations(int32 Iterations) { CurrentStats.SolverIterations = Iterations; }

	/** Set GPU simulation mode */
	void SetGPUSimulation(bool bGPU) { CurrentStats.bIsGPUSimulation = bGPU; }

	//========================================
	// Performance Timing
	//========================================

	void SetTotalSimulationTime(double Ms) { CurrentStats.TotalSimulationTimeMs = Ms; }
	void SetSpatialHashTime(double Ms) { CurrentStats.SpatialHashTimeMs = Ms; }
	void SetDensitySolveTime(double Ms) { CurrentStats.DensitySolveTimeMs = Ms; }
	void SetViscosityTime(double Ms) { CurrentStats.ViscosityTimeMs = Ms; }
	void SetCohesionTime(double Ms) { CurrentStats.CohesionTimeMs = Ms; }
	void SetCollisionTime(double Ms) { CurrentStats.CollisionTimeMs = Ms; }
	void SetGPUSimulationTime(double Ms) { CurrentStats.GPUSimulationTimeMs = Ms; }
	void SetGPUReadbackTime(double Ms) { CurrentStats.GPUReadbackTimeMs = Ms; }

	/** Update UE4 stat system with current values */
	void UpdateEngineStats() const;

private:
	FKawaiiFluidSimulationStats CurrentStats;
	FKawaiiFluidSimulationStats PreviousStats;

	// Accumulators for averaging
	double VelocitySum = 0.0;
	int32 VelocitySampleCount = 0;

	double DensitySum = 0.0;
	int32 DensitySampleCount = 0;

	double NeighborSum = 0.0;
	int32 NeighborSampleCount = 0;

	double PressureCorrectionSum = 0.0;
	int32 PressureCorrectionSampleCount = 0;

	double ViscosityForceSum = 0.0;
	int32 ViscosityForceSampleCount = 0;

	double CohesionForceSum = 0.0;
	int32 CohesionForceSampleCount = 0;

	bool bEnabled = false;
	bool bDetailedGPU = false;  // Enable detailed GPU stats (requires readback)
	bool bReadbackRequested = false;  // Debug draw/visualization requests readback
	bool bFrameActive = false;
};

/**
 * Global stat collector instance
 * Access via GetFluidStatsCollector()
 */
KAWAIIFLUIDRUNTIME_API FKawaiiFluidSimulationStatsCollector& GetFluidStatsCollector();

/**
 * Console command handler for fluid stats
 * Registered as "KawaiiFluid.Stats" command
 */
class KAWAIIFLUIDRUNTIME_API FKawaiiFluidStatsCommand
{
public:
	/** Register console commands */
	static void Register();

	/** Unregister console commands */
	static void Unregister();

	/** Handle stats command */
	static void HandleStatsCommand(const TArray<FString>& Args, UWorld* World);

private:
	static FAutoConsoleCommand StatsCommand;
};
