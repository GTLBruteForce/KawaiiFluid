// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/KawaiiSlimeComponent.h"
#include "Data/KawaiiFluidPresetDataAsset.h"

UKawaiiSlimeComponent::UKawaiiSlimeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// Slime uses independent simulation (has custom logic)
	bIndependentSimulation = true;

	// Enable slime features by default
	bEnableShapeMatching = true;
	bEnableClustering = true;
	bEnableSurfaceTension = true;

	NucleusPosition = FVector::ZeroVector;
	NucleusVelocity = FVector::ZeroVector;
}

void UKawaiiSlimeComponent::BeginPlay()
{
	Super::BeginPlay();

	// Initialize nucleus at spawn location
	if (AActor* Owner = GetOwner())
	{
		NucleusPosition = Owner->GetActorLocation();
	}
}

void UKawaiiSlimeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	// === Initialize Rest Shape BEFORE simulation (critical!) ===
	// Must capture original spawn positions before gravity affects them
	if (!bRestShapeInitialized && GetParticles().Num() > 0)
	{
		InitializeRestShape();
		bRestShapeInitialized = true;
	}

	// Parent handles: PBF simulation, render update, debug mesh
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Early out if no particles
	//if (GetParticles().Num() == 0)
	//{
	//	return;
	//}

	// === Update Core Particles ===
	//UpdateCoreParticles();

	// === Clustering (Section 5) ===
	//if (bEnableClustering)
	//{
	//	UpdateClusters();
	//}

	// === Surface Detection (Section 7) ===
	//if (bEnableSurfaceTension)
	//{
	//	UpdateSurfaceParticles();
	//	ApplySurfaceTension();
	//}

	// === Ground State (for anti-gravity) ===
	//UpdateGroundedState();

	// === Core Slime Logic (disabled in decompose mode) ===
	//if (!bDecomposeMode)
	//{
	//	// Nucleus attraction - pull particles toward center (Section 6.2)
	//	ApplyNucleusAttraction(DeltaTime);
	//}

	// === Anti-gravity during jump (Section 13.1) ===
	//if (bIsInAir)
	//{
	//	ApplyAntiGravity(DeltaTime);
	//}

	// === Nucleus Control ===
	//UpdateNucleus(DeltaTime);

	// === Decompose Mode Timer ===
	//if (bDecomposeMode && RecomposeDelay > 0.0f)
	//{
	//	DecomposeTimer += DeltaTime;
	//	if (DecomposeTimer >= RecomposeDelay)
	//	{
	//		SetDecomposeMode(false);
	//	}
	//}

	// === Interaction Events (Section 11) ===
	//CheckGroundContact();
	//UpdateObjectTracking();
}

//========================================
// Shape Matching Initialization (Section 4.3)
//========================================

void UKawaiiSlimeComponent::InitializeRestShape()
{
	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();

	if (ParticleArray.Num() == 0)
	{
		return;
	}

	// Compute center of mass
	FVector Center = FVector::ZeroVector;
	for (const FFluidParticle& P : ParticleArray)
	{
		Center += P.Position;
	}
	Center /= ParticleArray.Num();

	// Compute RestOffset for each particle (relative to center)
	// Also track max distance for core particle calculation
	CachedMaxDistanceFromCenter = 0.0f;

	for (FFluidParticle& P : ParticleArray)
	{
		P.RestOffset = P.Position - Center;
		float Dist = P.RestOffset.Size();
		CachedMaxDistanceFromCenter = FMath::Max(CachedMaxDistanceFromCenter, Dist);
	}

	UE_LOG(LogTemp, Log, TEXT("SlimeComponent: Initialized rest shape for %d particles, MaxDist=%.2f"),
		ParticleArray.Num(), CachedMaxDistanceFromCenter);
}

//========================================
// Core Particle Update
//========================================

