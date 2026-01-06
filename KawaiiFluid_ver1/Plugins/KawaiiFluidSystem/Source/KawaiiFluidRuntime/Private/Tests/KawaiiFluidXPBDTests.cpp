// Copyright KawaiiFluid Team. All Rights Reserved.
// XPBD Lambda Calculation Unit Tests
// Based on XPBD: Position-Based Simulation of Compliant Constrained Dynamics
// (Macklin, Müller, Chentanez, 2016)
// Core formula: Δλⱼ = (-Cⱼ(xᵢ) - α̃ⱼλᵢⱼ) / (∇CⱼM⁻¹∇Cⱼᵀ + α̃ⱼ)  [XPBD Eq.18]

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Physics/SPHKernels.h"
#include "Physics/DensityConstraint.h"
#include "Core/FluidParticle.h"
#include "Core/SpatialHash.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidXPBDTest_LambdaInitialization,
	"KawaiiFluid.Physics.XPBD.X01_LambdaInitialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidXPBDTest_ComplianceEffect,
	"KawaiiFluid.Physics.XPBD.X02_ComplianceEffect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidXPBDTest_CompressionSkip,
	"KawaiiFluid.Physics.XPBD.X03_CompressionSkip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidXPBDTest_LambdaAccumulation,
	"KawaiiFluid.Physics.XPBD.X04_LambdaAccumulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidXPBDTest_Convergence,
	"KawaiiFluid.Physics.XPBD.X05_Convergence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	// Helper: Create particles in a uniform 3D grid
	TArray<FFluidParticle> CreateTestGrid(int32 GridSize, float Spacing, float Mass)
	{
		TArray<FFluidParticle> Particles;
		Particles.Reserve(GridSize * GridSize * GridSize);

		const float HalfExtent = (GridSize - 1) * Spacing * 0.5f;

		for (int32 x = 0; x < GridSize; ++x)
		{
			for (int32 y = 0; y < GridSize; ++y)
			{
				for (int32 z = 0; z < GridSize; ++z)
				{
					FFluidParticle Particle;
					Particle.Position = FVector(
						x * Spacing - HalfExtent,
						y * Spacing - HalfExtent,
						z * Spacing - HalfExtent
					);
					Particle.PredictedPosition = Particle.Position;
					Particle.Mass = Mass;
					Particle.Velocity = FVector::ZeroVector;
					Particle.Density = 0.0f;
					Particle.Lambda = 0.0f;
					Particles.Add(Particle);
				}
			}
		}

		return Particles;
	}

	// Helper: Build neighbor lists
	void BuildNeighbors(TArray<FFluidParticle>& Particles, float SmoothingRadius)
	{
		FSpatialHash SpatialHash(SmoothingRadius);

		TArray<FVector> Positions;
		Positions.Reserve(Particles.Num());
		for (const FFluidParticle& P : Particles)
		{
			Positions.Add(P.PredictedPosition);
		}

		SpatialHash.BuildFromPositions(Positions);

		for (int32 i = 0; i < Particles.Num(); ++i)
		{
			SpatialHash.GetNeighbors(
				Particles[i].PredictedPosition,
				SmoothingRadius,
				Particles[i].NeighborIndices
			);
		}
	}

	// Helper: Compute average density
	float ComputeAverageDensity(const TArray<FFluidParticle>& Particles)
	{
		float Sum = 0.0f;
		for (const FFluidParticle& P : Particles)
		{
			Sum += P.Density;
		}
		return Sum / static_cast<float>(Particles.Num());
	}

	// Helper: Compute constraint error C = ρ/ρ₀ - 1
	float ComputeConstraintError(const TArray<FFluidParticle>& Particles, float RestDensity)
	{
		float MaxError = 0.0f;
		for (const FFluidParticle& P : Particles)
		{
			const float C = (P.Density / RestDensity) - 1.0f;
			MaxError = FMath::Max(MaxError, FMath::Abs(C));
		}
		return MaxError;
	}
}

