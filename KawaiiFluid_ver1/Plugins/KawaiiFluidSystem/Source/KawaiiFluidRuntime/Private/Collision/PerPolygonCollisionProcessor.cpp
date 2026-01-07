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
	, DetachAccelerationThreshold(5000.0f)  // cm/s²
	, MinAdhesionForAttachment(0.3f)
	, GravityDetachInfluence(0.5f)
	, GravityVector(FVector(0.0f, 0.0f, -980.0f))
	, LastProcessedCount(0)
	, LastCollisionCount(0)
	, LastAttachmentCount(0)
	, LastDetachmentCount(0)
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
	float AdhesionStrength,
	float ContactOffset,
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

	// Get world time for attachment tracking
	const float WorldTime = static_cast<float>(FPlatformTime::Seconds());

	// Collect new attachments (use mutex instead of thread-local for simplicity)
	struct FNewAttachmentData
	{
		uint32 ParticleIndex;
		int32 InteractionIndex;
		int32 TriangleIndex;
		FVector ClosestPoint;
	};
	TArray<FNewAttachmentData> NewAttachments;
	FCriticalSection NewAttachmentsLock;

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

		// Skip already attached particles (they are updated via UpdateAttachedParticles)
		if (AttachedParticles.Contains(Candidate.ParticleIndex))
		{
			return;
		}

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

		// Process collision with component-specific parameters + fluid adhesion
		int32 CollisionTriangleIndex = INDEX_NONE;
		FVector CollisionClosestPoint;

		if (ProcessSingleParticle(Candidate, BVH, ParticleRadius, CompCollisionMargin, CompFriction, CompRestitution,
			AdhesionStrength, ContactOffset, Correction, CollisionTriangleIndex, CollisionClosestPoint))
		{
			CollisionCount.fetch_add(1, std::memory_order_relaxed);

			// If adhesion is strong enough, queue for attachment
			if (AdhesionStrength >= MinAdhesionForAttachment && CollisionTriangleIndex != INDEX_NONE)
			{
				// Mark particle as attached in the correction flags
				Correction.Flags |= FParticleCorrection::FLAG_ATTACHED;

				// Store attachment data for later processing (thread-safe)
				FScopeLock Lock(&NewAttachmentsLock);
				NewAttachments.Add({
					Candidate.ParticleIndex,
					Candidate.InteractionIndex,
					CollisionTriangleIndex,
					CollisionClosestPoint
				});
			}
		}
	});

	// Create attachments from collected data
	for (const auto& AttachData : NewAttachments)
	{
		FSkeletalMeshBVH* BVH = BVHLookup[AttachData.InteractionIndex];
		if (BVH && BVH->IsValid())
		{
			const TArray<FSkinnedTriangle>& Triangles = BVH->GetTriangles();
			if (Triangles.IsValidIndex(AttachData.TriangleIndex))
			{
				TryAttachParticle(
					AttachData.ParticleIndex,
					AttachData.InteractionIndex,
					AttachData.TriangleIndex,
					AttachData.ClosestPoint,
					Triangles[AttachData.TriangleIndex],
					AdhesionStrength,
					WorldTime
				);
			}
		}
	}

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
	float InAdhesionStrength,
	float ContactOffset,
	FParticleCorrection& OutCorrection,
	int32& OutTriangleIndex,
	FVector& OutClosestPoint)
{
	OutTriangleIndex = INDEX_NONE;
	OutClosestPoint = FVector::ZeroVector;

	const FVector Position(Candidate.Position);
	const FVector Velocity(Candidate.Velocity);

	// Query BVH for nearby triangles
	TArray<int32> NearbyTriangles;
	const float SearchRadius = ParticleRadius * 2.0f + InCollisionMargin + ContactOffset;
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
	int32 ClosestTriangleIndex = INDEX_NONE;
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
		const float AdjustedDistance = FMath::Max(0.0f, Distance - ContactOffset);

		if (AdjustedDistance < MinDistance)
		{
			MinDistance = AdjustedDistance;
			ClosestPoint = TriClosestPoint;
			ClosestNormal = Tri.Normal;
			ClosestTriangleIndex = TriIdx;
		}
	}

	// Check for collision
	const float EffectiveRadius = ParticleRadius + InCollisionMargin;

	// DEBUG: Log distance check
	static int32 DistanceDebugCounter = 0;
	if (++DistanceDebugCounter % 500 == 1)
	{
		UE_LOG(LogPerPolygonCollision, Warning,
			TEXT("Distance DEBUG: MinDist=%.2f, EffectiveRadius=%.2f, ParticleRadius=%.2f, CollisionMargin=%.2f, ContactOffset=%.2f, Collides=%s"),
			MinDistance, EffectiveRadius, ParticleRadius, InCollisionMargin, ContactOffset,
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
		// Compute velocity correction (reflection + damping + adhesion)
		// ========================================
		const float VelDotNormal = FVector::DotProduct(Velocity, ClosestNormal);

		FVector VelocityCorrection = FVector::ZeroVector;

		// Reflect if moving into the surface
		if (VelDotNormal < 0.0f)
		{
			// Decompose velocity into normal and tangent components
			const FVector VelNormal = ClosestNormal * VelDotNormal;
			const FVector VelTangent = Velocity - VelNormal;

			// Reflect normal component with restitution (bounce)
			// Dampen tangent component with friction
			const FVector NewVelocity = VelTangent * (1.0f - InFriction) - VelNormal * InRestitution;
			VelocityCorrection = NewVelocity - Velocity;
		}

		// ========================================
		// Apply Adhesion (표면으로 당기는 힘)
		// ========================================
		if (InAdhesionStrength > 0.0f)
		{
			// 표면 방향(-Normal)으로 당기는 힘
			// 강도는 유체의 AdhesionStrength에 비례
			// 속도의 표면 이탈 성분을 감쇠시킴
			const float AdhesionForce = InAdhesionStrength * 50.0f;  // 스케일 조절

			// 표면에서 멀어지려는 속도 성분 감쇠
			const FVector CurrentVel = Velocity + VelocityCorrection;
			const float AwaySpeed = FVector::DotProduct(CurrentVel, ClosestNormal);

			if (AwaySpeed > 0.0f)
			{
				// 표면에서 멀어지려는 속도를 접착력으로 감쇠
				const float DampenFactor = FMath::Min(AwaySpeed, AdhesionForce);
				VelocityCorrection -= ClosestNormal * DampenFactor;
			}
			else
			{
				// 이미 표면 방향으로 이동 중이면 약간의 당기는 힘 추가
				VelocityCorrection -= ClosestNormal * (AdhesionForce * 0.1f);
			}
		}

		if (!VelocityCorrection.IsNearlyZero())
		{
			OutCorrection.VelocityDelta = FVector3f(VelocityCorrection);
			OutCorrection.Flags |= FParticleCorrection::FLAG_VELOCITY_CORRECTED;

			// DEBUG: Log velocity correction
			static int32 VelCorrectionDebugCounter = 0;
			if (++VelCorrectionDebugCounter % 100 == 1)
			{
				UE_LOG(LogPerPolygonCollision, Warning,
					TEXT("VelCorrection DEBUG: ParticleIdx=%d, OldVel=(%.1f,%.1f,%.1f), VelDotN=%.1f, Adhesion=%.2f, VelDelta=(%.1f,%.1f,%.1f)"),
					OutCorrection.ParticleIndex,
					Velocity.X, Velocity.Y, Velocity.Z,
					VelDotNormal, InAdhesionStrength,
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

		// Output triangle info for attachment
		OutTriangleIndex = ClosestTriangleIndex;
		OutClosestPoint = ClosestPoint;

		return true;
	}

	return false;
}

//=============================================================================
// Attachment System Implementation
//=============================================================================

void FPerPolygonCollisionProcessor::ComputeBarycentricCoordinates(
	const FVector& Point, const FVector& V0, const FVector& V1, const FVector& V2,
	float& OutU, float& OutV)
{
	// Compute vectors
	const FVector V0V1 = V1 - V0;
	const FVector V0V2 = V2 - V0;
	const FVector V0P = Point - V0;

	// Compute dot products
	const float Dot00 = FVector::DotProduct(V0V1, V0V1);
	const float Dot01 = FVector::DotProduct(V0V1, V0V2);
	const float Dot02 = FVector::DotProduct(V0V1, V0P);
	const float Dot11 = FVector::DotProduct(V0V2, V0V2);
	const float Dot12 = FVector::DotProduct(V0V2, V0P);

	// Compute barycentric coordinates
	const float InvDenom = 1.0f / (Dot00 * Dot11 - Dot01 * Dot01);
	OutU = (Dot11 * Dot02 - Dot01 * Dot12) * InvDenom;
	OutV = (Dot00 * Dot12 - Dot01 * Dot02) * InvDenom;

	// Clamp to valid range (handle numerical errors for points slightly outside triangle)
	OutU = FMath::Clamp(OutU, 0.0f, 1.0f);
	OutV = FMath::Clamp(OutV, 0.0f, 1.0f);
	if (OutU + OutV > 1.0f)
	{
		const float Scale = 1.0f / (OutU + OutV);
		OutU *= Scale;
		OutV *= Scale;
	}
}

bool FPerPolygonCollisionProcessor::ShouldDetach(
	const FParticleAttachmentInfo& Info,
	const FVector& CurrentPosition,
	const FVector& CurrentNormal,
	float DeltaTime,
	FVector& OutDetachVelocity) const
{
	if (DeltaTime <= SMALL_NUMBER)
	{
		OutDetachVelocity = FVector::ZeroVector;
		return false;
	}

	// Calculate surface velocity
	const FVector SurfaceVelocity = (CurrentPosition - Info.PreviousWorldPosition) / DeltaTime;

	// Calculate surface acceleration (simplified: velocity change / dt)
	// For more accuracy, we'd need to store previous velocity too
	const FVector SurfaceAcceleration = SurfaceVelocity / DeltaTime;
	const float AccelerationMagnitude = SurfaceAcceleration.Size();

	// Adhesion-adjusted threshold
	// Higher adhesion = higher threshold needed to detach
	const float AdjustedThreshold = DetachAccelerationThreshold * Info.CurrentAdhesionStrength;

	// Check 1: Surface acceleration exceeds threshold
	if (AccelerationMagnitude > AdjustedThreshold)
	{
		// Give particle the surface velocity when detaching (momentum transfer)
		OutDetachVelocity = SurfaceVelocity;

		UE_LOG(LogPerPolygonCollision, Verbose,
			TEXT("Detach by acceleration: Particle %d, Accel=%.1f > Threshold=%.1f"),
			Info.ParticleIndex, AccelerationMagnitude, AdjustedThreshold);

		return true;
	}

	// Check 2: Gravity vs adhesion on angled/inverted surfaces
	if (GravityDetachInfluence > 0.0f)
	{
		// How much gravity is pulling the particle away from the surface?
		// Dot product of gravity with surface normal (positive when gravity pulls away)
		const float GravityDotNormal = FVector::DotProduct(GravityVector, CurrentNormal);

		if (GravityDotNormal > 0.0f)  // Gravity is pulling away from surface
		{
			// Force needed to overcome adhesion
			const float AdhesionForce = Info.CurrentAdhesionStrength * 500.0f;  // Scaled

			// Gravity force component pulling away (assuming unit mass)
			const float GravityPullForce = GravityDotNormal * GravityDetachInfluence;

			if (GravityPullForce > AdhesionForce)
			{
				// Detach with gravity direction
				OutDetachVelocity = SurfaceVelocity + GravityVector * 0.1f;

				UE_LOG(LogPerPolygonCollision, Verbose,
					TEXT("Detach by gravity: Particle %d, GravityPull=%.1f > AdhesionForce=%.1f"),
					Info.ParticleIndex, GravityPullForce, AdhesionForce);

				return true;
			}
		}
	}

	// Check 3: Centrifugal force (for rotating surfaces)
	// Surface velocity perpendicular to normal indicates rotation
	const FVector VelTangent = SurfaceVelocity - CurrentNormal * FVector::DotProduct(SurfaceVelocity, CurrentNormal);
	const float TangentSpeed = VelTangent.Size();

	if (TangentSpeed > 100.0f)  // Minimum speed for centrifugal consideration
	{
		// Approximate radius (distance from rotation center) - rough estimate
		const float ApproxRadius = 50.0f;  // Assume ~50cm from rotation center

		// Centrifugal acceleration = v² / r
		const float CentrifugalAccel = (TangentSpeed * TangentSpeed) / ApproxRadius;

		if (CentrifugalAccel > AdjustedThreshold)
		{
			// Detach in tangent direction (fly off tangentially)
			OutDetachVelocity = SurfaceVelocity;

			UE_LOG(LogPerPolygonCollision, Verbose,
				TEXT("Detach by centrifugal: Particle %d, CentrifugalAccel=%.1f > Threshold=%.1f"),
				Info.ParticleIndex, CentrifugalAccel, AdjustedThreshold);

			return true;
		}
	}

	OutDetachVelocity = FVector::ZeroVector;
	return false;
}

void FPerPolygonCollisionProcessor::TryAttachParticle(
	uint32 ParticleIndex,
	int32 InteractionIndex,
	int32 TriangleIndex,
	const FVector& ClosestPoint,
	const FSkinnedTriangle& Triangle,
	float AdhesionStrength,
	float WorldTime)
{
	// Check minimum adhesion requirement
	if (AdhesionStrength < MinAdhesionForAttachment)
	{
		return;
	}

	// Already attached? Skip
	if (AttachedParticles.Contains(ParticleIndex))
	{
		return;
	}

	// Compute barycentric coordinates
	float U, V;
	ComputeBarycentricCoordinates(ClosestPoint, Triangle.V0, Triangle.V1, Triangle.V2, U, V);

	// Create attachment info
	FParticleAttachmentInfo NewAttachment(
		ParticleIndex,
		InteractionIndex,
		TriangleIndex,
		U, V,
		ClosestPoint,
		Triangle.Normal,
		WorldTime,
		AdhesionStrength
	);

	// Thread-safe add
	{
		FScopeLock Lock(&AttachmentLock);
		AttachedParticles.Add(ParticleIndex, NewAttachment);
	}

	UE_LOG(LogPerPolygonCollision, Verbose,
		TEXT("Attached particle %d to triangle %d (Barycentric: U=%.3f, V=%.3f)"),
		ParticleIndex, TriangleIndex, U, V);
}

void FPerPolygonCollisionProcessor::UpdateAttachedParticles(
	const TArray<UFluidInteractionComponent*>& InteractionComponents,
	float DeltaTime,
	TArray<FAttachedParticleUpdate>& OutUpdates)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(PerPolygonCollision_UpdateAttachedParticles);

	OutUpdates.Reset();
	LastAttachmentCount = 0;
	LastDetachmentCount = 0;

	if (AttachedParticles.Num() == 0)
	{
		return;
	}

	// Build BVH lookup by interaction index
	TArray<FSkeletalMeshBVH*> BVHLookup;
	BVHLookup.SetNum(InteractionComponents.Num());
	for (int32 i = 0; i < InteractionComponents.Num(); ++i)
	{
		BVHLookup[i] = GetBVH(InteractionComponents[i]);
	}

	// Particles to remove (can't modify map while iterating)
	TArray<uint32> ParticlesToRemove;

	// Pre-allocate output
	OutUpdates.Reserve(AttachedParticles.Num());

	// Process each attached particle
	for (auto& Pair : AttachedParticles)
	{
		FParticleAttachmentInfo& Info = Pair.Value;

		// Validate interaction index
		if (Info.InteractionIndex < 0 || Info.InteractionIndex >= BVHLookup.Num())
		{
			ParticlesToRemove.Add(Pair.Key);
			continue;
		}

		FSkeletalMeshBVH* BVH = BVHLookup[Info.InteractionIndex];
		if (!BVH || !BVH->IsValid())
		{
			ParticlesToRemove.Add(Pair.Key);
			continue;
		}

		// Get triangle data
		const TArray<FSkinnedTriangle>& Triangles = BVH->GetTriangles();
		if (!Triangles.IsValidIndex(Info.TriangleIndex))
		{
			ParticlesToRemove.Add(Pair.Key);
			continue;
		}

		const FSkinnedTriangle& Triangle = Triangles[Info.TriangleIndex];

		// Compute current position using barycentric coordinates
		const FVector CurrentPosition = Info.ComputePosition(Triangle.V0, Triangle.V1, Triangle.V2);
		const FVector CurrentNormal = Triangle.Normal;

		// Check for detachment
		FVector DetachVelocity;
		if (ShouldDetach(Info, CurrentPosition, CurrentNormal, DeltaTime, DetachVelocity))
		{
			// Create detachment update
			FAttachedParticleUpdate Update;
			Update.ParticleIndex = Info.ParticleIndex;
			Update.Flags = FAttachedParticleUpdate::FLAG_DETACH | FAttachedParticleUpdate::FLAG_SET_VELOCITY;
			Update.NewPosition = FVector3f(CurrentPosition);
			Update.NewVelocity = FVector3f(DetachVelocity);
			OutUpdates.Add(Update);

			ParticlesToRemove.Add(Pair.Key);
			LastDetachmentCount++;
		}
		else
		{
			// Create position update
			FAttachedParticleUpdate Update;
			Update.ParticleIndex = Info.ParticleIndex;
			Update.Flags = FAttachedParticleUpdate::FLAG_UPDATE_POSITION;
			Update.NewPosition = FVector3f(CurrentPosition);
			Update.NewVelocity = FVector3f::ZeroVector;  // Attached particles have surface velocity applied implicitly
			OutUpdates.Add(Update);

			// Update previous position for next frame's acceleration calculation
			Info.PreviousWorldPosition = CurrentPosition;
			Info.PreviousSurfaceNormal = CurrentNormal;

			LastAttachmentCount++;
		}
	}

	// Remove detached particles
	{
		FScopeLock Lock(&AttachmentLock);
		for (uint32 ParticleIdx : ParticlesToRemove)
		{
			AttachedParticles.Remove(ParticleIdx);
		}
	}

	// Debug log
	static int32 AttachmentDebugCounter = 0;
	if (++AttachmentDebugCounter % 60 == 1 && (LastAttachmentCount > 0 || LastDetachmentCount > 0))
	{
		UE_LOG(LogPerPolygonCollision, Log,
			TEXT("Attached: %d, Detached: %d, Total Updates: %d"),
			LastAttachmentCount, LastDetachmentCount, OutUpdates.Num());
	}
}