void UKawaiiSlimeComponent::UpdateCoreParticles()
{
	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();

	if (ParticleArray.Num() == 0 || CachedMaxDistanceFromCenter < KINDA_SMALL_NUMBER)
	{
		return;
	}

	// Compute current center
	FVector Center = GetMainClusterCenter();
	float CoreDistance = CachedMaxDistanceFromCenter * CoreRadiusRatio;

	for (FFluidParticle& P : ParticleArray)
	{
		if (P.ClusterID != MainClusterID)
		{
			P.bIsCoreParticle = false;
			P.DistanceFromCoreRatio = 1.0f;
			continue;
		}

		float Dist = FVector::Dist(P.Position, Center);
		P.DistanceFromCoreRatio = FMath::Clamp(Dist / CachedMaxDistanceFromCenter, 0.0f, 1.0f);
		P.bIsCoreParticle = (Dist <= CoreDistance);
	}
}

//========================================
// Nucleus Attraction (Section 6.2 - Method 1)
//========================================

void UKawaiiSlimeComponent::ApplyNucleusAttraction(float DeltaTime)
{
	if (NucleusAttractionStrength <= 0.0f)
	{
		return;
	}

	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();
	FVector Center = GetMainClusterCenter();

	if (CachedMaxDistanceFromCenter < KINDA_SMALL_NUMBER)
	{
		return;
	}

	for (FFluidParticle& P : ParticleArray)
	{
		// Only affect main cluster
		if (P.ClusterID != MainClusterID)
		{
			continue;
		}

		FVector ToCenter = Center - P.Position;
		float DistFromCenter = ToCenter.Size();

		if (DistFromCenter < KINDA_SMALL_NUMBER)
		{
			continue;
		}

		// Normalize
		FVector Direction = ToCenter / DistFromCenter;

		// Compute attraction strength with falloff
		// Core particles: stronger attraction
		// Surface particles: weaker attraction (based on AttractionFalloff)
		float DistanceRatio = P.DistanceFromCoreRatio;
		float Falloff = 1.0f - (DistanceRatio * AttractionFalloff);

		// Apply attraction force
		float ForceMagnitude = NucleusAttractionStrength * Falloff;
		P.Velocity += Direction * ForceMagnitude * DeltaTime;
	}
}

//========================================
// Anti-Gravity (Section 13.1)
//========================================

void UKawaiiSlimeComponent::ApplyAntiGravity(float DeltaTime)
{
	if (AntiGravityStrength <= 0.0f)
	{
		return;
	}

	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();

	// Get gravity from preset
	FVector Gravity = FVector(0.0f, 0.0f, -980.0f);
	if (Preset)
	{
		Gravity = Preset->Gravity;
	}

	// Counter-gravity force (partial, to maintain form but still fall)
	FVector AntiGravityForce = -Gravity * AntiGravityStrength;

	for (FFluidParticle& P : ParticleArray)
	{
		if (P.ClusterID != MainClusterID)
		{
			continue;
		}

		P.Velocity += AntiGravityForce * DeltaTime;
	}
}

//========================================
// Ground Detection
//========================================

void UKawaiiSlimeComponent::UpdateGroundedState()
{
	const TArray<FFluidParticle>& ParticleArray = GetParticles();

	if (ParticleArray.Num() == 0)
	{
		bIsInAir = false;
		return;
	}

	// Count grounded particles (those near ground based on bNearGround flag)
	int32 GroundedCount = 0;
	int32 MainClusterCount = 0;

	for (const FFluidParticle& P : ParticleArray)
	{
		if (P.ClusterID == MainClusterID)
		{
			MainClusterCount++;
			if (P.bNearGround)
			{
				GroundedCount++;
			}
		}
	}

	// If less than threshold of particles are grounded, consider in air
	if (MainClusterCount > 0)
	{
		float GroundedRatio = static_cast<float>(GroundedCount) / static_cast<float>(MainClusterCount);
		bIsInAir = (GroundedRatio < GroundedThreshold);
	}
}

bool UKawaiiSlimeComponent::IsGrounded() const
{
	return !bIsInAir;
}

//========================================
// Nucleus Control
//========================================

