// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/KawaiiFluidSimulationComponent.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Core/SpatialHash.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Collision/FluidCollider.h"
#include "Components/FluidInteractionComponent.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

UKawaiiFluidSimulationComponent::UKawaiiFluidSimulationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	bTickInEditor = false;
}

UKawaiiFluidSimulationComponent::~UKawaiiFluidSimulationComponent()
{
}

void UKawaiiFluidSimulationComponent::BeginPlay()
{
	Super::BeginPlay();

	// Create default Preset if none assigned (critical for simulation!)
	if (!Preset)
	{
		Preset = NewObject<UKawaiiFluidPresetDataAsset>(this, NAME_None, RF_Transient);
		UE_LOG(LogTemp, Warning, TEXT("FluidSimulationComponent [%s]: No Preset assigned, using default values"), *GetName());
	}

	InitializeSpatialHash();
	InitializeRenderResource();
	InitializeDebugMesh();

	if (UWorld* World = GetWorld())
	{
		// Register with KawaiiFluidSimulatorSubsystem (for simulation)
		if (UKawaiiFluidSimulatorSubsystem* SimSubsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			SimSubsystem->RegisterComponent(this);
			UE_LOG(LogTemp, Log, TEXT("FluidSimulationComponent registered with SimulatorSubsystem: %s"), *GetName());
		}

		// Register with FluidRendererSubsystem (for rendering)
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->RegisterRenderableComponent(this);
			UE_LOG(LogTemp, Log, TEXT("FluidSimulationComponent registered with RendererSubsystem: %s"), *GetName());
		}
	}

	// Auto spawn
	if (bSpawnOnBeginPlay && AutoSpawnCount > 0)
	{
		AActor* Owner = GetOwner();
		FVector SpawnLocation = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
		SpawnParticles(SpawnLocation, AutoSpawnCount, AutoSpawnRadius);
	}
}

void UKawaiiFluidSimulationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Clear delegates
	OnParticleHit.Clear();
	bEnableParticleHitEvents = false;

	if (UWorld* World = GetWorld())
	{
		// Unregister from KawaiiFluidSimulatorSubsystem
		if (UKawaiiFluidSimulatorSubsystem* SimSubsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			SimSubsystem->UnregisterComponent(this);
			UE_LOG(LogTemp, Log, TEXT("FluidSimulationComponent unregistered from SimulatorSubsystem: %s"), *GetName());
		}

		// Unregister from FluidRendererSubsystem
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->UnregisterRenderableComponent(this);
			UE_LOG(LogTemp, Log, TEXT("FluidSimulationComponent unregistered from RendererSubsystem: %s"), *GetName());
		}
	}

	Super::EndPlay(EndPlayReason);
}

void UKawaiiFluidSimulationComponent::BeginDestroy()
{
	// Release render resource
	if (RenderResource.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(ReleaseFluidRenderResource)(
			[RenderResource = MoveTemp(RenderResource)](FRHICommandListImmediate& RHICmdList) mutable
			{
				if (RenderResource.IsValid())
				{
					RenderResource->ReleaseResource();
					RenderResource.Reset();
				}
			}
		);
	}

	Super::BeginDestroy();
}

void UKawaiiFluidSimulationComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Reset event counter at frame start
	EventCountThisFrame.store(0, std::memory_order_relaxed);

	// Continuous spawn handling
	if (bContinuousSpawn && ParticlesPerSecond > 0.0f)
	{
		// Check max particle limit
		const bool bCanSpawn = (MaxParticleCount <= 0) || (Particles.Num() < MaxParticleCount);

		if (bCanSpawn)
		{
			SpawnAccumulatedTime += DeltaTime;
			const float SpawnInterval = 1.0f / ParticlesPerSecond;

			while (SpawnAccumulatedTime >= SpawnInterval)
			{
				// Check limit again for each spawn
				if (MaxParticleCount > 0 && Particles.Num() >= MaxParticleCount)
				{
					SpawnAccumulatedTime = 0.0f;
					break;
				}

				// Calculate spawn position
				FVector SpawnLocation = GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
				SpawnLocation += SpawnOffset;

				// Add random offset within radius
				if (ContinuousSpawnRadius > 0.0f)
				{
					FVector RandomOffset = FMath::VRand() * FMath::FRandRange(0.0f, ContinuousSpawnRadius);
					SpawnLocation += RandomOffset;
				}

				// Spawn particle with initial velocity
				SpawnParticle(SpawnLocation, SpawnVelocity);

				SpawnAccumulatedTime -= SpawnInterval;
			}
		}
	}

	// Note: Actual simulation is handled by KawaiiFluidSimulatorSubsystem
	// This tick is for independent mode only or render updates

	// Update render data
	UpdateRenderData();

	// Update debug mesh
	if (ShouldUseDebugMesh())
	{
		UpdateDebugInstances();
	}

	// Reset external force after frame
	AccumulatedExternalForce = FVector::ZeroVector;
}

