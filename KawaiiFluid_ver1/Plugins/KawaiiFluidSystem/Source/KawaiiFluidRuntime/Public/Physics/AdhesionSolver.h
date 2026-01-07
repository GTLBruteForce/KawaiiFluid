// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/FluidParticle.h"

class UFluidCollider;

/**
 * 접착력 솔버
 *
 * Akinci et al. 2013 "Versatile Surface Tension and Adhesion for SPH Fluids" 기반
 * 유체 입자가 표면(캐릭터, 벽 등)에 달라붙는 효과 구현
 */
class KAWAIIFLUIDRUNTIME_API FAdhesionSolver
{
public:
	FAdhesionSolver();

	/**
	 * 접착력 적용
	 *
	 * @param Particles 입자 배열
	 * @param Colliders 콜라이더 목록 (접착 대상)
	 * @param AdhesionStrength 접착 강도 (0.0 ~ 1.0)
	 * @param AdhesionRadius 접착 범위
	 * @param DetachThreshold 분리 임계값
	 * @param ColliderContactOffset 접촉 거리 보정값 (양수면 더 깊이 겹침 허용)
	 */
	void Apply(
		TArray<FFluidParticle>& Particles,
		const TArray<UFluidCollider*>& Colliders,
		float AdhesionStrength,
		float AdhesionRadius,
		float DetachThreshold,
		float ColliderContactOffset
	);

	/**
	 * 입자 간 응집력 (표면 장력) 적용
	 *
	 * @param Particles 입자 배열
	 * @param CohesionStrength 응집 강도
	 * @param SmoothingRadius 커널 반경
	 */
	void ApplyCohesion(
		TArray<FFluidParticle>& Particles,
		float CohesionStrength,
		float SmoothingRadius
	);

private:
	/**
	 * 경계 표면과의 접착력 계산
	 */
	FVector ComputeAdhesionForce(
		const FVector& ParticlePos,
		const FVector& SurfacePoint,
		const FVector& SurfaceNormal,
		float Distance,
		float AdhesionStrength,
		float AdhesionRadius
	);

	/**
	 * 접착 상태 업데이트
	 */
	void UpdateAttachmentState(
		FFluidParticle& Particle,
		AActor* Collider,
		float Force,
		float DetachThreshold,
		FName BoneName,
		const FTransform& BoneTransform,
		const FVector& ParticlePosition,
		const FVector& SurfaceNormal
	);
};
