// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Components/FluidInteractionComponent.h"
#include "Components/KawaiiFluidComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Collision/MeshFluidCollider.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidParticle.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/PositionVertexBuffer.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

UFluidInteractionComponent::UFluidInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	TargetSubsystem = nullptr;
	//bCanAttachFluid = true;
	//AdhesionMultiplier = 1.0f;
	//DragAlongStrength = 0.5f;
	bAutoCreateCollider = true;

	AutoCollider = nullptr;
}

void UFluidInteractionComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITOR
	// Register with Subsystem in editor mode (for brush mode)
	UWorld* World = GetWorld();
	if (World && !World->IsGameWorld())
	{
		if (!TargetSubsystem)
		{
			TargetSubsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>();
		}

		if (bAutoCreateCollider && !AutoCollider)
		{
			CreateAutoCollider();
		}

		RegisterWithSimulator();
	}
#endif
}

void UFluidInteractionComponent::OnUnregister()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (World && !World->IsGameWorld())
	{
		UnregisterFromSimulator();
	}
#endif

	Super::OnUnregister();
}

void UFluidInteractionComponent::BeginPlay()
{
	Super::BeginPlay();

	// Find subsystem automatically
	if (!TargetSubsystem)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			TargetSubsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>();
		}
	}

	if (bAutoCreateCollider)
	{
		CreateAutoCollider();
	}

	RegisterWithSimulator();

	// Generate boundary particles (Flex-style Adhesion)
	if (bEnableBoundaryParticles)
	{
		GenerateBoundaryParticles();
	}
}

void UFluidInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromSimulator();

	Super::EndPlay(EndPlayReason);
}

void UFluidInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!TargetSubsystem)
	{
		return;
	}

	// Bone level tracking is handled in FluidSimulator::UpdateAttachedParticlePositions()

	// Process GPU Collision Feedback (Particle -> Player Interaction)
	if (bEnableForceFeedback)
	{
		// Auto-enable GPU feedback (on first tick)
		EnableGPUCollisionFeedbackIfNeeded();

		ProcessCollisionFeedback(DeltaTime);
	}

	// Update boundary particles and debug display (Flex-style Adhesion)
	if (bEnableBoundaryParticles && bBoundaryParticlesInitialized)
	{
		UpdateBoundaryParticlePositions();

		if (bShowBoundaryParticles)
		{
			DrawDebugBoundaryParticles();
		}
	}
}

void UFluidInteractionComponent::CreateAutoCollider()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	AutoCollider = NewObject<UMeshFluidCollider>(Owner);
	if (AutoCollider)
	{
		AutoCollider->RegisterComponent();
		//AutoCollider->bAllowAdhesion = bCanAttachFluid;
		//AutoCollider->AdhesionMultiplier = AdhesionMultiplier;

		// Auto-set TargetMeshComponent
		// Priority: SkeletalMeshComponent > CapsuleComponent > StaticMeshComponent
		USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		if (SkelMesh)
		{
			AutoCollider->TargetMeshComponent = SkelMesh;
		}
		else
		{
			UCapsuleComponent* Capsule = Owner->FindComponentByClass<UCapsuleComponent>();
			if (Capsule)
			{
				AutoCollider->TargetMeshComponent = Capsule;
			}
			else
			{
				UStaticMeshComponent* StaticMesh = Owner->FindComponentByClass<UStaticMeshComponent>();
				if (StaticMesh)
				{
					AutoCollider->TargetMeshComponent = StaticMesh;
				}
			}
		}
	}
}

void UFluidInteractionComponent::RegisterWithSimulator()
{
	if (TargetSubsystem)
	{
		if (AutoCollider)
		{
			TargetSubsystem->RegisterGlobalCollider(AutoCollider);
		}
		TargetSubsystem->RegisterGlobalInteractionComponent(this);
	}
}

void UFluidInteractionComponent::UnregisterFromSimulator()
{
	if (TargetSubsystem)
	{
		if (AutoCollider)
		{
			TargetSubsystem->UnregisterGlobalCollider(AutoCollider);
		}
		TargetSubsystem->UnregisterGlobalInteractionComponent(this);
	}
}

void UFluidInteractionComponent::DetachAllFluid()
{
	AActor* Owner = GetOwner();

	auto DetachFromParticles = [Owner](TArray<FFluidParticle>& Particles)
	{
		for (FFluidParticle& Particle : Particles)
		{
			if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
			{
				Particle.bIsAttached = false;
				Particle.AttachedActor.Reset();
				Particle.AttachedBoneName = NAME_None;
				Particle.AttachedLocalOffset = FVector::ZeroVector;
			}
		}
	};

	if (TargetSubsystem)
	{
		for (auto Module : TargetSubsystem->GetAllModules())
		{
			if (!Module) continue;
			DetachFromParticles(Module->GetParticlesMutable());
		}
	}

}

void UFluidInteractionComponent::PushFluid(FVector Direction, float Force)
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	FVector NormalizedDir = Direction.GetSafeNormal();
	FVector OwnerLocation = Owner->GetActorLocation();

	auto PushParticles = [Owner, NormalizedDir, OwnerLocation, Force](TArray<FFluidParticle>& Particles)
	{
		for (FFluidParticle& Particle : Particles)
		{
			float Distance = FVector::Dist(Particle.Position, OwnerLocation);

			if (Distance < 200.0f)
			{
				float FallOff = 1.0f - (Distance / 200.0f);
				Particle.Velocity += NormalizedDir * Force * FallOff;

				if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
				{
					Particle.bIsAttached = false;
					Particle.AttachedActor.Reset();
					Particle.AttachedBoneName = NAME_None;
					Particle.AttachedLocalOffset = FVector::ZeroVector;
				}
			}
		}
	};

	if (TargetSubsystem)
	{
		for (auto Module : TargetSubsystem->GetAllModules())
		{
			if (!Module) continue;
			PushParticles(Module->GetParticlesMutable());
		}
	}
}

//=============================================================================
// GPU Collision Feedback Implementation (Particle -> Player Interaction)
//=============================================================================

