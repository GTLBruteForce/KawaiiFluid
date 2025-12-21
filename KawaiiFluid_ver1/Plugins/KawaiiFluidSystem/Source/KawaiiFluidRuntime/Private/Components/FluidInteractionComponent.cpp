// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/FluidInteractionComponent.h"
#include "Core/FluidSimulator.h"
#include "Collision/MeshFluidCollider.h"
#include "Kismet/GameplayStatics.h"

UFluidInteractionComponent::UFluidInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	TargetSimulator = nullptr;
	bCanAttachFluid = true;
	AdhesionMultiplier = 1.0f;
	DragAlongStrength = 0.5f;
	bAutoCreateCollider = true;

	AttachedParticleCount = 0;
	bIsWet = false;

	AutoCollider = nullptr;
	PreviousLocation = FVector::ZeroVector;
}

void UFluidInteractionComponent::BeginPlay()
{
	Super::BeginPlay();

	PreviousLocation = GetOwner()->GetActorLocation();

	if (!TargetSimulator)
	{
		AActor* FoundActor = UGameplayStatics::GetActorOfClass(GetWorld(), AFluidSimulator::StaticClass());
		if (FoundActor)
		{
			TargetSimulator = Cast<AFluidSimulator>(FoundActor);
		}
	}

	if (bAutoCreateCollider)
	{
		CreateAutoCollider();
	}

	RegisterWithSimulator();
}

void UFluidInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromSimulator();

	Super::EndPlay(EndPlayReason);
}

void UFluidInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!TargetSimulator)
	{
		return;
	}

	int32 PrevCount = AttachedParticleCount;
	UpdateAttachedParticleCount();

	if (AttachedParticleCount > 0 && PrevCount == 0)
	{
		bIsWet = true;
		OnFluidAttached.Broadcast(AttachedParticleCount);
	}
	else if (AttachedParticleCount == 0 && PrevCount > 0)
	{
		bIsWet = false;
		OnFluidDetached.Broadcast();
	}

	if (DragAlongStrength > 0.0f && AttachedParticleCount > 0)
	{
		ApplyDragAlong(DeltaTime);
	}

	PreviousLocation = GetOwner()->GetActorLocation();
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
		AutoCollider->bAllowAdhesion = bCanAttachFluid;
		AutoCollider->AdhesionMultiplier = AdhesionMultiplier;
	}
}

void UFluidInteractionComponent::RegisterWithSimulator()
{
	if (TargetSimulator && AutoCollider)
	{
		TargetSimulator->RegisterCollider(AutoCollider);
	}
}

void UFluidInteractionComponent::UnregisterFromSimulator()
{
	if (TargetSimulator && AutoCollider)
	{
		TargetSimulator->UnregisterCollider(AutoCollider);
	}
}

void UFluidInteractionComponent::UpdateAttachedParticleCount()
{
	if (!TargetSimulator)
	{
		AttachedParticleCount = 0;
		return;
	}

	AActor* Owner = GetOwner();
	int32 Count = 0;

	const TArray<FFluidParticle>& Particles = TargetSimulator->GetParticles();
	for (const FFluidParticle& Particle : Particles)
	{
		if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
		{
			++Count;
		}
	}

	AttachedParticleCount = Count;
}

void UFluidInteractionComponent::ApplyDragAlong(float DeltaTime)
{
	if (!TargetSimulator)
	{
		return;
	}

	AActor* Owner = GetOwner();
	FVector CurrentLocation = Owner->GetActorLocation();
	FVector Delta = CurrentLocation - PreviousLocation;  // 위치 변화량

	if (Delta.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		return;
	}

	TArray<FFluidParticle>& Particles = TargetSimulator->GetParticlesMutable();
	for (FFluidParticle& Particle : Particles)
	{
		if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
		{
			// 위치 직접 이동 (속도가 아닌 위치를 업데이트)
			FVector PositionDelta = Delta * DragAlongStrength;
			Particle.Position += PositionDelta;
			Particle.PredictedPosition += PositionDelta;
		}
	}
}

void UFluidInteractionComponent::DetachAllFluid()
{
	if (!TargetSimulator)
	{
		return;
	}

	AActor* Owner = GetOwner();

	TArray<FFluidParticle>& Particles = TargetSimulator->GetParticlesMutable();
	for (FFluidParticle& Particle : Particles)
	{
		if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
		{
			Particle.bIsAttached = false;
			Particle.AttachedActor.Reset();
		}
	}

	AttachedParticleCount = 0;
	bIsWet = false;
}

void UFluidInteractionComponent::PushFluid(FVector Direction, float Force)
{
	if (!TargetSimulator)
	{
		return;
	}

	AActor* Owner = GetOwner();
	FVector NormalizedDir = Direction.GetSafeNormal();

	TArray<FFluidParticle>& Particles = TargetSimulator->GetParticlesMutable();
	for (FFluidParticle& Particle : Particles)
	{
		float Distance = FVector::Dist(Particle.Position, Owner->GetActorLocation());

		if (Distance < 200.0f)
		{
			float FallOff = 1.0f - (Distance / 200.0f);
			Particle.Velocity += NormalizedDir * Force * FallOff;

			if (Particle.bIsAttached && Particle.AttachedActor.Get() == Owner)
			{
				Particle.bIsAttached = false;
				Particle.AttachedActor.Reset();
			}
		}
	}
}

void UFluidInteractionComponent::SetTargetSimulator(AFluidSimulator* Simulator)
{
	UnregisterFromSimulator();
	TargetSimulator = Simulator;
	RegisterWithSimulator();
}