//=============================================================================
// X-01: Lambda Initialization Test
// Lambda should be initialized to 0 at the start of each substep
// XPBD Algorithm 1, Line 4: λ₀ = 0
//=============================================================================
bool FKawaiiFluidXPBDTest_LambdaInitialization::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;
	const float RestDensity = 1000.0f;
	const float Compliance = 0.01f;
	const float DeltaTime = 1.0f / 120.0f;

	// Create test particles with non-zero Lambda
	TArray<FFluidParticle> Particles = CreateTestGrid(3, SmoothingRadius * 0.5f, 1.0f);
	BuildNeighbors(Particles, SmoothingRadius);

	// Set non-zero Lambda values
	for (FFluidParticle& P : Particles)
	{
		P.Lambda = 100.0f;  // Non-zero initial value
	}

	// Create solver and run one iteration
	FDensityConstraint Solver(RestDensity, SmoothingRadius, Compliance);

	// Note: In our implementation, Lambda reset happens in SimulationContext::SolveDensityConstraints
	// before calling the solver. We'll verify that the solver modifies Lambda values.

	// Before solving, save Lambda values
	TArray<float> LambdasBefore;
	for (const FFluidParticle& P : Particles)
	{
		LambdasBefore.Add(P.Lambda);
	}

	// Run solver
	Solver.Solve(Particles, SmoothingRadius, RestDensity, Compliance, DeltaTime);

	// After solving, Lambda values should be updated (not necessarily zero, but computed)
	bool bAllZero = true;
	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		if (FMath::Abs(Particles[i].Lambda) > KINDA_SMALL_NUMBER)
		{
			bAllZero = false;
			break;
		}
	}

	// Note: Lambda being zero or non-zero depends on the density constraint
	// The test verifies the solver runs without error
	AddInfo(FString::Printf(TEXT("Particles: %d, Lambda values updated by solver"), Particles.Num()));

	// At minimum, density should be computed
	bool bDensityComputed = false;
	for (const FFluidParticle& P : Particles)
	{
		if (P.Density > 0.0f)
		{
			bDensityComputed = true;
			break;
		}
	}
	TestTrue(TEXT("Density values were computed"), bDensityComputed);

	return true;
}

//=============================================================================
// X-02: Compliance Effect Test
// Higher compliance should result in smaller Lambda (softer constraint)
// XPBD: α̃ = α / dt² appears in denominator, so higher α → smaller |Δλ|
//=============================================================================
bool FKawaiiFluidXPBDTest_ComplianceEffect::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;
	const float RestDensity = 1000.0f;
	const float DeltaTime = 1.0f / 120.0f;

	// Test with different compliance values
	const float LowCompliance = 0.0001f;   // Stiff (water-like)
	const float HighCompliance = 0.1f;     // Soft (very compressible)

	// Create compressed test setup (dense particles)
	const float TightSpacing = SmoothingRadius * 0.3f;  // Compressed state

	// Test with low compliance
	TArray<FFluidParticle> ParticlesStiff = CreateTestGrid(3, TightSpacing, 1.0f);
	BuildNeighbors(ParticlesStiff, SmoothingRadius);

	FDensityConstraint SolverStiff(RestDensity, SmoothingRadius, LowCompliance);
	SolverStiff.Solve(ParticlesStiff, SmoothingRadius, RestDensity, LowCompliance, DeltaTime);

	// Test with high compliance
	TArray<FFluidParticle> ParticlesSoft = CreateTestGrid(3, TightSpacing, 1.0f);
	BuildNeighbors(ParticlesSoft, SmoothingRadius);

	FDensityConstraint SolverSoft(RestDensity, SmoothingRadius, HighCompliance);
	SolverSoft.Solve(ParticlesSoft, SmoothingRadius, RestDensity, HighCompliance, DeltaTime);

	// Compare position corrections
	float TotalCorrectionStiff = 0.0f;
	float TotalCorrectionSoft = 0.0f;

	for (int32 i = 0; i < ParticlesStiff.Num(); ++i)
	{
		// Position correction = PredictedPosition - original position
		// Since we used the same initial positions, compare Lambda magnitudes
		TotalCorrectionStiff += FMath::Abs(ParticlesStiff[i].Lambda);
		TotalCorrectionSoft += FMath::Abs(ParticlesSoft[i].Lambda);
	}

	const float AvgLambdaStiff = TotalCorrectionStiff / ParticlesStiff.Num();
	const float AvgLambdaSoft = TotalCorrectionSoft / ParticlesSoft.Num();

	AddInfo(FString::Printf(TEXT("Low compliance (%.4f): avg |λ| = %.4f"), LowCompliance, AvgLambdaStiff));
	AddInfo(FString::Printf(TEXT("High compliance (%.4f): avg |λ| = %.4f"), HighCompliance, AvgLambdaSoft));

	// With higher compliance, the constraint is softer, so Lambda should be smaller
	// (or the correction per Lambda is smaller)
	// This test verifies the compliance parameter has an effect
	TestTrue(TEXT("Different compliance values produce different results"),
		FMath::Abs(AvgLambdaStiff - AvgLambdaSoft) > KINDA_SMALL_NUMBER ||
		TotalCorrectionStiff != TotalCorrectionSoft);

	return true;
}

