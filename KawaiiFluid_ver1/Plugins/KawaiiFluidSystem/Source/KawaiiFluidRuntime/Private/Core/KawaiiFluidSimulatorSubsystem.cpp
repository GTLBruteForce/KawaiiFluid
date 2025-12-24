// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/SpatialHash.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Components/KawaiiFluidSimulationComponent.h"
#include "Components/FluidInteractionComponent.h"
#include "Collision/FluidCollider.h"

// Profiling
DECLARE_STATS_GROUP(TEXT("KawaiiFluidSubsystem"), STATGROUP_KawaiiFluidSubsystem, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Subsystem Tick"), STAT_SubsystemTick, STATGROUP_KawaiiFluidSubsystem);
DECLARE_CYCLE_STAT(TEXT("Simulate Independent"), STAT_SimulateIndependent, STATGROUP_KawaiiFluidSubsystem);
DECLARE_CYCLE_STAT(TEXT("Simulate Batched"), STAT_SimulateBatched, STATGROUP_KawaiiFluidSubsystem);
DECLARE_CYCLE_STAT(TEXT("Merge Particles"), STAT_MergeParticles, STATGROUP_KawaiiFluidSubsystem);
DECLARE_CYCLE_STAT(TEXT("Split Particles"), STAT_SplitParticles, STATGROUP_KawaiiFluidSubsystem);

UKawaiiFluidSimulatorSubsystem::UKawaiiFluidSimulatorSubsystem()
{
}

UKawaiiFluidSimulatorSubsystem::~UKawaiiFluidSimulatorSubsystem()
{
}

void UKawaiiFluidSimulatorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Create shared spatial hash with default cell size
	SharedSpatialHash = MakeShared<FSpatialHash>(20.0f);

	// Create default context
	DefaultContext = NewObject<UKawaiiFluidSimulationContext>(this);

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidSimulatorSubsystem initialized"));
}

void UKawaiiFluidSimulatorSubsystem::Deinitialize()
{
	AllComponents.Empty();
	GlobalColliders.Empty();
	GlobalInteractionComponents.Empty();
	ContextCache.Empty();
	DefaultContext = nullptr;
	SharedSpatialHash.Reset();

	Super::Deinitialize();

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidSimulatorSubsystem deinitialized"));
}

TStatId UKawaiiFluidSimulatorSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UKawaiiFluidSimulatorSubsystem, STATGROUP_Tickables);
}

void UKawaiiFluidSimulatorSubsystem::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_SubsystemTick);
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidSubsystem_Tick);

	// Reset event counter at frame start
	EventCountThisFrame.store(0, std::memory_order_relaxed);

	if (AllComponents.Num() == 0)
	{
		return;
	}

	// Simulate independent components
	{
		SCOPE_CYCLE_COUNTER(STAT_SimulateIndependent);
		SimulateIndependentComponents(DeltaTime);
	}

	// Simulate batched components
	{
		SCOPE_CYCLE_COUNTER(STAT_SimulateBatched);
		SimulateBatchedComponents(DeltaTime);
	}
}

//========================================
// Component Registration
//========================================

void UKawaiiFluidSimulatorSubsystem::RegisterComponent(UKawaiiFluidSimulationComponent* Component)
{
	if (Component && !AllComponents.Contains(Component))
	{
		AllComponents.Add(Component);
		UE_LOG(LogTemp, Verbose, TEXT("Component registered: %s"), *Component->GetName());
	}
}

void UKawaiiFluidSimulatorSubsystem::UnregisterComponent(UKawaiiFluidSimulationComponent* Component)
{
	AllComponents.Remove(Component);
	UE_LOG(LogTemp, Verbose, TEXT("Component unregistered: %s"), Component ? *Component->GetName() : TEXT("nullptr"));
}

//========================================
// Global Colliders
//========================================

void UKawaiiFluidSimulatorSubsystem::RegisterGlobalCollider(UFluidCollider* Collider)
{
	if (Collider && !GlobalColliders.Contains(Collider))
	{
		GlobalColliders.Add(Collider);
	}
}

void UKawaiiFluidSimulatorSubsystem::UnregisterGlobalCollider(UFluidCollider* Collider)
{
	GlobalColliders.Remove(Collider);
}

//========================================
// Global Interaction Components
//========================================

void UKawaiiFluidSimulatorSubsystem::RegisterGlobalInteractionComponent(UFluidInteractionComponent* Component)
{
	if (Component && !GlobalInteractionComponents.Contains(Component))
	{
		GlobalInteractionComponents.Add(Component);
	}
}

void UKawaiiFluidSimulatorSubsystem::UnregisterGlobalInteractionComponent(UFluidInteractionComponent* Component)
{
	GlobalInteractionComponents.Remove(Component);
}

//========================================
// Query API
//========================================