void UKawaiiFluidSimulationComponent::InitializeSpatialHash()
{
	float CellSize = 20.0f;
	if (bOverride_SmoothingRadius)
	{
		CellSize = Override_SmoothingRadius;
	}
	else if (Preset)
	{
		CellSize = Preset->SmoothingRadius;
	}
	SpatialHash = MakeShared<FSpatialHash>(CellSize);
}

void UKawaiiFluidSimulationComponent::InitializeRenderResource()
{
	RenderResource = MakeShared<FKawaiiFluidRenderResource>();

	ENQUEUE_RENDER_COMMAND(InitFluidRenderResource)(
		[RenderResourcePtr = RenderResource.Get()](FRHICommandListImmediate& RHICmdList)
		{
			RenderResourcePtr->InitResource(RHICmdList);
		}
	);
}

void UKawaiiFluidSimulationComponent::InitializeDebugMesh()
{
	if (!bEnableDebugRendering)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Create instanced mesh component
	DebugMeshComponent = NewObject<UInstancedStaticMeshComponent>(Owner);
	DebugMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DebugMeshComponent->SetAbsolute(true, true, true);  // Absolute location/rotation/scale - ignore parent transform
	DebugMeshComponent->RegisterComponent();
	DebugMeshComponent->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);

	// Load default sphere mesh
	static UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh)
	{
		DebugMeshComponent->SetStaticMesh(SphereMesh);
	}

	// Load default material
	static UMaterial* DefaultMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (DefaultMaterial)
	{
		DebugMeshComponent->SetMaterial(0, DefaultMaterial);
	}
}

//========================================
// IKawaiiFluidRenderable Implementation
//========================================

FKawaiiFluidRenderResource* UKawaiiFluidSimulationComponent::GetFluidRenderResource() const
{
	return RenderResource.Get();
}

bool UKawaiiFluidSimulationComponent::IsFluidRenderResourceValid() const
{
	return RenderResource.IsValid() && RenderResource->IsValid();
}

float UKawaiiFluidSimulationComponent::GetParticleRenderRadius() const
{
	return Preset ? Preset->ParticleRadius : 5.0f;
}

FString UKawaiiFluidSimulationComponent::GetDebugName() const
{
	AActor* Owner = GetOwner();
	return FString::Printf(TEXT("SimComponent_%s"), Owner ? *Owner->GetName() : TEXT("NoOwner"));
}

bool UKawaiiFluidSimulationComponent::ShouldUseSSFR() const
{
	return RenderingMode == EKawaiiFluidRenderingMode::SSFR;
}

bool UKawaiiFluidSimulationComponent::ShouldUseDebugMesh() const
{
	return bEnableDebugRendering &&
	       RenderingMode == EKawaiiFluidRenderingMode::ISM;
}

UInstancedStaticMeshComponent* UKawaiiFluidSimulationComponent::GetDebugMeshComponent() const
{
	return DebugMeshComponent;
}

int32 UKawaiiFluidSimulationComponent::GetParticleCount() const
{
	return Particles.Num();
}

//========================================
// Blueprint API
//========================================

void UKawaiiFluidSimulationComponent::SpawnParticles(FVector Location, int32 Count, float SpawnRadius)
{
	int32 MaxParticles = Preset ? Preset->MaxParticles : 10000;
	int32 ActualCount = FMath::Min(Count, MaxParticles - Particles.Num());
	float ParticleMass = Preset ? Preset->ParticleMass : 1.0f;

	for (int32 i = 0; i < ActualCount; ++i)
	{
		FVector RandomOffset = FMath::VRand() * FMath::FRandRange(0.0f, SpawnRadius);
		FVector SpawnPos = Location + RandomOffset;

		FFluidParticle NewParticle(SpawnPos, NextParticleID++);
		NewParticle.Mass = ParticleMass;

		Particles.Add(NewParticle);
	}
}