void UFluidInteractionComponent::ProcessCollisionFeedback(float DeltaTime)
{
	AActor* Owner = GetOwner();
	const int32 MyOwnerID = Owner ? Owner->GetUniqueID() : 0;

	if (!TargetSubsystem)
	{
		// No contact → decay force
		SmoothedForce = FMath::VInterpTo(SmoothedForce, FVector::ZeroVector, DeltaTime, ForceSmoothingSpeed);
		CurrentFluidForce = SmoothedForce;
		CurrentContactCount = 0;
		CurrentAveragePressure = 0.0f;
		return;
	}

	// Get feedback from GPUSimulator (from first GPU mode module)
	FGPUFluidSimulator* GPUSimulator = nullptr;
	UKawaiiFluidSimulationModule* SourceModule = nullptr;
	for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (Module && Module->GetGPUSimulator())
		{
			GPUSimulator = Module->GetGPUSimulator();
			SourceModule = Module;
			break;
		}
	}

	if (!GPUSimulator)
	{
		// No GPUSimulator → decay force
		SmoothedForce = FMath::VInterpTo(SmoothedForce, FVector::ZeroVector, DeltaTime, ForceSmoothingSpeed);
		CurrentFluidForce = SmoothedForce;
		CurrentContactCount = 0;
		CurrentAveragePressure = 0.0f;
		return;
	}

	// =====================================================
	// New approach: use per-collider count buffer
	// GPU aggregates collision counts per collider index,
	// then filters by OwnerID to get particles colliding with this actor's colliders
	// =====================================================
	const int32 OwnerContactCount = GPUSimulator->GetContactCountForOwner(MyOwnerID);

	// Debug: log collider counts
	static int32 DebugLogCounter = 0;
	if (++DebugLogCounter % 60 == 0)  // Log once every 60 frames
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidInteraction: OwnerID=%d, ContactCount=%d, TotalColliders=%d"),
			MyOwnerID, OwnerContactCount, GPUSimulator->GetTotalColliderCount());
	}

	// Initialize and set per-fluid-tag counts
	CurrentFluidTagCounts.Empty();
	if (OwnerContactCount > 0)
	{
		// Currently processed with default tag (tag system can be expanded later)
		CurrentFluidTagCounts.FindOrAdd(NAME_None) = OwnerContactCount;
	}

	// Trigger events with collider contact count
	CurrentContactCount = OwnerContactCount;

	// =====================================================
	// Force calculation: only process when detailed feedback is needed
	// (Count-based events work even when feedback is disabled)
	// =====================================================
	if (GPUSimulator->IsCollisionFeedbackEnabled())
	{
		TArray<FGPUCollisionFeedback> AllFeedback;
		int32 FeedbackCount = 0;
		GPUSimulator->GetAllCollisionFeedback(AllFeedback, FeedbackCount);

		// Process per-bone forces
		if (bEnablePerBoneForce)
		{
			const float ParticleRadius = FMath::Max(SourceModule ? SourceModule->GetParticleRadius() : 3.0f, 0.1f);
			ProcessPerBoneForces(DeltaTime, AllFeedback, FeedbackCount, ParticleRadius);

			// Process bone collision events (for Niagara spawning)
			ProcessBoneCollisionEvents(DeltaTime, AllFeedback, FeedbackCount);
		}

		if (FeedbackCount > 0)
		{
			// Drag calculation parameters
			const float ParticleRadius = FMath::Max(SourceModule ? SourceModule->GetParticleRadius() : 3.0f, 0.1f);
			const float ParticleArea = PI * ParticleRadius * ParticleRadius;  // cm²
			const float AreaInM2 = ParticleArea * 0.0001f;  // m² (cm² → m²)

			// Get character/object velocity
			FVector BodyVelocity = FVector::ZeroVector;
			if (Owner)
			{
				if (UCharacterMovementComponent* MovementComp = Owner->FindComponentByClass<UCharacterMovementComponent>())
				{
					BodyVelocity = MovementComp->Velocity;
				}
				else if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
				{
					BodyVelocity = RootPrimitive->GetPhysicsLinearVelocity();
				}
			}
			const FVector BodyVelocityInMS = BodyVelocity * 0.01f;

			FVector ForceAccum = FVector::ZeroVector;
			float DensitySum = 0.0f;
			int32 ForceContactCount = 0;

			for (int32 i = 0; i < FeedbackCount; ++i)
			{
				const FGPUCollisionFeedback& Feedback = AllFeedback[i];

				// Filter by ColliderOwnerID: only use feedback from collisions with this actor's colliders for force calculation
				if (Feedback.ColliderOwnerID != 0 && Feedback.ColliderOwnerID != MyOwnerID)
				{
					continue;
				}

				// Particle velocity (cm/s → m/s)
				FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
				FVector ParticleVelocityInMS = ParticleVelocity * 0.01f;

				// Velocity selection: relative velocity vs absolute velocity
				FVector EffectiveVelocity;
				if (bUseRelativeVelocityForForce)
				{
					// Relative velocity (v_fluid - v_body): suitable for resistance in water
					// Running through still water generates resistance
					EffectiveVelocity = ParticleVelocityInMS - BodyVelocityInMS;
				}
				else
				{
					// Absolute velocity (v_fluid): suitable for wave/waterfall push effects
					// Only fast-moving fluid pushes the character
					EffectiveVelocity = ParticleVelocityInMS;
				}
				
				float EffectiveSpeed = EffectiveVelocity.Size();

				DensitySum += Feedback.Density;
				ForceContactCount++;

				if (EffectiveSpeed < SMALL_NUMBER)
				{
					continue;
				}

				// Impact force formula: F = ½ρCdA|v|²
				float ImpactMagnitude = 0.5f * Feedback.Density * DragCoefficient * AreaInM2 * EffectiveSpeed * EffectiveSpeed;
				FVector ImpactDirection = EffectiveVelocity.GetSafeNormal();
				ForceAccum += ImpactDirection * ImpactMagnitude;
			}

			// Convert force to cm units
			ForceAccum *= 100.0f;

			// Apply smoothing
			FVector TargetForce = ForceAccum * DragForceMultiplier;
			SmoothedForce = FMath::VInterpTo(SmoothedForce, TargetForce, DeltaTime, ForceSmoothingSpeed);
			CurrentFluidForce = SmoothedForce;
			CurrentAveragePressure = (ForceContactCount > 0) ? (DensitySum / ForceContactCount) : 0.0f;
		}
		else
		{
			// No feedback → decay force
			SmoothedForce = FMath::VInterpTo(SmoothedForce, FVector::ZeroVector, DeltaTime, ForceSmoothingSpeed);
			CurrentFluidForce = SmoothedForce;
			CurrentAveragePressure = 0.0f;
		}
	}
	else
	{
		// Detailed feedback disabled → no force, count only
		SmoothedForce = FMath::VInterpTo(SmoothedForce, FVector::ZeroVector, DeltaTime, ForceSmoothingSpeed);
		CurrentFluidForce = SmoothedForce;
		CurrentAveragePressure = 0.0f;
	}

	// Broadcast event
	if (OnFluidForceUpdate.IsBound())
	{
		OnFluidForceUpdate.Broadcast(CurrentFluidForce, CurrentAveragePressure, CurrentContactCount);
	}

	// Monitor per-bone impacts (auto events)
	if (bEnableBoneImpactMonitoring && OnBoneFluidImpact.IsBound())
	{
		CheckBoneImpacts();
	}

	// Update fluid tag events (OnFluidEnter/OnFluidExit)
	UpdateFluidTagEvents();
}

void UFluidInteractionComponent::UpdateFluidTagEvents()
{
	// Check tags colliding with enough particles in current frame
	TSet<FName> CurrentlyColliding;
	for (const auto& Pair : CurrentFluidTagCounts)
	{
		if (Pair.Value >= MinParticleCountForFluidEvent)
		{
			CurrentlyColliding.Add(Pair.Key);
		}
	}

	// Exit event: was colliding previously but not now
	for (auto& Pair : PreviousFluidTagStates)
	{
		if (Pair.Value && !CurrentlyColliding.Contains(Pair.Key))
		{
			if (OnFluidExit.IsBound())
			{
				OnFluidExit.Broadcast(Pair.Key);
			}
			Pair.Value = false;
		}
	}

	// Enter event: was not colliding previously but now is
	for (const FName& Tag : CurrentlyColliding)
	{
		bool* bWasCollidingWithTag = PreviousFluidTagStates.Find(Tag);
		if (!bWasCollidingWithTag || !(*bWasCollidingWithTag))
		{
			int32* CountPtr = CurrentFluidTagCounts.Find(Tag);
			int32 Count = CountPtr ? *CountPtr : 0;

			if (OnFluidEnter.IsBound())
			{
				OnFluidEnter.Broadcast(Tag, Count);
			}
			PreviousFluidTagStates.FindOrAdd(Tag) = true;
		}
	}
}

void UFluidInteractionComponent::CheckBoneImpacts()
{
	// Skip check if MonitoredBones is empty
	if (MonitoredBones.Num() == 0)
	{
		return;
	}

	// [Debug] Collect all actually collided BoneIndex values
	TSet<int32> ActualCollidedBones;
	if (TargetSubsystem)
	{
		for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
		{
			if (!Module) continue;

			FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
			if (!GPUSimulator) continue;

			TArray<FGPUCollisionFeedback> CollisionFeedbacks;
			int32 FeedbackCount = 0;
			GPUSimulator->GetAllCollisionFeedback(CollisionFeedbacks, FeedbackCount);

			for (const FGPUCollisionFeedback& Feedback : CollisionFeedbacks)
			{
				ActualCollidedBones.Add(Feedback.BoneIndex);
			}
		}
	}

	// [Debug] Print collided BoneIndex and ColliderIndex
	if (ActualCollidedBones.Num() > 0)
	{
		//UE_LOG(LogTemp, Warning, TEXT("[BoneImpact] Actually collided BoneIndex: %s"), *FString::JoinBy(ActualCollidedBones, TEXT(", "), [](int32 Idx) { return FString::FromInt(Idx); }));

		// Also print ColliderIndex
		for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
		{
			if (!Module) continue;

			FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
			if (!GPUSimulator) continue;

			TArray<FGPUCollisionFeedback> CollisionFeedbacks;
			int32 FeedbackCount = 0;
			GPUSimulator->GetAllCollisionFeedback(CollisionFeedbacks, FeedbackCount);

			for (const FGPUCollisionFeedback& Feedback : CollisionFeedbacks)
			{
				if (ActualCollidedBones.Contains(Feedback.BoneIndex))
				{
					//UE_LOG(LogTemp, Warning, TEXT("[BoneImpact]   ColliderIndex=%d, ColliderType=%d → BoneIndex=%d"),Feedback.ColliderIndex, Feedback.ColliderType, Feedback.BoneIndex);
					break;  // Print first only
				}
			}
			break;  // First module only
		}
	}

	// Get SkeletalMesh
	AActor* Owner = GetOwner();
	USkeletalMeshComponent* SkelMesh = Owner ? Owner->FindComponentByClass<USkeletalMeshComponent>() : nullptr;

	// Check each monitored bone
	for (const FName& BoneName : MonitoredBones)
	{
		// [Debug] BoneName → BoneIndex conversion log
		int32 ExpectedBoneIndex = SkelMesh ? SkelMesh->GetBoneIndex(BoneName) : INDEX_NONE;
		//UE_LOG(LogTemp, Warning, TEXT("[BoneImpact] MonitoredBone: %s → BoneIndex: %d"), *BoneName.ToString(), ExpectedBoneIndex);

		// Get per-bone impact data
		float ImpactSpeed = GetFluidImpactSpeedForBone(BoneName);

		// Fire event when threshold exceeded
		if (ImpactSpeed > BoneImpactSpeedThreshold)
		{
			float ImpactForce = GetFluidImpactForceMagnitudeForBone(BoneName);
			FVector ImpactDirection = GetFluidImpactDirectionForBone(BoneName);

			// [Debug] Event fired log
			//UE_LOG(LogTemp, Warning, TEXT("[BoneImpact] Event fired - BoneName: %s, Speed: %.1f, Force: %.1f"), *BoneName.ToString(), ImpactSpeed, ImpactForce);

			// Broadcast event
			OnBoneFluidImpact.Broadcast(BoneName, ImpactSpeed, ImpactForce, ImpactDirection);
		}
	}
}