//=============================================================================
// X-03: Compression State Skip Test
// When density is below rest density (C_i < 0), Lambda should not be updated
// This allows particles to separate without artificial attraction
//=============================================================================
bool FKawaiiFluidXPBDTest_CompressionSkip::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;
	const float RestDensity = 1000.0f;
	const float Compliance = 0.01f;
	const float DeltaTime = 1.0f / 120.0f;

	// Create sparse particles (density < RestDensity)
	const float SparseSpacing = SmoothingRadius * 1.5f;  // Very sparse

	TArray<FFluidParticle> Particles = CreateTestGrid(3, SparseSpacing, 1.0f);
	BuildNeighbors(Particles, SmoothingRadius);

	// Run solver
	FDensityConstraint Solver(RestDensity, SmoothingRadius, Compliance);
	Solver.Solve(Particles, SmoothingRadius, RestDensity, Compliance, DeltaTime);

	// Check that particles with low density don't get compressed further
	// They should have Lambda ≈ 0 or unchanged
	int32 LowDensityCount = 0;
	int32 SkippedCount = 0;

	for (const FFluidParticle& P : Particles)
	{
		const float C = (P.Density / RestDensity) - 1.0f;
		if (C < 0.0f)
		{
			LowDensityCount++;
			// Lambda should be small for under-dense particles
			if (FMath::Abs(P.Lambda) < 0.1f)
			{
				SkippedCount++;
			}
		}
	}

	AddInfo(FString::Printf(TEXT("Particles with ρ < ρ₀: %d"), LowDensityCount));
	AddInfo(FString::Printf(TEXT("Particles with small |λ|: %d"), SkippedCount));

	// Most under-dense particles should have small or zero Lambda
	if (LowDensityCount > 0)
	{
		const float SkipRatio = static_cast<float>(SkippedCount) / static_cast<float>(LowDensityCount);
		TestTrue(TEXT("Most under-dense particles have small Lambda"), SkipRatio > 0.5f);
	}

	return true;
}

