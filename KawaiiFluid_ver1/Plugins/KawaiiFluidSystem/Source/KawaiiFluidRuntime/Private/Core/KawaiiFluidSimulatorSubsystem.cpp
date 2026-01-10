// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/SpatialHash.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Components/KawaiiFluidComponent.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"
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

		// Early GPU setup: Initialize GPU state at registration time
		// so spawn calls before first Tick use the correct path
		UKawaiiFluidPresetDataAsset* Preset = Module->GetEffectivePreset();

		// Check GPU setting from owner component (not preset)
		bool bWantsGPUSimulation = false;
		if (UKawaiiFluidComponent* OwnerComp = Cast<UKawaiiFluidComponent>(Module->GetOuter()))
		{
			bWantsGPUSimulation = OwnerComp->bUseGPUSimulation;
		}

		// Always setup Context reference (needed for rendering)
		// Context is keyed by (Preset + SimulationMode) to allow GPU/CPU mixing
		if (Preset)
		{
			UKawaiiFluidSimulationContext* Context = GetOrCreateContext(Preset, bWantsGPUSimulation);
			if (Context)
			{
				// Set context reference for rendering access
				Module->SetSimulationContext(Context);

				// Cache preset in context for rendering parameter access
				if (!Context->GetCachedPreset())
				{
					Context->SetCachedPreset(Preset);
				}

				// GPU simulation setup
				if (bWantsGPUSimulation)
				{
					if (!Context->IsGPUSimulatorReady())
					{
						Context->InitializeGPUSimulator(Preset->MaxParticles);
					}

					if (Context->IsGPUSimulatorReady())
					{
						Module->SetGPUSimulator(Context->GetGPUSimulator());
						Module->SetGPUSimulationActive(true);
						UE_LOG(LogTemp, Log, TEXT("SimulationModule: GPU simulation initialized at registration"));
					}
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
							UE_LOG(LogTemp, Log, TEXT("SimulationModule: Connected MetaballRenderer to Context"));
						}
					}
				}
			}
		}

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

//========================================
// Context Management
//========================================

UKawaiiFluidSimulationContext* UKawaiiFluidSimulatorSubsystem::GetOrCreateContext(
	UKawaiiFluidPresetDataAsset* Preset, bool bUseGPUSimulation)
{
	if (!Preset)
	{
		return DefaultContext;
	}

	// Check cache - keyed by (Preset + SimulationMode)
	// This allows same Preset to have different Contexts for GPU vs CPU
	FContextCacheKey CacheKey(Preset, bUseGPUSimulation);
	if (TObjectPtr<UKawaiiFluidSimulationContext>* Found = ContextCache.Find(CacheKey))
	{
		return *Found;
	}

	// Create new context - use ContextClass if specified, otherwise default class
	TSubclassOf<UKawaiiFluidSimulationContext> ContextClass = Preset->ContextClass;
	if (!ContextClass)
	{
		ContextClass = UKawaiiFluidSimulationContext::StaticClass();
	}

	UKawaiiFluidSimulationContext* NewContext = NewObject<UKawaiiFluidSimulationContext>(this, ContextClass);
	NewContext->InitializeSolvers(Preset);
	ContextCache.Add(CacheKey, NewContext);

	UE_LOG(LogTemp, Log, TEXT("Created Context for Preset '%s' (GPU=%d)"),
		*Preset->GetName(), bUseGPUSimulation ? 1 : 0);

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

		// Get GPU setting from owner component
		bool bWantsGPUSimulation = false;
		if (UKawaiiFluidComponent* OwnerComp = Cast<UKawaiiFluidComponent>(Module->GetOuter()))
		{
			bWantsGPUSimulation = OwnerComp->bUseGPUSimulation;
		}

		// Get or create context (keyed by Preset + SimulationMode)
		UKawaiiFluidSimulationContext* Context = GetOrCreateContext(EffectivePreset, bWantsGPUSimulation);
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

		// Phase 1: Setup GPU state BEFORE Simulate so spawn calls go to GPU
		// This ensures SpawnParticle() during this frame will use GPU path
		if (Params.bUseGPUSimulation)
		{
			// Initialize GPU simulator if needed (before any spawning)
			if (!Context->IsGPUSimulatorReady())
			{
				Context->InitializeGPUSimulator(EffectivePreset->MaxParticles);
			}

			if (Context->IsGPUSimulatorReady())
			{
				Module->SetGPUSimulator(Context->GetGPUSimulator());
				Module->SetGPUSimulationActive(true);
			}
		}
		else
		{
			Module->SetGPUSimulationActive(false);
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
	// Group modules by (Preset + SimulationMode) - allows GPU/CPU mixing
	TMap<FContextCacheKey, TArray<UKawaiiFluidSimulationModule*>> ContextGroups = GroupModulesByContext();

	// Process each context group
	for (auto& Pair : ContextGroups)
	{
		const FContextCacheKey& CacheKey = Pair.Key;
		TArray<UKawaiiFluidSimulationModule*>& Modules = Pair.Value;

		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		if (!Preset || Modules.Num() == 0)
		{
			continue;
		}

		// Get context for this (Preset + SimulationMode) combination
		UKawaiiFluidSimulationContext* Context = GetOrCreateContext(Preset, CacheKey.bUseGPUSimulation);
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
		Params.bUseGPUSimulation = CacheKey.bUseGPUSimulation;

		// Phase 1: Setup GPU state BEFORE Simulate so spawn calls go to GPU
		if (Params.bUseGPUSimulation)
		{
			// Initialize GPU simulator if needed (before any spawning)
			if (!Context->IsGPUSimulatorReady())
			{
				Context->InitializeGPUSimulator(Preset->MaxParticles);
			}

			if (Context->IsGPUSimulatorReady())
			{
				FGPUFluidSimulator* BatchGPUSimulator = Context->GetGPUSimulator();
				for (UKawaiiFluidSimulationModule* Module : Modules)
				{
					if (Module)
					{
						Module->SetGPUSimulator(BatchGPUSimulator);
						Module->SetGPUSimulationActive(true);
					}
				}
			}
		}
		else
		{
			for (UKawaiiFluidSimulationModule* Module : Modules)
			{
				if (Module)
				{
					Module->SetGPUSimulationActive(false);
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

TMap<FContextCacheKey, TArray<UKawaiiFluidSimulationModule*>>
UKawaiiFluidSimulatorSubsystem::GroupModulesByContext() const
{
	TMap<FContextCacheKey, TArray<UKawaiiFluidSimulationModule*>> Result;

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
			// Get GPU setting from owner component
			bool bUseGPU = false;
			if (UKawaiiFluidComponent* OwnerComp = Cast<UKawaiiFluidComponent>(Module->GetOuter()))
			{
				bUseGPU = OwnerComp->bUseGPUSimulation;
			}

			// Group by (Preset + GPUMode)
			FContextCacheKey Key(Module->GetPreset(), bUseGPU);
			Result.FindOrAdd(Key).Add(Module);
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