void UFluidInteractionComponent::ApplyFluidForceToCharacterMovement(float ForceScale)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	UCharacterMovementComponent* MovementComp = Owner->FindComponentByClass<UCharacterMovementComponent>();
	if (!MovementComp)
	{
		return;
	}

	// Apply force (AddForce converts to acceleration)
	FVector ScaledForce = CurrentFluidForce * ForceScale;
	if (!ScaledForce.IsNearlyZero())
	{
		MovementComp->AddForce(ScaledForce);
	}
}

bool UFluidInteractionComponent::IsCollidingWithFluidTag(FName FluidTag) const
{
	const bool* bIsColliding = PreviousFluidTagStates.Find(FluidTag);
	return bIsColliding && *bIsColliding;
}

float UFluidInteractionComponent::GetFluidImpactSpeed() const
{
	if (!TargetSubsystem)
	{
		return 0.0f;
	}

	float TotalSpeed = 0.0f;
	int32 TotalFeedbackCount = 0;

	for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (!Module) continue;

		FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
		if (!GPUSimulator) continue;

		TArray<FGPUCollisionFeedback> CollisionFeedbacks;
		int32 FeedbackCount = 0;
		GPUSimulator->GetAllCollisionFeedback(CollisionFeedbacks, FeedbackCount);

		for (const FGPUCollisionFeedback& Feedback : CollisionFeedbacks)
		{
			// Particle velocity (absolute velocity, cm/s)
			FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
			float Speed = ParticleVelocity.Size();
			TotalSpeed += Speed;
			TotalFeedbackCount++;
		}
	}

	return (TotalFeedbackCount > 0) ? (TotalSpeed / TotalFeedbackCount) : 0.0f;
}

float UFluidInteractionComponent::GetFluidImpactForceMagnitude() const
{
	if (!TargetSubsystem)
	{
		return 0.0f;
	}

	const float LocalDragCoefficient = 1.0f;  // Sphere drag coefficient
	const float AreaInM2 = 0.01f;  // 100 cm² = 0.01 m²
	float TotalForceMagnitude = 0.0f;

	for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (!Module) continue;

		FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
		if (!GPUSimulator) continue;

		TArray<FGPUCollisionFeedback> CollisionFeedbacks;
		int32 FeedbackCount = 0;
		GPUSimulator->GetAllCollisionFeedback(CollisionFeedbacks, FeedbackCount);

		for (const FGPUCollisionFeedback& Feedback : CollisionFeedbacks)
		{
			// Particle velocity (cm/s → m/s)
			FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
			float ParticleSpeed = ParticleVelocity.Size() * 0.01f;  // cm/s → m/s

			// Impact force formula: F = ½ρCdA|v|² (v is fluid's absolute velocity)
			float ImpactMagnitude = 0.5f * Feedback.Density * LocalDragCoefficient * AreaInM2 * ParticleSpeed * ParticleSpeed;
			TotalForceMagnitude += ImpactMagnitude;
		}
	}

	return TotalForceMagnitude;
}

FVector UFluidInteractionComponent::GetFluidImpactDirection() const
{
	if (!TargetSubsystem)
	{
		return FVector::ZeroVector;
	}

	FVector TotalVelocity = FVector::ZeroVector;
	int32 TotalFeedbackCount = 0;

	for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (!Module) continue;

		FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
		if (!GPUSimulator) continue;

		TArray<FGPUCollisionFeedback> CollisionFeedbacks;
		int32 FeedbackCount = 0;
		GPUSimulator->GetAllCollisionFeedback(CollisionFeedbacks, FeedbackCount);

		for (const FGPUCollisionFeedback& Feedback : CollisionFeedbacks)
		{
			// Particle velocity (absolute velocity, cm/s)
			FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
			TotalVelocity += ParticleVelocity;
			TotalFeedbackCount++;
		}
	}

	if (TotalFeedbackCount > 0 && !TotalVelocity.IsNearlyZero())
	{
		return TotalVelocity.GetSafeNormal();
	}

	return FVector::ZeroVector;
}

float UFluidInteractionComponent::GetFluidImpactSpeedForBone(FName BoneName) const
{
	if (!TargetSubsystem)
	{
		return 0.0f;
	}

	// Convert BoneName → BoneIndex
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return 0.0f;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh)
	{
		return 0.0f;
	}

	int32 TargetBoneIndex = SkelMesh->GetBoneIndex(BoneName);
	if (TargetBoneIndex == INDEX_NONE)
	{
		return 0.0f;  // Bone does not exist
	}

	// Filter only particles that collided with this bone
	float TotalSpeed = 0.0f;
	int32 TotalFeedbackCount = 0;

	for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (!Module) continue;

		FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
		if (!GPUSimulator) continue;

		TArray<FGPUCollisionFeedback> CollisionFeedbacks;
		int32 FeedbackCount = 0;
		GPUSimulator->GetAllCollisionFeedback(CollisionFeedbacks, FeedbackCount);

		for (const FGPUCollisionFeedback& Feedback : CollisionFeedbacks)
		{
			// Filter by BoneIndex
			if (Feedback.BoneIndex != TargetBoneIndex)
			{
				continue;
			}

			FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
			float Speed = ParticleVelocity.Size();
			TotalSpeed += Speed;
			TotalFeedbackCount++;
		}
	}

	return (TotalFeedbackCount > 0) ? (TotalSpeed / TotalFeedbackCount) : 0.0f;
}

float UFluidInteractionComponent::GetFluidImpactForceMagnitudeForBone(FName BoneName) const
{
	if (!TargetSubsystem)
	{
		return 0.0f;
	}

	// Convert BoneName → BoneIndex
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return 0.0f;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh)
	{
		return 0.0f;
	}

	int32 TargetBoneIndex = SkelMesh->GetBoneIndex(BoneName);
	if (TargetBoneIndex == INDEX_NONE)
	{
		return 0.0f;
	}

	const float LocalDragCoefficient = 1.0f;
	const float AreaInM2 = 0.01f;
	float TotalForceMagnitude = 0.0f;

	for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (!Module) continue;

		FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
		if (!GPUSimulator) continue;

		TArray<FGPUCollisionFeedback> CollisionFeedbacks;
		int32 FeedbackCount = 0;
		GPUSimulator->GetAllCollisionFeedback(CollisionFeedbacks, FeedbackCount);

		for (const FGPUCollisionFeedback& Feedback : CollisionFeedbacks)
		{
			// Filter by BoneIndex
			if (Feedback.BoneIndex != TargetBoneIndex)
			{
				continue;
			}

			FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
			float ParticleSpeed = ParticleVelocity.Size() * 0.01f;  // cm/s → m/s

			float ImpactMagnitude = 0.5f * Feedback.Density * LocalDragCoefficient * AreaInM2 * ParticleSpeed * ParticleSpeed;
			TotalForceMagnitude += ImpactMagnitude;
		}
	}

	return TotalForceMagnitude;
}

FVector UFluidInteractionComponent::GetFluidImpactDirectionForBone(FName BoneName) const
{
	if (!TargetSubsystem)
	{
		return FVector::ZeroVector;
	}

	// Convert BoneName → BoneIndex
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return FVector::ZeroVector;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh)
	{
		return FVector::ZeroVector;
	}

	int32 TargetBoneIndex = SkelMesh->GetBoneIndex(BoneName);
	if (TargetBoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	FVector TotalVelocity = FVector::ZeroVector;
	int32 TotalFeedbackCount = 0;

	for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (!Module) continue;

		FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
		if (!GPUSimulator) continue;

		TArray<FGPUCollisionFeedback> CollisionFeedbacks;
		int32 FeedbackCount = 0;
		GPUSimulator->GetAllCollisionFeedback(CollisionFeedbacks, FeedbackCount);

		for (const FGPUCollisionFeedback& Feedback : CollisionFeedbacks)
		{
			// Filter by BoneIndex
			if (Feedback.BoneIndex != TargetBoneIndex)
			{
				continue;
			}

			FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
			TotalVelocity += ParticleVelocity;
			TotalFeedbackCount++;
		}
	}

	if (TotalFeedbackCount > 0 && !TotalVelocity.IsNearlyZero())
	{
		FVector WorldDirection = TotalVelocity.GetSafeNormal();

		// Convert to character local space (can determine forward/backward/left/right relative to character)
		FTransform ActorTransform = Owner->GetActorTransform();
		FVector LocalDirection = ActorTransform.InverseTransformVectorNoScale(WorldDirection);

		return LocalDirection;
	}

	return FVector::ZeroVector;
}