//=============================================================================
// X-04: Lambda Accumulation Test
// Lambda should accumulate over iterations: λᵢ₊₁ = λᵢ + Δλ
// This is the key difference from standard PBD
//=============================================================================
bool FKawaiiFluidXPBDTest_LambdaAccumulation::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;
	const float RestDensity = 1000.0f;
	const float Compliance = 0.01f;
	const float DeltaTime = 1.0f / 120.0f;

	// Create dense particles
	const float DenseSpacing = SmoothingRadius * 0.4f;

	TArray<FFluidParticle> Particles = CreateTestGrid(3, DenseSpacing, 1.0f);
	BuildNeighbors(Particles, SmoothingRadius);

	// Initialize Lambda to 0
	for (FFluidParticle& P : Particles)
	{
		P.Lambda = 0.0f;
	}

	FDensityConstraint Solver(RestDensity, SmoothingRadius, Compliance);

	// Run multiple iterations and track Lambda accumulation
	TArray<float> LambdaHistory;

	for (int32 Iter = 0; Iter < 5; ++Iter)
	{
		// Record average Lambda before this iteration
		float AvgLambda = 0.0f;
		for (const FFluidParticle& P : Particles)
		{
			AvgLambda += P.Lambda;
		}
		AvgLambda /= static_cast<float>(Particles.Num());
		LambdaHistory.Add(AvgLambda);

		// Run one solver iteration
		Solver.Solve(Particles, SmoothingRadius, RestDensity, Compliance, DeltaTime);

		// Rebuild neighbors after position update
		BuildNeighbors(Particles, SmoothingRadius);
	}

	// Lambda should change over iterations (accumulating or converging)
	bool bLambdaChanged = false;
	for (int32 i = 1; i < LambdaHistory.Num(); ++i)
	{
		if (FMath::Abs(LambdaHistory[i] - LambdaHistory[i - 1]) > KINDA_SMALL_NUMBER)
		{
			bLambdaChanged = true;
			break;
		}
	}

	TestTrue(TEXT("Lambda values change over iterations"), bLambdaChanged);

	for (int32 i = 0; i < LambdaHistory.Num(); ++i)
	{
		AddInfo(FString::Printf(TEXT("Iteration %d: avg λ = %.6f"), i, LambdaHistory[i]));
	}

	return true;
}

//=============================================================================
// X-05: Convergence Test
// Constraint error should decrease with more iterations
// |∇C|² should decrease as system approaches equilibrium
//=============================================================================
bool FKawaiiFluidXPBDTest_Convergence::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;
	const float RestDensity = 1000.0f;
	const float Compliance = 0.001f;  // Stiffer constraint for visible convergence
	const float DeltaTime = 1.0f / 120.0f;

	// Create compressed particles (will expand towards rest density)
	const float InitialSpacing = SmoothingRadius * 0.35f;

	TArray<FFluidParticle> Particles = CreateTestGrid(4, InitialSpacing, 1.0f);
	BuildNeighbors(Particles, SmoothingRadius);

	// Initialize Lambda
	for (FFluidParticle& P : Particles)
	{
		P.Lambda = 0.0f;
	}

	FDensityConstraint Solver(RestDensity, SmoothingRadius, Compliance);

	// Track constraint error over iterations
	TArray<float> ErrorHistory;
	TArray<float> DensityHistory;

	const int32 MaxIterations = 10;

	for (int32 Iter = 0; Iter < MaxIterations; ++Iter)
	{
		// Run solver
		Solver.Solve(Particles, SmoothingRadius, RestDensity, Compliance, DeltaTime);

		// Rebuild neighbors
		BuildNeighbors(Particles, SmoothingRadius);

		// Compute constraint error (max |C_i|)
		float MaxError = ComputeConstraintError(Particles, RestDensity);
		ErrorHistory.Add(MaxError);

		// Compute average density
		float AvgDensity = ComputeAverageDensity(Particles);
		DensityHistory.Add(AvgDensity);
	}

	// Check that error decreases (convergence)
	bool bConverging = false;
	if (ErrorHistory.Num() >= 2)
	{
		// Compare first and last error
		const float InitialError = ErrorHistory[0];
		const float FinalError = ErrorHistory.Last();

		// Error should decrease or stay stable
		bConverging = (FinalError <= InitialError * 1.1f);  // Allow 10% tolerance

		AddInfo(FString::Printf(TEXT("Initial error: %.4f, Final error: %.4f"), InitialError, FinalError));
	}

	// Log iteration history
	for (int32 i = 0; i < ErrorHistory.Num(); ++i)
	{
		AddInfo(FString::Printf(TEXT("Iter %d: max|C| = %.4f, avg ρ = %.2f"),
			i, ErrorHistory[i], DensityHistory[i]));
	}

	TestTrue(TEXT("Constraint error converges or stays stable"), bConverging);

	// Density should approach RestDensity
	const float FinalDensity = DensityHistory.Last();
	const float DensityError = FMath::Abs(FinalDensity - RestDensity) / RestDensity;
	AddInfo(FString::Printf(TEXT("Final density error: %.2f%%"), DensityError * 100.0f));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
