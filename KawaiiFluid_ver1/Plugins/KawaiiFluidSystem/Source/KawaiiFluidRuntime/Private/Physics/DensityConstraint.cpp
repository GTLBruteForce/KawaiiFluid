// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Physics/DensityConstraint.h"
#include "Physics/SPHKernels.h"
#include "Math/UnrealMathSSE.h"
#include "Async/ParallelFor.h"

//========================================
// Constants
//========================================
namespace
{
	constexpr float CM_TO_M = 0.01f;
	constexpr float CM_TO_M_SQ = CM_TO_M * CM_TO_M;
}

//========================================
// SIMD Helpers
//========================================
FORCEINLINE VectorRegister4Float VectorInvSqrtSafe(VectorRegister4Float V)
{
	const VectorRegister4Float MinValue = VectorSetFloat1(KINDA_SMALL_NUMBER);
	V = VectorMax(V, MinValue);
	return VectorReciprocalSqrt(V);
}

//========================================
// Constructor
//========================================
FDensityConstraint::FDensityConstraint()
	: RestDensity(1000.0f)
	, Epsilon(100.0f)
	, SmoothingRadius(0.1f)
{
}

FDensityConstraint::FDensityConstraint(float InRestDensity, float InSmoothingRadius, float InEpsilon)
	: RestDensity(InRestDensity)
	, Epsilon(InEpsilon)
	, SmoothingRadius(InSmoothingRadius)
{
}

//========================================
// SoA Management
//========================================
void FDensityConstraint::ResizeSoAArrays(int32 NumParticles)
{
	if (PosX.Num() != NumParticles)
	{
		PosX.SetNum(NumParticles);
		PosY.SetNum(NumParticles);
		PosZ.SetNum(NumParticles);
		Masses.SetNum(NumParticles);
		Densities.SetNum(NumParticles);
		Lambdas.SetNum(NumParticles);
		DeltaPX.SetNum(NumParticles);
		DeltaPY.SetNum(NumParticles);
		DeltaPZ.SetNum(NumParticles);
	}
}

void FDensityConstraint::CopyToSoA(const TArray<FFluidParticle>& Particles)
{
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		const FFluidParticle& P = Particles[i];
		PosX[i] = P.PredictedPosition.X;
		PosY[i] = P.PredictedPosition.Y;
		PosZ[i] = P.PredictedPosition.Z;
		Masses[i] = P.Mass;
	});
}

void FDensityConstraint::ApplyFromSoA(TArray<FFluidParticle>& Particles)
{
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		FFluidParticle& P = Particles[i];
		P.PredictedPosition.X += DeltaPX[i];
		P.PredictedPosition.Y += DeltaPY[i];
		P.PredictedPosition.Z += DeltaPZ[i];
		P.Density = Densities[i];
		P.Lambda = Lambdas[i];
	});
}

//========================================
// Main Solver
//========================================
void FDensityConstraint::Solve(TArray<FFluidParticle>& Particles, float InSmoothingRadius, float InRestDensity, float InCompliance, float DeltaTime)
{
	SmoothingRadius = InSmoothingRadius;
	RestDensity = InRestDensity;

	// XPBD: α̃ = α / dt²
	const float DtSq = DeltaTime * DeltaTime;
	Epsilon = InCompliance / FMath::Max(DtSq, 1e-8f);

	const int32 NumParticles = Particles.Num();
	if (NumParticles == 0) return;

	// 1. Prepare SoA
	ResizeSoAArrays(NumParticles);
	CopyToSoA(Particles);

	// 2. Compute kernel coefficients
	const float h = SmoothingRadius * CM_TO_M;
	const float h2 = h * h;
	const float h6 = h2 * h2 * h2;
	const float h9 = h6 * h2 * h;

	FSPHKernelCoeffs Coeffs;
	Coeffs.h = h;
	Coeffs.h2 = h2;
	Coeffs.Poly6Coeff = 315.0f / (64.0f * PI * h9);
	Coeffs.SpikyCoeff = -45.0f / (PI * h6);
	Coeffs.InvRestDensity = 1.0f / RestDensity;
	Coeffs.SmoothingRadiusSq = SmoothingRadius * SmoothingRadius;

	// 3. SIMD computation
	ComputeDensityAndLambda_SIMD(Particles, Coeffs);
	ComputeDeltaP_SIMD(Particles, Coeffs);

	// 4. Apply results
	ApplyFromSoA(Particles);
}