void UFluidInteractionComponent::EnableGPUCollisionFeedbackIfNeeded()
{
	// Skip if already enabled
	if (bGPUFeedbackEnabled)
	{
		return;
	}

	if (!TargetSubsystem)
	{
		return;
	}

	// Enable feedback in all GPU modules
for (UKawaiiFluidSimulationModule* Module : TargetSubsystem->GetAllModules())
	{
		if (Module)
		{
			FGPUFluidSimulator* GPUSimulator = Module->GetGPUSimulator();
			if (GPUSimulator)
			{
				GPUSimulator->SetCollisionFeedbackEnabled(true);
				bGPUFeedbackEnabled = true;
				UE_LOG(LogTemp, Log, TEXT("FluidInteractionComponent: GPU Collision Feedback Enabled"));
			}
		}
	}
}

//=============================================================================
// Per-Bone Force Implementation
//=============================================================================

void UFluidInteractionComponent::InitializeBoneNameCache()
{
	if (bBoneNameCacheInitialized)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh || !SkelMesh->GetSkeletalMeshAsset())
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetSkeletalMeshAsset()->GetRefSkeleton();
	const int32 BoneCount = RefSkeleton.GetNum();

	BoneIndexToNameCache.Empty(BoneCount);
	for (int32 i = 0; i < BoneCount; ++i)
	{
		BoneIndexToNameCache.Add(i, RefSkeleton.GetBoneName(i));
	}

	bBoneNameCacheInitialized = true;
}

void UFluidInteractionComponent::ProcessPerBoneForces(float DeltaTime, const TArray<FGPUCollisionFeedback>& AllFeedback, int32 FeedbackCount, float ParticleRadius)
{
	AActor* Owner = GetOwner();
	const int32 MyOwnerID = Owner ? Owner->GetUniqueID() : 0;

	// Initialize bone name cache (on first call)
	if (!bBoneNameCacheInitialized)
	{
		InitializeBoneNameCache();
	}

	// Accumulate raw forces per bone (this frame)
	TMap<int32, FVector> RawBoneForces;
	TMap<int32, int32> BoneContactCounts;

	// Get character/object velocity
	FVector BodyVelocity = FVector::ZeroVector;
	if (Owner)
	{
		if (UCharacterMovementComponent* MovementComp = Owner->FindComponentByClass<UCharacterMovementComponent>())
		{
			BodyVelocity = MovementComp->Velocity;
		}
		else if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
		{
			BodyVelocity = RootPrimitive->GetPhysicsLinearVelocity();
		}
	}
	const FVector BodyVelocityInMS = BodyVelocity * 0.01f;  // cm/s → m/s

	// Drag calculation parameters
	const float ParticleArea = PI * ParticleRadius * ParticleRadius;  // cm²
	const float AreaInM2 = ParticleArea * 0.0001f;  // m² (cm² → m²)

	for (int32 i = 0; i < FeedbackCount; ++i)
	{
		const FGPUCollisionFeedback& Feedback = AllFeedback[i];

		// Filter by OwnerID
		if (Feedback.ColliderOwnerID != 0 && Feedback.ColliderOwnerID != MyOwnerID)
		{
			continue;
		}

		// Only process if BoneIndex is valid
		if (Feedback.BoneIndex < 0)
		{
			continue;
		}

		// Particle velocity (cm/s → m/s)
		FVector ParticleVelocity(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
		FVector ParticleVelocityInMS = ParticleVelocity * 0.01f;

		// =====================================================================
		// Use absolute velocity (impact force for knockdown detection)
		// =====================================================================
		// Previous: RelativeVelocity = ParticleVel - BodyVel (resistance force)
		// Changed: Use ParticleVel only (impact force)
		//
		// Reason: Player running through still water should not count as impact
		//         Only fast-moving fluid should count as impact
		// =====================================================================

		float ParticleSpeed = ParticleVelocityInMS.Size();  // Absolute velocity

		if (ParticleSpeed < SMALL_NUMBER)
		{
			continue;
		}

		// Impact force formula: F = ½ρCdA|v_fluid|² (absolute velocity)
		float ImpactMagnitude = 0.5f * Feedback.Density * DragCoefficient * AreaInM2 * ParticleSpeed * ParticleSpeed;
		FVector ImpactDirection = ParticleVelocityInMS.GetSafeNormal();
		FVector ImpactForce = ImpactDirection * ImpactMagnitude;

		// Convert to cm units and apply multiplier
		ImpactForce *= 100.0f * PerBoneForceMultiplier;

		// Accumulate force per bone
		FVector& BoneForce = RawBoneForces.FindOrAdd(Feedback.BoneIndex, FVector::ZeroVector);
		BoneForce += ImpactForce;

		int32& ContactCount = BoneContactCounts.FindOrAdd(Feedback.BoneIndex, 0);
		ContactCount++;
	}

	// Apply smoothing and save results
	// First decay existing bones
	TArray<int32> BonesToRemove;
	for (auto& Pair : SmoothedPerBoneForces)
	{
		const int32 BoneIdx = Pair.Key;
		FVector& SmoothedForceRef = Pair.Value;

		FVector* RawForce = RawBoneForces.Find(BoneIdx);
		FVector TargetForce = RawForce ? *RawForce : FVector::ZeroVector;

		SmoothedForceRef = FMath::VInterpTo(SmoothedForceRef, TargetForce, DeltaTime, PerBoneForceSmoothingSpeed);
		CurrentPerBoneForces.FindOrAdd(BoneIdx) = SmoothedForceRef;

		// Mark for removal if force is nearly 0
		if (SmoothedForceRef.SizeSquared() < 0.01f && !RawForce)
		{
			BonesToRemove.Add(BoneIdx);
		}
	}

	// Add new bones
	for (const auto& Pair : RawBoneForces)
	{
		const int32 BoneIdx = Pair.Key;
		if (!SmoothedPerBoneForces.Contains(BoneIdx))
		{
			FVector& SmoothedForceRef = SmoothedPerBoneForces.Add(BoneIdx, FVector::ZeroVector);
			SmoothedForceRef = FMath::VInterpTo(SmoothedForceRef, Pair.Value, DeltaTime, PerBoneForceSmoothingSpeed);
			CurrentPerBoneForces.FindOrAdd(BoneIdx) = SmoothedForceRef;
		}
	}

	// Remove bones with near-zero force
	for (int32 BoneIdx : BonesToRemove)
	{
		SmoothedPerBoneForces.Remove(BoneIdx);
		CurrentPerBoneForces.Remove(BoneIdx);
	}

	// =====================================================
	// Debug log: print per-bone drag every 3 seconds
	// =====================================================
	PerBoneForceDebugTimer += DeltaTime;
	if (PerBoneForceDebugTimer >= 3.0f)
	{
		PerBoneForceDebugTimer = 0.0f;

		// Feedback data analysis log
		int32 TotalFeedback = FeedbackCount;
		int32 MatchedOwner = 0;
		int32 ValidBoneIndex = 0;
		int32 InvalidBoneIndex = 0;

		for (int32 i = 0; i < FeedbackCount; ++i)
		{
			const FGPUCollisionFeedback& Feedback = AllFeedback[i];

			if (Feedback.ColliderOwnerID == 0 || Feedback.ColliderOwnerID == MyOwnerID)
			{
				MatchedOwner++;
				if (Feedback.BoneIndex >= 0)
				{
					ValidBoneIndex++;
				}
				else
				{
					InvalidBoneIndex++;
				}
			}
		}

		// Collect feedback OwnerID samples
		TSet<int32> UniqueOwnerIDs;
		for (int32 i = 0; i < FMath::Min(FeedbackCount, 100); ++i)
		{
			UniqueOwnerIDs.Add(AllFeedback[i].ColliderOwnerID);
		}
		FString OwnerIDSamples;
		for (int32 ID : UniqueOwnerIDs)
		{
			OwnerIDSamples += FString::Printf(TEXT("%d, "), ID);
		}

		UE_LOG(LogTemp, Warning, TEXT("========== Per-Bone Force Debug (Owner: %s, ID: %d) =========="),
			Owner ? *Owner->GetName() : TEXT("None"), MyOwnerID);
		UE_LOG(LogTemp, Warning, TEXT("  Feedback: Total=%d, MatchedOwner=%d, ValidBone=%d, InvalidBone(=-1)=%d"),
			TotalFeedback, MatchedOwner, ValidBoneIndex, InvalidBoneIndex);
		UE_LOG(LogTemp, Warning, TEXT("  Feedback OwnerIDs samples: [%s]"), *OwnerIDSamples);

		if (CurrentPerBoneForces.Num() > 0)
		{
			for (const auto& Pair : CurrentPerBoneForces)
			{
				const int32 BoneIdx = Pair.Key;
				const FVector& Force = Pair.Value;
				const float ForceMagnitude = Force.Size();

				// Get bone name
				FName BoneName = NAME_None;
				if (const FName* CachedName = BoneIndexToNameCache.Find(BoneIdx))
				{
					BoneName = *CachedName;
				}

				UE_LOG(LogTemp, Warning, TEXT("  [%d] %s: Force=(%.2f, %.2f, %.2f) Magnitude=%.2f"),
					BoneIdx,
					*BoneName.ToString(),
					Force.X, Force.Y, Force.Z,
					ForceMagnitude);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("  -> No active bone forces"));
		}

		UE_LOG(LogTemp, Warning, TEXT("================================================================"));
	}
}

FVector UFluidInteractionComponent::GetFluidForceForBone(int32 BoneIndex) const
{
	const FVector* Force = CurrentPerBoneForces.Find(BoneIndex);
	return Force ? *Force : FVector::ZeroVector;
}

FVector UFluidInteractionComponent::GetFluidForceForBoneByName(FName BoneName) const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return FVector::ZeroVector;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh)
	{
		return FVector::ZeroVector;
	}

	int32 BoneIndex = SkelMesh->GetBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	return GetFluidForceForBone(BoneIndex);
}