TArray<FFluidParticle> UKawaiiFluidSimulatorSubsystem::GetAllParticlesInRadius(FVector Location, float Radius) const
{
	TArray<FFluidParticle> Result;
	const float RadiusSq = Radius * Radius;

	for (const UKawaiiFluidSimulationComponent* Component : AllComponents)
	{
		if (!Component)
		{
			continue;
		}

		for (const FFluidParticle& Particle : Component->GetParticles())
		{
			if (FVector::DistSquared(Particle.Position, Location) <= RadiusSq)
			{
				Result.Add(Particle);
			}
		}
	}

	return Result;
}

int32 UKawaiiFluidSimulatorSubsystem::GetTotalParticleCount() const
{
	int32 Total = 0;
	for (const UKawaiiFluidSimulationComponent* Component : AllComponents)
	{
		if (Component)
		{
			Total += Component->GetParticleCount();
		}
	}
	return Total;
}

//========================================
// Context Management
//========================================

UKawaiiFluidSimulationContext* UKawaiiFluidSimulatorSubsystem::GetOrCreateContext(const UKawaiiFluidPresetDataAsset* Preset)
{
	if (!Preset || !Preset->ContextClass)
	{
		return DefaultContext;
	}

	// Check cache
	if (UKawaiiFluidSimulationContext** Found = ContextCache.Find(Preset->ContextClass))
	{
		return *Found;
	}

	// Create new context
	UKawaiiFluidSimulationContext* NewContext = NewObject<UKawaiiFluidSimulationContext>(this, Preset->ContextClass);
	NewContext->InitializeSolvers(Preset);
	ContextCache.Add(Preset->ContextClass, NewContext);

	return NewContext;
}

//========================================
// Simulation Methods
//========================================

void UKawaiiFluidSimulatorSubsystem::SimulateIndependentComponents(float DeltaTime)
{
	// Collect independent components
	TArray<UKawaiiFluidSimulationComponent*> IndependentComponents;
	for (UKawaiiFluidSimulationComponent* Component : AllComponents)
	{
		if (Component && Component->bSimulationEnabled && Component->bIndependentSimulation)
		{
			IndependentComponents.Add(Component);
		}
	}

	if (IndependentComponents.Num() == 0)
	{
		return;
	}

	// Simulate each independently (can use ParallelFor for true independence)
	for (UKawaiiFluidSimulationComponent* Component : IndependentComponents)
	{
		UKawaiiFluidPresetDataAsset* Preset = Component->Preset;
		if (!Preset)
		{
			continue;
		}

		UKawaiiFluidSimulationContext* Context = GetOrCreateContext(Preset);
		if (!Context)
		{
			continue;
		}

		FSpatialHash* SpatialHash = Component->GetSpatialHash();
		if (!SpatialHash)
		{
			continue;
		}

		TArray<FFluidParticle>& Particles = Component->GetParticlesMutable();
		FKawaiiFluidSimulationParams Params = Component->BuildSimulationParams();

		// Add global colliders and interaction components
		Params.Colliders.Append(GlobalColliders);
		Params.InteractionComponents.Append(GlobalInteractionComponents);

		float AccumulatedTime = Component->GetAccumulatedTime();
		Context->Simulate(Particles, Preset, Params, *SpatialHash, DeltaTime, AccumulatedTime);
		Component->SetAccumulatedTime(AccumulatedTime);
		Component->ResetExternalForce();
	}
}

void UKawaiiFluidSimulatorSubsystem::SimulateBatchedComponents(float DeltaTime)
{
	// Group components by preset (only non-independent)
	TMap<UKawaiiFluidPresetDataAsset*, TArray<UKawaiiFluidSimulationComponent*>> PresetGroups = GroupComponentsByPreset();

	// Process each preset group
	for (auto& Pair : PresetGroups)
	{
		UKawaiiFluidPresetDataAsset* Preset = Pair.Key;
		TArray<UKawaiiFluidSimulationComponent*>& Components = Pair.Value;

		if (!Preset || Components.Num() == 0)
		{
			continue;
		}

		// Get context for this preset
		UKawaiiFluidSimulationContext* Context = GetOrCreateContext(Preset);
		if (!Context)
		{
			continue;
		}

		// Update shared spatial hash cell size
		SharedSpatialHash->SetCellSize(Preset->SmoothingRadius);

		// Merge particles from all components
		{
			SCOPE_CYCLE_COUNTER(STAT_MergeParticles);
			MergeParticles(Components);
		}

		if (MergedParticleBuffer.Num() == 0)
		{
			continue;
		}

		// Build merged simulation params
		FKawaiiFluidSimulationParams Params = BuildMergedSimulationParams(Components);
		Params.Colliders.Append(GlobalColliders);

		// Simulate merged buffer
		float AccumulatedTime = 0.0f; // Use first component's time
		if (Components.Num() > 0 && Components[0])
		{
			AccumulatedTime = Components[0]->GetAccumulatedTime();
		}

		Context->Simulate(MergedParticleBuffer, Preset, Params, *SharedSpatialHash, DeltaTime, AccumulatedTime);

		// Update accumulated time for all components
		for (UKawaiiFluidSimulationComponent* Component : Components)
		{
			if (Component)
			{
				Component->SetAccumulatedTime(AccumulatedTime);
				Component->ResetExternalForce();
			}
		}

		// Split particles back to components
		{
			SCOPE_CYCLE_COUNTER(STAT_SplitParticles);
			SplitParticles(Components);
		}
	}
}