//========================================
// Main Solver (with Tensile Instability Correction)
//========================================
void FDensityConstraint::SolveWithTensileCorrection(
	TArray<FFluidParticle>& Particles,
	float InSmoothingRadius,
	float InRestDensity,
	float InCompliance,
	float DeltaTime,
	const FTensileInstabilityParams& TensileParams)
{
	SmoothingRadius = InSmoothingRadius;
	RestDensity = InRestDensity;

	// XPBD: α̃ = α / dt²
	const float DtSq = DeltaTime * DeltaTime;
	Epsilon = InCompliance / FMath::Max(DtSq, 1e-8f);

	const int32 NumParticles = Particles.Num();
	if (NumParticles == 0) return;

	// 1. Prepare SoA
	ResizeSoAArrays(NumParticles);
	CopyToSoA(Particles);

	// 2. Compute kernel coefficients
	const float h = SmoothingRadius * CM_TO_M;
	const float h2 = h * h;
	const float h6 = h2 * h2 * h2;
	const float h9 = h6 * h2 * h;

	FSPHKernelCoeffs Coeffs;
	Coeffs.h = h;
	Coeffs.h2 = h2;
	Coeffs.Poly6Coeff = 315.0f / (64.0f * PI * h9);
	Coeffs.SpikyCoeff = -45.0f / (PI * h6);
	Coeffs.InvRestDensity = 1.0f / RestDensity;
	Coeffs.SmoothingRadiusSq = SmoothingRadius * SmoothingRadius;

	// 3. Configure Tensile Instability parameters
	Coeffs.TensileParams = TensileParams;
	if (TensileParams.bEnabled)
	{
		// Precompute W(Δq, h) - using Poly6 kernel
		// Δq = DeltaQ * h (in meters)
		const float DeltaQ_m = TensileParams.DeltaQ * h;
		const float DeltaQ2 = DeltaQ_m * DeltaQ_m;
		const float Diff = h2 - DeltaQ2;
		Coeffs.TensileParams.W_DeltaQ = Coeffs.Poly6Coeff * Diff * Diff * Diff;
	}

	// 4. SIMD computation
	ComputeDensityAndLambda_SIMD(Particles, Coeffs);
	ComputeDeltaP_SIMD(Particles, Coeffs);

	// 5. Apply results
	ApplyFromSoA(Particles);
}

