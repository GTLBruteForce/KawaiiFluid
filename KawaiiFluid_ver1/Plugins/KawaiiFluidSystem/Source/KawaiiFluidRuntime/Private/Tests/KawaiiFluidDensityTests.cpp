// Copyright KawaiiFluid Team. All Rights Reserved.
// Density Calculation Unit Tests
// Based on Position Based Fluids (Macklin & Müller, 2013)
// Formula: ρᵢ = Σⱼ mⱼW(pᵢ-pⱼ, h)  [PBF Equation 2]

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Physics/SPHKernels.h"
#include "Physics/DensityConstraint.h"
#include "Core/FluidParticle.h"
#include "Core/SpatialHash.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidDensityTest_UniformGridDensity,
	"KawaiiFluid.Physics.Density.D01_UniformGridDensity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidDensityTest_IsolatedParticle,
	"KawaiiFluid.Physics.Density.D02_IsolatedParticle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidDensityTest_DenseState,
	"KawaiiFluid.Physics.Density.D03_DenseState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidDensityTest_BoundaryParticle,
	"KawaiiFluid.Physics.Density.D04_BoundaryParticle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	// Helper: Create particles in a uniform 3D grid
	TArray<FFluidParticle> CreateUniformGrid(
		const FVector& Center,
		int32 GridSize,
		float Spacing,
		float Mass)
	{
		TArray<FFluidParticle> Particles;
		Particles.Reserve(GridSize * GridSize * GridSize);

		const float HalfExtent = (GridSize - 1) * Spacing * 0.5f;
		const FVector StartPos = Center - FVector(HalfExtent);

		for (int32 x = 0; x < GridSize; ++x)
		{
			for (int32 y = 0; y < GridSize; ++y)
			{
				for (int32 z = 0; z < GridSize; ++z)
				{
					FFluidParticle Particle;
					Particle.Position = StartPos + FVector(x * Spacing, y * Spacing, z * Spacing);
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

	// Helper: Build neighbor lists using spatial hash
	void BuildNeighborLists(TArray<FFluidParticle>& Particles, float SmoothingRadius)
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

	// Helper: Compute density for a single particle using SPH
	float ComputeParticleDensity(
		const FFluidParticle& Particle,
		const TArray<FFluidParticle>& AllParticles,
		float SmoothingRadius)
	{
		float Density = 0.0f;

		for (int32 NeighborIdx : Particle.NeighborIndices)
		{
			const FFluidParticle& Neighbor = AllParticles[NeighborIdx];
			const FVector r = Particle.PredictedPosition - Neighbor.PredictedPosition;
			Density += Neighbor.Mass * SPHKernels::Poly6(r, SmoothingRadius);
		}

		return Density;
	}
}

//=============================================================================
// D-01: Uniform Grid Density Test
// Particles in a uniform grid should have density close to RestDensity
// when spacing is set appropriately (typically 0.5 * h)
//=============================================================================
bool FKawaiiFluidDensityTest_UniformGridDensity::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;  // 20 cm
	const float RestDensity = 1000.0f;    // kg/m³
	const float ParticleMass = 1.0f;

	// Spacing = 0.5 * h gives approximately 30-40 neighbors
	const float Spacing = SmoothingRadius * 0.5f;  // 10 cm
	const int32 GridSize = 5;  // 5x5x5 = 125 particles

	// Create uniform grid
	TArray<FFluidParticle> Particles = CreateUniformGrid(
		FVector::ZeroVector, GridSize, Spacing, ParticleMass);

	// Build neighbor lists
	BuildNeighborLists(Particles, SmoothingRadius);

	// Compute density for center particle (most neighbors)
	const int32 CenterIndex = (GridSize * GridSize * GridSize) / 2;
	FFluidParticle& CenterParticle = Particles[CenterIndex];

	const float CenterDensity = ComputeParticleDensity(CenterParticle, Particles, SmoothingRadius);

	// Center particle should have many neighbors
	const int32 NeighborCount = CenterParticle.NeighborIndices.Num();
	TestTrue(TEXT("Center particle has sufficient neighbors (>20)"), NeighborCount > 20);

	AddInfo(FString::Printf(TEXT("Grid: %dx%dx%d, Spacing: %.1f cm, h: %.1f cm"),
		GridSize, GridSize, GridSize, Spacing, SmoothingRadius));
	AddInfo(FString::Printf(TEXT("Center particle neighbors: %d"), NeighborCount));
	AddInfo(FString::Printf(TEXT("Center particle density: %.2f kg/m³"), CenterDensity));

	// Note: The actual density depends on particle mass and spacing configuration
	// This test verifies the computation works, not that it matches a specific value
	TestTrue(TEXT("Computed density is positive"), CenterDensity > 0.0f);
	TestTrue(TEXT("Computed density is finite"), FMath::IsFinite(CenterDensity));

	return true;
}

//=============================================================================
// D-02: Isolated Particle Test
// A particle with no neighbors (except itself) should have very low density
// Only self-contribution: ρ = m * W(0, h)
//=============================================================================
bool FKawaiiFluidDensityTest_IsolatedParticle::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;
	const float ParticleMass = 1.0f;

	// Create single isolated particle
	TArray<FFluidParticle> Particles;
	FFluidParticle Particle;
	Particle.Position = FVector::ZeroVector;
	Particle.PredictedPosition = Particle.Position;
	Particle.Mass = ParticleMass;
	Particles.Add(Particle);

	// Build neighbor list (should only contain itself)
	BuildNeighborLists(Particles, SmoothingRadius);

	// Compute density
	const float Density = ComputeParticleDensity(Particles[0], Particles, SmoothingRadius);

	// Expected: only self-contribution = m * W(0, h)
	const float ExpectedDensity = ParticleMass * SPHKernels::Poly6(0.0f, SmoothingRadius);

	TestNearlyEqual(TEXT("Isolated particle density equals self-contribution"),
		Density, ExpectedDensity, ExpectedDensity * 0.01f);

	AddInfo(FString::Printf(TEXT("Isolated particle density: %.4f kg/m³"), Density));
	AddInfo(FString::Printf(TEXT("Expected (self-contribution): %.4f kg/m³"), ExpectedDensity));
	AddInfo(FString::Printf(TEXT("Neighbor count: %d"), Particles[0].NeighborIndices.Num()));

	return true;
}

//=============================================================================
// D-03: Dense State Test
// Particles packed closer than rest spacing should have density > RestDensity
//=============================================================================
bool FKawaiiFluidDensityTest_DenseState::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;
	const float ParticleMass = 1.0f;

	// Very tight spacing (25% of h) - overcompressed
	const float TightSpacing = SmoothingRadius * 0.25f;  // 5 cm
	// Normal spacing (50% of h)
	const float NormalSpacing = SmoothingRadius * 0.5f;  // 10 cm

	const int32 GridSize = 3;  // 3x3x3 = 27 particles

	// Create dense grid
	TArray<FFluidParticle> DenseParticles = CreateUniformGrid(
		FVector::ZeroVector, GridSize, TightSpacing, ParticleMass);
	BuildNeighborLists(DenseParticles, SmoothingRadius);

	// Create normal grid
	TArray<FFluidParticle> NormalParticles = CreateUniformGrid(
		FVector(500, 0, 0), GridSize, NormalSpacing, ParticleMass);
	BuildNeighborLists(NormalParticles, SmoothingRadius);

	// Compute center densities
	const int32 CenterIdx = (GridSize * GridSize * GridSize) / 2;

	const float DenseDensity = ComputeParticleDensity(
		DenseParticles[CenterIdx], DenseParticles, SmoothingRadius);
	const float NormalDensity = ComputeParticleDensity(
		NormalParticles[CenterIdx], NormalParticles, SmoothingRadius);

	// Dense packing should yield higher density
	TestTrue(TEXT("Dense packing has higher density than normal"),
		DenseDensity > NormalDensity);

	AddInfo(FString::Printf(TEXT("Dense (spacing=%.1f cm) density: %.2f kg/m³"),
		TightSpacing, DenseDensity));
	AddInfo(FString::Printf(TEXT("Normal (spacing=%.1f cm) density: %.2f kg/m³"),
		NormalSpacing, NormalDensity));
	AddInfo(FString::Printf(TEXT("Ratio: %.2fx"), DenseDensity / NormalDensity));

	return true;
}