void UKawaiiSlimeComponent::UpdateNucleus(float DeltaTime)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Clamp velocity
	if (NucleusVelocity.Size() > MaxMoveSpeed)
	{
		NucleusVelocity = NucleusVelocity.GetSafeNormal() * MaxMoveSpeed;
	}

	// Move actor based on nucleus velocity
	if (!NucleusVelocity.IsNearlyZero())
	{
		FVector CurrentLocation = Owner->GetActorLocation();
		FVector NewLocation = CurrentLocation + NucleusVelocity * DeltaTime;
		Owner->SetActorLocation(NewLocation);
	}

	// Damping
	NucleusVelocity *= 0.95f;

	// Nucleus follows particle center (bidirectional - Section 6.2 Method 2)
	FVector ParticleCenter = GetMainClusterCenter();
	if (!ParticleCenter.IsZero())
	{
		NucleusPosition = FMath::Lerp(Owner->GetActorLocation(), ParticleCenter, NucleusFollowStrength);
	}
	else
	{
		NucleusPosition = Owner->GetActorLocation();
	}
}

//========================================
// Movement Input (Section 6.2/10.3)
//========================================

void UKawaiiSlimeComponent::ApplyMovementInput(FVector Input)
{
	if (Input.IsNearlyZero())
	{
		return;
	}

	Input = Input.GetClampedToMaxSize(1.0f);
	float DeltaTime = GetWorld()->GetDeltaSeconds();

	// Apply input to nucleus velocity
	NucleusVelocity += Input * MoveForce * DeltaTime;

	// Apply force to particles based on distance from center (Section 6.2 Method 1)
	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();
	FVector Center = GetMainClusterCenter();
	float CoreRadius = CachedMaxDistanceFromCenter * CoreRadiusRatio;

	for (FFluidParticle& P : ParticleArray)
	{
		if (P.ClusterID != MainClusterID)
		{
			continue;
		}

		float DistFromCenter = FVector::Dist(P.Position, Center);

		if (DistFromCenter < CoreRadius)
		{
			// Core: strong input
			P.Velocity += Input * MoveForce * DeltaTime;
		}
		else
		{
			// Outer: weak input with falloff
			float MaxDist = FMath::Max(CachedMaxDistanceFromCenter - CoreRadius, KINDA_SMALL_NUMBER);
			float Falloff = 1.0f - FMath::Clamp((DistFromCenter - CoreRadius) / MaxDist, 0.0f, 1.0f);
			P.Velocity += Input * MoveForce * DeltaTime * Falloff * 0.3f;
		}
	}
}

void UKawaiiSlimeComponent::ApplyJumpImpulse()
{
	// Only jump if grounded
	if (bIsInAir)
	{
		return;
	}

	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();
	FVector JumpDir = FVector::UpVector;

	for (FFluidParticle& P : ParticleArray)
	{
		if (P.ClusterID != MainClusterID)
		{
			continue;
		}

		P.Velocity += JumpDir * JumpStrength;
	}

	// Also apply to nucleus
	NucleusVelocity += JumpDir * JumpStrength;

	// Set in air state
	bIsInAir = true;
}

//========================================
// Decompose Mode
//========================================

void UKawaiiSlimeComponent::SetDecomposeMode(bool bEnable)
{
	bDecomposeMode = bEnable;
	DecomposeTimer = 0.0f;

	if (bEnable)
	{
		UE_LOG(LogTemp, Log, TEXT("SlimeComponent: Decompose mode ENABLED - particles behave like fluid"));
	}
	else
	{
		// Re-initialize rest shape when recomposing
		bRestShapeInitialized = false;
		UE_LOG(LogTemp, Log, TEXT("SlimeComponent: Decompose mode DISABLED - particles will regroup"));
	}
}

//========================================
// Clustering (Section 5.3 - Union-Find)
//========================================

