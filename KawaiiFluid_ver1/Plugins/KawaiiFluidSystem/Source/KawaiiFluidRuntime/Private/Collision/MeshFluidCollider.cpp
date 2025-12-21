// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Collision/MeshFluidCollider.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

UMeshFluidCollider::UMeshFluidCollider()
{
	TargetMeshComponent = nullptr;
	bAutoFindMesh = true;
	bUseSimplifiedCollision = true;
	CollisionMargin = 1.0f;
}

void UMeshFluidCollider::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoFindMesh)
	{
		AutoFindMeshComponent();
	}
}

void UMeshFluidCollider::AutoFindMeshComponent()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// 1순위: SkeletalMeshComponent (PhysicsAsset 기반 정밀 충돌)
	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (SkelMesh)
	{
		TargetMeshComponent = SkelMesh;

		// 디버그: PhysicsAsset 정보 출력
		UPhysicsAsset* PhysAsset = SkelMesh->GetPhysicsAsset();
		if (PhysAsset)
		{
			int32 TotalCapsules = 0;
			for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
			{
				if (BodySetup)
				{
					TotalCapsules += BodySetup->AggGeom.SphylElems.Num();
				}
			}
			//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: Found SkeletalMesh with PhysicsAsset '%s', Bodies: %d, Total Capsules: %d"),*PhysAsset->GetName(), PhysAsset->SkeletalBodySetups.Num(), TotalCapsules);
		}
		else
		{
			//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: Found SkeletalMesh but NO PhysicsAsset!"));
		}
		return;
	}

	// 2순위: CapsuleComponent (단순 캡슐 충돌)
	UCapsuleComponent* Capsule = Owner->FindComponentByClass<UCapsuleComponent>();
	if (Capsule)
	{
		TargetMeshComponent = Capsule;
		//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: Using CapsuleComponent"));
		return;
	}

	// 3순위: StaticMeshComponent
	UStaticMeshComponent* StaticMesh = Owner->FindComponentByClass<UStaticMeshComponent>();
	if (StaticMesh)
	{
		TargetMeshComponent = StaticMesh;
		//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: Using StaticMeshComponent"));
		return;
	}

	//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: No mesh component found!"));
}