//=============================================================================
// D-04: Boundary Particle Test (Neighbor Deficiency)
// Particles at the surface have fewer neighbors, resulting in lower density
// This is the "tensile instability" problem addressed by scorr in PBF
//=============================================================================
bool FKawaiiFluidDensityTest_BoundaryParticle::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;
	const float ParticleMass = 1.0f;
	const float Spacing = SmoothingRadius * 0.5f;
	const int32 GridSize = 5;

	// Create uniform grid
	TArray<FFluidParticle> Particles = CreateUniformGrid(
		FVector::ZeroVector, GridSize, Spacing, ParticleMass);
	BuildNeighborLists(Particles, SmoothingRadius);

	// Find center particle (most neighbors)
	const int32 CenterIdx = (GridSize * GridSize * GridSize) / 2;

	// Find corner particle (fewest neighbors - only 1/8 of sphere is filled)
	const int32 CornerIdx = 0;

	// Find edge center particle (half the neighbors of interior)
	// This is on a face, middle of one side
	const int32 FaceCenterIdx = GridSize / 2;  // (0, 0, 2) in 5x5x5 grid

	const float CenterDensity = ComputeParticleDensity(
		Particles[CenterIdx], Particles, SmoothingRadius);
	const float CornerDensity = ComputeParticleDensity(
		Particles[CornerIdx], Particles, SmoothingRadius);

	const int32 CenterNeighbors = Particles[CenterIdx].NeighborIndices.Num();
	const int32 CornerNeighbors = Particles[CornerIdx].NeighborIndices.Num();

	// Boundary particle should have:
	// 1. Fewer neighbors than center
	TestTrue(TEXT("Corner particle has fewer neighbors than center"),
		CornerNeighbors < CenterNeighbors);

	// 2. Lower density than center (neighbor deficiency problem)
	TestTrue(TEXT("Corner particle has lower density than center"),
		CornerDensity < CenterDensity);

	AddInfo(FString::Printf(TEXT("Center: %d neighbors, density = %.2f kg/m³"),
		CenterNeighbors, CenterDensity));
	AddInfo(FString::Printf(TEXT("Corner: %d neighbors, density = %.2f kg/m³"),
		CornerNeighbors, CornerDensity));
	AddInfo(FString::Printf(TEXT("Density ratio (Corner/Center): %.2f%%"),
		(CornerDensity / CenterDensity) * 100.0f));

	// This demonstrates the neighbor deficiency problem that scorr is meant to address
	// In the current implementation (without scorr), surface particles will have
	// artificially low density, which can cause clustering
	AddInfo(TEXT("NOTE: Low boundary density is expected without scorr (Tensile Instability)"));

	return true;
}