void UFluidInteractionComponent::GetActiveBoneIndices(TArray<int32>& OutBoneIndices) const
{
	OutBoneIndices.Empty(CurrentPerBoneForces.Num());
	for (const auto& Pair : CurrentPerBoneForces)
	{
		if (Pair.Value.SizeSquared() > 0.01f)
		{
			OutBoneIndices.Add(Pair.Key);
		}
	}
}

bool UFluidInteractionComponent::GetStrongestBoneForce(int32& OutBoneIndex, FVector& OutForce) const
{
	OutBoneIndex = -1;
	OutForce = FVector::ZeroVector;
	float MaxForceSq = 0.0f;

	for (const auto& Pair : CurrentPerBoneForces)
	{
		float ForceSq = Pair.Value.SizeSquared();
		if (ForceSq > MaxForceSq)
		{
			MaxForceSq = ForceSq;
			OutBoneIndex = Pair.Key;
			OutForce = Pair.Value;
		}
	}

	return OutBoneIndex >= 0;
}

//=============================================================================
// Bone Collision Events Implementation (for Niagara Spawning)
//=============================================================================

void UFluidInteractionComponent::ProcessBoneCollisionEvents(float DeltaTime, const TArray<FGPUCollisionFeedback>& AllFeedback, int32 FeedbackCount)
{
	AActor* Owner = GetOwner();
	const int32 MyOwnerID = Owner ? Owner->GetUniqueID() : 0;

	// Update cooldown timers (all bones)
	TArray<int32> ExpiredCooldowns;
	for (auto& Pair : BoneEventCooldownTimers)
	{
		Pair.Value -= DeltaTime;
		if (Pair.Value <= 0.0f)
		{
			ExpiredCooldowns.Add(Pair.Key);
		}
	}
	for (int32 BoneIdx : ExpiredCooldowns)
	{
		BoneEventCooldownTimers.Remove(BoneIdx);
	}

	// Aggregate per-bone contact counts, velocities, FluidTag
	TMap<int32, int32> NewBoneContactCounts;
	TMap<int32, FVector> BoneVelocitySums;
	TMap<int32, int32> BoneVelocityCounts;
	TMap<int32, FVector> BoneImpactOffsetSums;
	TMap<int32, int32> BoneImpactOffsetCounts;
	TMap<int32, TMap<int32, int32>> BoneSourceCounts;  // BoneIndex → (SourceID → Count)

	for (int32 i = 0; i < FeedbackCount; ++i)
	{
		const FGPUCollisionFeedback& Feedback = AllFeedback[i];

		// Filter by OwnerID
		if (Feedback.ColliderOwnerID != 0 && Feedback.ColliderOwnerID != MyOwnerID)
		{
			continue;
		}

		// Only process if BoneIndex is valid
		if (Feedback.BoneIndex < 0)
		{
			continue;
		}

		// Increment contact count
		int32& ContactCount = NewBoneContactCounts.FindOrAdd(Feedback.BoneIndex, 0);
		ContactCount++;

		// Sum velocities
		FVector ParticleVel(Feedback.ParticleVelocity.X, Feedback.ParticleVelocity.Y, Feedback.ParticleVelocity.Z);
		FVector& VelSum = BoneVelocitySums.FindOrAdd(Feedback.BoneIndex, FVector::ZeroVector);
		VelSum += ParticleVel;

		int32& VelCount = BoneVelocityCounts.FindOrAdd(Feedback.BoneIndex, 0);
		VelCount++;

		// Sum ImpactOffset
		FVector ImpactOffset(Feedback.ImpactOffset.X, Feedback.ImpactOffset.Y, Feedback.ImpactOffset.Z);
		FVector& OffsetSum = BoneImpactOffsetSums.FindOrAdd(Feedback.BoneIndex, FVector::ZeroVector);
		OffsetSum += ImpactOffset;

		int32& OffsetCount = BoneImpactOffsetCounts.FindOrAdd(Feedback.BoneIndex, 0);
		OffsetCount++;

		// Count per SourceID (for FluidTag determination)
		TMap<int32, int32>& SourceCounts = BoneSourceCounts.FindOrAdd(Feedback.BoneIndex);
		int32& SourceCount = SourceCounts.FindOrAdd(Feedback.ParticleSourceID, 0);
		SourceCount++;
	}

	// Calculate average velocities
	CurrentBoneAverageVelocities.Empty();
	for (const auto& Pair : BoneVelocitySums)
	{
		int32 BoneIdx = Pair.Key;
		int32* VelCountPtr = BoneVelocityCounts.Find(BoneIdx);
		int32 VelCount = VelCountPtr ? *VelCountPtr : 1;
		CurrentBoneAverageVelocities.Add(BoneIdx, Pair.Value / FMath::Max(1, VelCount));
	}

	// Store current contact counts
	CurrentBoneContactCounts = NewBoneContactCounts;

	// Fire collision events (when bEnableBoneCollisionEvents is enabled)
	if (bEnableBoneCollisionEvents && OnBoneParticleCollision.IsBound())
	{
		// Bones with enough contacts currently
		TSet<int32> CurrentContactBones;
		for (const auto& Pair : CurrentBoneContactCounts)
		{
			if (Pair.Value >= MinParticleCountForBoneEvent)
			{
				CurrentContactBones.Add(Pair.Key);
			}
		}

		// Detect new collisions (not in previous frame or cooldown expired)
		for (int32 BoneIdx : CurrentContactBones)
		{
			// Skip if on cooldown
			if (BoneEventCooldownTimers.Contains(BoneIdx))
			{
				continue;
			}

			// New collision or cooldown expired → fire event
			const int32* ContactCountPtr = CurrentBoneContactCounts.Find(BoneIdx);
			int32 ContactCount = ContactCountPtr ? *ContactCountPtr : 0;

			const FVector* AvgVelPtr = CurrentBoneAverageVelocities.Find(BoneIdx);
			FVector AverageVelocity = AvgVelPtr ? *AvgVelPtr : FVector::ZeroVector;

			// Calculate average ImpactOffset
			FVector ImpactOffsetAverage = FVector::ZeroVector;
			const FVector* OffsetSumPtr = BoneImpactOffsetSums.Find(BoneIdx);
			const int32* OffsetCountPtr = BoneImpactOffsetCounts.Find(BoneIdx);
			if (OffsetSumPtr && OffsetCountPtr && *OffsetCountPtr > 0)
			{
				ImpactOffsetAverage = *OffsetSumPtr / *OffsetCountPtr;
			}

			// Get bone name
			FName BoneName = GetBoneNameFromIndex(BoneIdx);

			// Determine FluidName: get FluidName from Preset of the most-collided SourceID
			FName FluidName = NAME_None;
			if (TargetSubsystem)
			{
				const TMap<int32, int32>* SourceCounts = BoneSourceCounts.Find(BoneIdx);
				if (SourceCounts)
				{
					int32 MaxSourceID = -1;
					int32 MaxCount = 0;
					for (const auto& SourcePair : *SourceCounts)
					{
						if (SourcePair.Value > MaxCount)
						{
							MaxCount = SourcePair.Value;
							MaxSourceID = SourcePair.Key;
						}
					}
					if (MaxSourceID >= 0)
					{
						UKawaiiFluidPresetDataAsset* Preset = TargetSubsystem->GetPresetBySourceID(MaxSourceID);
						if (Preset)
						{
							FluidName = Preset->GetFluidName();
						}
					}
				}
			}

			// Broadcast event (with FluidName)
			OnBoneParticleCollision.Broadcast(BoneIdx, BoneName, ContactCount, AverageVelocity, FluidName, ImpactOffsetAverage);

			// Set cooldown
			BoneEventCooldownTimers.Add(BoneIdx, BoneEventCooldown);
		}

		// Update previous frame state
		PreviousContactBones = CurrentContactBones;
	}
}