int32 UKawaiiFluidSimulationComponent::SpawnParticle(FVector Position, FVector Velocity)
{
	int32 MaxParticles = Preset ? Preset->MaxParticles : 10000;
	if (Particles.Num() >= MaxParticles)
	{
		return -1;
	}

	FFluidParticle NewParticle(Position, NextParticleID++);
	NewParticle.Velocity = Velocity;
	NewParticle.Mass = Preset ? Preset->ParticleMass : 1.0f;

	return Particles.Add(NewParticle);
}

void UKawaiiFluidSimulationComponent::ApplyExternalForce(FVector Force)
{
	AccumulatedExternalForce += Force;
}

void UKawaiiFluidSimulationComponent::ApplyForceToParticle(int32 ParticleIndex, FVector Force)
{
	if (Particles.IsValidIndex(ParticleIndex))
	{
		Particles[ParticleIndex].Velocity += Force;
	}
}

void UKawaiiFluidSimulationComponent::RegisterCollider(UFluidCollider* Collider)
{
	if (Collider && !Colliders.Contains(Collider))
	{
		Colliders.Add(Collider);
	}
}

void UKawaiiFluidSimulationComponent::UnregisterCollider(UFluidCollider* Collider)
{
	Colliders.Remove(Collider);
}

void UKawaiiFluidSimulationComponent::RegisterInteractionComponent(UFluidInteractionComponent* Component)
{
	if (Component && !InteractionComponents.Contains(Component))
	{
		InteractionComponents.Add(Component);
	}
}

void UKawaiiFluidSimulationComponent::UnregisterInteractionComponent(UFluidInteractionComponent* Component)
{
	InteractionComponents.Remove(Component);
}

TArray<FVector> UKawaiiFluidSimulationComponent::GetParticlePositions() const
{
	TArray<FVector> Positions;
	Positions.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Positions.Add(Particle.Position);
	}

	return Positions;
}

TArray<FVector> UKawaiiFluidSimulationComponent::GetParticleVelocities() const
{
	TArray<FVector> Velocities;
	Velocities.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Velocities.Add(Particle.Velocity);
	}

	return Velocities;
}

void UKawaiiFluidSimulationComponent::ClearAllParticles()
{
	Particles.Empty();
	ParticleLastEventTime.Empty();

	if (DebugMeshComponent)
	{
		DebugMeshComponent->ClearInstances();
	}
}

void UKawaiiFluidSimulationComponent::SetContinuousSpawnEnabled(bool bEnabled)
{
	bContinuousSpawn = bEnabled;
	if (!bEnabled)
	{
		SpawnAccumulatedTime = 0.0f;
	}
}

void UKawaiiFluidSimulationComponent::SetParticlesPerSecond(float NewRate)
{
	ParticlesPerSecond = FMath::Max(0.1f, NewRate);
}

void UKawaiiFluidSimulationComponent::SetSpawnVelocity(FVector NewVelocity)
{
	SpawnVelocity = NewVelocity;
}

//========================================
// Query Functions
//========================================

TArray<int32> UKawaiiFluidSimulationComponent::GetParticlesInRadius(FVector Location, float Radius) const
{
	TArray<int32> Result;
	const float RadiusSq = Radius * Radius;

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		const float DistSq = FVector::DistSquared(Particles[i].Position, Location);
		if (DistSq <= RadiusSq)
		{
			Result.Add(i);
		}
	}

	return Result;
}

TArray<int32> UKawaiiFluidSimulationComponent::GetParticlesInBox(FVector Center, FVector Extent) const
{
	TArray<int32> Result;
	const FBox Box(Center - Extent, Center + Extent);

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		if (Box.IsInside(Particles[i].Position))
		{
			Result.Add(i);
		}
	}

	return Result;
}

TArray<int32> UKawaiiFluidSimulationComponent::GetParticlesNearActor(AActor* Actor, float Radius) const
{
	if (!Actor)
	{
		return TArray<int32>();
	}

	return GetParticlesInRadius(Actor->GetActorLocation(), Radius);
}

