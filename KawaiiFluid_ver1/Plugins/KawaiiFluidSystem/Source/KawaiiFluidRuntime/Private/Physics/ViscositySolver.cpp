// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Physics/ViscositySolver.h"
#include "Physics/SPHKernels.h"

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

	// [개선 1] 커널 계수 캐싱 - 프레임당 한 번만 계산
	SPHKernels::FKernelCoefficients KernelCoeffs;
	KernelCoeffs.Precompute(SmoothingRadius);

	// 반경 제곱 (sqrt 호출 회피용)
	const float RadiusSquared = SmoothingRadius * SmoothingRadius;

	// 임시 배열에 새 속도 저장
	TArray<FVector> NewVelocities;
	NewVelocities.SetNum(ParticleCount);

	// [개선 4] 작업량 균등화 - Unbalanced 플래그로 이웃 수 편차 대응
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

			// [개선 2] 반경 기반 필터링 - r² > h² 이면 조기 스킵 (sqrt 회피)
			const float rSquared = r.SizeSquared();
			if (rSquared > RadiusSquared)
			{
				continue;
			}

			// [개선 1] 캐싱된 계수로 Poly6 직접 계산
			// W(r, h) = Poly6Coeff * (h² - r²)³
			// 단위 변환: cm -> m (계수는 이미 m 단위로 계산됨)
			const float h2_m = KernelCoeffs.h2;
			const float r2_m = rSquared * ViscosityConstants::CM_TO_M_SQ;
			const float diff = h2_m - r2_m;
			const float Weight = (diff > 0.0f) ? KernelCoeffs.Poly6Coeff * diff * diff * diff : 0.0f;

			// 속도 차이
			const FVector VelocityDiff = Neighbor.Velocity - Particle.Velocity;

			VelocityCorrection += VelocityDiff * Weight;
			WeightSum += Weight;
		}

		// 정규화 (선택적)
		if (WeightSum > 0.0f)
		{
			VelocityCorrection /= WeightSum;
		}

		// XSPH 점성 적용: v_new = v + c * Σ(v_j - v_i) * W
		NewVelocities[i] = Particle.Velocity + ViscosityCoeff * VelocityCorrection;

	}, EParallelForFlags::Unbalanced);

	// [개선 3] 속도 적용 루프 단순화 - ParallelFor 대신 단순 for 루프
	// 단순 복사 작업은 스케줄러 오버헤드가 더 큼
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

		// 변위
		float Displacement = CurrentLength - Spring.RestLength;

		// 스프링 힘: F = -k * x
		FVector Force = SpringStiffness * Displacement * (Delta / CurrentLength);

		// 속도에 힘 적용 (질량으로 나눔)
		ParticleA.Velocity -= Force * DeltaTime / ParticleA.Mass;
		ParticleB.Velocity += Force * DeltaTime / ParticleB.Mass;
	}
}

void FViscositySolver::UpdateSprings(const TArray<FFluidParticle>& Particles, float SmoothingRadius)
{
	// 기존 스프링 중 유효한 것만 유지
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

		// 너무 멀어지면 스프링 끊기
		return Distance > SmoothingRadius * 2.0f;
	});

	// 새 스프링 추가 (가까운 이웃들 사이)
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

			// 스프링 생성 조건
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
