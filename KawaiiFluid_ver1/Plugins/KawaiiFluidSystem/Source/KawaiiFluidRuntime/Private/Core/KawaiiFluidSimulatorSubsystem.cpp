// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/SpatialHash.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Components/KawaiiFluidComponent.h"
#include "Components/KawaiiFluidVolumeComponent.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Components/FluidInteractionComponent.h"
#include "Collision/FluidCollider.h"
#include "Engine/Level.h"
#include "Engine/World.h"

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

	if (UWorld* World = GetWorld())
	{
		OnActorSpawnedHandle = World->AddOnActorSpawnedHandler(
			FOnActorSpawned::FDelegate::CreateUObject(this, &UKawaiiFluidSimulatorSubsystem::HandleActorSpawned));
	}

	//OnActorDestroyedHandle = FWorldDelegates::OnActorDestroyed.AddUObject(this, &UKawaiiFluidSimulatorSubsystem::HandleActorDestroyed);
	OnLevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(
		this, &UKawaiiFluidSimulatorSubsystem::HandleLevelAdded);
	OnLevelRemovedHandle = FWorldDelegates::LevelRemovedFromWorld.AddUObject(
		this, &UKawaiiFluidSimulatorSubsystem::HandleLevelRemoved);

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidSimulatorSubsystem initialized"));
}

void UKawaiiFluidSimulatorSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (OnActorSpawnedHandle.IsValid())
		{
			World->RemoveOnActorSpawnedHandler(OnActorSpawnedHandle);
		}
	}

	if (OnLevelAddedHandle.IsValid())
	{
		FWorldDelegates::LevelAddedToWorld.Remove(OnLevelAddedHandle);
	}

	if (OnLevelRemovedHandle.IsValid())
	{
		FWorldDelegates::LevelRemovedFromWorld.Remove(OnLevelRemovedHandle);
	}

	AllModules.Empty();
	AllVolumes.Empty();
	AllVolumeComponents.Empty();
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
	// New: Module-based simulation
	//========================================
	if (AllModules.Num() > 0)
	{
		SimulateIndependentFluidComponents(DeltaTime);
		SimulateBatchedFluidComponents(DeltaTime);

		//========================================
		// Collision Feedback Processing (GPU + CPU)
		//========================================
		// Build OwnerID → InteractionComponent map (once, for O(1) lookup)
		TMap<int32, UFluidInteractionComponent*> OwnerIDToIC;
		OwnerIDToIC.Reserve(GlobalInteractionComponents.Num());
		for (UFluidInteractionComponent* IC : GlobalInteractionComponents)
		{
			if (IC && IC->GetOwner())
			{
				OwnerIDToIC.Add(IC->GetOwner()->GetUniqueID(), IC);
			}
		}

		// Call ProcessCollisionFeedback for each Module
		for (UKawaiiFluidSimulationModule* Module : AllModules)
		{
			if (Module && Module->bEnableCollisionEvents)
			{
				Module->ProcessCollisionFeedback(OwnerIDToIC, CPUCollisionFeedbackBuffer);
			}
		}

		// Clear CPU buffer
		CPUCollisionFeedbackBuffer.Reset();
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

		// Allocate SourceID for per-component GPU counter tracking (0 ~ MaxSourceCount-1)
		const int32 NewSourceID = AllocateSourceID();
		Module->SetSourceID(NewSourceID);

		// Early GPU setup: Initialize GPU state at registration time
		// so spawn calls before first Tick use the correct path
		UKawaiiFluidPresetDataAsset* Preset = Module->GetPreset();

		// Always setup Context reference (needed for rendering)
		// Context is keyed by (VolumeComponent + Preset)
		// Same VolumeComponent = same Z-Order space = particles can interact
		if (Preset)
		{
			UKawaiiFluidSimulationVolumeComponent* TargetVolume = Module->GetTargetVolumeComponent();
			UKawaiiFluidSimulationContext* Context = GetOrCreateContext(TargetVolume, Preset);
			if (Context)
			{
				// Set context reference for rendering access
				Module->SetSimulationContext(Context);

				// Cache preset in context for rendering parameter access
				if (!Context->GetCachedPreset())
				{
					Context->SetCachedPreset(Preset);
				}

			// GPU simulation setup (always enabled)
				if (!Context->IsGPUSimulatorReady() && TargetVolume)
				{
					Context->InitializeGPUSimulator(TargetVolume->MaxParticleCount);
				}

			if (Context->IsGPUSimulatorReady())
				{
					Module->SetGPUSimulator(Context->GetGPUSimulatorShared());
					Module->SetGPUSimulationActive(true);

					// Upload cached CPU particles to GPU after PIE/Load
					Module->UploadCPUParticlesToGPU();

					UE_LOG(LogTemp, Log, TEXT("SimulationModule: GPU simulation initialized at registration"));
				}

				// Initialize Context's RenderResource for batch rendering
				if (!Context->HasValidRenderResource())
				{
					Context->InitializeRenderResource();
				}

				// Connect MetaballRenderer to SimulationContext
				// Same Context → Same RenderResource → Single Draw Call
				if (UKawaiiFluidComponent* OwnerComp = Cast<UKawaiiFluidComponent>(Module->GetOuter()))
				{
					if (UKawaiiFluidRenderingModule* RenderingMod = OwnerComp->GetRenderingModule())
					{
						if (UKawaiiFluidMetaballRenderer* MR = RenderingMod->GetMetaballRenderer())
						{
							MR->SetSimulationContext(Context);
							UE_LOG(LogTemp, Log, TEXT("SimulationModule: Connected MetaballRenderer to Context (Component)"));
						}
					}
				}
				// Support for AKawaiiFluidVolume (new architecture)
				else if (AKawaiiFluidVolume* OwnerVolume = Cast<AKawaiiFluidVolume>(Module->GetOuter()))
				{
					if (UKawaiiFluidRenderingModule* RenderingMod = OwnerVolume->GetRenderingModule())
					{
						if (UKawaiiFluidMetaballRenderer* MR = RenderingMod->GetMetaballRenderer())
						{
							MR->SetSimulationContext(Context);
							UE_LOG(LogTemp, Log, TEXT("SimulationModule: Connected MetaballRenderer to Context (Volume)"));
						}
					}
				}
			}
		}

		UE_LOG(LogTemp, Log, TEXT("SimulationModule registered: SourceID=%d, GPUActive=%d"),
			NewSourceID, Module->IsGPUSimulationActive() ? 1 : 0);
	}
}

