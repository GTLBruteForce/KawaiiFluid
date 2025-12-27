// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/SpatialHash.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Components/KawaiiFluidSimulationComponent.h"
#include "Components/KawaiiFluidComponent.h"
#include "Modules/KawaiiFluidSimulationModule.h"
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
	AllModules.Empty();
	AllComponents.Empty();
	AllFluidComponents.Empty();
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

	//========================================
	// Legacy: UKawaiiFluidSimulationComponent
	//========================================
	if (AllComponents.Num() > 0)
	{
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
	// New: Module-based simulation
	//========================================
	if (AllModules.Num() > 0)
	{
		SimulateIndependentFluidComponents(DeltaTime);
		SimulateBatchedFluidComponents(DeltaTime);
	}
}

//========================================
// Module Registration (New)
//========================================

void UKawaiiFluidSimulatorSubsystem::RegisterModule(UKawaiiFluidSimulationModule* Module)
{
	if (Module && !AllModules.Contains(Module))
	{
		AllModules.Add(Module);
		UE_LOG(LogTemp, Verbose, TEXT("SimulationModule registered"));
	}
}

void UKawaiiFluidSimulatorSubsystem::UnregisterModule(UKawaiiFluidSimulationModule* Module)
{
	AllModules.Remove(Module);
	UE_LOG(LogTemp, Verbose, TEXT("SimulationModule unregistered"));
}

//========================================
// Component Registration (Deprecated)
//========================================

void UKawaiiFluidSimulatorSubsystem::RegisterComponent(UKawaiiFluidComponent* Component)
{
	if (Component && !AllFluidComponents.Contains(Component))
	{
		AllFluidComponents.Add(Component);
		UE_LOG(LogTemp, Verbose, TEXT("FluidComponent registered: %s"), *Component->GetName());
	}
}