int32 UFluidInteractionComponent::GetBoneContactCount(int32 BoneIndex) const
{
	const int32* Count = CurrentBoneContactCounts.Find(BoneIndex);
	return Count ? *Count : 0;
}

void UFluidInteractionComponent::GetBonesWithContacts(TArray<int32>& OutBoneIndices) const
{
	OutBoneIndices.Empty(CurrentBoneContactCounts.Num());
	for (const auto& Pair : CurrentBoneContactCounts)
	{
		if (Pair.Value > 0)
		{
			OutBoneIndices.Add(Pair.Key);
		}
	}
}

FName UFluidInteractionComponent::GetBoneNameFromIndex(int32 BoneIndex) const
{
	// Check cache first
	const FName* CachedName = BoneIndexToNameCache.Find(BoneIndex);
	if (CachedName)
	{
		return *CachedName;
	}

	// If not in cache, query SkeletalMesh directly
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return NAME_None;
	}

	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!SkelMesh || !SkelMesh->GetSkeletalMeshAsset())
	{
		return NAME_None;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetSkeletalMeshAsset()->GetRefSkeleton();
	if (BoneIndex >= 0 && BoneIndex < RefSkeleton.GetNum())
	{
		return RefSkeleton.GetBoneName(BoneIndex);
	}

	return NAME_None;
}

USkeletalMeshComponent* UFluidInteractionComponent::GetOwnerSkeletalMesh() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}

	return Owner->FindComponentByClass<USkeletalMeshComponent>();
}

bool UFluidInteractionComponent::GetMostContactedBone(int32& OutBoneIndex, int32& OutContactCount) const
{
	OutBoneIndex = -1;
	OutContactCount = 0;

	for (const auto& Pair : CurrentBoneContactCounts)
	{
		if (Pair.Value > OutContactCount)
		{
			OutContactCount = Pair.Value;
			OutBoneIndex = Pair.Key;
		}
	}

	return OutBoneIndex >= 0;
}

//=============================================================================
// Boundary Particles Implementation (Flex-style Adhesion)
//=============================================================================

void UFluidInteractionComponent::SampleTriangleSurface(const FVector& V0, const FVector& V1, const FVector& V2,
                                                        float Spacing, TArray<FVector>& OutPoints)
{
	// 삼각형 변 길이
	FVector Edge1 = V1 - V0;
	FVector Edge2 = V2 - V0;

	float Len1 = Edge1.Size();
	float Len2 = Edge2.Size();

	if (Len1 < SMALL_NUMBER || Len2 < SMALL_NUMBER)
	{
		return;
	}

	// 삼각형 면적 기반으로 샘플링 개수 결정
	FVector Cross = FVector::CrossProduct(Edge1, Edge2);
	float Area = Cross.Size() * 0.5f;

	if (Area < Spacing * Spacing * 0.1f)
	{
		// 너무 작은 삼각형은 중심점만
		OutPoints.Add((V0 + V1 + V2) / 3.0f);
		return;
	}

	// Barycentric 좌표로 균일 샘플링
	int32 NumSamplesU = FMath::Max(1, FMath::CeilToInt(Len1 / Spacing));
	int32 NumSamplesV = FMath::Max(1, FMath::CeilToInt(Len2 / Spacing));

	for (int32 i = 0; i <= NumSamplesU; ++i)
	{
		float u = (float)i / (float)NumSamplesU;

		for (int32 j = 0; j <= NumSamplesV; ++j)
		{
			float v = (float)j / (float)NumSamplesV;

			// u + v <= 1 조건 (삼각형 내부)
			if (u + v > 1.0f)
			{
				continue;
			}

			// Barycentric 좌표로 점 계산
			FVector Point = V0 + Edge1 * u + Edge2 * v;
			OutPoints.Add(Point);
		}
	}
}

void UFluidInteractionComponent::GenerateBoundaryParticles()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// 배열 초기화
	BoundaryParticleLocalPositions.Empty();
	BoundaryParticleBoneIndices.Empty();
	BoundaryParticlePositions.Empty();
	BoundaryParticleVertexIndices.Empty();
	BoundaryParticleLocalNormals.Empty();
	bIsSkeletalMesh = false;

	// 1. 스켈레탈 메시 + Physics Asset 확인
	USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (SkelMesh && SkelMesh->GetSkeletalMeshAsset())
	{
		bIsSkeletalMesh = true;
		USkeletalMesh* SkeletalMesh = SkelMesh->GetSkeletalMeshAsset();

		// Physics Asset 가져오기
		UPhysicsAsset* PhysAsset = SkeletalMesh->GetPhysicsAsset();
		if (!PhysAsset)
		{
			UE_LOG(LogTemp, Warning, TEXT("FluidInteraction: No PhysicsAsset found for SkeletalMesh '%s'. Boundary particles not generated."),
				*SkeletalMesh->GetName());
			return;
		}

		// RefSkeleton (본 인덱스 조회용)
		const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();

		// 각 BodySetup 순회
		for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
		{
			if (!BodySetup) continue;

			// 본 인덱스 찾기
			int32 BoneIndex = RefSkeleton.FindBoneIndex(BodySetup->BoneName);
			if (BoneIndex == INDEX_NONE)
			{
				UE_LOG(LogTemp, Verbose, TEXT("FluidInteraction: Bone '%s' not found in skeleton, skipping"),
					*BodySetup->BoneName.ToString());
				continue;
			}

			// [디버깅] BodySetup의 BoneName과 BoneIndex 출력
			int32 ParticlesBeforeSampling = BoundaryParticleLocalPositions.Num();

			// AggGeom에서 모든 프리미티브 샘플링
			SampleAggGeomSurfaces(BodySetup->AggGeom, BoneIndex);

			int32 ParticlesAfterSampling = BoundaryParticleLocalPositions.Num();
			int32 GeneratedParticles = ParticlesAfterSampling - ParticlesBeforeSampling;

			if (GeneratedParticles > 0)
			{
				UE_LOG(LogTemp, Warning, TEXT("[BoundaryInit] BodySetup: '%s' (BoneIndex: %d) → Generated %d particles"),
					*BodySetup->BoneName.ToString(), BoneIndex, GeneratedParticles);
			}
		}

		UE_LOG(LogTemp, Log, TEXT("FluidInteraction: Generated %d boundary particles from PhysicsAsset '%s' (%d bodies)"),
			BoundaryParticleLocalPositions.Num(), *PhysAsset->GetName(), PhysAsset->SkeletalBodySetups.Num());
	}
	else
	{
		// 2. 스태틱 메시 + Simple Collision 확인
		UStaticMeshComponent* StaticMeshComp = Owner->FindComponentByClass<UStaticMeshComponent>();
		if (StaticMeshComp && StaticMeshComp->GetStaticMesh())
		{
			UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh();
			UBodySetup* BodySetup = StaticMesh->GetBodySetup();

			if (!BodySetup || (BodySetup->AggGeom.SphereElems.Num() == 0 &&
			                   BodySetup->AggGeom.SphylElems.Num() == 0 &&
			                   BodySetup->AggGeom.BoxElems.Num() == 0))
			{
				UE_LOG(LogTemp, Warning, TEXT("FluidInteraction: No Simple Collision (Sphere/Capsule/Box) found for StaticMesh '%s'. Boundary particles not generated."),
					*StaticMesh->GetName());
				return;
			}

			// 스태틱 메시는 본 없음 (BoneIndex = -1)
			SampleAggGeomSurfaces(BodySetup->AggGeom, -1);

			UE_LOG(LogTemp, Log, TEXT("FluidInteraction: Generated %d boundary particles from StaticMesh Simple Collision '%s'"),
				BoundaryParticleLocalPositions.Num(), *StaticMesh->GetName());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("FluidInteraction: No SkeletalMesh or StaticMesh found. Boundary particles not generated."));
			return;
		}
	}

	// 월드 위치/노멀 배열 초기화
	BoundaryParticlePositions.SetNum(BoundaryParticleLocalPositions.Num());
	BoundaryParticleNormals.SetNum(BoundaryParticleLocalNormals.Num());
	bBoundaryParticlesInitialized = BoundaryParticleLocalPositions.Num() > 0;

	// 첫 프레임 위치 업데이트
	if (bBoundaryParticlesInitialized)
	{
		UpdateBoundaryParticlePositions();
	}
}