void UKawaiiSlimeComponent::UpdateClusters()
{
	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();
	const int32 NumParticles = ParticleArray.Num();

	if (NumParticles == 0)
	{
		return;
	}

	// Union-Find initialization
	TArray<int32> Parent;
	TArray<int32> RankArray;
	Parent.SetNum(NumParticles);
	RankArray.SetNum(NumParticles);

	for (int32 i = 0; i < NumParticles; ++i)
	{
		Parent[i] = i;
		RankArray[i] = 0;
	}

	// Union neighbors (Section 5.2 - contact within SmoothingRadius)
	for (int32 i = 0; i < NumParticles; ++i)
	{
		for (int32 NeighborIdx : ParticleArray[i].NeighborIndices)
		{
			if (NeighborIdx >= 0 && NeighborIdx < NumParticles)
			{
				UnionSets(Parent, RankArray, i, NeighborIdx);
			}
		}
	}

	// Assign cluster IDs
	TMap<int32, int32> RootToClusterID;
	int32 NextClusterID = 0;

	for (int32 i = 0; i < NumParticles; ++i)
	{
		int32 Root = FindRoot(Parent, i);

		if (!RootToClusterID.Contains(Root))
		{
			RootToClusterID.Add(Root, NextClusterID++);
		}

		ParticleArray[i].ClusterID = RootToClusterID[Root];
	}

	ClusterCount = NextClusterID;

	// Find main cluster (largest one)
	TMap<int32, int32> ClusterSizes;
	for (const FFluidParticle& P : ParticleArray)
	{
		ClusterSizes.FindOrAdd(P.ClusterID)++;
	}

	int32 LargestClusterID = 0;
	int32 LargestSize = 0;
	for (const auto& SizePair : ClusterSizes)
	{
		if (SizePair.Value > LargestSize)
		{
			LargestSize = SizePair.Value;
			LargestClusterID = SizePair.Key;
		}
	}

	MainClusterID = LargestClusterID;
}

int32 UKawaiiSlimeComponent::FindRoot(TArray<int32>& Parent, int32 Index)
{
	if (Parent[Index] != Index)
	{
		Parent[Index] = FindRoot(Parent, Parent[Index]); // Path compression
	}
	return Parent[Index];
}

void UKawaiiSlimeComponent::UnionSets(TArray<int32>& Parent, TArray<int32>& RankArray, int32 A, int32 B)
{
	int32 RootA = FindRoot(Parent, A);
	int32 RootB = FindRoot(Parent, B);

	if (RootA == RootB)
	{
		return;
	}

	// Union by rank
	if (RankArray[RootA] < RankArray[RootB])
	{
		Parent[RootA] = RootB;
	}
	else if (RankArray[RootA] > RankArray[RootB])
	{
		Parent[RootB] = RootA;
	}
	else
	{
		Parent[RootB] = RootA;
		RankArray[RootA]++;
	}
}

//========================================
// Surface Tension (Section 7)
//========================================

void UKawaiiSlimeComponent::UpdateSurfaceParticles()
{
	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();

	float SmoothingRadius = 20.0f;
	if (Preset)
	{
		SmoothingRadius = Preset->SmoothingRadius;
	}

	for (FFluidParticle& P : ParticleArray)
	{
		// Compute color field gradient (surface normal approximation) - Section 7.3
		FVector Normal = FVector::ZeroVector;

		for (int32 NeighborIdx : P.NeighborIndices)
		{
			if (NeighborIdx < 0 || NeighborIdx >= ParticleArray.Num())
			{
				continue;
			}

			const FFluidParticle& Neighbor = ParticleArray[NeighborIdx];
			FVector Diff = P.Position - Neighbor.Position;
			float Dist = Diff.Size();

			if (Dist < KINDA_SMALL_NUMBER || Dist > SmoothingRadius)
			{
				continue;
			}

			// Gradient of SPH kernel (simplified)
			float Weight = 1.0f - (Dist / SmoothingRadius);
			Normal += Diff.GetSafeNormal() * Weight;
		}

		float NormalMagnitude = Normal.Size();

		// If normal magnitude > threshold, this is a surface particle
		if (NormalMagnitude > SurfaceThreshold)
		{
			P.bIsSurfaceParticle = true;
			P.SurfaceNormal = Normal.GetSafeNormal();
		}
		else
		{
			P.bIsSurfaceParticle = false;
			P.SurfaceNormal = FVector::ZeroVector;
		}
	}
}

void UKawaiiSlimeComponent::ApplySurfaceTension()
{
	if (SurfaceTensionCoefficient <= 0.0f)
	{
		return;
	}

	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();

	for (FFluidParticle& P : ParticleArray)
	{
		if (!P.bIsSurfaceParticle)
		{
			continue;
		}

		// Surface tension pulls surface particles inward (Section 7.2)
		// F_surface = -gamma * kappa * n (simplified: just pull inward)
		FVector TensionForce = -P.SurfaceNormal * SurfaceTensionCoefficient;
		P.Velocity += TensionForce;
	}
}

//========================================
// Interaction Events (Section 11)
//========================================