void UKawaiiFluidSimulatorSubsystem::UnregisterModule(UKawaiiFluidSimulationModule* Module)
{
	if (Module)
	{
		// Release SourceID back to pool
		const int32 SourceID = Module->GetSourceID();
		if (SourceID >= 0)
		{
			ReleaseSourceID(SourceID);
			Module->SetSourceID(EGPUParticleSource::InvalidSourceID);
		}
	}

	AllModules.Remove(Module);
	UE_LOG(LogTemp, Verbose, TEXT("SimulationModule unregistered"));
}

//========================================
// SourceID Allocation
//========================================

int32 UKawaiiFluidSimulatorSubsystem::AllocateSourceID()
{
	// Initialize bitfield on first use
	if (UsedSourceIDs.Num() == 0)
	{
		UsedSourceIDs.Init(false, EGPUParticleSource::MaxSourceCount);
	}

	// Search for first available ID starting from hint
	for (int32 i = 0; i < EGPUParticleSource::MaxSourceCount; ++i)
	{
		const int32 Index = (NextSourceIDHint + i) % EGPUParticleSource::MaxSourceCount;
		if (!UsedSourceIDs[Index])
		{
			UsedSourceIDs[Index] = true;
			NextSourceIDHint = (Index + 1) % EGPUParticleSource::MaxSourceCount;
			UE_LOG(LogTemp, Log, TEXT("Subsystem::AllocateSourceID = %d"), Index);
			return Index;
		}
	}

	// All slots used - return invalid
	UE_LOG(LogTemp, Warning, TEXT("Subsystem::AllocateSourceID FAILED - all %d slots in use!"), EGPUParticleSource::MaxSourceCount);
	return EGPUParticleSource::InvalidSourceID;
}