void UFluidInteractionComponent::UpdateBoundaryParticlePositions()
{
	AActor* Owner = GetOwner();
	if (!Owner || BoundaryParticlePositions.Num() == 0)
	{
		return;
	}

	const int32 NumParticles = BoundaryParticlePositions.Num();
	const bool bHasNormals = (BoundaryParticleLocalNormals.Num() == NumParticles);

	// 스켈레탈 메시인 경우 본 트랜스폼 적용
	if (bIsSkeletalMesh)
	{
		USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		if (SkelMesh && BoundaryParticleLocalPositions.Num() == NumParticles)
		{
			for (int32 i = 0; i < NumParticles; ++i)
			{
				int32 BoneIdx = BoundaryParticleBoneIndices[i];
				if (BoneIdx >= 0)
				{
					// 현재 본의 월드 트랜스폼으로 본 로컬 좌표를 월드로 변환
					FTransform BoneWorldTransform = SkelMesh->GetBoneTransform(BoneIdx);
					BoundaryParticlePositions[i] = BoneWorldTransform.TransformPosition(BoundaryParticleLocalPositions[i]);
					// 노멀도 변환
					if (bHasNormals)
					{
						BoundaryParticleNormals[i] = BoneWorldTransform.TransformVectorNoScale(BoundaryParticleLocalNormals[i]);
					}
				}
				else
				{
					FTransform ComponentTransform = SkelMesh->GetComponentTransform();
					BoundaryParticlePositions[i] = ComponentTransform.TransformPosition(BoundaryParticleLocalPositions[i]);
					if (bHasNormals)
					{
						BoundaryParticleNormals[i] = ComponentTransform.TransformVectorNoScale(BoundaryParticleLocalNormals[i]);
					}
				}
			}
		}
	}
	else
	{
		// 스태틱 메시 또는 다른 컴포넌트 - 로컬 → 월드 변환
		USceneComponent* RootComp = Owner->GetRootComponent();
		if (RootComp && BoundaryParticleLocalPositions.Num() == NumParticles)
		{
			FTransform ComponentTransform = RootComp->GetComponentTransform();

			for (int32 i = 0; i < NumParticles; ++i)
			{
				BoundaryParticlePositions[i] = ComponentTransform.TransformPosition(BoundaryParticleLocalPositions[i]);
				if (bHasNormals)
				{
					BoundaryParticleNormals[i] = ComponentTransform.TransformVectorNoScale(BoundaryParticleLocalNormals[i]);
				}
			}
		}
	}
}

void UFluidInteractionComponent::DrawDebugBoundaryParticles()
{
	UWorld* World = GetWorld();
	if (!World || BoundaryParticlePositions.Num() == 0)
	{
		return;
	}

	const int32 NumParticles = BoundaryParticlePositions.Num();
	const bool bHasNormals = (BoundaryParticleNormals.Num() == NumParticles);

	for (int32 i = 0; i < NumParticles; ++i)
	{
		const FVector& Pos = BoundaryParticlePositions[i];

		// 점 그리기
		DrawDebugPoint(
			World,
			Pos,
			BoundaryParticleDebugSize,
			BoundaryParticleDebugColor,
			false,  // bPersistentLines
			-1.0f,  // LifeTime
			SDPG_Foreground  // DepthPriority - 메쉬 앞에 항상 표시
		);

		// 노말 화살표 그리기
		if (bShowBoundaryNormals && bHasNormals)
		{
			const FVector& Normal = BoundaryParticleNormals[i];
			FVector EndPos = Pos + Normal * BoundaryNormalLength;

			DrawDebugDirectionalArrow(
				World,
				Pos,
				EndPos,
				3.0f,  // ArrowSize
				FColor::Yellow,
				false,  // bPersistentLines
				-1.0f,  // LifeTime
				SDPG_Foreground,  // DepthPriority
				1.0f   // Thickness
			);
		}
	}
}

void UFluidInteractionComponent::RegenerateBoundaryParticles()
{
	bBoundaryParticlesInitialized = false;
	bIsSkeletalMesh = false;
	BoundaryParticleLocalPositions.Empty();
	BoundaryParticleBoneIndices.Empty();
	BoundaryParticlePositions.Empty();
	BoundaryParticleVertexIndices.Empty();
	BoundaryParticleNormals.Empty();
	BoundaryParticleLocalNormals.Empty();

	if (bEnableBoundaryParticles)
	{
		GenerateBoundaryParticles();
	}
}

void UFluidInteractionComponent::CollectGPUBoundaryParticles(FGPUBoundaryParticles& OutBoundaryParticles) const
{
	if (!bBoundaryParticlesInitialized || BoundaryParticlePositions.Num() == 0)
	{
		return;
	}

	const int32 NumParticles = BoundaryParticlePositions.Num();
	OutBoundaryParticles.Particles.Reserve(OutBoundaryParticles.Particles.Num() + NumParticles);

	// Component의 고유 ID 생성
	const int32 OwnerID = GetUniqueID();

	for (int32 i = 0; i < NumParticles; ++i)
	{
		FVector3f Position = FVector3f(BoundaryParticlePositions[i]);
		FVector3f Normal = (i < BoundaryParticleNormals.Num())
			? FVector3f(BoundaryParticleNormals[i])
			: FVector3f(0.0f, 0.0f, 1.0f);

		// Psi는 경계 입자의 볼륨 기여도
		// 낮을수록 밀려나는 힘 감소, 높을수록 강하게 밀려남
		float Psi = 0.1f;

		OutBoundaryParticles.Add(Position, Normal, OwnerID, Psi, BoundaryFrictionCoefficient);
	}
}

void UFluidInteractionComponent::CollectLocalBoundaryParticles(TArray<FGPUBoundaryParticleLocal>& OutLocalParticles, float Psi, float Friction) const
{
	if (!bBoundaryParticlesInitialized || BoundaryParticleLocalPositions.Num() == 0)
	{
		return;
	}

	const int32 NumParticles = BoundaryParticleLocalPositions.Num();
	OutLocalParticles.Reserve(OutLocalParticles.Num() + NumParticles);

	for (int32 i = 0; i < NumParticles; ++i)
	{
		FVector3f LocalPosition = FVector3f(BoundaryParticleLocalPositions[i]);
		FVector3f LocalNormal = (i < BoundaryParticleLocalNormals.Num())
			? FVector3f(BoundaryParticleLocalNormals[i])
			: FVector3f(0.0f, 0.0f, 1.0f);
		int32 BoneIndex = (i < BoundaryParticleBoneIndices.Num()) ? BoundaryParticleBoneIndices[i] : -1;

		OutLocalParticles.Add(FGPUBoundaryParticleLocal(LocalPosition, BoneIndex, LocalNormal, Psi, Friction));
	}
}

void UFluidInteractionComponent::CollectBoneTransformsForBoundary(TArray<FMatrix>& OutBoneTransforms, FMatrix& OutComponentTransform) const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		OutComponentTransform = FMatrix::Identity;
		return;
	}

	if (bIsSkeletalMesh)
	{
		USkeletalMeshComponent* SkelMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		if (SkelMesh)
		{
			// 모든 본의 월드 트랜스폼 수집
			const int32 NumBones = SkelMesh->GetNumBones();
			OutBoneTransforms.SetNum(NumBones);

			for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
			{
				FTransform BoneWorldTransform = SkelMesh->GetBoneTransform(BoneIdx);
				OutBoneTransforms[BoneIdx] = BoneWorldTransform.ToMatrixWithScale();
			}

			OutComponentTransform = SkelMesh->GetComponentTransform().ToMatrixWithScale();
		}
		else
		{
			OutComponentTransform = FMatrix::Identity;
		}
	}
	else
	{
		// 스태틱 메시 또는 다른 컴포넌트
		USceneComponent* RootComp = Owner->GetRootComponent();
		if (RootComp)
		{
			OutComponentTransform = RootComp->GetComponentTransform().ToMatrixWithScale();
		}
		else
		{
			OutComponentTransform = FMatrix::Identity;
		}
		OutBoneTransforms.Empty();
	}
}

//=============================================================================
// Physics Asset / Simple Collision 기반 표면 샘플링
//=============================================================================

void UFluidInteractionComponent::SampleSphereSurface(const FKSphereElem& Sphere, int32 BoneIndex, const FTransform& LocalTransform)
{
	float Radius = Sphere.Radius;
	FVector Center = Sphere.Center;

	// 샘플 수 결정: 표면적 기반 (4πr²)
	float SurfaceArea = 4.0f * PI * Radius * Radius;
	float SpacingSq = BoundaryParticleSpacing * BoundaryParticleSpacing;
	int32 NumSamples = FMath::Max(8, FMath::CeilToInt(SurfaceArea / SpacingSq));
	NumSamples = FMath::Min(NumSamples, 200);  // 최대 제한

	// 피보나치 구 샘플링 (균일 분포)
	float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
	float AngleIncrement = 2.0f * PI * GoldenRatio;

	for (int32 i = 0; i < NumSamples; ++i)
	{
		float t = (NumSamples > 1) ? (float)i / (float)(NumSamples - 1) : 0.5f;
		float Phi = FMath::Acos(1.0f - 2.0f * t);
		float Theta = AngleIncrement * i;

		// 구면 좌표 → 카르테시안
		FVector LocalPoint(
			Radius * FMath::Sin(Phi) * FMath::Cos(Theta),
			Radius * FMath::Sin(Phi) * FMath::Sin(Theta),
			Radius * FMath::Cos(Phi)
		);

		// 프리미티브 로컬 → 본 로컬
		FVector BoneLocalPos = LocalTransform.TransformPosition(LocalPoint + Center);

		// 노멀은 중심에서 외부로
		FVector LocalNormal = LocalPoint.GetSafeNormal();
		FVector BoneLocalNormal = LocalTransform.TransformVectorNoScale(LocalNormal);

		BoundaryParticleLocalPositions.Add(BoneLocalPos);
		BoundaryParticleBoneIndices.Add(BoneIndex);
		BoundaryParticleLocalNormals.Add(BoneLocalNormal);
	}
}