TMap<UKawaiiFluidPresetDataAsset*, TArray<UKawaiiFluidSimulationComponent*>>
UKawaiiFluidSimulatorSubsystem::GroupComponentsByPreset() const
{
	TMap<UKawaiiFluidPresetDataAsset*, TArray<UKawaiiFluidSimulationComponent*>> Result;

	for (UKawaiiFluidSimulationComponent* Component : AllComponents)
	{
		// Only batch non-independent, enabled components
		if (Component &&
		    Component->bSimulationEnabled &&
		    !Component->bIndependentSimulation &&
		    Component->Preset)
		{
			Result.FindOrAdd(Component->Preset).Add(Component);
		}
	}

	return Result;
}

void UKawaiiFluidSimulatorSubsystem::MergeParticles(const TArray<UKawaiiFluidSimulationComponent*>& Components)
{
	// Calculate total particle count
	int32 TotalParticles = 0;
	for (const UKawaiiFluidSimulationComponent* Component : Components)
	{
		if (Component)
		{
			TotalParticles += Component->GetParticleCount();
		}
	}

	// Resize merged buffer
	MergedParticleBuffer.Reset(TotalParticles);
	BatchInfos.Reset(Components.Num());

	// Copy particles to merged buffer
	int32 CurrentOffset = 0;
	for (UKawaiiFluidSimulationComponent* Component : Components)
	{
		if (!Component)
		{
			continue;
		}

		const TArray<FFluidParticle>& Particles = Component->GetParticles();
		const int32 Count = Particles.Num();

		// Store batch info
		BatchInfos.Add(FKawaiiFluidBatchInfo(Component, CurrentOffset, Count));

		// Copy particles
		for (const FFluidParticle& Particle : Particles)
		{
			MergedParticleBuffer.Add(Particle);
		}

		CurrentOffset += Count;
	}
}

void UKawaiiFluidSimulatorSubsystem::SplitParticles(const TArray<UKawaiiFluidSimulationComponent*>& Components)
{
	// Copy particles back from merged buffer
	for (const FKawaiiFluidBatchInfo& Info : BatchInfos)
	{
		UKawaiiFluidSimulationComponent* Component = Info.Component;
		if (!Component)
		{
			continue;
		}

		TArray<FFluidParticle>& Particles = Component->GetParticlesMutable();

		// Verify size match
		if (Particles.Num() != Info.ParticleCount)
		{
			continue;
		}

		// Copy back
		for (int32 i = 0; i < Info.ParticleCount; ++i)
		{
			Particles[i] = MergedParticleBuffer[Info.StartIndex + i];
		}
	}
}

FKawaiiFluidSimulationParams UKawaiiFluidSimulatorSubsystem::BuildMergedSimulationParams(
	const TArray<UKawaiiFluidSimulationComponent*>& Components)
{
	FKawaiiFluidSimulationParams Params;
	Params.World = GetWorld();
	Params.EventCountPtr = &EventCountThisFrame;

	// Merge external forces (average or sum)
	FVector TotalForce = FVector::ZeroVector;
	bool bAnyUseWorldCollision = false;
	ECollisionChannel MergedChannel = ECC_GameTraceChannel1;

	for (const UKawaiiFluidSimulationComponent* Component : Components)
	{
		if (!Component)
		{
			continue;
		}

		TotalForce += Component->GetAccumulatedExternalForce();

		// Merge colliders
		Params.Colliders.Append(Component->GetColliders());

		// Merge interaction components
		Params.InteractionComponents.Append(Component->GetInteractionComponents());

		// Use world collision if any component wants it
		if (Component->bUseWorldCollision)
		{
			bAnyUseWorldCollision = true;
			if (Component->Preset)
			{
				MergedChannel = Component->Preset->CollisionChannel;
			}
		}
	}

	// Append global colliders and interaction components
	Params.Colliders.Append(GlobalColliders);
	Params.InteractionComponents.Append(GlobalInteractionComponents);

	Params.ExternalForce = TotalForce;
	Params.bUseWorldCollision = bAnyUseWorldCollision;
	Params.CollisionChannel = MergedChannel;

	// Use first component's particle radius
	if (Components.Num() > 0 && Components[0] && Components[0]->Preset)
	{
		Params.ParticleRadius = Components[0]->Preset->ParticleRadius;
	}

	// Set current game time for potential cooldown checks
	if (UWorld* World = GetWorld())
	{
		Params.CurrentGameTime = World->GetTimeSeconds();
	}

	return Params;
}