bool UKawaiiFluidSimulationComponent::GetParticleInfo(int32 ParticleIndex, FVector& OutPosition, FVector& OutVelocity, float& OutDensity) const
{
	if (!Particles.IsValidIndex(ParticleIndex))
	{
		return false;
	}

	const FFluidParticle& Particle = Particles[ParticleIndex];
	OutPosition = Particle.Position;
	OutVelocity = Particle.Velocity;
	OutDensity = Particle.Density;

	return true;
}

//========================================
// Helpers
//========================================

FKawaiiFluidSimulationParams UKawaiiFluidSimulationComponent::BuildSimulationParams() const
{
	FKawaiiFluidSimulationParams Params;
	Params.ExternalForce = AccumulatedExternalForce;
	Params.Colliders = Colliders;
	Params.InteractionComponents = InteractionComponents;
	Params.World = GetWorld();
	Params.bUseWorldCollision = bUseWorldCollision;
	Params.CollisionChannel = Preset ? Preset->CollisionChannel : TEnumAsByte<ECollisionChannel>(ECC_GameTraceChannel1);
	Params.ParticleRadius = Preset ? Preset->ParticleRadius : 5.0f;
	Params.IgnoreActor = GetOwner();

	// Collision event settings
	Params.bEnableCollisionEvents = bEnableParticleHitEvents;
	Params.MinVelocityForEvent = MinVelocityForEvent;
	Params.MaxEventsPerFrame = MaxEventsPerFrame;
	Params.EventCountPtr = &EventCountThisFrame;
	Params.EventCooldownPerParticle = EventCooldownPerParticle;
	Params.ParticleLastEventTimePtr = &ParticleLastEventTime;
	Params.CurrentGameTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	// Bind collision event callback
	if (bEnableParticleHitEvents && OnParticleHit.IsBound())
	{
		// Capture weak reference to this component
		TWeakObjectPtr<UKawaiiFluidSimulationComponent> WeakThis(const_cast<UKawaiiFluidSimulationComponent*>(this));
		Params.OnCollisionEvent.BindLambda([WeakThis](const FKawaiiFluidCollisionEvent& Event)
		{
			if (UKawaiiFluidSimulationComponent* Comp = WeakThis.Get())
			{
				if (Comp->OnParticleHit.IsBound())
				{
					Comp->OnParticleHit.Broadcast(
						Event.ParticleIndex,
						Event.HitActor.Get(),
						Event.HitLocation,
						Event.HitNormal,
						Event.HitSpeed
					);
				}
			}
		});
	}

	return Params;
}

//========================================
// Override System
//========================================

#if WITH_EDITOR
void UKawaiiFluidSimulationComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const FString PropertyNameStr = PropertyName.ToString();

	// Mark RuntimePreset dirty when override properties change
	if (PropertyNameStr.StartsWith(TEXT("bOverride_")) || PropertyNameStr.StartsWith(TEXT("Override_")))
	{
		MarkRuntimePresetDirty();
	}

	// Also mark dirty when Preset reference changes
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationComponent, Preset))
	{
		MarkRuntimePresetDirty();
	}
}
#endif

UKawaiiFluidPresetDataAsset* UKawaiiFluidSimulationComponent::GetEffectivePreset()
{
	// No overrides - return original preset
	if (!HasAnyOverride())
	{
		return Preset;
	}

	// Update RuntimePreset if dirty
	if (bRuntimePresetDirty)
	{
		UpdateRuntimePreset();
	}

	return RuntimePreset ? RuntimePreset : Preset;
}