void UFluidInteractionComponent::SampleHemisphere(const FTransform& Transform, float Radius, float ZOffset,
                                                   int32 ZDirection, int32 BoneIndex, int32 NumSamples)
{
	if (NumSamples <= 0) return;

	float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
	float AngleIncrement = 2.0f * PI * GoldenRatio;

	for (int32 i = 0; i < NumSamples; ++i)
	{
		// 반구만 샘플링
		float t = (float)i / (float)(NumSamples * 2);
		if (ZDirection < 0) t = 1.0f - t;

		// t가 반구 범위를 벗어나면 스킵
		if ((ZDirection > 0 && t > 0.5f) || (ZDirection < 0 && t < 0.5f))
			continue;

		float Phi = FMath::Acos(1.0f - 2.0f * t);
		float Theta = AngleIncrement * i;

		FVector LocalPoint(
			Radius * FMath::Sin(Phi) * FMath::Cos(Theta),
			Radius * FMath::Sin(Phi) * FMath::Sin(Theta),
			Radius * FMath::Cos(Phi) + ZOffset
		);

		FVector LocalNormal = FVector(
			LocalPoint.X,
			LocalPoint.Y,
			LocalPoint.Z - ZOffset
		).GetSafeNormal();

		FVector BoneLocalPos = Transform.TransformPosition(LocalPoint);
		FVector BoneLocalNormal = Transform.TransformVectorNoScale(LocalNormal);

		BoundaryParticleLocalPositions.Add(BoneLocalPos);
		BoundaryParticleBoneIndices.Add(BoneIndex);
		BoundaryParticleLocalNormals.Add(BoneLocalNormal);
	}
}

void UFluidInteractionComponent::SampleCapsuleSurface(const FKSphylElem& Capsule, int32 BoneIndex)
{
	float Radius = Capsule.Radius;
	float HalfLength = Capsule.Length * 0.5f;
	FTransform CapsuleTransform = Capsule.GetTransform();

	// 원통 부분
	float CylinderHeight = Capsule.Length;
	float CylinderArea = 2.0f * PI * Radius * CylinderHeight;

	// 반구 2개 = 완전한 구
	float SphereArea = 4.0f * PI * Radius * Radius;

	float TotalArea = CylinderArea + SphereArea;
	float SpacingSq = BoundaryParticleSpacing * BoundaryParticleSpacing;
	int32 TotalSamples = FMath::Max(12, FMath::CeilToInt(TotalArea / SpacingSq));
	TotalSamples = FMath::Min(TotalSamples, 500);

	// 면적 비율로 샘플 분배
	float CylinderRatio = CylinderArea / TotalArea;
	int32 CylinderSamples = FMath::CeilToInt(TotalSamples * CylinderRatio);
	int32 SphereSamples = TotalSamples - CylinderSamples;

	// 원통 부분 샘플링
	int32 NumRings = FMath::Max(2, FMath::CeilToInt(CylinderHeight / BoundaryParticleSpacing));
	int32 NumSegments = FMath::Max(6, CylinderSamples / FMath::Max(1, NumRings));

	for (int32 Ring = 0; Ring <= NumRings; ++Ring)
	{
		float Z = -HalfLength + (CylinderHeight * Ring / FMath::Max(1, NumRings));

		for (int32 Seg = 0; Seg < NumSegments; ++Seg)
		{
			float Angle = 2.0f * PI * Seg / NumSegments;

			FVector LocalPoint(
				Radius * FMath::Cos(Angle),
				Radius * FMath::Sin(Angle),
				Z
			);

			FVector LocalNormal(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);

			FVector BoneLocalPos = CapsuleTransform.TransformPosition(LocalPoint);
			FVector BoneLocalNormal = CapsuleTransform.TransformVectorNoScale(LocalNormal);

			BoundaryParticleLocalPositions.Add(BoneLocalPos);
			BoundaryParticleBoneIndices.Add(BoneIndex);
			BoundaryParticleLocalNormals.Add(BoneLocalNormal);
		}
	}

	// 반구 부분 (상단/하단)
	int32 HalfSphereSamples = SphereSamples / 2;
	SampleHemisphere(CapsuleTransform, Radius, +HalfLength, +1, BoneIndex, HalfSphereSamples);
	SampleHemisphere(CapsuleTransform, Radius, -HalfLength, -1, BoneIndex, HalfSphereSamples);
}

void UFluidInteractionComponent::SampleBoxSurface(const FKBoxElem& Box, int32 BoneIndex)
{
	FVector HalfExtent(Box.X * 0.5f, Box.Y * 0.5f, Box.Z * 0.5f);
	FTransform BoxTransform = Box.GetTransform();

	// 6개 면 정의: 노멀, 거리, 축1, 축2, 크기1, 크기2
	struct FBoxFace
	{
		FVector Normal;
		float Distance;
		FVector Axis1, Axis2;
		float Size1, Size2;
	};

	TArray<FBoxFace> Faces;
	Faces.Add({ FVector( 1, 0, 0), (float)HalfExtent.X, FVector(0, 1, 0), FVector(0, 0, 1), (float)Box.Y, (float)Box.Z });
	Faces.Add({ FVector(-1, 0, 0), (float)HalfExtent.X, FVector(0, 1, 0), FVector(0, 0, 1), (float)Box.Y, (float)Box.Z });
	Faces.Add({ FVector( 0, 1, 0), (float)HalfExtent.Y, FVector(1, 0, 0), FVector(0, 0, 1), (float)Box.X, (float)Box.Z });
	Faces.Add({ FVector( 0,-1, 0), (float)HalfExtent.Y, FVector(1, 0, 0), FVector(0, 0, 1), (float)Box.X, (float)Box.Z });
	Faces.Add({ FVector( 0, 0, 1), (float)HalfExtent.Z, FVector(1, 0, 0), FVector(0, 1, 0), (float)Box.X, (float)Box.Y });
	Faces.Add({ FVector( 0, 0,-1), (float)HalfExtent.Z, FVector(1, 0, 0), FVector(0, 1, 0), (float)Box.X, (float)Box.Y });

	float SpacingSq = BoundaryParticleSpacing * BoundaryParticleSpacing;

	for (const FBoxFace& Face : Faces)
	{
		float FaceArea = Face.Size1 * Face.Size2;
		int32 NumU = FMath::Max(1, FMath::CeilToInt(Face.Size1 / BoundaryParticleSpacing));
		int32 NumV = FMath::Max(1, FMath::CeilToInt(Face.Size2 / BoundaryParticleSpacing));

		for (int32 u = 0; u <= NumU; ++u)
		{
			for (int32 v = 0; v <= NumV; ++v)
			{
				float t1 = (NumU > 0) ? ((float)u / (float)NumU - 0.5f) : 0.0f;
				float t2 = (NumV > 0) ? ((float)v / (float)NumV - 0.5f) : 0.0f;

				FVector LocalPoint = Face.Normal * Face.Distance
				                   + Face.Axis1 * (t1 * Face.Size1)
				                   + Face.Axis2 * (t2 * Face.Size2);

				FVector BoneLocalPos = BoxTransform.TransformPosition(LocalPoint);
				FVector BoneLocalNormal = BoxTransform.TransformVectorNoScale(Face.Normal);

				BoundaryParticleLocalPositions.Add(BoneLocalPos);
				BoundaryParticleBoneIndices.Add(BoneIndex);
				BoundaryParticleLocalNormals.Add(BoneLocalNormal);
			}
		}
	}
}

void UFluidInteractionComponent::SampleAggGeomSurfaces(const FKAggregateGeom& AggGeom, int32 BoneIndex)
{
	// Sphere 콜라이더
	for (const FKSphereElem& Sphere : AggGeom.SphereElems)
	{
		SampleSphereSurface(Sphere, BoneIndex, Sphere.GetTransform());
	}

	// Capsule(Sphyl) 콜라이더
	for (const FKSphylElem& Capsule : AggGeom.SphylElems)
	{
		SampleCapsuleSurface(Capsule, BoneIndex);
	}

	// Box 콜라이더
	for (const FKBoxElem& Box : AggGeom.BoxElems)
	{
		SampleBoxSurface(Box, BoneIndex);
	}
}
