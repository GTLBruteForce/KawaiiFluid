// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Collision/FluidCollider.h"
#include "Async/ParallelFor.h"

UFluidCollider::UFluidCollider()
{
	PrimaryComponentTick.bCanEverTick = false;

	bColliderEnabled = true;
	Friction = 0.3f;
	Restitution = 0.2f;
	bAllowAdhesion = true;
	AdhesionMultiplier = 1.0f;
}

void UFluidCollider::BeginPlay()
{
	Super::BeginPlay();
}

void UFluidCollider::ResolveCollisions(TArray<FFluidParticle>& Particles)
{
	if (!bColliderEnabled)
	{
		return;
	}

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		ResolveParticleCollision(Particles[i]);
	});
}

bool UFluidCollider::GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const
{
	return false;
}

bool UFluidCollider::GetClosestPointWithBone(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance, FName& OutBoneName, FTransform& OutBoneTransform) const
{
	// 기본 구현: 본 정보 없음
	OutBoneName = NAME_None;
	OutBoneTransform = FTransform::Identity;
	return GetClosestPoint(Point, OutClosestPoint, OutNormal, OutDistance);
}

bool UFluidCollider::IsPointInside(const FVector& Point) const
{
	return false;
}

void UFluidCollider::ResolveParticleCollision(FFluidParticle& Particle)
{
	FVector ClosestPoint;
	FVector Normal;
	float Distance;

	if (!GetClosestPoint(Particle.PredictedPosition, ClosestPoint, Normal, Distance))
	{
		return;
	}

	// 충돌 마진: 입자가 표면에 가까워지면 미리 충돌 처리 (터널링 방지)
	const float CollisionMargin = 5.0f;  // 5cm 마진

	if (Distance <= CollisionMargin)
	{
		// 표면 바깥쪽으로 밀어냄
		FVector CollisionPos = ClosestPoint + Normal * (CollisionMargin + 0.01f);

		// Position도 함께 업데이트 (튀어오름 방지)
		Particle.PredictedPosition = CollisionPos;
		Particle.Position = CollisionPos;

		// 점성 유체: 수직 성분 제거 (표면에 달라붙음)
		float VelDotNormal = FVector::DotProduct(Particle.Velocity, Normal);

		if (VelDotNormal < 0.0f)
		{
			Particle.Velocity -= VelDotNormal * Normal;
		}
	}
}