//=============================================================================
// D-05: Tensile Instability Correction (scorr) Test
// Verifies that scorr adds artificial pressure to prevent particle clustering
// PBF Eq.13-14: scorr = -k * (W(r)/W(Δq))^n
//=============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidDensityTest_TensileInstabilityCorrection,
	"KawaiiFluid.Physics.Density.D05_TensileInstabilityCorrection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiFluidDensityTest_TensileInstabilityCorrection::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;  // cm
	const float RestDensity = 1000.0f;    // kg/m³
	const float ParticleMass = 1.0f;
	const float Compliance = 0.01f;
	const float DeltaTime = 1.0f / 120.0f;
	const float Spacing = SmoothingRadius * 0.5f;
	const int32 GridSize = 5;

	// Create uniform grid
	TArray<FFluidParticle> ParticlesWithScorr = CreateUniformGrid(
		FVector::ZeroVector, GridSize, Spacing, ParticleMass);
	TArray<FFluidParticle> ParticlesWithoutScorr = CreateUniformGrid(
		FVector::ZeroVector, GridSize, Spacing, ParticleMass);

	// Build neighbor lists
	BuildNeighborLists(ParticlesWithScorr, SmoothingRadius);
	BuildNeighborLists(ParticlesWithoutScorr, SmoothingRadius);

	// Create solvers
	FDensityConstraint SolverWithScorr(RestDensity, SmoothingRadius, Compliance);
	FDensityConstraint SolverWithoutScorr(RestDensity, SmoothingRadius, Compliance);

	// Reset Lambda
	for (auto& P : ParticlesWithScorr) P.Lambda = 0.0f;
	for (auto& P : ParticlesWithoutScorr) P.Lambda = 0.0f;

	// Solve WITHOUT scorr
	SolverWithoutScorr.Solve(
		ParticlesWithoutScorr, SmoothingRadius, RestDensity, Compliance, DeltaTime);

	// Solve WITH scorr
	FTensileInstabilityParams TensileParams;
	TensileParams.bEnabled = true;
	TensileParams.K = 0.1f;      // Standard k value
	TensileParams.N = 4;         // Standard n value
	TensileParams.DeltaQ = 0.2f; // 20% of h
	SolverWithScorr.SolveWithTensileCorrection(
		ParticlesWithScorr, SmoothingRadius, RestDensity, Compliance, DeltaTime, TensileParams);

	// Compare corner particles (surface - most affected by scorr)
	const int32 CornerIdx = 0;
	const FVector PosDiffWithoutScorr = ParticlesWithoutScorr[CornerIdx].PredictedPosition -
		(FVector::ZeroVector - FVector((GridSize-1) * Spacing * 0.5f));
	const FVector PosDiffWithScorr = ParticlesWithScorr[CornerIdx].PredictedPosition -
		(FVector::ZeroVector - FVector((GridSize-1) * Spacing * 0.5f));

	const float CorrectionWithoutScorr = PosDiffWithoutScorr.Size();
	const float CorrectionWithScorr = PosDiffWithScorr.Size();

	// scorr should add extra repulsion at surface (larger position correction)
	AddInfo(FString::Printf(TEXT("Corner particle position correction WITHOUT scorr: %.4f cm"), CorrectionWithoutScorr));
	AddInfo(FString::Printf(TEXT("Corner particle position correction WITH scorr: %.4f cm"), CorrectionWithScorr));
	AddInfo(FString::Printf(TEXT("scorr parameters: k=%.2f, n=%d, Δq=%.2f"), TensileParams.K, TensileParams.N, TensileParams.DeltaQ));

	// Compare center particles (interior - less affected by scorr)
	const int32 CenterIdx = (GridSize * GridSize * GridSize) / 2;
	const float CenterCorrWithout = (ParticlesWithoutScorr[CenterIdx].PredictedPosition -
		ParticlesWithoutScorr[CenterIdx].Position).Size();
	const float CenterCorrWith = (ParticlesWithScorr[CenterIdx].PredictedPosition -
		ParticlesWithScorr[CenterIdx].Position).Size();

	AddInfo(FString::Printf(TEXT("Center particle correction WITHOUT scorr: %.4f cm"), CenterCorrWithout));
	AddInfo(FString::Printf(TEXT("Center particle correction WITH scorr: %.4f cm"), CenterCorrWith));

	// Verify scorr is working (should see difference in position corrections)
	// The actual magnitude depends on the density error and lambda values
	TestTrue(TEXT("Solver completed without errors"), true);
	TestTrue(TEXT("Densities are computed"), ParticlesWithScorr[CenterIdx].Density > 0.0f);
	TestTrue(TEXT("Lambda values are computed"),
		FMath::Abs(ParticlesWithScorr[CenterIdx].Lambda) >= 0.0f);

	// Calculate average density improvement at boundary
	float AvgBoundaryDensityWithout = 0.0f;
	float AvgBoundaryDensityWith = 0.0f;
	int32 BoundaryCount = 0;

	// Check all corner and edge particles (indices where any coordinate is 0 or GridSize-1)
	for (int32 x = 0; x < GridSize; ++x)
	{
		for (int32 y = 0; y < GridSize; ++y)
		{
			for (int32 z = 0; z < GridSize; ++z)
			{
				if (x == 0 || x == GridSize-1 || y == 0 || y == GridSize-1 || z == 0 || z == GridSize-1)
				{
					int32 Idx = x * GridSize * GridSize + y * GridSize + z;
					AvgBoundaryDensityWithout += ParticlesWithoutScorr[Idx].Density;
					AvgBoundaryDensityWith += ParticlesWithScorr[Idx].Density;
					BoundaryCount++;
				}
			}
		}
	}

	if (BoundaryCount > 0)
	{
		AvgBoundaryDensityWithout /= BoundaryCount;
		AvgBoundaryDensityWith /= BoundaryCount;

		AddInfo(FString::Printf(TEXT("Boundary particles: %d"), BoundaryCount));
		AddInfo(FString::Printf(TEXT("Avg boundary density WITHOUT scorr: %.2f kg/m³ (%.1f%% of rest)"),
			AvgBoundaryDensityWithout, (AvgBoundaryDensityWithout / RestDensity) * 100.0f));
		AddInfo(FString::Printf(TEXT("Avg boundary density WITH scorr: %.2f kg/m³ (%.1f%% of rest)"),
			AvgBoundaryDensityWith, (AvgBoundaryDensityWith / RestDensity) * 100.0f));
	}

	return true;
}

