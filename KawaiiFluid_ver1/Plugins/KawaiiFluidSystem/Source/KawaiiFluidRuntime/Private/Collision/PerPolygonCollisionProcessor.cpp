// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Collision/PerPolygonCollisionProcessor.h"
#include "Components/FluidInteractionComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Async/ParallelFor.h"

DECLARE_LOG_CATEGORY_EXTERN(LogPerPolygonCollision, Log, All);
DEFINE_LOG_CATEGORY(LogPerPolygonCollision);

FPerPolygonCollisionProcessor::FPerPolygonCollisionProcessor()
	: CollisionMargin(1.0f)
	, Friction(0.1f)
	, Restitution(0.3f)
	, LastProcessedCount(0)
	, LastCollisionCount(0)
	, LastProcessingTimeMs(0.0f)
	, LastBVHUpdateTimeMs(0.0f)
{
}

FPerPolygonCollisionProcessor::~FPerPolygonCollisionProcessor()
{
	ClearBVHCache();
}

void FPerPolygonCollisionProcessor::ClearBVHCache()
{
	BVHCache.Empty();
}

FSkeletalMeshBVH* FPerPolygonCollisionProcessor::GetBVH(UFluidInteractionComponent* Component)
{
	if (!Component)
	{
		return nullptr;
	}

	TWeakObjectPtr<UFluidInteractionComponent> WeakComp(Component);
	TSharedPtr<FSkeletalMeshBVH>* BVHPtr = BVHCache.Find(WeakComp);

	if (BVHPtr && BVHPtr->IsValid())
	{
		return BVHPtr->Get();
	}

	return nullptr;
}

TSharedPtr<FSkeletalMeshBVH> FPerPolygonCollisionProcessor::CreateOrGetBVH(USkeletalMeshComponent* SkelMesh)
{
	if (!SkelMesh)
	{
		return nullptr;
	}

	// Create new BVH
	TSharedPtr<FSkeletalMeshBVH> NewBVH = MakeShared<FSkeletalMeshBVH>();
	if (NewBVH->Initialize(SkelMesh, 0))
	{
		return NewBVH;
	}

	return nullptr;
}

void FPerPolygonCollisionProcessor::UpdateBVHCache(const TArray<UFluidInteractionComponent*>& InteractionComponents)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(PerPolygonCollision_UpdateBVHCache);

	const double StartTime = FPlatformTime::Seconds();

	// Clean up stale entries (components that have been destroyed)
	for (auto It = BVHCache.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// Update/create BVH for each component
	for (UFluidInteractionComponent* Component : InteractionComponents)
	{
		if (!Component || !Component->IsPerPolygonCollisionEnabled())
		{
			continue;
		}

		AActor* Owner = Component->GetOwner();
		if (!Owner)
		{
			continue;
		}

		// Find skeletal mesh component
		USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		if (!SkelMesh)
		{
			continue;
		}

		TWeakObjectPtr<UFluidInteractionComponent> WeakComp(Component);
		TSharedPtr<FSkeletalMeshBVH>* ExistingBVH = BVHCache.Find(WeakComp);

		if (ExistingBVH && ExistingBVH->IsValid())
		{
			// Update existing BVH
			FSkeletalMeshBVH* BVH = ExistingBVH->Get();

			// Check if the skeletal mesh component is still the same
			if (BVH->GetSkeletalMeshComponent() == SkelMesh)
			{
				BVH->UpdateSkinnedPositions();
			}
			else
			{
				// Skeletal mesh changed, reinitialize
				BVH->Initialize(SkelMesh, 0);
			}
		}
		else
		{
			// Create new BVH
			TSharedPtr<FSkeletalMeshBVH> NewBVH = CreateOrGetBVH(SkelMesh);
			if (NewBVH)
			{
				BVHCache.Add(WeakComp, NewBVH);

				// Get BVH bounds for debug
				FBox BVHBounds = NewBVH->GetRootBounds();

				UE_LOG(LogPerPolygonCollision, Warning, TEXT("Created BVH for %s: %d triangles, %d nodes, Bounds Min=(%.1f,%.1f,%.1f) Max=(%.1f,%.1f,%.1f)"),
					*Owner->GetName(),
					NewBVH->GetTriangleCount(),
					NewBVH->GetNodeCount(),
					BVHBounds.Min.X, BVHBounds.Min.Y, BVHBounds.Min.Z,
					BVHBounds.Max.X, BVHBounds.Max.Y, BVHBounds.Max.Z);
			}
			else
			{
				UE_LOG(LogPerPolygonCollision, Error, TEXT("Failed to create BVH for %s"), *Owner->GetName());
			}
		}
	}

	LastBVHUpdateTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
}