bool UMeshFluidCollider::GetClosestPoint(const FVector& Point, FVector& OutClosestPoint, FVector& OutNormal, float& OutDistance) const
{
	if (!TargetMeshComponent)
	{
		return false;
	}

	// CapsuleComponent인 경우
	UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(TargetMeshComponent);
	if (Capsule)
	{
		FVector CapsuleCenter = Capsule->GetComponentLocation();
		float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		FVector CapsuleUp = Capsule->GetUpVector();

		FVector ToPoint = Point - CapsuleCenter;
		float AxisProjection = FVector::DotProduct(ToPoint, CapsuleUp);

		float ClampedProjection = FMath::Clamp(AxisProjection, -CapsuleHalfHeight + CapsuleRadius, CapsuleHalfHeight - CapsuleRadius);
		FVector ClosestOnAxis = CapsuleCenter + CapsuleUp * ClampedProjection;

		FVector RadialVector = Point - ClosestOnAxis;
		float RadialDistance = RadialVector.Size();

		if (RadialDistance < KINDA_SMALL_NUMBER)
		{
			OutNormal = FVector::ForwardVector;
			OutClosestPoint = ClosestOnAxis + OutNormal * CapsuleRadius;
			OutDistance = CapsuleRadius;
		}
		else
		{
			OutNormal = RadialVector / RadialDistance;
			OutClosestPoint = ClosestOnAxis + OutNormal * CapsuleRadius;
			OutDistance = RadialDistance - CapsuleRadius;
		}

		return true;
	}

	// SkeletalMeshComponent인 경우 - PhysicsAsset 사용
	USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(TargetMeshComponent);
	if (SkelMesh)
	{
		UPhysicsAsset* PhysAsset = SkelMesh->GetPhysicsAsset();
		if (PhysAsset)
		{
			bool bFoundAny = false;
			float MinDistance = TNumericLimits<float>::Max();

			for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
			{
				if (!BodySetup)
				{
					continue;
				}

				int32 BoneIndex = SkelMesh->GetBoneIndex(BodySetup->BoneName);
				if (BoneIndex == INDEX_NONE)
				{
					continue;
				}

				FTransform BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);

				// PhysicsAsset의 Sphyl(캡슐) 요소들 처리
				for (const FKSphylElem& SphylElem : BodySetup->AggGeom.SphylElems)
				{
					FTransform CapsuleLocalTransform = SphylElem.GetTransform();
					FTransform CapsuleWorldTransform = CapsuleLocalTransform * BoneTransform;

					FVector CapsuleCenter = CapsuleWorldTransform.GetLocation();
					float CapsuleRadius = SphylElem.Radius + CollisionMargin;
					float CapsuleLength = SphylElem.Length;
					FVector CapsuleUp = CapsuleWorldTransform.GetRotation().GetUpVector();

					float HalfLength = CapsuleLength * 0.5f;
					FVector CapsuleStart = CapsuleCenter - CapsuleUp * HalfLength;
					FVector CapsuleEnd = CapsuleCenter + CapsuleUp * HalfLength;

					// 캡슐 축에서 가장 가까운 점 찾기
					FVector SegmentDir = CapsuleEnd - CapsuleStart;
					float SegmentLengthSq = SegmentDir.SizeSquared();

					FVector ClosestOnAxis;
					if (SegmentLengthSq < KINDA_SMALL_NUMBER)
					{
						ClosestOnAxis = CapsuleStart;
					}
					else
					{
						float t = FVector::DotProduct(Point - CapsuleStart, SegmentDir) / SegmentLengthSq;
						t = FMath::Clamp(t, 0.0f, 1.0f);
						ClosestOnAxis = CapsuleStart + SegmentDir * t;
					}

					FVector ToPointVec = Point - ClosestOnAxis;
					float DistToAxis = ToPointVec.Size();

					FVector TempNormal;
					FVector TempClosestPoint;
					float TempDistance;

					if (DistToAxis < KINDA_SMALL_NUMBER)
					{
						TempNormal = FVector::ForwardVector;
						TempClosestPoint = ClosestOnAxis + TempNormal * CapsuleRadius;
						TempDistance = -CapsuleRadius;
					}
					else
					{
						TempNormal = ToPointVec / DistToAxis;
						TempClosestPoint = ClosestOnAxis + TempNormal * CapsuleRadius;
						TempDistance = DistToAxis - CapsuleRadius;
					}

					if (TempDistance < MinDistance)
					{
						MinDistance = TempDistance;
						OutClosestPoint = TempClosestPoint;
						OutNormal = TempNormal;
						OutDistance = TempDistance;
						bFoundAny = true;
					}
				}

				// PhysicsAsset의 Sphere 요소들 처리
				for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
				{
					FTransform SphereLocalTransform = SphereElem.GetTransform();
					FTransform SphereWorldTransform = SphereLocalTransform * BoneTransform;

					FVector SphereCenter = SphereWorldTransform.GetLocation();
					float SphereRadius = SphereElem.Radius + CollisionMargin;

					FVector ToPointVec = Point - SphereCenter;
					float DistToCenter = ToPointVec.Size();

					FVector TempNormal;
					FVector TempClosestPoint;
					float TempDistance;

					if (DistToCenter < KINDA_SMALL_NUMBER)
					{
						TempNormal = FVector::UpVector;
						TempClosestPoint = SphereCenter + TempNormal * SphereRadius;
						TempDistance = -SphereRadius;
					}
					else
					{
						TempNormal = ToPointVec / DistToCenter;
						TempClosestPoint = SphereCenter + TempNormal * SphereRadius;
						TempDistance = DistToCenter - SphereRadius;
					}

					if (TempDistance < MinDistance)
					{
						MinDistance = TempDistance;
						OutClosestPoint = TempClosestPoint;
						OutNormal = TempNormal;
						OutDistance = TempDistance;
						bFoundAny = true;
					}
				}
			}

			if (bFoundAny)
			{
				// 디버그: 가장 가까운 캡슐까지의 거리 (가끔 출력)
				static int32 DebugCounter = 0;
				if (++DebugCounter % 1000 == 0)
				{
					//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: Closest distance = %.2f"), MinDistance);
				}
				return true;
			}
			else
			{
				// 캡슐을 찾지 못함
				static int32 DebugCounter2 = 0;
				if (++DebugCounter2 % 1000 == 0)
				{
					//UE_LOG(LogTemp, Warning, TEXT("MeshFluidCollider: No capsules found in GetClosestPoint!"));
				}
			}
		}
	}

	// 폴백: 바운딩 박스 사용
	FBoxSphereBounds Bounds = TargetMeshComponent->Bounds;
	FVector BoxCenter = Bounds.Origin;
	FVector BoxExtent = Bounds.BoxExtent;

	FVector LocalPoint = Point - BoxCenter;
	FVector ClampedPoint;
	ClampedPoint.X = FMath::Clamp(LocalPoint.X, -BoxExtent.X, BoxExtent.X);
	ClampedPoint.Y = FMath::Clamp(LocalPoint.Y, -BoxExtent.Y, BoxExtent.Y);
	ClampedPoint.Z = FMath::Clamp(LocalPoint.Z, -BoxExtent.Z, BoxExtent.Z);

	OutClosestPoint = BoxCenter + ClampedPoint;
	FVector ToPointVec = Point - OutClosestPoint;
	OutDistance = ToPointVec.Size();
	OutNormal = OutDistance > KINDA_SMALL_NUMBER ? ToPointVec / OutDistance : FVector::UpVector;

	return true;
}