void UKawaiiFluidSimulatorSubsystem::UnregisterComponent(UKawaiiFluidComponent* Component)
{
	AllFluidComponents.Remove(Component);
	UE_LOG(LogTemp, Verbose, TEXT("FluidComponent unregistered: %s"), Component ? *Component->GetName() : TEXT("nullptr"));
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

	// Legacy components
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

	// New modular components
	for (const UKawaiiFluidComponent* Component : AllFluidComponents)
	{
		if (!Component || !Component->SimulationModule)
		{
			continue;
		}

		for (const FFluidParticle& Particle : Component->SimulationModule->GetParticles())
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

	// Legacy components
	for (const UKawaiiFluidSimulationComponent* Component : AllComponents)
	{
		if (Component)
		{
			Total += Component->GetParticleCount();
		}
	}

	// New modular components
	for (const UKawaiiFluidComponent* Component : AllFluidComponents)
	{
		if (Component)
		{
			Total += Component->SimulationModule->GetParticleCount();
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
// Simulation Methods (Module-based)
//========================================

void UKawaiiFluidSimulatorSubsystem::SimulateIndependentFluidComponents(float DeltaTime)
{
	static bool bLoggedOnce = false;
	if (!bLoggedOnce && AllModules.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Subsystem SimulateIndependent: AllModules.Num()=%d"), AllModules.Num());
		bLoggedOnce = true;
	}

	for (UKawaiiFluidSimulationModule* Module : AllModules)
	{
		if (!Module)
		{
			continue;
		}

		if (!Module->IsSimulationEnabled() || Module->GetParticleCount() == 0)
		{
			continue;
		}

		// Skip non-independent modules (they'll be batched)
		if (!Module->IsIndependentSimulation())
		{
			continue;
		}

		// Get effective preset
		UKawaiiFluidPresetDataAsset* EffectivePreset = Module->GetEffectivePreset();
		if (!EffectivePreset)
		{
			continue;
		}

		// Get or create context
		UKawaiiFluidSimulationContext* Context = GetOrCreateContext(EffectivePreset);
		if (!Context)
		{
			continue;
		}

		// Get spatial hash (use module's own)
		FSpatialHash* SpatialHash = Module->GetSpatialHash();
		if (!SpatialHash)
		{
			continue;
		}

		// Build simulation params - Module에서 직접 빌드!
		FKawaiiFluidSimulationParams Params = Module->BuildSimulationParams();
		Params.Colliders.Append(GlobalColliders);
		Params.InteractionComponents.Append(GlobalInteractionComponents);

		// Simulate
		TArray<FFluidParticle>& Particles = Module->GetParticlesMutable();
		float AccumulatedTime = Module->GetAccumulatedTime();

		static bool bSimLoggedOnce = false;
		if (!bSimLoggedOnce)
		{
			UE_LOG(LogTemp, Warning, TEXT("Subsystem: Calling Context->Simulate for Module with %d particles"), Particles.Num());
			bSimLoggedOnce = true;
		}

		Context->Simulate(Particles, EffectivePreset, Params, *SpatialHash, DeltaTime, AccumulatedTime);

		Module->SetAccumulatedTime(AccumulatedTime);
		Module->ResetExternalForce();
	}
}

void UKawaiiFluidSimulatorSubsystem::SimulateBatchedFluidComponents(float DeltaTime)
{
	// Group modules by preset (only non-independent)
	TMap<UKawaiiFluidPresetDataAsset*, TArray<UKawaiiFluidSimulationModule*>> PresetGroups = GroupModulesByPreset();

	// Process each preset group
	for (auto& Pair : PresetGroups)
	{
		UKawaiiFluidPresetDataAsset* Preset = Pair.Key;
		TArray<UKawaiiFluidSimulationModule*>& Modules = Pair.Value;

		if (!Preset || Modules.Num() == 0)
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

		// Merge particles from all modules
		MergeModuleParticles(Modules);

		if (MergedFluidParticleBuffer.Num() == 0)
		{
			continue;
		}

		// Build merged simulation params - Module에서 직접!
		FKawaiiFluidSimulationParams Params = BuildMergedModuleSimulationParams(Modules);
		Params.Colliders.Append(GlobalColliders);
		Params.InteractionComponents.Append(GlobalInteractionComponents);

		// Simulate merged buffer
		float AccumulatedTime = 0.0f;
		if (Modules.Num() > 0 && Modules[0])
		{
			AccumulatedTime = Modules[0]->GetAccumulatedTime();
		}

		Context->Simulate(MergedFluidParticleBuffer, Preset, Params, *SharedSpatialHash, DeltaTime, AccumulatedTime);

		// Update accumulated time and reset external force for all modules
		for (UKawaiiFluidSimulationModule* Module : Modules)
		{
			if (Module)
			{
				Module->SetAccumulatedTime(AccumulatedTime);
				Module->ResetExternalForce();
			}
		}

		// Split particles back to modules
		SplitModuleParticles(Modules);
	}
}

TMap<UKawaiiFluidPresetDataAsset*, TArray<UKawaiiFluidSimulationModule*>>
UKawaiiFluidSimulatorSubsystem::GroupModulesByPreset() const
{
	TMap<UKawaiiFluidPresetDataAsset*, TArray<UKawaiiFluidSimulationModule*>> Result;

	for (UKawaiiFluidSimulationModule* Module : AllModules)
	{
		if (!Module)
		{
			continue;
		}

		// Only batch non-independent, enabled modules with particles
		if (Module->IsSimulationEnabled() &&
		    !Module->IsIndependentSimulation() &&
		    Module->GetParticleCount() > 0 &&
		    Module->GetPreset())
		{
			Result.FindOrAdd(Module->GetPreset()).Add(Module);
		}
	}

	return Result;
}

void UKawaiiFluidSimulatorSubsystem::MergeModuleParticles(const TArray<UKawaiiFluidSimulationModule*>& Modules)
{
	// Calculate total particle count
	int32 TotalParticles = 0;
	for (const UKawaiiFluidSimulationModule* Module : Modules)
	{
		if (Module)
		{
			TotalParticles += Module->GetParticleCount();
		}
	}

	// Resize merged buffer
	MergedFluidParticleBuffer.Reset(TotalParticles);
	ModuleBatchInfos.Reset(Modules.Num());

	// Copy particles to merged buffer
	int32 CurrentOffset = 0;
	for (UKawaiiFluidSimulationModule* Module : Modules)
	{
		if (!Module)
		{
			continue;
		}

		const TArray<FFluidParticle>& Particles = Module->GetParticles();
		const int32 Count = Particles.Num();

		// Store batch info
		ModuleBatchInfos.Add(FKawaiiFluidModuleBatchInfo(Module, CurrentOffset, Count));

		// Copy particles
		for (const FFluidParticle& Particle : Particles)
		{
			MergedFluidParticleBuffer.Add(Particle);
		}

		CurrentOffset += Count;
	}
}

void UKawaiiFluidSimulatorSubsystem::SplitModuleParticles(const TArray<UKawaiiFluidSimulationModule*>& Modules)
{
	// Copy particles back from merged buffer
	for (const FKawaiiFluidModuleBatchInfo& Info : ModuleBatchInfos)
	{
		UKawaiiFluidSimulationModule* Module = Info.Module;
		if (!Module)
		{
			continue;
		}

		TArray<FFluidParticle>& Particles = Module->GetParticlesMutable();

		// Verify size match
		if (Particles.Num() != Info.ParticleCount)
		{
			continue;
		}

		// Copy back
		for (int32 i = 0; i < Info.ParticleCount; ++i)
		{
			Particles[i] = MergedFluidParticleBuffer[Info.StartIndex + i];
		}
	}
}

FKawaiiFluidSimulationParams UKawaiiFluidSimulatorSubsystem::BuildMergedModuleSimulationParams(
	const TArray<UKawaiiFluidSimulationModule*>& Modules)
{
	FKawaiiFluidSimulationParams Params;
	Params.World = GetWorld();
	Params.EventCountPtr = &EventCountThisFrame;

	FVector TotalForce = FVector::ZeroVector;
	bool bAnyUseWorldCollision = false;
	ECollisionChannel MergedChannel = ECC_GameTraceChannel1;

	for (const UKawaiiFluidSimulationModule* Module : Modules)
	{
		if (!Module)
		{
			continue;
		}

		TotalForce += Module->GetAccumulatedExternalForce();

		// Merge colliders
		Params.Colliders.Append(Module->GetColliders());

		// Merge interaction components
		Params.InteractionComponents.Append(Module->GetInteractionComponents());

		// Use world collision if any module wants it
		if (Module->bUseWorldCollision)
		{
			bAnyUseWorldCollision = true;
			if (Module->GetPreset())
			{
				MergedChannel = Module->GetPreset()->CollisionChannel;
			}
		}
	}

	Params.ExternalForce = TotalForce;
	Params.bUseWorldCollision = bAnyUseWorldCollision;
	Params.CollisionChannel = MergedChannel;

	// Use first module's particle radius
	if (Modules.Num() > 0 && Modules[0])
	{
		UKawaiiFluidPresetDataAsset* Preset = Modules[0]->GetPreset();
		if (Preset)
		{
			Params.ParticleRadius = Preset->ParticleRadius;
		}
	}

	// Set current game time
	if (UWorld* World = GetWorld())
	{
		Params.CurrentGameTime = World->GetTimeSeconds();
	}

	return Params;
}

//##########################################################################
// DEPRECATED - UKawaiiFluidSimulationComponent (Legacy)
//##########################################################################
#pragma region DEPRECATED_LEGACY

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

void UKawaiiFluidSimulatorSubsystem::SimulateIndependentComponents(float DeltaTime)
{
	// Collect independent components (explicit flag OR has overrides)
	TArray<UKawaiiFluidSimulationComponent*> IndependentComponents;
	for (UKawaiiFluidSimulationComponent* Component : AllComponents)
	{
		if (Component && Component->bSimulationEnabled && Component->ShouldSimulateIndependently())
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
		// Get effective preset (RuntimePreset if overrides, otherwise original Preset)
		UKawaiiFluidPresetDataAsset* EffectivePreset = Component->GetEffectivePreset();
		if (!EffectivePreset)
		{
			continue;
		}

		UKawaiiFluidSimulationContext* Context = GetOrCreateContext(EffectivePreset);
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
		Context->Simulate(Particles, EffectivePreset, Params, *SpatialHash, DeltaTime, AccumulatedTime);
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
		// Only batch non-independent, enabled components (no overrides)
		if (Component &&
		    Component->bSimulationEnabled &&
		    !Component->ShouldSimulateIndependently() &&
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

#pragma endregion DEPRECATED_LEGACY