//========================================
// Step 1: Density + Lambda (SIMD)
//========================================
void FDensityConstraint::ComputeDensityAndLambda_SIMD(
	const TArray<FFluidParticle>& Particles,
	const FSPHKernelCoeffs& Coeffs)
{
	const int32 NumParticles = Particles.Num();

	// RESTRICT pointers
	const float* RESTRICT PosXPtr = PosX.GetData();
	const float* RESTRICT PosYPtr = PosY.GetData();
	const float* RESTRICT PosZPtr = PosZ.GetData();
	const float* RESTRICT MassPtr = Masses.GetData();
	float* RESTRICT DensityPtr = Densities.GetData();
	float* RESTRICT LambdaPtr = Lambdas.GetData();

	// SIMD constants
	const VectorRegister4Float VecH2 = VectorSetFloat1(Coeffs.h2);
	const VectorRegister4Float VecH = VectorSetFloat1(Coeffs.h);
	const VectorRegister4Float VecCmToM = VectorSetFloat1(CM_TO_M);
	const VectorRegister4Float VecCmToMSq = VectorSetFloat1(CM_TO_M_SQ);
	const VectorRegister4Float VecPoly6Coeff = VectorSetFloat1(Coeffs.Poly6Coeff);
	const VectorRegister4Float VecSpikyCoeff = VectorSetFloat1(Coeffs.SpikyCoeff);
	const VectorRegister4Float VecInvRestDensity = VectorSetFloat1(Coeffs.InvRestDensity);
	const VectorRegister4Float VecSmoothingRadiusSq = VectorSetFloat1(Coeffs.SmoothingRadiusSq);
	const VectorRegister4Float VecZero = VectorZeroFloat();
	const VectorRegister4Float VecMinR2 = VectorSetFloat1(KINDA_SMALL_NUMBER);

	ParallelFor(NumParticles, [&](int32 i)
	{
		const TArray<int32>& Neighbors = Particles[i].NeighborIndices;
		const int32 NumNeighbors = Neighbors.Num();
		const int32* NeighborData = Neighbors.GetData();

		const float PiX = PosXPtr[i];
		const float PiY = PosYPtr[i];
		const float PiZ = PosZPtr[i];

		const VectorRegister4Float VecPiX = VectorSetFloat1(PiX);
		const VectorRegister4Float VecPiY = VectorSetFloat1(PiY);
		const VectorRegister4Float VecPiZ = VectorSetFloat1(PiZ);

		VectorRegister4Float VecDensity = VecZero;
		VectorRegister4Float VecSumGradC2 = VecZero;
		VectorRegister4Float VecGradC_iX = VecZero;
		VectorRegister4Float VecGradC_iY = VecZero;
		VectorRegister4Float VecGradC_iZ = VecZero;

		// Process 4 at a time with SIMD
		int32 n = 0;
		for (; n + 4 <= NumNeighbors; n += 4)
		{
			const int32 n0 = NeighborData[n];
			const int32 n1 = NeighborData[n + 1];
			const int32 n2 = NeighborData[n + 2];
			const int32 n3 = NeighborData[n + 3];

			// Gather
			const VectorRegister4Float VecNX = MakeVectorRegisterFloat(PosXPtr[n0], PosXPtr[n1], PosXPtr[n2], PosXPtr[n3]);
			const VectorRegister4Float VecNY = MakeVectorRegisterFloat(PosYPtr[n0], PosYPtr[n1], PosYPtr[n2], PosYPtr[n3]);
			const VectorRegister4Float VecNZ = MakeVectorRegisterFloat(PosZPtr[n0], PosZPtr[n1], PosZPtr[n2], PosZPtr[n3]);
			const VectorRegister4Float VecMass = MakeVectorRegisterFloat(MassPtr[n0], MassPtr[n1], MassPtr[n2], MassPtr[n3]);

			// r = Pi - Pj
			const VectorRegister4Float VecDX = VectorSubtract(VecPiX, VecNX);
			const VectorRegister4Float VecDY = VectorSubtract(VecPiY, VecNY);
			const VectorRegister4Float VecDZ = VectorSubtract(VecPiZ, VecNZ);

			// r²
			VectorRegister4Float VecR2 = VectorMultiply(VecDX, VecDX);
			VecR2 = VectorMultiplyAdd(VecDY, VecDY, VecR2);
			VecR2 = VectorMultiplyAdd(VecDZ, VecDZ, VecR2);

			// Range mask
			const VectorRegister4Float VecMask = VectorCompareLT(VecR2, VecSmoothingRadiusSq);

			// Poly6 density
			const VectorRegister4Float VecR2_m = VectorMultiply(VecR2, VecCmToMSq);
			VectorRegister4Float VecDiff = VectorSubtract(VecH2, VecR2_m);
			VectorRegister4Float VecDiff3 = VectorMultiply(VecDiff, VectorMultiply(VecDiff, VecDiff));
			VectorRegister4Float VecDensityContrib = VectorMultiply(VectorMultiply(VecMass, VecPoly6Coeff), VecDiff3);
			VecDensityContrib = VectorSelect(VecMask, VecDensityContrib, VecZero);
			VecDensity = VectorAdd(VecDensity, VecDensityContrib);

			// Spiky gradient
			const VectorRegister4Float VecValidMask = VectorBitwiseAnd(VecMask, VectorCompareGT(VecR2, VecMinR2));
			const VectorRegister4Float VecInvRLen = VectorInvSqrtSafe(VecR2);
			const VectorRegister4Float VecRLen = VectorMultiply(VecR2, VecInvRLen);
			const VectorRegister4Float VecRLen_m = VectorMultiply(VecRLen, VecCmToM);

			VecDiff = VectorSubtract(VecH, VecRLen_m);
			VectorRegister4Float VecCoeff = VectorMultiply(VecSpikyCoeff, VectorMultiply(VecDiff, VecDiff));
			VecCoeff = VectorMultiply(VecCoeff, VectorMultiply(VecCmToM, VecInvRLen));

			VectorRegister4Float VecGradWX = VectorSelect(VecValidMask, VectorMultiply(VecCoeff, VecDX), VecZero);
			VectorRegister4Float VecGradWY = VectorSelect(VecValidMask, VectorMultiply(VecCoeff, VecDY), VecZero);
			VectorRegister4Float VecGradWZ = VectorSelect(VecValidMask, VectorMultiply(VecCoeff, VecDZ), VecZero);

			// ∇Cⱼ = -∇W / ρ₀
			const VectorRegister4Float VecGradC_jX = VectorMultiply(VectorNegate(VecGradWX), VecInvRestDensity);
			const VectorRegister4Float VecGradC_jY = VectorMultiply(VectorNegate(VecGradWY), VecInvRestDensity);
			const VectorRegister4Float VecGradC_jZ = VectorMultiply(VectorNegate(VecGradWZ), VecInvRestDensity);

			// Accumulate |∇Cⱼ|²
			VectorRegister4Float VecGradC_j2 = VectorMultiply(VecGradC_jX, VecGradC_jX);
			VecGradC_j2 = VectorMultiplyAdd(VecGradC_jY, VecGradC_jY, VecGradC_j2);
			VecGradC_j2 = VectorMultiplyAdd(VecGradC_jZ, VecGradC_jZ, VecGradC_j2);
			VecSumGradC2 = VectorAdd(VecSumGradC2, VecGradC_j2);

			// Accumulate ∇Cᵢ
			VecGradC_iX = VectorAdd(VecGradC_iX, VectorMultiply(VecGradWX, VecInvRestDensity));
			VecGradC_iY = VectorAdd(VecGradC_iY, VectorMultiply(VecGradWY, VecInvRestDensity));
			VecGradC_iZ = VectorAdd(VecGradC_iZ, VectorMultiply(VecGradWZ, VecInvRestDensity));
		}

		// Reduce SIMD results
		alignas(16) float Temp[4];
		float Density = 0.0f, SumGradC2 = 0.0f;
		float GradC_iX = 0.0f, GradC_iY = 0.0f, GradC_iZ = 0.0f;

		VectorStoreAligned(VecDensity, Temp);
		Density = Temp[0] + Temp[1] + Temp[2] + Temp[3];

		VectorStoreAligned(VecSumGradC2, Temp);
		SumGradC2 = Temp[0] + Temp[1] + Temp[2] + Temp[3];

		VectorStoreAligned(VecGradC_iX, Temp);
		GradC_iX = Temp[0] + Temp[1] + Temp[2] + Temp[3];

		VectorStoreAligned(VecGradC_iY, Temp);
		GradC_iY = Temp[0] + Temp[1] + Temp[2] + Temp[3];

		VectorStoreAligned(VecGradC_iZ, Temp);
		GradC_iZ = Temp[0] + Temp[1] + Temp[2] + Temp[3];

		// Process remaining scalar elements
		for (; n < NumNeighbors; ++n)
		{
			const int32 NeighborIdx = NeighborData[n];
			const float dx = PiX - PosXPtr[NeighborIdx];
			const float dy = PiY - PosYPtr[NeighborIdx];
			const float dz = PiZ - PosZPtr[NeighborIdx];
			const float r2_cm = dx * dx + dy * dy + dz * dz;

			if (r2_cm <= Coeffs.SmoothingRadiusSq)
			{
				const float r2_m = r2_cm * CM_TO_M_SQ;
				const float diff = Coeffs.h2 - r2_m;
				Density += MassPtr[NeighborIdx] * Coeffs.Poly6Coeff * diff * diff * diff;
			}

			if (r2_cm > KINDA_SMALL_NUMBER && r2_cm <= Coeffs.SmoothingRadiusSq)
			{
				const float rLen = FMath::Sqrt(r2_cm);
				const float rLen_m = rLen * CM_TO_M;
				const float diff = Coeffs.h - rLen_m;
				const float coeff = Coeffs.SpikyCoeff * diff * diff * CM_TO_M / rLen;

				const float GradWX = coeff * dx;
				const float GradWY = coeff * dy;
				const float GradWZ = coeff * dz;

				const float GradC_jX = -GradWX * Coeffs.InvRestDensity;
				const float GradC_jY = -GradWY * Coeffs.InvRestDensity;
				const float GradC_jZ = -GradWZ * Coeffs.InvRestDensity;
				SumGradC2 += GradC_jX * GradC_jX + GradC_jY * GradC_jY + GradC_jZ * GradC_jZ;

				GradC_iX += GradWX * Coeffs.InvRestDensity;
				GradC_iY += GradWY * Coeffs.InvRestDensity;
				GradC_iZ += GradWZ * Coeffs.InvRestDensity;
			}
		}

		DensityPtr[i] = Density;

		// Compute Lambda (XPBD)
		const float C_i = (Density * Coeffs.InvRestDensity) - 1.0f;
		if (C_i < 0.0f)
		{
			// No correction in compressed state - preserve Lambda (do not reset to 0)
			return;
		}

		SumGradC2 += GradC_iX * GradC_iX + GradC_iY * GradC_iY + GradC_iZ * GradC_iZ;

		// XPBD: Δλ = (-C - α̃λ_prev) / (|∇C|² + α̃)
		const float Lambda_prev = LambdaPtr[i];
		const float DeltaLambda = (-C_i - Epsilon * Lambda_prev) / (SumGradC2 + Epsilon);
		LambdaPtr[i] = Lambda_prev + DeltaLambda;

	}, EParallelForFlags::Unbalanced);
}