void UKawaiiSlimeComponent::CheckGroundContact()
{
	TArray<FFluidParticle>& ParticleArray = GetParticlesMutable();

	for (FFluidParticle& P : ParticleArray)
	{
		// Check if particle just touched ground and hasn't spawned trail yet
		if (P.bNearGround && !P.bTrailSpawned)
		{
			// Fire ground contact event
			if (OnGroundContact.IsBound())
			{
				// Use particle position as contact location, up vector as normal
				OnGroundContact.Broadcast(P.Position, FVector::UpVector);
			}
			P.bTrailSpawned = true;
		}
		else if (!P.bNearGround)
		{
			// Reset trail spawn flag when particle leaves ground
			P.bTrailSpawned = false;
		}
	}
}

void UKawaiiSlimeComponent::UpdateObjectTracking()
{
	// Clean up invalid weak pointers
	TrackedActors.RemoveAll([](const TWeakObjectPtr<AActor>& WeakActor)
	{
		return !WeakActor.IsValid();
	});

	// Check each tracked actor
	for (const TWeakObjectPtr<AActor>& WeakActor : TrackedActors)
	{
		if (!WeakActor.IsValid())
		{
			continue;
		}

		AActor* Actor = WeakActor.Get();
		bool bCurrentlyInside = IsActorInsideSlime(Actor);
		bool bWasInside = ActorsInsideSlime.Contains(WeakActor);

		if (bCurrentlyInside && !bWasInside)
		{
			// Object entered slime
			ActorsInsideSlime.Add(WeakActor);
			if (OnObjectEntered.IsBound())
			{
				OnObjectEntered.Broadcast(Actor);
			}
		}
		else if (!bCurrentlyInside && bWasInside)
		{
			// Object exited slime
			ActorsInsideSlime.Remove(WeakActor);
			if (OnObjectExited.IsBound())
			{
				OnObjectExited.Broadcast(Actor);
			}
		}
	}

	// Clean up actors that are no longer valid from the inside set
	for (auto It = ActorsInsideSlime.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

//========================================
// Query Functions
//========================================

FVector UKawaiiSlimeComponent::GetMainClusterCenter() const
{
	const TArray<FFluidParticle>& ParticleArray = GetParticles();

	FVector Center = FVector::ZeroVector;
	int32 Count = 0;

	for (const FFluidParticle& P : ParticleArray)
	{
		if (P.ClusterID == MainClusterID)
		{
			Center += P.Position;
			Count++;
		}
	}

	if (Count > 0)
	{
		Center /= Count;
	}

	return Center;
}

int32 UKawaiiSlimeComponent::GetMainClusterParticleCount() const
{
	const TArray<FFluidParticle>& ParticleArray = GetParticles();

	int32 Count = 0;
	for (const FFluidParticle& P : ParticleArray)
	{
		if (P.ClusterID == MainClusterID)
		{
			Count++;
		}
	}

	return Count;
}

bool UKawaiiSlimeComponent::IsActorInsideSlime(AActor* Actor) const
{
	if (!Actor)
	{
		return false;
	}

	const TArray<FFluidParticle>& ParticleArray = GetParticles();
	FVector ActorPos = Actor->GetActorLocation();

	float SmoothingRadius = 20.0f;
	if (Preset)
	{
		SmoothingRadius = Preset->SmoothingRadius;
	}

	int32 NearbyCount = 0;

	for (const FFluidParticle& P : ParticleArray)
	{
		if (FVector::Dist(P.Position, ActorPos) < SmoothingRadius * 2.0f)
		{
			NearbyCount++;

			if (NearbyCount >= InsideThreshold)
			{
				return true;
			}
		}
	}

	return false;
}

//========================================
// Override: BuildSimulationParams
//========================================

FKawaiiFluidSimulationParams UKawaiiSlimeComponent::BuildSimulationParams() const
{
	// Get base params from parent
	FKawaiiFluidSimulationParams Params = Super::BuildSimulationParams();

	// Enable shape matching if not in decompose mode (Section 4)
	Params.bEnableShapeMatching = bEnableShapeMatching && !bDecomposeMode;
	Params.ShapeMatchingStiffness = ShapeMatchingStiffness;
	Params.ShapeMatchingCoreMultiplier = CoreStiffnessMultiplier;

	return Params;
}