bool UMeshFluidCollider::IsPointInside(const FVector& Point) const
{
	if (!TargetMeshComponent)
	{
		return false;
	}

	// CapsuleComponent인 경우
	UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(TargetMeshComponent);
	if (Capsule)
	{
		FVector CapsuleCenter = Capsule->GetComponentLocation();
		float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		FVector CapsuleUp = Capsule->GetUpVector();

		FVector ToPoint = Point - CapsuleCenter;
		float AxisProjection = FVector::DotProduct(ToPoint, CapsuleUp);

		if (FMath::Abs(AxisProjection) > CapsuleHalfHeight)
		{
			FVector SphereCenter = CapsuleCenter + CapsuleUp * FMath::Sign(AxisProjection) * (CapsuleHalfHeight - CapsuleRadius);
			return FVector::DistSquared(Point, SphereCenter) <= CapsuleRadius * CapsuleRadius;
		}
		else
		{
			FVector ClosestOnAxis = CapsuleCenter + CapsuleUp * AxisProjection;
			return FVector::DistSquared(Point, ClosestOnAxis) <= CapsuleRadius * CapsuleRadius;
		}
	}

	// SkeletalMeshComponent인 경우 - PhysicsAsset 사용
	USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(TargetMeshComponent);
	if (SkelMesh)
	{
		UPhysicsAsset* PhysAsset = SkelMesh->GetPhysicsAsset();
		if (PhysAsset)
		{
			for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
			{
				if (!BodySetup)
				{
					continue;
				}

				int32 BoneIndex = SkelMesh->GetBoneIndex(BodySetup->BoneName);
				if (BoneIndex == INDEX_NONE)
				{
					continue;
				}

				FTransform BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);

				// PhysicsAsset의 Sphyl(캡슐) 요소들 처리
				for (const FKSphylElem& SphylElem : BodySetup->AggGeom.SphylElems)
				{
					FTransform CapsuleLocalTransform = SphylElem.GetTransform();
					FTransform CapsuleWorldTransform = CapsuleLocalTransform * BoneTransform;

					FVector CapsuleCenter = CapsuleWorldTransform.GetLocation();
					float CapsuleRadius = SphylElem.Radius + CollisionMargin;
					float CapsuleLength = SphylElem.Length;
					FVector CapsuleUp = CapsuleWorldTransform.GetRotation().GetUpVector();

					float HalfLength = CapsuleLength * 0.5f;
					FVector CapsuleStart = CapsuleCenter - CapsuleUp * HalfLength;
					FVector CapsuleEnd = CapsuleCenter + CapsuleUp * HalfLength;

					// 캡슐 축에서 가장 가까운 점 찾기
					FVector SegmentDir = CapsuleEnd - CapsuleStart;
					float SegmentLengthSq = SegmentDir.SizeSquared();

					FVector ClosestOnAxis;
					if (SegmentLengthSq < KINDA_SMALL_NUMBER)
					{
						ClosestOnAxis = CapsuleStart;
					}
					else
					{
						float t = FVector::DotProduct(Point - CapsuleStart, SegmentDir) / SegmentLengthSq;
						t = FMath::Clamp(t, 0.0f, 1.0f);
						ClosestOnAxis = CapsuleStart + SegmentDir * t;
					}

					float DistSq = FVector::DistSquared(Point, ClosestOnAxis);
					if (DistSq <= CapsuleRadius * CapsuleRadius)
					{
						return true;
					}
				}

				// PhysicsAsset의 Sphere 요소들 처리
				for (const FKSphereElem& SphereElem : BodySetup->AggGeom.SphereElems)
				{
					FTransform SphereLocalTransform = SphereElem.GetTransform();
					FTransform SphereWorldTransform = SphereLocalTransform * BoneTransform;

					FVector SphereCenter = SphereWorldTransform.GetLocation();
					float SphereRadius = SphereElem.Radius + CollisionMargin;

					float DistSq = FVector::DistSquared(Point, SphereCenter);
					if (DistSq <= SphereRadius * SphereRadius)
					{
						return true;
					}
				}
			}

			return false;
		}
	}

	// 폴백: 바운딩 박스 사용
	FBoxSphereBounds Bounds = TargetMeshComponent->Bounds;
	return Bounds.GetBox().IsInside(Point);
}