void FPerPolygonCollisionProcessor::ProcessCollisions(
	const TArray<FGPUCandidateParticle>& Candidates,
	const TArray<UFluidInteractionComponent*>& InteractionComponents,
	float ParticleRadius,
	TArray<FParticleCorrection>& OutCorrections)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(PerPolygonCollision_ProcessCollisions);

	const double StartTime = FPlatformTime::Seconds();

	OutCorrections.Reset();
	LastCollisionCount = 0;

	if (Candidates.Num() == 0)
	{
		LastProcessedCount = 0;
		LastProcessingTimeMs = 0.0f;
		return;
	}

	// Pre-allocate output array
	OutCorrections.SetNumUninitialized(Candidates.Num());

	// Atomic counter for collision count
	std::atomic<int32> CollisionCount{0};

	// Build lookup arrays for BVH and collision parameters by interaction index
	TArray<FSkeletalMeshBVH*> BVHLookup;
	TArray<float> CollisionMarginLookup;
	TArray<float> FrictionLookup;
	TArray<float> RestitutionLookup;

	BVHLookup.SetNum(InteractionComponents.Num());
	CollisionMarginLookup.SetNum(InteractionComponents.Num());
	FrictionLookup.SetNum(InteractionComponents.Num());
	RestitutionLookup.SetNum(InteractionComponents.Num());

	int32 ValidBVHCount = 0;
	for (int32 i = 0; i < InteractionComponents.Num(); ++i)
	{
		BVHLookup[i] = GetBVH(InteractionComponents[i]);
		if (BVHLookup[i] && BVHLookup[i]->IsValid())
		{
			ValidBVHCount++;
		}

		// Get collision parameters from InteractionComponent (or use defaults)
		if (InteractionComponents[i] && InteractionComponents[i]->IsPerPolygonCollisionEnabled())
		{
			CollisionMarginLookup[i] = InteractionComponents[i]->PerPolygonCollisionMargin;
			FrictionLookup[i] = InteractionComponents[i]->PerPolygonFriction;
			RestitutionLookup[i] = InteractionComponents[i]->PerPolygonRestitution;
		}
		else
		{
			// Defaults
			CollisionMarginLookup[i] = CollisionMargin;
			FrictionLookup[i] = Friction;
			RestitutionLookup[i] = Restitution;
		}
	}

	// DEBUG: Log BVH lookup status
	static int32 BVHLookupDebugCounter = 0;
	if (++BVHLookupDebugCounter % 60 == 1)
	{
		UE_LOG(LogPerPolygonCollision, Warning, TEXT("ProcessCollisions: InteractionComponents=%d, ValidBVHs=%d, Candidates=%d"),
			InteractionComponents.Num(), ValidBVHCount, Candidates.Num());
	}

	// Process particles in parallel
	ParallelFor(Candidates.Num(), [&](int32 CandidateIdx)
	{
		const FGPUCandidateParticle& Candidate = Candidates[CandidateIdx];
		FParticleCorrection& Correction = OutCorrections[CandidateIdx];

		// Initialize correction
		Correction.ParticleIndex = Candidate.ParticleIndex;
		Correction.Flags = FParticleCorrection::FLAG_NONE;
		Correction.VelocityDelta = FVector3f::ZeroVector;
		Correction.PositionDelta = FVector3f::ZeroVector;

		// Validate interaction index
		if (Candidate.InteractionIndex < 0 || Candidate.InteractionIndex >= BVHLookup.Num())
		{
			// DEBUG: Log invalid index (only first few)
			static std::atomic<int32> InvalidIndexCount{0};
			if (InvalidIndexCount.fetch_add(1) < 5)
			{
				UE_LOG(LogPerPolygonCollision, Warning, TEXT("Invalid InteractionIndex: %d (BVHLookup size=%d)"),
					Candidate.InteractionIndex, BVHLookup.Num());
			}
			return;
		}

		FSkeletalMeshBVH* BVH = BVHLookup[Candidate.InteractionIndex];
		if (!BVH || !BVH->IsValid())
		{
			// DEBUG: Log null BVH (only first few)
			static std::atomic<int32> NullBVHCount{0};
			if (NullBVHCount.fetch_add(1) < 5)
			{
				UE_LOG(LogPerPolygonCollision, Warning, TEXT("Null or invalid BVH at InteractionIndex: %d"),
					Candidate.InteractionIndex);
			}
			return;
		}

		// Get per-component collision parameters
		const float CompCollisionMargin = CollisionMarginLookup[Candidate.InteractionIndex];
		const float CompFriction = FrictionLookup[Candidate.InteractionIndex];
		const float CompRestitution = RestitutionLookup[Candidate.InteractionIndex];

		// Process collision with component-specific parameters
		if (ProcessSingleParticle(Candidate, BVH, ParticleRadius, CompCollisionMargin, CompFriction, CompRestitution, Correction))
		{
			CollisionCount.fetch_add(1, std::memory_order_relaxed);
		}
	});

	// Remove empty corrections to reduce GPU upload size
	OutCorrections.RemoveAllSwap([](const FParticleCorrection& C)
	{
		return C.Flags == FParticleCorrection::FLAG_NONE;
	});

	LastProcessedCount = Candidates.Num();
	LastCollisionCount = CollisionCount.load();
	LastProcessingTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (LastCollisionCount > 0)
	{
		UE_LOG(LogPerPolygonCollision, Verbose, TEXT("Processed %d candidates, %d collisions in %.2fms"),
			LastProcessedCount, LastCollisionCount, LastProcessingTimeMs);
	}
}