void UKawaiiFluidSimulationComponent::UpdateRuntimePreset()
{
	if (!HasAnyOverride() || !Preset)
	{
		RuntimePreset = nullptr;
		bRuntimePresetDirty = false;
		return;
	}

	// Create or update RuntimePreset from original Preset
	if (!RuntimePreset)
	{
		RuntimePreset = DuplicateObject<UKawaiiFluidPresetDataAsset>(Preset, this);
	}
	else
	{
		// Copy ALL values from original preset (in case it changed)
		RuntimePreset->ContextClass = Preset->ContextClass;
		RuntimePreset->RestDensity = Preset->RestDensity;
		RuntimePreset->ParticleMass = Preset->ParticleMass;
		RuntimePreset->SmoothingRadius = Preset->SmoothingRadius;
		RuntimePreset->SubstepDeltaTime = Preset->SubstepDeltaTime;
		RuntimePreset->MaxSubsteps = Preset->MaxSubsteps;
		RuntimePreset->Gravity = Preset->Gravity;
		RuntimePreset->Compliance = Preset->Compliance;
		RuntimePreset->ViscosityCoefficient = Preset->ViscosityCoefficient;
		RuntimePreset->AdhesionStrength = Preset->AdhesionStrength;
		RuntimePreset->AdhesionRadius = Preset->AdhesionRadius;
		RuntimePreset->DetachThreshold = Preset->DetachThreshold;
		RuntimePreset->Restitution = Preset->Restitution;
		RuntimePreset->Friction = Preset->Friction;
		RuntimePreset->CollisionChannel = Preset->CollisionChannel;
		RuntimePreset->ParticleRadius = Preset->ParticleRadius;
		RuntimePreset->Color = Preset->Color;
		RuntimePreset->MaxParticles = Preset->MaxParticles;
	}

	// Apply overrides
	if (bOverride_RestDensity)
	{
		RuntimePreset->RestDensity = Override_RestDensity;
	}
	if (bOverride_Compliance)
	{
		RuntimePreset->Compliance = Override_Compliance;
	}
	if (bOverride_SmoothingRadius)
	{
		RuntimePreset->SmoothingRadius = Override_SmoothingRadius;
	}
	if (bOverride_ViscosityCoefficient)
	{
		RuntimePreset->ViscosityCoefficient = Override_ViscosityCoefficient;
	}
	if (bOverride_Gravity)
	{
		RuntimePreset->Gravity = Override_Gravity;
	}
	if (bOverride_AdhesionStrength)
	{
		RuntimePreset->AdhesionStrength = Override_AdhesionStrength;
	}

	bRuntimePresetDirty = false;
}

void UKawaiiFluidSimulationComponent::UpdateRenderData()
{
	if (!RenderResource.IsValid() || Particles.Num() == 0)
	{
		return;
	}

	TArray<FKawaiiRenderParticle> RenderParticles = ConvertToRenderParticles();
	RenderResource->UpdateParticleData(RenderParticles);
}

void UKawaiiFluidSimulationComponent::UpdateDebugInstances()
{
	if (!DebugMeshComponent)
	{
		return;
	}

	const int32 ParticleCount = Particles.Num();
	const int32 InstanceCount = DebugMeshComponent->GetInstanceCount();

	// Adjust instance count
	if (InstanceCount < ParticleCount)
	{
		for (int32 i = InstanceCount; i < ParticleCount; ++i)
		{
			DebugMeshComponent->AddInstance(FTransform::Identity);
		}
	}
	else if (InstanceCount > ParticleCount)
	{
		for (int32 i = InstanceCount - 1; i >= ParticleCount; --i)
		{
			DebugMeshComponent->RemoveInstance(i);
		}
	}

	// Calculate scale (default Sphere has radius 50cm)
	const float Scale = DebugMeshRadius / 50.0f;
	const FVector ScaleVec(Scale, Scale, Scale);

	// Update transforms
	for (int32 i = 0; i < ParticleCount; ++i)
	{
		FTransform InstanceTransform;
		InstanceTransform.SetLocation(Particles[i].Position);
		InstanceTransform.SetScale3D(ScaleVec);

		DebugMeshComponent->UpdateInstanceTransform(i, InstanceTransform, true, false, false);
	}

	DebugMeshComponent->MarkRenderStateDirty();
}

TArray<FKawaiiRenderParticle> UKawaiiFluidSimulationComponent::ConvertToRenderParticles() const
{
	TArray<FKawaiiRenderParticle> RenderParticles;
	RenderParticles.Reserve(Particles.Num());

	float ParticleRadius = Preset ? Preset->ParticleRadius : 5.0f;

	for (const FFluidParticle& SimParticle : Particles)
	{
		FKawaiiRenderParticle RenderParticle;
		RenderParticle.Position = FVector3f(SimParticle.Position);
		RenderParticle.Velocity = FVector3f(SimParticle.Velocity);
		RenderParticle.Radius = ParticleRadius;
		RenderParticle.Padding = 0.0f;

		RenderParticles.Add(RenderParticle);
	}

	return RenderParticles;
}
