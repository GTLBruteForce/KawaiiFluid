// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Physics/DensityConstraint.h"
#include "Physics/SPHKernels.h"

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

void FDensityConstraint::Solve(TArray<FFluidParticle>& Particles, float InSmoothingRadius, float InRestDensity, float InEpsilon)
{
	SmoothingRadius = InSmoothingRadius;
	RestDensity = InRestDensity;
	Epsilon = InEpsilon;

	// 1. 밀도 계산
	ComputeDensities(Particles);

	// 2. Lambda 계산
	ComputeLambdas(Particles);

	// 3. 위치 보정
	ApplyPositionCorrection(Particles);

	// 디버그 출력 (첫 번째 입자)
	static int32 DebugCounter = 0;
	if (++DebugCounter % 60 == 0 && Particles.Num() > 0)
	{
		//UE_LOG(LogTemp, Warning, TEXT("[PBF Debug] Particles=%d, Neighbors=%d, Density=%.6f, RestDensity=%.2f, Lambda=%.6f, h=%.2f, Epsilon=%.2f"),
		//	Particles.Num(),
		//	Particles[0].NeighborIndices.Num(),
		//	Particles[0].Density,
		//	RestDensity,
		//	Particles[0].Lambda,
		//	SmoothingRadius,
		//	Epsilon);
	}
}

void FDensityConstraint::ComputeDensities(TArray<FFluidParticle>& Particles)
{
	for (FFluidParticle& Particle : Particles)
	{
		Particle.Density = ComputeParticleDensity(Particle, Particles);
	}
}

void FDensityConstraint::ComputeLambdas(TArray<FFluidParticle>& Particles)
{
	for (FFluidParticle& Particle : Particles)
	{
		Particle.Lambda = ComputeParticleLambda(Particle, Particles);
	}
}

void FDensityConstraint::ApplyPositionCorrection(TArray<FFluidParticle>& Particles)
{
	// 임시 배열에 보정량 저장 (동시 업데이트 방지)
	TArray<FVector> DeltaPositions;
	DeltaPositions.SetNum(Particles.Num());

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		DeltaPositions[i] = ComputeDeltaPosition(i, Particles);
	}

	// 보정 적용
	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		Particles[i].PredictedPosition += DeltaPositions[i];
	}
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
	// 제약 조건: C_i = (ρ_i / ρ_0) - 1
	float C_i = (Particle.Density / RestDensity) - 1.0f;

	// C_i >= 0 이면 압축 상태 (보정 필요)
	// C_i < 0 이면 팽창 상태 (허용 - 비압축성 유체에서)
	if (C_i < 0.0f)
	{
		return 0.0f;
	}

	// 그래디언트 제곱합 계산
	float SumGradC2 = 0.0f;
	FVector GradC_i = FVector::ZeroVector;

	for (int32 NeighborIdx : Particle.NeighborIndices)
	{
		const FFluidParticle& Neighbor = Particles[NeighborIdx];
		FVector r = Particle.PredictedPosition - Neighbor.PredictedPosition;

		// Spiky 커널 그래디언트
		FVector GradW = SPHKernels::SpikyGradient(r, SmoothingRadius);

		// 이웃 j에 대한 제약 그래디언트
		FVector GradC_j = -GradW / RestDensity;
		SumGradC2 += GradC_j.SizeSquared();

		// 자신에 대한 그래디언트 누적
		GradC_i += GradW / RestDensity;
	}

	// 자신의 그래디언트 추가
	SumGradC2 += GradC_i.SizeSquared();

	// Lambda 계산: λ = -C / (Σ|∇C|² + ε)
	return -C_i / (SumGradC2 + Epsilon);
}

FVector FDensityConstraint::ComputeDeltaPosition(int32 ParticleIndex, const TArray<FFluidParticle>& Particles)
{
	const FFluidParticle& Particle = Particles[ParticleIndex];
	FVector DeltaP = FVector::ZeroVector;

	for (int32 NeighborIdx : Particle.NeighborIndices)
	{
		if (NeighborIdx == ParticleIndex)
		{
			continue;
		}

		const FFluidParticle& Neighbor = Particles[NeighborIdx];
		FVector r = Particle.PredictedPosition - Neighbor.PredictedPosition;

		// Spiky 커널 그래디언트
		FVector GradW = SPHKernels::SpikyGradient(r, SmoothingRadius);

		// 위치 보정: Δp = (1/ρ_0) * Σ(λ_i + λ_j) * ∇W
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