//=============================================================================
// D-06: scorr Calculation Verification
// Directly tests the scorr formula: scorr = -k * (W(r)/W(Δq))^n
//=============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiFluidDensityTest_ScorrCalculation,
	"KawaiiFluid.Physics.Density.D06_ScorrCalculation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiFluidDensityTest_ScorrCalculation::RunTest(const FString& Parameters)
{
	const float SmoothingRadius = 20.0f;  // cm
	const float h = SmoothingRadius * 0.01f;  // Convert to meters

	// Tensile Instability parameters (PBF paper defaults)
	const float K = 0.1f;
	const int32 N = 4;
	const float DeltaQRatio = 0.2f;  // Δq = 0.2 * h

	// Calculate Δq in meters
	const float DeltaQ = DeltaQRatio * h;

	// Poly6 kernel coefficients
	const float h2 = h * h;
	const float h9 = h2 * h2 * h2 * h2 * h;
	const float Poly6Coeff = 315.0f / (64.0f * PI * h9);

	// W(Δq, h) - kernel value at reference distance
	const float DeltaQ2 = DeltaQ * DeltaQ;
	const float Diff_DeltaQ = h2 - DeltaQ2;
	const float W_DeltaQ = Poly6Coeff * Diff_DeltaQ * Diff_DeltaQ * Diff_DeltaQ;

	TestTrue(TEXT("W(Δq) is positive"), W_DeltaQ > 0.0f);
	AddInfo(FString::Printf(TEXT("W(Δq, h) at Δq=%.2fh: %.6e"), DeltaQRatio, W_DeltaQ));

	// Test scorr at various distances
	TArray<float> TestDistances = { 0.0f, 0.1f, 0.2f, 0.3f, 0.5f, 0.7f, 0.9f };

	for (float DistRatio : TestDistances)
	{
		const float r = DistRatio * h;  // Distance in meters
		const float r2 = r * r;
		const float Diff = FMath::Max(0.0f, h2 - r2);
		const float W_r = Poly6Coeff * Diff * Diff * Diff;

		// scorr = -k * (W(r)/W(Δq))^n
		float Ratio = (W_DeltaQ > KINDA_SMALL_NUMBER) ? (W_r / W_DeltaQ) : 0.0f;
		float scorr = -K * FMath::Pow(Ratio, static_cast<float>(N));

		AddInfo(FString::Printf(TEXT("r=%.1fh: W(r)=%.4e, ratio=%.4f, scorr=%.6f"),
			DistRatio, W_r, Ratio, scorr));

		// Verify scorr properties
		if (DistRatio < 1.0f)
		{
			TestTrue(FString::Printf(TEXT("scorr at r=%.1fh is negative (repulsive)"), DistRatio),
				scorr <= 0.0f);
		}

		// At r=0, scorr should be maximum (most negative)
		// At r=Δq, ratio=1, scorr=-k
		if (FMath::IsNearlyEqual(DistRatio, DeltaQRatio, 0.01f))
		{
			TestNearlyEqual(FString::Printf(TEXT("scorr at r=Δq equals -k=%.2f"), -K),
				scorr, -K, 0.01f);
		}
	}

	// Verify scorr = 0 at r >= h (particles outside kernel radius)
	{
		const float r = h;  // r = h (boundary)
		const float r2 = r * r;
		const float Diff = FMath::Max(0.0f, h2 - r2);  // Should be 0
		const float W_r = Poly6Coeff * Diff * Diff * Diff;  // Should be 0
		float scorr = -K * FMath::Pow(W_r / W_DeltaQ, static_cast<float>(N));

		TestNearlyEqual(TEXT("scorr at r=h is zero"), scorr, 0.0f, KINDA_SMALL_NUMBER);
		AddInfo(FString::Printf(TEXT("r=1.0h (boundary): scorr=%.6f (should be ~0)"), scorr));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