void UKawaiiFluidSimulatorSubsystem::ReleaseSourceID(int32 SourceID)
{
	if (SourceID >= 0 && SourceID < UsedSourceIDs.Num())
	{
		UsedSourceIDs[SourceID] = false;
		UE_LOG(LogTemp, Log, TEXT("Subsystem::ReleaseSourceID = %d"), SourceID);
	}
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
// Volume Component Registration
//========================================

//========================================
// Volume Actor Registration (New Architecture)
//========================================

void UKawaiiFluidSimulatorSubsystem::RegisterVolume(AKawaiiFluidVolume* Volume)
{
	if (Volume && !AllVolumes.Contains(Volume))
	{
		AllVolumes.Add(Volume);
		UE_LOG(LogTemp, Log, TEXT("KawaiiFluidVolume registered: %s"), *Volume->GetName());
	}
}

void UKawaiiFluidSimulatorSubsystem::UnregisterVolume(AKawaiiFluidVolume* Volume)
{
	AllVolumes.Remove(Volume);
	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidVolume unregistered: %s"), Volume ? *Volume->GetName() : TEXT("nullptr"));
}

//========================================
// Volume Component Registration (Legacy)
//========================================

void UKawaiiFluidSimulatorSubsystem::RegisterVolumeComponent(UKawaiiFluidVolumeComponent* VolumeComponent)
{
	if (VolumeComponent && !AllVolumeComponents.Contains(VolumeComponent))
	{
		AllVolumeComponents.Add(VolumeComponent);
		UE_LOG(LogTemp, Log, TEXT("VolumeComponent registered: %s"), *VolumeComponent->GetName());
	}
}

void UKawaiiFluidSimulatorSubsystem::UnregisterVolumeComponent(UKawaiiFluidVolumeComponent* VolumeComponent)
{
	AllVolumeComponents.Remove(VolumeComponent);
	UE_LOG(LogTemp, Log, TEXT("VolumeComponent unregistered: %s"), VolumeComponent ? *VolumeComponent->GetName() : TEXT("nullptr"));
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

	// New modular components
	for (const UKawaiiFluidComponent* Component : AllFluidComponents)
	{
		if (!Component || !Component->GetSimulationModule())
		{
			continue;
		}

		for (const FFluidParticle& Particle : Component->GetSimulationModule()->GetParticles())
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

	// New modular components
	for (const UKawaiiFluidComponent* Component : AllFluidComponents)
	{
		if (Component && Component->GetSimulationModule())
		{
			Total += Component->GetSimulationModule()->GetParticleCount();
		}
	}

	return Total;
}

UKawaiiFluidPresetDataAsset* UKawaiiFluidSimulatorSubsystem::GetPresetBySourceID(int32 SourceID) const
{
	UKawaiiFluidSimulationModule* Module = GetModuleBySourceID(SourceID);
	return Module ? Module->GetPreset() : nullptr;
}

UKawaiiFluidSimulationModule* UKawaiiFluidSimulatorSubsystem::GetModuleBySourceID(int32 SourceID) const
{
	if (SourceID < 0)
	{
		return nullptr;
	}

	// Method 1: SourceID slot matching (AllocateSourceID result)
	for (UKawaiiFluidSimulationModule* Module : AllModules)
	{
		if (Module && Module->GetSourceID() == SourceID)
		{
			return Module;
		}
	}

	// Method 2: Search in AllFluidComponents (legacy UniqueID matching)
	for (UKawaiiFluidComponent* Component : AllFluidComponents)
	{
		if (Component && Component->GetUniqueID() == SourceID)
		{
			return Component->GetSimulationModule();
		}
	}

	// Method 3: Search in AllModules' Outer(Component) (legacy UniqueID matching)
	for (UKawaiiFluidSimulationModule* Module : AllModules)
	{
		if (Module)
		{
			UObject* Outer = Module->GetOuter();
			if (Outer && Outer->GetUniqueID() == SourceID)
			{
				return Module;
			}
		}
	}

	return nullptr;
}

//========================================
// Context Management
//========================================

UKawaiiFluidSimulationContext* UKawaiiFluidSimulatorSubsystem::GetOrCreateContext(
	UKawaiiFluidVolumeComponent* VolumeComponent, UKawaiiFluidPresetDataAsset* Preset)
{
	if (!Preset)
	{
		return DefaultContext;
	}

	// Check cache - keyed by (VolumeComponent + Preset)
	// Same VolumeComponent = same Z-Order space = particles can interact
	// Same Preset = same physics parameters
	FContextCacheKey CacheKey(VolumeComponent, Preset);
	if (TObjectPtr<UKawaiiFluidSimulationContext>* Found = ContextCache.Find(CacheKey))
	{
		return *Found;
	}

	// Create new context - always use default UKawaiiFluidSimulationContext
	UKawaiiFluidSimulationContext* NewContext = NewObject<UKawaiiFluidSimulationContext>(this);
	NewContext->InitializeSolvers(Preset);

	// Store VolumeComponent reference in Context for bounds access
	NewContext->SetTargetVolumeComponent(VolumeComponent);

	ContextCache.Add(CacheKey, NewContext);

	UE_LOG(LogTemp, Log, TEXT("Created Context for VolumeComponent '%s' + Preset '%s'"),
		VolumeComponent ? *VolumeComponent->GetName() : TEXT("None"),
		*Preset->GetName());

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
		UKawaiiFluidPresetDataAsset* EffectivePreset = Module->GetPreset();
		if (!EffectivePreset)
		{
			continue;
		}

		// Get target volume component for Z-Order space bounds
		UKawaiiFluidSimulationVolumeComponent* TargetVolume = Module->GetTargetVolumeComponent();

		// Get or create context (keyed by VolumeComponent + Preset)
		UKawaiiFluidSimulationContext* Context = GetOrCreateContext(TargetVolume, EffectivePreset);
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

		// Build simulation params - directly from Module!
		FKawaiiFluidSimulationParams Params = Module->BuildSimulationParams();
		Params.Colliders.Append(GlobalColliders);
		Params.InteractionComponents.Append(GlobalInteractionComponents);

		// Set CPU collision feedback buffer pointers (used by Context)
		Params.CPUCollisionFeedbackBufferPtr = &CPUCollisionFeedbackBuffer;
		Params.CPUCollisionFeedbackLockPtr = &CPUCollisionFeedbackLock;

		// GPU simulation setup (always enabled)
		if (!Context->IsGPUSimulatorReady() && TargetVolume)
		{
			Context->InitializeGPUSimulator(TargetVolume->MaxParticleCount);
		}

		if (Context->IsGPUSimulatorReady())
		{
			Module->SetGPUSimulator(Context->GetGPUSimulatorShared());
			Module->SetGPUSimulationActive(true);
		}

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
	// Group modules by Preset
	TMap<FContextCacheKey, TArray<TObjectPtr<UKawaiiFluidSimulationModule>>> ContextGroups = GroupModulesByContext();

	// Process each context group
	for (auto& Pair : ContextGroups)
	{
		const FContextCacheKey& CacheKey = Pair.Key;
		TArray<TObjectPtr<UKawaiiFluidSimulationModule>>& Modules = Pair.Value;

		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		if (!Preset || Modules.Num() == 0)
		{
			continue;
		}

		// Get context for this (VolumeComponent + Preset) combination
		UKawaiiFluidSimulationContext* Context = GetOrCreateContext(CacheKey.VolumeComponent, Preset);
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

		// Build merged simulation params - directly from Module!
		FKawaiiFluidSimulationParams Params = BuildMergedModuleSimulationParams(Modules);
		Params.Colliders.Append(GlobalColliders);
		Params.InteractionComponents.Append(GlobalInteractionComponents);

		// Set CPU collision feedback buffer pointers (used by Context)
		Params.CPUCollisionFeedbackBufferPtr = &CPUCollisionFeedbackBuffer;
		Params.CPUCollisionFeedbackLockPtr = &CPUCollisionFeedbackLock;

		// GPU simulation setup (always enabled)
		if (!Context->IsGPUSimulatorReady() && CacheKey.VolumeComponent)
		{
			Context->InitializeGPUSimulator(CacheKey.VolumeComponent->MaxParticleCount);
		}

		if (Context->IsGPUSimulatorReady())
		{
			TSharedPtr<FGPUFluidSimulator> BatchGPUSimulator = Context->GetGPUSimulatorShared();
			for (UKawaiiFluidSimulationModule* Module : Modules)
			{
				if (Module)
				{
					Module->SetGPUSimulator(BatchGPUSimulator);
					Module->SetGPUSimulationActive(true);
					// Note: Upload after PIE/Load is handled in RegisterModule
				}
			}
		}

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

TMap<FContextCacheKey, TArray<TObjectPtr<UKawaiiFluidSimulationModule>>>
UKawaiiFluidSimulatorSubsystem::GroupModulesByContext() const
{
	TMap<FContextCacheKey, TArray<TObjectPtr<UKawaiiFluidSimulationModule>>> Result;

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
			// Get target volume component for Z-Order space bounds
			UKawaiiFluidSimulationVolumeComponent* TargetVolume = Module->GetTargetVolumeComponent();

			// Group by (VolumeComponent + Preset)
			// Same VolumeComponent = same Z-Order space = particles can interact
			FContextCacheKey Key(TargetVolume, Module->GetPreset());
			Result.FindOrAdd(Key).Add(Module);
		}
	}

	return Result;
}

void UKawaiiFluidSimulatorSubsystem::MergeModuleParticles(const TArray<TObjectPtr<UKawaiiFluidSimulationModule>>& Modules)
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

void UKawaiiFluidSimulatorSubsystem::SplitModuleParticles(const TArray<TObjectPtr<UKawaiiFluidSimulationModule>>& Modules)
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
	const TArray<TObjectPtr<UKawaiiFluidSimulationModule>>& Modules)
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
	Params.InteractionComponents.Append(GlobalInteractionComponents);
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

void UKawaiiFluidSimulatorSubsystem::HandleActorSpawned(AActor* Actor)
{
	if (!Actor || Actor->GetWorld() != GetWorld())
	{
		return;
	}

	MarkAllContextsWorldCollisionDirty();
}

void UKawaiiFluidSimulatorSubsystem::HandleActorDestroyed(AActor* Actor)
{
	if (!Actor || Actor->GetWorld() != GetWorld())
	{
		return;
	}

	MarkAllContextsWorldCollisionDirty();
}

void UKawaiiFluidSimulatorSubsystem::HandleLevelAdded(ULevel* InLevel, UWorld* InWorld)
{
	if (!InWorld || InWorld != GetWorld())
	{
		return;
	}

	MarkAllContextsWorldCollisionDirty();
}

void UKawaiiFluidSimulatorSubsystem::HandleLevelRemoved(ULevel* InLevel, UWorld* InWorld)
{
	if (!InWorld || InWorld != GetWorld())
	{
		return;
	}

	MarkAllContextsWorldCollisionDirty();
}

void UKawaiiFluidSimulatorSubsystem::MarkAllContextsWorldCollisionDirty()
{
	for (TPair<FContextCacheKey, TObjectPtr<UKawaiiFluidSimulationContext>>& Pair : ContextCache)
	{
		if (Pair.Value)
		{
			Pair.Value->MarkGPUWorldCollisionCacheDirty();
		}
	}

	if (DefaultContext)
	{
		DefaultContext->MarkGPUWorldCollisionCacheDirty();
	}
}