bool FPerPolygonCollisionProcessor::ProcessSingleParticle(
	const FGPUCandidateParticle& Candidate,
	FSkeletalMeshBVH* BVH,
	float ParticleRadius,
	float InCollisionMargin,
	float InFriction,
	float InRestitution,
	FParticleCorrection& OutCorrection)
{
	const FVector Position(Candidate.Position);
	const FVector Velocity(Candidate.Velocity);

	// Query BVH for nearby triangles
	TArray<int32> NearbyTriangles;
	const float SearchRadius = ParticleRadius * 2.0f + InCollisionMargin;
	BVH->QuerySphere(Position, SearchRadius, NearbyTriangles);

	// DEBUG: Log first particle's query results
	static int32 DebugSingleCounter = 0;
	if (++DebugSingleCounter % 1000 == 1)
	{
		UE_LOG(LogPerPolygonCollision, Warning,
			TEXT("ProcessSingle DEBUG: Pos=(%.1f,%.1f,%.1f), SearchRadius=%.1f, NearbyTris=%d, BVH TriCount=%d"),
			Position.X, Position.Y, Position.Z,
			SearchRadius, NearbyTriangles.Num(), BVH->GetTriangleCount());
	}

	if (NearbyTriangles.Num() == 0)
	{
		return false;
	}

	// Find closest triangle
	float MinDistance = FLT_MAX;
	FVector ClosestPoint = FVector::ZeroVector;
	FVector ClosestNormal = FVector::UpVector;
	const TArray<FSkinnedTriangle>& Triangles = BVH->GetTriangles();

	for (int32 TriIdx : NearbyTriangles)
	{
		if (!Triangles.IsValidIndex(TriIdx))
		{
			continue;
		}

		const FSkinnedTriangle& Tri = Triangles[TriIdx];
		const FVector TriClosestPoint = FSkeletalMeshBVH::ClosestPointOnTriangle(
			Position, Tri.V0, Tri.V1, Tri.V2);
		const float Distance = FVector::Dist(Position, TriClosestPoint);

		if (Distance < MinDistance)
		{
			MinDistance = Distance;
			ClosestPoint = TriClosestPoint;
			ClosestNormal = Tri.Normal;
		}
	}

	// Check for collision
	const float EffectiveRadius = ParticleRadius + InCollisionMargin;

	// DEBUG: Log distance check
	static int32 DistanceDebugCounter = 0;
	if (++DistanceDebugCounter % 500 == 1)
	{
		UE_LOG(LogPerPolygonCollision, Warning,
			TEXT("Distance DEBUG: MinDist=%.2f, EffectiveRadius=%.2f, ParticleRadius=%.2f, CollisionMargin=%.2f, Collides=%s"),
			MinDistance, EffectiveRadius, ParticleRadius, InCollisionMargin,
			MinDistance < EffectiveRadius ? TEXT("YES") : TEXT("NO"));
	}

	if (MinDistance < EffectiveRadius)
	{
		// Compute penetration depth
		const float Penetration = EffectiveRadius - MinDistance;

		// Compute correction direction
		FVector CorrectionDir = Position - ClosestPoint;
		if (CorrectionDir.IsNearlyZero())
		{
			CorrectionDir = ClosestNormal;
		}
		else
		{
			CorrectionDir.Normalize();
		}

		// Make sure correction pushes particle out (not into) the surface
		if (FVector::DotProduct(CorrectionDir, ClosestNormal) < 0.0f)
		{
			CorrectionDir = ClosestNormal;
		}

		// Compute position correction
		// 딱 표면까지만 밀어내기 (Penetration) + 작은 버퍼
		// 너무 크면 진동, 너무 작으면 침투
		const float CorrectionBuffer = FMath::Min(ParticleRadius * 0.15f, 1.0f);  // 최대 1cm 버퍼
		const float CorrectionMagnitude = Penetration + CorrectionBuffer;
		const FVector PositionCorrection = CorrectionDir * CorrectionMagnitude;

		OutCorrection.PositionDelta = FVector3f(PositionCorrection);
		OutCorrection.Flags = FParticleCorrection::FLAG_COLLIDED;

		// ========================================
		// Compute velocity correction (reflection + damping)
		// ========================================
		const float VelDotNormal = FVector::DotProduct(Velocity, ClosestNormal);

		// Only reflect if moving into the surface
		if (VelDotNormal < 0.0f)
		{
			// Decompose velocity into normal and tangent components
			const FVector VelNormal = ClosestNormal * VelDotNormal;
			const FVector VelTangent = Velocity - VelNormal;

			// Reflect normal component with restitution (bounce)
			// Dampen tangent component with friction
			const FVector NewVelocity = VelTangent * (1.0f - InFriction) - VelNormal * InRestitution;
			const FVector VelocityCorrection = NewVelocity - Velocity;

			OutCorrection.VelocityDelta = FVector3f(VelocityCorrection);
			OutCorrection.Flags |= FParticleCorrection::FLAG_VELOCITY_CORRECTED;

			// DEBUG: Log velocity correction
			static int32 VelCorrectionDebugCounter = 0;
			if (++VelCorrectionDebugCounter % 100 == 1)
			{
				UE_LOG(LogPerPolygonCollision, Warning,
					TEXT("VelCorrection DEBUG: ParticleIdx=%d, OldVel=(%.1f,%.1f,%.1f), VelDotN=%.1f, VelDelta=(%.1f,%.1f,%.1f)"),
					OutCorrection.ParticleIndex,
					Velocity.X, Velocity.Y, Velocity.Z,
					VelDotNormal,
					VelocityCorrection.X, VelocityCorrection.Y, VelocityCorrection.Z);
			}
		}

		// DEBUG: Log position correction
		static int32 CorrectionDebugCounter = 0;
		if (++CorrectionDebugCounter % 100 == 1)
		{
			UE_LOG(LogPerPolygonCollision, Warning,
				TEXT("Correction DEBUG: ParticleIdx=%d, Penetration=%.2f, CorrectionMag=%.2f, PosDelta=(%.2f,%.2f,%.2f)"),
				OutCorrection.ParticleIndex, Penetration, CorrectionMagnitude,
				PositionCorrection.X, PositionCorrection.Y, PositionCorrection.Z);
		}

		return true;
	}

	return false;
}