//========================================
// Step 2: DeltaP (SIMD) - with Tensile Instability (scorr) Correction
//========================================
void FDensityConstraint::ComputeDeltaP_SIMD(
	const TArray<FFluidParticle>& Particles,
	const FSPHKernelCoeffs& Coeffs)
{
	const int32 NumParticles = Particles.Num();

	const float* RESTRICT PosXPtr = PosX.GetData();
	const float* RESTRICT PosYPtr = PosY.GetData();
	const float* RESTRICT PosZPtr = PosZ.GetData();
	const float* RESTRICT LambdaPtr = Lambdas.GetData();
	float* RESTRICT DeltaPXPtr = DeltaPX.GetData();
	float* RESTRICT DeltaPYPtr = DeltaPY.GetData();
	float* RESTRICT DeltaPZPtr = DeltaPZ.GetData();

	const VectorRegister4Float VecH = VectorSetFloat1(Coeffs.h);
	const VectorRegister4Float VecH2 = VectorSetFloat1(Coeffs.h2);
	const VectorRegister4Float VecCmToM = VectorSetFloat1(CM_TO_M);
	const VectorRegister4Float VecCmToMSq = VectorSetFloat1(CM_TO_M_SQ);
	const VectorRegister4Float VecSpikyCoeff = VectorSetFloat1(Coeffs.SpikyCoeff);
	const VectorRegister4Float VecPoly6Coeff = VectorSetFloat1(Coeffs.Poly6Coeff);
	const VectorRegister4Float VecInvRestDensity = VectorSetFloat1(Coeffs.InvRestDensity);
	const VectorRegister4Float VecSmoothingRadiusSq = VectorSetFloat1(Coeffs.SmoothingRadiusSq);
	const VectorRegister4Float VecZero = VectorZeroFloat();
	const VectorRegister4Float VecMinR2 = VectorSetFloat1(KINDA_SMALL_NUMBER);

	// Tensile Instability parameters
	const bool bUseTensileCorrection = Coeffs.TensileParams.bEnabled && Coeffs.TensileParams.W_DeltaQ > KINDA_SMALL_NUMBER;
	const float TensileK = Coeffs.TensileParams.K;
	const int32 TensileN = Coeffs.TensileParams.N;
	const float InvW_DeltaQ = bUseTensileCorrection ? (1.0f / Coeffs.TensileParams.W_DeltaQ) : 0.0f;
	const VectorRegister4Float VecNegK = VectorSetFloat1(-TensileK);
	const VectorRegister4Float VecInvW_DeltaQ = VectorSetFloat1(InvW_DeltaQ);

	ParallelFor(NumParticles, [&](int32 i)
	{
		const TArray<int32>& Neighbors = Particles[i].NeighborIndices;
		const int32 NumNeighbors = Neighbors.Num();
		const int32* NeighborData = Neighbors.GetData();

		const float PiX = PosXPtr[i];
		const float PiY = PosYPtr[i];
		const float PiZ = PosZPtr[i];
		const float Lambda_i = LambdaPtr[i];

		const VectorRegister4Float VecPiX = VectorSetFloat1(PiX);
		const VectorRegister4Float VecPiY = VectorSetFloat1(PiY);
		const VectorRegister4Float VecPiZ = VectorSetFloat1(PiZ);
		const VectorRegister4Float VecLambda_i = VectorSetFloat1(Lambda_i);
		const VectorRegister4Float VecI = VectorSetFloat1(static_cast<float>(i));

		VectorRegister4Float VecDeltaX = VecZero;
		VectorRegister4Float VecDeltaY = VecZero;
		VectorRegister4Float VecDeltaZ = VecZero;

		int32 n = 0;
		for (; n + 4 <= NumNeighbors; n += 4)
		{
			const int32 n0 = NeighborData[n];
			const int32 n1 = NeighborData[n + 1];
			const int32 n2 = NeighborData[n + 2];
			const int32 n3 = NeighborData[n + 3];

			const VectorRegister4Float VecNeighborIdx = MakeVectorRegisterFloat(
				static_cast<float>(n0), static_cast<float>(n1),
				static_cast<float>(n2), static_cast<float>(n3));
			const VectorRegister4Float VecNotSelfMask = VectorCompareNE(VecNeighborIdx, VecI);

			const VectorRegister4Float VecNX = MakeVectorRegisterFloat(PosXPtr[n0], PosXPtr[n1], PosXPtr[n2], PosXPtr[n3]);
			const VectorRegister4Float VecNY = MakeVectorRegisterFloat(PosYPtr[n0], PosYPtr[n1], PosYPtr[n2], PosYPtr[n3]);
			const VectorRegister4Float VecNZ = MakeVectorRegisterFloat(PosZPtr[n0], PosZPtr[n1], PosZPtr[n2], PosZPtr[n3]);
			const VectorRegister4Float VecLambda_j = MakeVectorRegisterFloat(LambdaPtr[n0], LambdaPtr[n1], LambdaPtr[n2], LambdaPtr[n3]);

			const VectorRegister4Float VecDX = VectorSubtract(VecPiX, VecNX);
			const VectorRegister4Float VecDY = VectorSubtract(VecPiY, VecNY);
			const VectorRegister4Float VecDZ = VectorSubtract(VecPiZ, VecNZ);

			VectorRegister4Float VecR2 = VectorMultiply(VecDX, VecDX);
			VecR2 = VectorMultiplyAdd(VecDY, VecDY, VecR2);
			VecR2 = VectorMultiplyAdd(VecDZ, VecDZ, VecR2);

			VectorRegister4Float VecValidMask = VectorBitwiseAnd(
				VectorCompareGT(VecR2, VecMinR2),
				VectorCompareLT(VecR2, VecSmoothingRadiusSq));
			VecValidMask = VectorBitwiseAnd(VecValidMask, VecNotSelfMask);

			const VectorRegister4Float VecInvRLen = VectorInvSqrtSafe(VecR2);
			const VectorRegister4Float VecRLen = VectorMultiply(VecR2, VecInvRLen);
			const VectorRegister4Float VecRLen_m = VectorMultiply(VecRLen, VecCmToM);
			const VectorRegister4Float VecDiff = VectorSubtract(VecH, VecRLen_m);

			VectorRegister4Float VecCoeff = VectorMultiply(VecSpikyCoeff, VectorMultiply(VecDiff, VecDiff));
			VecCoeff = VectorMultiply(VecCoeff, VectorMultiply(VecCmToM, VecInvRLen));

			VectorRegister4Float VecGradWX = VectorSelect(VecValidMask, VectorMultiply(VecCoeff, VecDX), VecZero);
			VectorRegister4Float VecGradWY = VectorSelect(VecValidMask, VectorMultiply(VecCoeff, VecDY), VecZero);
			VectorRegister4Float VecGradWZ = VectorSelect(VecValidMask, VectorMultiply(VecCoeff, VecDZ), VecZero);

			// Compute Lambda sum (basic)
			VectorRegister4Float VecLambdaSum = VectorAdd(VecLambda_i, VecLambda_j);

			// Add Tensile Instability correction (scorr)
			// PBF Eq.13: scorr = -k * (W(r) / W(Δq))^n
			if (bUseTensileCorrection)
			{
				// W(r) = Poly6(r, h)
				const VectorRegister4Float VecR2_m = VectorMultiply(VecR2, VecCmToMSq);
				VectorRegister4Float VecDiff_Poly6 = VectorSubtract(VecH2, VecR2_m);
				VecDiff_Poly6 = VectorMax(VecDiff_Poly6, VecZero);  // Ensure h² - r² >= 0
				const VectorRegister4Float VecDiff3 = VectorMultiply(VecDiff_Poly6, VectorMultiply(VecDiff_Poly6, VecDiff_Poly6));
				const VectorRegister4Float VecW_r = VectorMultiply(VecPoly6Coeff, VecDiff3);

				// W(r) / W(Δq)
				VectorRegister4Float VecRatio = VectorMultiply(VecW_r, VecInvW_DeltaQ);
				VecRatio = VectorSelect(VecValidMask, VecRatio, VecZero);

				// (W(r) / W(Δq))^n - integer exponentiation in SIMD
				VectorRegister4Float VecRatioPowN = VecRatio;
				for (int32 p = 1; p < TensileN; ++p)
				{
					VecRatioPowN = VectorMultiply(VecRatioPowN, VecRatio);
				}

				// scorr = -k * ratio^n
				const VectorRegister4Float VecScorr = VectorMultiply(VecNegK, VecRatioPowN);
				VecLambdaSum = VectorAdd(VecLambdaSum, VecScorr);
			}

			VecDeltaX = VectorMultiplyAdd(VecLambdaSum, VecGradWX, VecDeltaX);
			VecDeltaY = VectorMultiplyAdd(VecLambdaSum, VecGradWY, VecDeltaY);
			VecDeltaZ = VectorMultiplyAdd(VecLambdaSum, VecGradWZ, VecDeltaZ);
		}

		// Reduce SIMD results
		alignas(16) float Temp[4];
		float DeltaX = 0.0f, DeltaY = 0.0f, DeltaZ = 0.0f;

		VectorStoreAligned(VecDeltaX, Temp);
		DeltaX = Temp[0] + Temp[1] + Temp[2] + Temp[3];

		VectorStoreAligned(VecDeltaY, Temp);
		DeltaY = Temp[0] + Temp[1] + Temp[2] + Temp[3];

		VectorStoreAligned(VecDeltaZ, Temp);
		DeltaZ = Temp[0] + Temp[1] + Temp[2] + Temp[3];

		// Process remaining scalar elements
		for (; n < NumNeighbors; ++n)
		{
			const int32 NeighborIdx = NeighborData[n];
			if (NeighborIdx == i) continue;

			const float dx = PiX - PosXPtr[NeighborIdx];
			const float dy = PiY - PosYPtr[NeighborIdx];
			const float dz = PiZ - PosZPtr[NeighborIdx];
			const float r2_cm = dx * dx + dy * dy + dz * dz;

			if (r2_cm > KINDA_SMALL_NUMBER && r2_cm <= Coeffs.SmoothingRadiusSq)
			{
				const float rLen = FMath::Sqrt(r2_cm);
				const float rLen_m = rLen * CM_TO_M;
				const float diff = Coeffs.h - rLen_m;
				const float coeff = Coeffs.SpikyCoeff * diff * diff * CM_TO_M / rLen;

				float LambdaSum = Lambda_i + LambdaPtr[NeighborIdx];

				// Tensile Instability correction (scorr) - scalar path
				if (bUseTensileCorrection)
				{
					const float r2_m = r2_cm * CM_TO_M_SQ;
					const float diff_poly6 = FMath::Max(0.0f, Coeffs.h2 - r2_m);
					const float diff3 = diff_poly6 * diff_poly6 * diff_poly6;
					const float W_r = Coeffs.Poly6Coeff * diff3;
					const float ratio = W_r * InvW_DeltaQ;
					const float scorr = -TensileK * FMath::Pow(ratio, static_cast<float>(TensileN));
					LambdaSum += scorr;
				}

				DeltaX += LambdaSum * coeff * dx;
				DeltaY += LambdaSum * coeff * dy;
				DeltaZ += LambdaSum * coeff * dz;
			}
		}

		DeltaPXPtr[i] = DeltaX * Coeffs.InvRestDensity;
		DeltaPYPtr[i] = DeltaY * Coeffs.InvRestDensity;
		DeltaPZPtr[i] = DeltaZ * Coeffs.InvRestDensity;

	}, EParallelForFlags::Unbalanced);
}

//========================================
// Legacy Functions (backward compatibility)
//========================================

void FDensityConstraint::ComputeDensities(TArray<FFluidParticle>& Particles)
{
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		Particles[i].Density = ComputeParticleDensity(Particles[i], Particles);
	}, EParallelForFlags::Unbalanced);
}

void FDensityConstraint::ComputeLambdas(TArray<FFluidParticle>& Particles)
{
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		Particles[i].Lambda = ComputeParticleLambda(Particles[i], Particles);
	}, EParallelForFlags::Unbalanced);
}

void FDensityConstraint::ApplyPositionCorrection(TArray<FFluidParticle>& Particles)
{
	TArray<FVector> DeltaPositions;
	DeltaPositions.SetNum(Particles.Num());

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		DeltaPositions[i] = ComputeDeltaPosition(i, Particles);
	}, EParallelForFlags::Unbalanced);

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		Particles[i].PredictedPosition += DeltaPositions[i];
	});
}

float FDensityConstraint::ComputeParticleDensity(const FFluidParticle& Particle, const TArray<FFluidParticle>& Particles)
{
	float Density = 0.0f;
	for (int32 NeighborIdx : Particle.NeighborIndices)
	{
		const FFluidParticle& Neighbor = Particles[NeighborIdx];
		FVector r = Particle.PredictedPosition - Neighbor.PredictedPosition;
		Density += Neighbor.Mass * SPHKernels::Poly6(r, SmoothingRadius);
	}
	return Density;
}

float FDensityConstraint::ComputeParticleLambda(const FFluidParticle& Particle, const TArray<FFluidParticle>& Particles)
{
	float C_i = (Particle.Density / RestDensity) - 1.0f;
	if (C_i < 0.0f) return Particle.Lambda;  // Compressed state: preserve Lambda

	float SumGradC2 = 0.0f;
	FVector GradC_i = FVector::ZeroVector;

	for (int32 NeighborIdx : Particle.NeighborIndices)
	{
		const FFluidParticle& Neighbor = Particles[NeighborIdx];
		FVector r = Particle.PredictedPosition - Neighbor.PredictedPosition;
		FVector GradW = SPHKernels::SpikyGradient(r, SmoothingRadius);

		FVector GradC_j = -GradW / RestDensity;
		SumGradC2 += GradC_j.SizeSquared();
		GradC_i += GradW / RestDensity;
	}

	SumGradC2 += GradC_i.SizeSquared();

	// XPBD: Δλ = (-C - α̃λ_prev) / (|∇C|² + α̃)
	const float Lambda_prev = Particle.Lambda;
	const float DeltaLambda = (-C_i - Epsilon * Lambda_prev) / (SumGradC2 + Epsilon);
	return Lambda_prev + DeltaLambda;
}

FVector FDensityConstraint::ComputeDeltaPosition(int32 ParticleIndex, const TArray<FFluidParticle>& Particles)
{
	const FFluidParticle& Particle = Particles[ParticleIndex];
	FVector DeltaP = FVector::ZeroVector;

	for (int32 NeighborIdx : Particle.NeighborIndices)
	{
		if (NeighborIdx == ParticleIndex) continue;

		const FFluidParticle& Neighbor = Particles[NeighborIdx];
		FVector r = Particle.PredictedPosition - Neighbor.PredictedPosition;
		FVector GradW = SPHKernels::SpikyGradient(r, SmoothingRadius);
		DeltaP += (Particle.Lambda + Neighbor.Lambda) * GradW;
	}

	return DeltaP / RestDensity;
}

void FDensityConstraint::SetRestDensity(float NewRestDensity)
{
	RestDensity = FMath::Max(NewRestDensity, 1.0f);
}

void FDensityConstraint::SetEpsilon(float NewEpsilon)
{
	Epsilon = FMath::Max(NewEpsilon, 0.01f);
}
