// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Actors/KawaiiFluidVolume.h"
#include "Actors/KawaiiFluidEmitter.h"
#include "Components/KawaiiFluidVolumeComponent.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "GPU/GPUFluidSimulator.h"
#include "Core/KawaiiFluidSimulationStats.h"
#include "DrawDebugHelpers.h"
#include "NiagaraFunctionLibrary.h"
#include "Engine/World.h"
#include "Async/ParallelFor.h"
#include "PhysicsEngine/BodySetup.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/OverlapResult.h"

AKawaiiFluidVolume::AKawaiiFluidVolume()
{
	// Create the volume component as root
	VolumeComponent = CreateDefaultSubobject<UKawaiiFluidVolumeComponent>(TEXT("VolumeComponent"));
	RootComponent = VolumeComponent;

	// Create simulation module (like UKawaiiFluidComponent pattern)
	// This ensures SimulationModule exists in editor mode for Brush functionality
	SimulationModule = CreateDefaultSubobject<UKawaiiFluidSimulationModule>(TEXT("SimulationModule"));

	// Create the rendering module
	RenderingModule = CreateDefaultSubobject<UKawaiiFluidRenderingModule>(TEXT("RenderingModule"));

	// Enable ticking for simulation
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AKawaiiFluidVolume::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Initialize simulation module (called in editor when actor is placed/loaded)
	// This enables Brush functionality in editor mode
	InitializeSimulation();

	// Register with Subsystem for GPU setup (needed for editor brush functionality)
	// This sets up SimulationContext and GPU simulator
	RegisterSimulationWithSubsystem();

#if WITH_EDITOR
	// Initialize rendering in editor mode for brush visualization
	// This enables ISM rendering to show particles in editor
	InitializeEditorRendering();
#endif
}

void AKawaiiFluidVolume::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Ensure simulation is initialized (for newly spawned actors)
	InitializeSimulation();
	RegisterSimulationWithSubsystem();
}

void AKawaiiFluidVolume::BeginPlay()
{
	Super::BeginPlay();

	// Register simulation module with Subsystem for GPU setup (runtime only)
	RegisterSimulationWithSubsystem();

	// Initialize rendering
	InitializeRendering();

	// Register to subsystem
	RegisterToSubsystem();
}

void AKawaiiFluidVolume::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Unregister from subsystem
	UnregisterFromSubsystem();

	// Cleanup rendering
	CleanupRendering();

	// Cleanup simulation
	CleanupSimulation();

	Super::EndPlay(EndPlayReason);
}

void AKawaiiFluidVolume::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	if (!World || !VolumeComponent)
	{
		return;
	}

	const bool bIsGameWorld = World->IsGameWorld();

	// Process pending spawn requests from emitters
	ProcessPendingSpawnRequests();

	// Set readback request for ISM/DebugDraw modes (required for GPU->CPU data transfer)
	// Without this, ISM renderer won't get updated particle data
	{
		const bool bNeedReadback = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::DebugDraw) ||
		                           (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM)
#if WITH_EDITORONLY_DATA
		                           || VolumeComponent->bBrushModeActive
#endif
		                           ;
		GetFluidStatsCollector().SetReadbackRequested(bNeedReadback);
	}

#if WITH_EDITOR
	// Editor-specific: Run GPU simulation for brush mode (like UKawaiiFluidComponent)
	if (!bIsGameWorld && SimulationModule && SimulationModule->GetSpatialHash())
	{
		if (UKawaiiFluidSimulationContext* Context = SimulationModule->GetSimulationContext())
		{
			// Initialize GPU simulator if not ready
			if (!Context->IsGPUSimulatorReady())
			{
				Context->InitializeGPUSimulator(VolumeComponent->MaxParticleCount);
			}

			// Get preset for simulation parameters
			UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset();

			// Set GPU simulator reference (like UKawaiiFluidComponent)
			if (Context->IsGPUSimulatorReady())
			{
				SimulationModule->SetGPUSimulator(Context->GetGPUSimulatorShared());
				SimulationModule->SetGPUSimulationActive(true);
			}

			// Process simulation OR pending ops (OUTSIDE GPU ready check - like UKawaiiFluidComponent)
			// This ensures pending despawn requests are processed even when GPU state changes
			if (VolumeComponent->bBrushModeActive)
			{
				// Brush mode: Run full simulation
				FKawaiiFluidSimulationParams Params = SimulationModule->BuildSimulationParams();
				Params.ExternalForce += SimulationModule->GetAccumulatedExternalForce();

				if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
				{
					Params.Colliders.Append(Subsystem->GetGlobalColliders());
					Params.InteractionComponents.Append(Subsystem->GetGlobalInteractionComponents());
				}

				Params.bEnableStaticBoundaryParticles = false;
				if (Preset)
				{
					Params.CollisionChannel = Preset->CollisionChannel;
				}

				float AccumulatedTime = SimulationModule->GetAccumulatedTime();
				Context->Simulate(
					SimulationModule->GetParticlesMutable(),
					Preset,
					Params,
					*SimulationModule->GetSpatialHash(),
					DeltaSeconds,
					AccumulatedTime
				);
				SimulationModule->SetAccumulatedTime(AccumulatedTime);
				SimulationModule->ResetExternalForce();
			}
			else
			{
				// Not brush mode: Process pending spawn/despawn only (no physics simulation)
				FGPUFluidSimulator* GPUSim = Context->GetGPUSimulator();
				if (GPUSim && GPUSim->IsReady())
				{
					GPUSim->BeginFrame();
					GPUSim->EndFrame();
				}
			}
		}
	}
#endif

	// Renderer mutual exclusion management (like KawaiiFluidComponent)
	// Check IsInitialized() for game world, but in editor mode we handle it separately
	const bool bRenderingReady = RenderingModule && RenderingModule->IsInitialized();
#if WITH_EDITOR
	// Editor rendering is ready if we initialized it (even without BeginPlay)
	const bool bEditorRenderingReady = !bIsGameWorld && RenderingModule && bEditorRenderingInitialized;
#else
	const bool bEditorRenderingReady = false;
#endif

	if (bRenderingReady || bEditorRenderingReady)
	{
		UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer();
		UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer();

		const EKawaiiFluidDebugDrawMode CurrentMode = VolumeComponent->DebugDrawMode;
		const bool bISMMode = (CurrentMode == EKawaiiFluidDebugDrawMode::ISM);
		const bool bDebugDrawMode = (CurrentMode == EKawaiiFluidDebugDrawMode::DebugDraw);

		// Sync ISM state based on DebugDrawMode (same logic for editor and runtime)
		if (ISMRenderer)
		{
			bool bModeChanged = (CachedDebugDrawMode != CurrentMode);
			bool bColorChanged = !CachedISMDebugColor.Equals(VolumeComponent->ISMDebugColor, 0.001f);

			if (bModeChanged)
			{
				ISMRenderer->bEnabled = bISMMode;
				CachedDebugDrawMode = CurrentMode;
			}

			if (bISMMode && (bModeChanged || bColorChanged))
			{
				ISMRenderer->SetFluidColor(VolumeComponent->ISMDebugColor);
				CachedISMDebugColor = VolumeComponent->ISMDebugColor;
			}

			// Update MaxRenderParticles if changed at runtime
			if (CachedISMMaxRenderParticles != VolumeComponent->ISMMaxRenderParticles)
			{
				ISMRenderer->SetMaxRenderParticles(VolumeComponent->ISMMaxRenderParticles);
				CachedISMMaxRenderParticles = VolumeComponent->ISMMaxRenderParticles;
			}
		}

		// Metaball is enabled when rendering is enabled AND not in ISM/DebugDraw mode
		// Same logic for editor and runtime - respects user's DebugDrawMode setting
		if (MetaballRenderer)
		{
			MetaballRenderer->SetEnabled(VolumeComponent->bEnableRendering && !bISMMode && !bDebugDrawMode);
		}

		// UpdateRenderers must be called when rendering is enabled OR ISM debug mode is active
		if (VolumeComponent->bEnableRendering || bISMMode)
		{
			RenderingModule->UpdateRenderers();
		}
	}

	// Debug visualization (read from VolumeComponent)
	// Only render if bEnableRendering is true
	if (VolumeComponent->bEnableRendering && VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::DebugDraw)
	{
		DrawDebugParticles();
	}
	if (VolumeComponent->bShowStaticBoundaryParticles)
	{
		DrawDebugStaticBoundaryParticles();
	}

	// Particle data readback and VFX/Shadow update
	if (SimulationModule)
	{
		// Use member buffer to avoid per-frame allocation
		ShadowPredictionBuffer.Reset();
		TArray<FVector>& Positions = ShadowPredictionBuffer;
		int32 NumParticles = 0;

		// Check if GPU simulation is active
		FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
		const bool bGPUActive = SimulationModule->IsGPUSimulationActive() && GPUSimulator != nullptr;

		// Skip if no particles
		const int32 ActualParticleCount = bGPUActive ? GPUSimulator->GetParticleCount() : SimulationModule->GetParticleCount();
		if (ActualParticleCount <= 0)
		{
			CachedShadowPositions.Empty();
			CachedShadowVelocities.Empty();
			CachedNeighborCounts.Empty();

			// Clear shadow instances if subsystem available
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				RendererSubsystem->UpdateShadowInstances(nullptr, 0, 0.0f);
			}
			return;
		}

		// =====================================================
		// Step 1: Get particle data (Position, Velocity, NeighborCount)
		// This data is used by both VFX and Shadow systems
		// =====================================================
		if (bGPUActive)
		{
			// GPU Mode: Enable async readback and get positions
			GPUSimulator->SetShadowReadbackEnabled(true);
			GPUSimulator->SetAnisotropyReadbackEnabled(true);

			TArray<FVector> NewVelocities;
			TArray<FVector4> NewAnisotropyAxis1, NewAnisotropyAxis2, NewAnisotropyAxis3;

			if (GPUSimulator->HasReadyShadowPositions())
			{
				// New readback data available - update cache (with anisotropy)
				GPUSimulator->GetShadowDataWithAnisotropy(
					Positions, NewVelocities,
					NewAnisotropyAxis1, NewAnisotropyAxis2, NewAnisotropyAxis3);
				NumParticles = Positions.Num();

				if (NumParticles > 0)
				{
					// Use Move for arrays that won't be used after this (avoids deep copy)
					CachedShadowPositions = Positions;  // Keep copy - needed for UpdateShadowInstances below
					CachedShadowVelocities = MoveTemp(NewVelocities);
					CachedAnisotropyAxis1 = MoveTemp(NewAnisotropyAxis1);
					CachedAnisotropyAxis2 = MoveTemp(NewAnisotropyAxis2);
					CachedAnisotropyAxis3 = MoveTemp(NewAnisotropyAxis3);
					LastShadowReadbackFrame = GFrameCounter;
					LastShadowReadbackTime = FPlatformTime::Seconds();

					// Also cache neighbor counts for isolation detection
					GPUSimulator->GetShadowNeighborCounts(CachedNeighborCounts);
				}
			}
			else if (CachedShadowPositions.Num() > 0 && CachedShadowVelocities.Num() == CachedShadowPositions.Num())
			{
				// No new data - predict positions using cached velocity
				const double CurrentTime = FPlatformTime::Seconds();
				const float PredictionDelta = static_cast<float>(CurrentTime - LastShadowReadbackTime);

				// Clamp prediction delta to avoid extreme extrapolation
				const float ClampedDelta = FMath::Clamp(PredictionDelta, 0.0f, 0.1f);

				NumParticles = CachedShadowPositions.Num();
				Positions.SetNumUninitialized(NumParticles);

				// Parallelize prediction loop
				ParallelFor(NumParticles, [&](int32 i)
				{
					// Predict: Position += Velocity * DeltaTime
					Positions[i] = CachedShadowPositions[i] + CachedShadowVelocities[i] * ClampedDelta;
				});
				// Note: Anisotropy is not predicted, use cached values directly
			}
			else if (CachedShadowPositions.Num() > 0)
			{
				// Fallback: use cached positions without prediction
				Positions = CachedShadowPositions;
				NumParticles = Positions.Num();
			}
		}
		else
		{
			// CPU Mode: Get positions from CPU particles
			const TArray<FFluidParticle>& Particles = SimulationModule->GetParticles();
			NumParticles = Particles.Num();

			if (NumParticles > 0)
			{
				Positions.SetNum(NumParticles);
				CachedShadowVelocities.SetNum(NumParticles);
				CachedNeighborCounts.SetNum(NumParticles);

				for (int32 i = 0; i < NumParticles; ++i)
				{
					Positions[i] = Particles[i].Position;
					CachedShadowVelocities[i] = Particles[i].Velocity;
					CachedNeighborCounts[i] = Particles[i].NeighborIndices.Num();
				}
			}
		}

		if (NumParticles > 0)
		{
			// Calculate fluid bounds from positions (Parallel Reduction)
			FBox FluidBounds(ForceInit);

			if (NumParticles > 1024)
			{
				const int32 ChunkSize = 1024;
				const int32 NumChunks = FMath::DivideAndRoundUp(NumParticles, ChunkSize);
				TArray<FBox> ChunkBounds;
				ChunkBounds.Init(FBox(ForceInit), NumChunks);

				ParallelFor(NumChunks, [&](int32 ChunkIndex)
				{
					const int32 StartIndex = ChunkIndex * ChunkSize;
					const int32 EndIndex = FMath::Min(StartIndex + ChunkSize, NumParticles);

					FBox LocalBox(ForceInit);
					for (int32 i = StartIndex; i < EndIndex; ++i)
					{
						LocalBox += Positions[i];
					}
					ChunkBounds[ChunkIndex] = LocalBox;
				});

				// Merge each chunk's result into main Bounds
				for (const FBox& Box : ChunkBounds)
				{
					FluidBounds += Box;
				}
			}
			else
			{
				// Process in single thread if particle count is low (avoid overhead)
				for (int32 i = 0; i < NumParticles; ++i)
				{
					FluidBounds += Positions[i];
				}
			}

			// Expand bounds by particle radius
			const float ParticleRadius = SimulationModule->GetParticleRadius();
			FluidBounds = FluidBounds.ExpandBy(ParticleRadius * 2.0f);

			// =====================================================
			// Step 2: VSM/Shadow Integration (conditional)
			// =====================================================
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				if (RendererSubsystem->bEnableVSMIntegration && VolumeComponent->bEnableShadow)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(VSM Integration Volume)

					// Update shadow proxy state (creates HISM component if needed)
					RendererSubsystem->UpdateShadowProxyState();

					// Update HISM shadow instances with uniform spheres
					RendererSubsystem->UpdateShadowInstances(
						Positions.GetData(),
						NumParticles,
						ParticleRadius
					);
				}
			}

			// =====================================================
			// Step 3: Splash VFX (independent of Shadow settings)
			// =====================================================
			UNiagaraSystem* SplashVFX = VolumeComponent->SplashVFX;
			if (SplashVFX)
			{
				const float SplashVelocityThreshold = VolumeComponent->SplashVelocityThreshold;
				const int32 MaxSplashVFXPerFrame = VolumeComponent->MaxSplashVFXPerFrame;
				const ESplashConditionMode SplashConditionMode = VolumeComponent->SplashConditionMode;
				const int32 IsolationNeighborThreshold = VolumeComponent->IsolationNeighborThreshold;

				int32 SpawnCount = 0;
				const bool bHasVelocityData = CachedShadowVelocities.Num() == NumParticles;
				const bool bHasNeighborData = CachedNeighborCounts.Num() == NumParticles;
				const bool bHasPrevNeighborData = PrevNeighborCounts.Num() == NumParticles;

				for (int32 i = 0; i < NumParticles && SpawnCount < MaxSplashVFXPerFrame; ++i)
				{
					// Velocity condition: fast-moving particle
					bool bFastMoving = false;
					FVector VelocityDir = FVector::UpVector;
					if (bHasVelocityData)
					{
						const float Speed = CachedShadowVelocities[i].Size();
						bFastMoving = Speed > SplashVelocityThreshold;
						VelocityDir = CachedShadowVelocities[i].GetSafeNormal();
					}

					// Isolation condition: few neighbors
					bool bIsolated = false;
					bool bJustBecameIsolated = false;
					if (bHasNeighborData)
					{
						bIsolated = CachedNeighborCounts[i] <= IsolationNeighborThreshold;

						// State change detection: was not isolated -> now isolated
						if (bHasPrevNeighborData)
						{
							const bool bWasIsolated = PrevNeighborCounts[i] <= IsolationNeighborThreshold;
							bJustBecameIsolated = bIsolated && !bWasIsolated;
						}
						else
						{
							// First frame with data - treat as state change if isolated
							bJustBecameIsolated = bIsolated;
						}
					}

					// Evaluate spawn condition based on mode
					// For isolation-related modes, only spawn on state change (non-isolated -> isolated)
					bool bShouldSpawn = false;
					switch (SplashConditionMode)
					{
					case ESplashConditionMode::VelocityAndIsolation:
						bShouldSpawn = bFastMoving && bJustBecameIsolated;
						break;
					case ESplashConditionMode::VelocityOrIsolation:
						bShouldSpawn = bFastMoving || bJustBecameIsolated;
						break;
					case ESplashConditionMode::VelocityOnly:
						bShouldSpawn = bFastMoving;
						break;
					case ESplashConditionMode::IsolationOnly:
						bShouldSpawn = bJustBecameIsolated;
						break;
					}

					if (bShouldSpawn)
					{
						UNiagaraFunctionLibrary::SpawnSystemAtLocation(
							this,
							SplashVFX,
							Positions[i],
							VelocityDir.Rotation()
						);
						++SpawnCount;
					}
				}

				// Update previous neighbor counts for next frame's state change detection
				if (bHasNeighborData)
				{
					PrevNeighborCounts = CachedNeighborCounts;
				}
			}
		}
	}

	// Simulation is handled by subsystem for proper batching
	// The Subsystem tick runs after actor ticks, so particles spawned above
	// will be simulated in the current frame.
}

#if WITH_EDITOR
void AKawaiiFluidVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Preset changes are now handled in VolumeComponent
	// This method can be used for actor-specific property changes if needed
}
#endif

void AKawaiiFluidVolume::Simulate(float DeltaTime)
{
	// Process pending spawn requests before simulation
	ProcessPendingSpawnRequests();

	// Simulation is handled by Subsystem which processes all registered modules.
	// The SimulationModule registered in InitializeSimulation() is automatically
	// simulated by the Subsystem.
}

void AKawaiiFluidVolume::QueueSpawnRequest(FVector Position, FVector Velocity, int32 SourceID)
{
	FGPUSpawnRequest Request;
	Request.Position = FVector3f(Position);
	Request.Velocity = FVector3f(Velocity);
	Request.SourceID = SourceID;
	Request.Mass = 1.0f;
	Request.Radius = 0.0f; // Use default from preset

	PendingSpawnRequests.Add(Request);
}

void AKawaiiFluidVolume::QueueSpawnRequests(const TArray<FVector>& Positions, const TArray<FVector>& Velocities, int32 SourceID)
{
	const int32 Count = FMath::Min(Positions.Num(), Velocities.Num());
	if (Count == 0)
	{
		return;
	}

	PendingSpawnRequests.Reserve(PendingSpawnRequests.Num() + Count);

	for (int32 i = 0; i < Count; ++i)
	{
		FGPUSpawnRequest Request;
		Request.Position = FVector3f(Positions[i]);
		Request.Velocity = FVector3f(Velocities[i]);
		Request.SourceID = SourceID;
		Request.Mass = 0.0f;
		Request.Radius = 0.0f;

		PendingSpawnRequests.Add(Request);
	}
}

void AKawaiiFluidVolume::ProcessPendingSpawnRequests()
{
	if (PendingSpawnRequests.Num() == 0 || !SimulationModule)
	{
		return;
	}

	// Get GPU simulator to send requests directly (preserving SourceID from each request)
	FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
	if (!GPUSimulator)
	{
		UE_LOG(LogTemp, Warning, TEXT("AKawaiiFluidVolume [%s]: No GPU simulator available for spawn requests"),
			*GetName());
		PendingSpawnRequests.Empty();
		return;
	}

	// Fill in default mass and radius from preset
	const float DefaultMass = SimulationModule->Preset ? SimulationModule->Preset->ParticleMass : 1.0f;
	const float DefaultRadius = SimulationModule->Preset ? SimulationModule->Preset->ParticleRadius : 5.0f;

	for (FGPUSpawnRequest& Request : PendingSpawnRequests)
	{
		if (Request.Mass <= 0.0f)
		{
			Request.Mass = DefaultMass;
		}
		if (Request.Radius <= 0.0f)
		{
			Request.Radius = DefaultRadius;
		}
	}

	// Send all requests directly to GPU (preserving each request's SourceID)
	GPUSimulator->AddSpawnRequests(PendingSpawnRequests);

	UE_LOG(LogTemp, Verbose, TEXT("AKawaiiFluidVolume [%s]: Sent %d spawn requests to GPU"),
		*GetName(), PendingSpawnRequests.Num());

	PendingSpawnRequests.Empty();
}

void AKawaiiFluidVolume::RegisterEmitter(AKawaiiFluidEmitter* Emitter)
{
	if (Emitter && !RegisteredEmitters.Contains(Emitter))
	{
		RegisteredEmitters.Add(Emitter);
		UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume: Registered emitter %s"), *Emitter->GetName());
	}
}

void AKawaiiFluidVolume::UnregisterEmitter(AKawaiiFluidEmitter* Emitter)
{
	if (Emitter)
	{
		RegisteredEmitters.Remove(Emitter);
		UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume: Unregistered emitter %s"), *Emitter->GetName());
	}
}

//========================================
// Delegate Getters (from VolumeComponent)
//========================================

UKawaiiFluidPresetDataAsset* AKawaiiFluidVolume::GetPreset() const
{
	return VolumeComponent ? VolumeComponent->GetPreset() : nullptr;
}

EFluidType AKawaiiFluidVolume::GetFluidType() const
{
	return VolumeComponent ? VolumeComponent->GetFluidType() : EFluidType::None;
}

void AKawaiiFluidVolume::SetFluidType(EFluidType InFluidType)
{
	if (VolumeComponent)
	{
		VolumeComponent->SetFluidType(InFluidType);
	}

	// Also forward to SimulationModule if initialized
	if (SimulationModule)
	{
		SimulationModule->SetFluidType(InFluidType);
	}
}

float AKawaiiFluidVolume::GetParticleSpacing() const
{
	return VolumeComponent ? VolumeComponent->GetParticleSpacing() : 10.0f;
}

FVector AKawaiiFluidVolume::GetVolumeSize() const
{
	return VolumeComponent ? VolumeComponent->GetEffectiveVolumeSize() : FVector(1000.0f);
}

//========================================
// Debug Methods (Delegate to VolumeComponent)
//========================================

void AKawaiiFluidVolume::SetDebugDrawMode(EKawaiiFluidDebugDrawMode Mode)
{
	if (VolumeComponent)
	{
		VolumeComponent->SetDebugDrawMode(Mode);
	}
}

EKawaiiFluidDebugDrawMode AKawaiiFluidVolume::GetDebugDrawMode() const
{
	return VolumeComponent ? VolumeComponent->GetDebugDrawMode() : EKawaiiFluidDebugDrawMode::None;
}

void AKawaiiFluidVolume::SetDebugVisualization(EFluidDebugVisualization Mode)
{
	if (VolumeComponent)
	{
		VolumeComponent->SetDebugVisualization(Mode);
	}
}

EFluidDebugVisualization AKawaiiFluidVolume::GetDebugVisualization() const
{
	return VolumeComponent ? VolumeComponent->GetDebugVisualization() : EFluidDebugVisualization::None;
}

void AKawaiiFluidVolume::EnableDebugDraw(EFluidDebugVisualization Mode, float PointSize)
{
	if (VolumeComponent)
	{
		VolumeComponent->EnableDebugDraw(Mode, PointSize);
	}
}

void AKawaiiFluidVolume::DisableDebugDraw()
{
	if (VolumeComponent)
	{
		VolumeComponent->DisableDebugDraw();
	}
}

//========================================
// Initialization
//========================================

void AKawaiiFluidVolume::InitializeSimulation()
{
	if (!VolumeComponent || !SimulationModule)
	{
		return;
	}

	// Skip if already initialized (handles re-initialization cases)
	if (SimulationModule->IsInitialized())
	{
		return;
	}

	// Get Preset from VolumeComponent
	UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset();

	// Create default Preset if not assigned
	if (!Preset)
	{
		Preset = NewObject<UKawaiiFluidPresetDataAsset>(this, NAME_None, RF_Transient);
		VolumeComponent->Preset = Preset;
		UE_LOG(LogTemp, Warning, TEXT("AKawaiiFluidVolume [%s]: No Preset assigned, using default values"), *GetName());
	}

	// Initialize SimulationModule (module was created in constructor via CreateDefaultSubobject)
	SimulationModule->Initialize(Preset);

	// Forward VolumeComponent properties to SimulationModule
	SimulationModule->FluidType = VolumeComponent->FluidType;
	SimulationModule->bUseWorldCollision = VolumeComponent->bUseWorldCollision;
	SimulationModule->bEnableCollisionEvents = VolumeComponent->bEnableCollisionEvents;
	SimulationModule->MinVelocityForEvent = VolumeComponent->MinVelocityForEvent;
	SimulationModule->MaxEventsPerFrame = VolumeComponent->MaxEventsPerFrame;
	SimulationModule->EventCooldownPerParticle = VolumeComponent->EventCooldownPerParticle;

	// Set the module to use this volume's VolumeComponent for bounds
	SimulationModule->SetTargetSimulationVolume(this);

	// Register module with VolumeComponent
	VolumeComponent->RegisterModule(SimulationModule);

	UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Simulation module initialized with Preset [%s]"),
		*GetName(), Preset ? *Preset->GetName() : TEXT("Default"));
}

void AKawaiiFluidVolume::RegisterSimulationWithSubsystem()
{
	UWorld* World = GetWorld();
	if (!World || !SimulationModule || !VolumeComponent)
	{
		return;
	}

	// Skip if already registered (has valid SimulationContext)
	if (SimulationContext != nullptr)
	{
		return;
	}

	UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset();

	// Get or create SimulationContext from Subsystem
	UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>();
	if (Subsystem)
	{
		// Register the module to subsystem (handles SourceID allocation and GPU setup)
		Subsystem->RegisterModule(SimulationModule);

		// Get context for this volume
		SimulationContext = Subsystem->GetOrCreateContext(VolumeComponent, Preset);
	}

	// Debug: Verify GPU initialization status
	const bool bGPUReady = SimulationModule->GetGPUSimulator() != nullptr;
	const bool bGPUActive = SimulationModule->IsGPUSimulationActive();
	const bool bContextReady = SimulationContext && SimulationContext->IsGPUSimulatorReady();

	UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Registered with Subsystem"),
		*GetName());
	UE_LOG(LogTemp, Log, TEXT("  - GPU Simulator: %s, Active: %s, Context Ready: %s"),
		bGPUReady ? TEXT("Ready") : TEXT("NOT Ready"),
		bGPUActive ? TEXT("Yes") : TEXT("No"),
		bContextReady ? TEXT("Yes") : TEXT("No"));
}

void AKawaiiFluidVolume::CleanupSimulation()
{
	UWorld* World = GetWorld();

	// Unregister module from VolumeComponent
	if (VolumeComponent && SimulationModule)
	{
		VolumeComponent->UnregisterModule(SimulationModule);
	}

	// Unregister module from Subsystem
	if (World)
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			if (SimulationModule)
			{
				Subsystem->UnregisterModule(SimulationModule);
			}
		}
	}

	// Shutdown SimulationModule (don't set to nullptr - it's a CreateDefaultSubobject)
	if (SimulationModule)
	{
		SimulationModule->Shutdown();
		// Note: SimulationModule is created via CreateDefaultSubobject, so we don't null it
	}

	// Clear context reference (context is owned by Subsystem)
	SimulationContext = nullptr;

	UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Simulation cleaned up"), *GetName());
}

void AKawaiiFluidVolume::InitializeRendering()
{
	if (!VolumeComponent || !VolumeComponent->bEnableRendering || !RenderingModule || !SimulationModule)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Get Preset from VolumeComponent
	UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset();

	// Initialize RenderingModule with SimulationModule as data provider
	// Rendering parameters (PipelineType, etc.) are taken from Preset->RenderingParameters
	RenderingModule->Initialize(World, VolumeComponent, SimulationModule, Preset);

	// Configure ISMRenderer based on initial DebugDrawMode (like KawaiiFluidComponent)
	if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
	{
		const bool bISMMode = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM);
		ISMRenderer->SetEnabled(bISMMode);
		ISMRenderer->SetMaxRenderParticles(VolumeComponent->ISMMaxRenderParticles);
		if (bISMMode)
		{
			ISMRenderer->SetFluidColor(VolumeComponent->ISMDebugColor);
		}
		UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: ISMRenderer %s (MaxRenderParticles: %d)"), *GetName(), bISMMode ? TEXT("enabled") : TEXT("disabled"), VolumeComponent->ISMMaxRenderParticles);
	}

	// Configure MetaballRenderer based on preset's RenderingParameters
	// Metaball is disabled when ISM or DebugDraw mode is active (mutual exclusion)
	if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
	{
		const bool bISMMode = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM);
		const bool bDebugDrawMode = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::DebugDraw);
		
		if (Preset)
		{
			// Disable Metaball if ISM or DebugDraw mode is active
			const bool bEnableMetaball = Preset->RenderingParameters.bEnableRendering && !bISMMode && !bDebugDrawMode;
			MetaballRenderer->SetEnabled(bEnableMetaball);

			// UpdatePipeline() creates the correct pipeline based on preset's PipelineType
			// GetLocalParameters() returns Preset->RenderingParameters
			MetaballRenderer->UpdatePipeline();

			UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: MetaballRenderer configured (Enabled: %s, Pipeline: %d)"),
				*GetName(),
				bEnableMetaball ? TEXT("true") : TEXT("false"),
				static_cast<int32>(Preset->RenderingParameters.PipelineType));
	}

	// Connect MetaballRenderer to SimulationContext
		// This is done here because MetaballRenderer wasn't created yet when
		// Subsystem::RegisterModule was called in InitializeSimulation
		if (SimulationContext)
		{
			MetaballRenderer->SetSimulationContext(SimulationContext);
		}
	}

	// Register RenderingModule with FluidRendererSubsystem (CRITICAL for SSFR rendering pipeline)
	if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
	{
		RendererSubsystem->RegisterRenderingModule(RenderingModule);
	}

	UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Rendering initialized"), *GetName());
}

#if WITH_EDITOR
void AKawaiiFluidVolume::InitializeEditorRendering()
{
	// Skip if already initialized
	if (bEditorRenderingInitialized)
	{
		return;
	}

	if (!VolumeComponent || !RenderingModule || !SimulationModule)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Skip if this is a game world (BeginPlay will handle it)
	if (World->IsGameWorld())
	{
		return;
	}

	// Get Preset from VolumeComponent
	UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset();

	// Initialize RenderingModule with SimulationModule as data provider
	// This is the same initialization as BeginPlay does for runtime
	RenderingModule->Initialize(World, VolumeComponent, SimulationModule, Preset);

	// Configure renderers based on DebugDrawMode (same as runtime behavior)
	// Respect the user's setting - don't force ISM
	const EKawaiiFluidDebugDrawMode CurrentMode = VolumeComponent->DebugDrawMode;
	const bool bISMMode = (CurrentMode == EKawaiiFluidDebugDrawMode::ISM);
	const bool bDebugDrawMode = (CurrentMode == EKawaiiFluidDebugDrawMode::DebugDraw);

	if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
	{
		ISMRenderer->SetEnabled(bISMMode);
		ISMRenderer->SetMaxRenderParticles(VolumeComponent->ISMMaxRenderParticles);
		if (bISMMode)
		{
			ISMRenderer->SetFluidColor(VolumeComponent->ISMDebugColor);
		}
		UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Editor ISMRenderer %s"), 
			*GetName(), bISMMode ? TEXT("enabled") : TEXT("disabled"));
	}

	// Metaball is enabled when rendering is enabled AND not in ISM/DebugDraw mode
	if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
	{
		const bool bEnableMetaball = VolumeComponent->bEnableRendering && !bISMMode && !bDebugDrawMode;
		MetaballRenderer->SetEnabled(bEnableMetaball);
		
		if (bEnableMetaball && Preset)
		{
			MetaballRenderer->UpdatePipeline();
		}
		
		// Connect to SimulationContext for GPU rendering
		if (SimulationContext)
		{
			MetaballRenderer->SetSimulationContext(SimulationContext);
		}
		
		UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Editor MetaballRenderer %s"), 
			*GetName(), bEnableMetaball ? TEXT("enabled") : TEXT("disabled"));
	}

	// Register RenderingModule with FluidRendererSubsystem (CRITICAL for SSFR rendering pipeline)
	// This is what actually triggers the Metaball rendering - same as InitializeRendering() does for runtime
	if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
	{
		RendererSubsystem->RegisterRenderingModule(RenderingModule);
		UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Editor RenderingModule registered with FluidRendererSubsystem"), *GetName());
	}

	bEditorRenderingInitialized = true;
	UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Editor rendering initialized (DebugDrawMode=%d)"), 
		*GetName(), static_cast<int32>(CurrentMode));
}
#endif

void AKawaiiFluidVolume::CleanupRendering()
{
	// Unregister from FluidRendererSubsystem first
	if (UWorld* World = GetWorld())
	{
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->UnregisterRenderingModule(RenderingModule);
		}
	}

	if (RenderingModule)
	{
		RenderingModule->Cleanup();
	}

	UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Rendering cleaned up"), *GetName());
}

void AKawaiiFluidVolume::RegisterToSubsystem()
{
	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			Subsystem->RegisterVolume(this);
		}
	}
}

void AKawaiiFluidVolume::UnregisterFromSubsystem()
{
	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			Subsystem->UnregisterVolume(this);
		}
	}
}

//========================================
// Debug Visualization
//========================================

void AKawaiiFluidVolume::DrawDebugParticles()
{
	UWorld* World = GetWorld();
	if (!World || !SimulationModule || !VolumeComponent)
	{
		return;
	}

	// Get particle data (GPU or CPU) - same pattern as KawaiiFluidComponent
	const TArray<FFluidParticle>* ParticlesPtr = nullptr;
	TArray<FFluidParticle> GPUParticlesCache;

	if (SimulationModule->IsGPUSimulationActive())
	{
		// GPU mode: Use readback data
		FGPUFluidSimulator* Simulator = SimulationModule->GetGPUSimulator();
		if (Simulator && Simulator->GetAllGPUParticles(GPUParticlesCache))
		{
			ParticlesPtr = &GPUParticlesCache;
		}
		else
		{
			// No readback data available
			return;
		}
	}
	else
	{
		// CPU mode: Direct particle array
		ParticlesPtr = &SimulationModule->GetParticles();
	}

	if (!ParticlesPtr || ParticlesPtr->Num() == 0)
	{
		return;
	}

	const TArray<FFluidParticle>& Particles = *ParticlesPtr;
	const int32 TotalCount = Particles.Num();

	// Get debug point size from VolumeComponent
	const float DebugPointSize = VolumeComponent->DebugPointSize;

	// Update bounds for position-based coloring
	DebugDrawBoundsMin = Particles[0].Position;
	DebugDrawBoundsMax = Particles[0].Position;
	for (const FFluidParticle& P : Particles)
	{
		DebugDrawBoundsMin = DebugDrawBoundsMin.ComponentMin(P.Position);
		DebugDrawBoundsMax = DebugDrawBoundsMax.ComponentMax(P.Position);
	}

	// Draw particles
	for (int32 i = 0; i < TotalCount; ++i)
	{
		const FFluidParticle& P = Particles[i];
		FColor Color = ComputeDebugDrawColor(i, TotalCount, P.Position, P.Density);
		DrawDebugPoint(World, P.Position, DebugPointSize, Color, false, -1.0f, 0);
	}
}

void AKawaiiFluidVolume::DrawDebugStaticBoundaryParticles()
{
	if (!VolumeComponent || !VolumeComponent->bShowStaticBoundaryParticles)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Get debug settings from VolumeComponent
	const float StaticBoundaryPointSize = VolumeComponent->StaticBoundaryPointSize;
	const FColor StaticBoundaryColor = VolumeComponent->StaticBoundaryColor;
	const bool bShowStaticBoundaryNormals = VolumeComponent->bShowStaticBoundaryNormals;
	const float StaticBoundaryNormalLength = VolumeComponent->StaticBoundaryNormalLength;

	const bool bIsGameWorld = World->IsGameWorld();

	// Game mode: Use actual boundary particles from GPU simulation (like KawaiiFluidComponent)
	if (bIsGameWorld)
	{
		if (!SimulationModule)
		{
			return;
		}

		FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
		if (!GPUSimulator || !GPUSimulator->HasStaticBoundaryParticles())
		{
			return;
		}

		const TArray<FGPUBoundaryParticle>& BoundaryParticles = GPUSimulator->GetStaticBoundaryParticles();
		const int32 NumParticles = BoundaryParticles.Num();

		if (NumParticles == 0)
		{
			return;
		}

		// Draw boundary particles
		for (int32 i = 0; i < NumParticles; ++i)
		{
			const FGPUBoundaryParticle& Particle = BoundaryParticles[i];
			const FVector Position(Particle.Position.X, Particle.Position.Y, Particle.Position.Z);

			DrawDebugPoint(World, Position, StaticBoundaryPointSize, StaticBoundaryColor, false, -1.0f, 0);

			if (bShowStaticBoundaryNormals)
			{
				const FVector Normal(Particle.Normal.X, Particle.Normal.Y, Particle.Normal.Z);
				const FVector NormalEnd = Position + Normal * StaticBoundaryNormalLength;
				DrawDebugDirectionalArrow(World, Position, NormalEnd, StaticBoundaryNormalLength * 0.3f, FColor::Yellow, false, -1.0f, 0, 1.0f);
			}
		}
	}
#if WITH_EDITOR
	// Editor mode: Use preview boundary particles
	else
	{
		// Periodically regenerate boundary particles (every 30 frames)
		if (GFrameCounter - LastEditorPreviewFrame > 30)
		{
			GenerateEditorBoundaryParticlesPreview();
			LastEditorPreviewFrame = GFrameCounter;
		}

		const int32 NumParticles = EditorPreviewBoundaryPositions.Num();
		if (NumParticles == 0)
		{
			return;
		}

		// Draw boundary particles
		for (int32 i = 0; i < NumParticles; ++i)
		{
			const FVector& Position = EditorPreviewBoundaryPositions[i];
			DrawDebugPoint(World, Position, StaticBoundaryPointSize, StaticBoundaryColor, false, -1.0f, 0);

			if (bShowStaticBoundaryNormals && EditorPreviewBoundaryNormals.IsValidIndex(i))
			{
				const FVector& Normal = EditorPreviewBoundaryNormals[i];
				const FVector NormalEnd = Position + Normal * StaticBoundaryNormalLength;
				DrawDebugDirectionalArrow(World, Position, NormalEnd, StaticBoundaryNormalLength * 0.3f, FColor::Yellow, false, -1.0f, 0, 1.0f);
			}
		}

		// Log particle count periodically
		static int32 LogCounter = 0;
		if (++LogCounter % 300 == 1)
		{
			UE_LOG(LogTemp, Log, TEXT("[StaticBoundary Editor Volume] Drawing %d boundary particles"), NumParticles);
		}
	}
#endif
}

#if WITH_EDITOR
void AKawaiiFluidVolume::GenerateEditorBoundaryParticlesPreview()
{
	EditorPreviewBoundaryPositions.Empty();
	EditorPreviewBoundaryNormals.Empty();

	UWorld* World = GetWorld();
	if (!World || !VolumeComponent)
	{
		return;
	}

	// Get spacing from preset's smoothing radius (like KawaiiFluidComponent)
	float SmoothingRadius = 20.0f;
	if (UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset())
	{
		SmoothingRadius = Preset->SmoothingRadius;
	}
	const float Spacing = SmoothingRadius * 0.5f;

	// Get volume bounds
	const FVector VolumeCenter = VolumeComponent->GetComponentLocation();
	const FVector BoundsMin = VolumeComponent->GetWorldBoundsMin();
	const FVector BoundsMax = VolumeComponent->GetWorldBoundsMax();
	const FVector HalfExtent = (BoundsMax - BoundsMin) * 0.5f;
	const FBox VolumeBounds(BoundsMin, BoundsMax);

	// Find overlapping static mesh actors (like KawaiiFluidComponent)
	TArray<FOverlapResult> OverlapResults;
	FCollisionQueryParams QueryParams;
	QueryParams.bReturnPhysicalMaterial = false;
	QueryParams.AddIgnoredActor(this);

	World->OverlapMultiByObjectType(
		OverlapResults,
		VolumeCenter,
		FQuat::Identity,
		FCollisionObjectQueryParams(ECollisionChannel::ECC_WorldStatic),
		FCollisionShape::MakeBox(HalfExtent),
		QueryParams
	);

	// Helper lambda: Generate boundary particles on a box face
	auto GenerateBoxFaceParticles = [this, Spacing](
		const FVector& FaceCenter, const FVector& Normal,
		const FVector& UAxis, const FVector& VAxis,
		float UExtent, float VExtent)
	{
		const int32 NumU = FMath::Max(1, FMath::CeilToInt(UExtent * 2.0f / Spacing));
		const int32 NumV = FMath::Max(1, FMath::CeilToInt(VExtent * 2.0f / Spacing));

		for (int32 iu = 0; iu <= NumU; ++iu)
		{
			for (int32 iv = 0; iv <= NumV; ++iv)
			{
				const float U = -UExtent + (2.0f * UExtent * iu / NumU);
				const float V = -VExtent + (2.0f * VExtent * iv / NumV);

				FVector Position = FaceCenter + UAxis * U + VAxis * V;
				EditorPreviewBoundaryPositions.Add(Position);
				EditorPreviewBoundaryNormals.Add(Normal);
			}
		}
	};

	// Helper lambda: Generate boundary particles on a sphere
	auto GenerateSphereParticles = [this, Spacing](const FVector& Center, float Radius)
	{
		const float GoldenRatio = (1.0f + FMath::Sqrt(5.0f)) / 2.0f;
		const float AngleIncrement = PI * 2.0f * GoldenRatio;
		const float SurfaceArea = 4.0f * PI * Radius * Radius;
		const int32 NumPoints = FMath::Max(4, FMath::CeilToInt(SurfaceArea / (Spacing * Spacing)));

		for (int32 i = 0; i < NumPoints; ++i)
		{
			const float T = static_cast<float>(i) / static_cast<float>(NumPoints - 1);
			const float Phi = FMath::Acos(1.0f - 2.0f * T);
			const float Theta = AngleIncrement * i;

			const float SinPhi = FMath::Sin(Phi);
			const float CosPhi = FMath::Cos(Phi);

			FVector Normal(SinPhi * FMath::Cos(Theta), SinPhi * FMath::Sin(Theta), CosPhi);
			FVector Position = Center + Normal * Radius;

			EditorPreviewBoundaryPositions.Add(Position);
			EditorPreviewBoundaryNormals.Add(Normal);
		}
	};

	// Process each overlapping static mesh (like KawaiiFluidComponent)
	for (const FOverlapResult& Result : OverlapResults)
	{
		UPrimitiveComponent* PrimComp = Result.GetComponent();
		UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(PrimComp);
		if (!StaticMeshComp || !StaticMeshComp->GetStaticMesh())
		{
			continue;
		}

		UBodySetup* BodySetup = StaticMeshComp->GetStaticMesh()->GetBodySetup();
		if (!BodySetup)
		{
			continue;
		}

		const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
		const FTransform ComponentTransform = StaticMeshComp->GetComponentTransform();

		// Process Spheres
		for (const FKSphereElem& SphereElem : AggGeom.SphereElems)
		{
			const FTransform SphereTransform = SphereElem.GetTransform() * ComponentTransform;
			const FVector Center = SphereTransform.GetLocation();
			const float Radius = SphereElem.Radius * ComponentTransform.GetScale3D().GetMax();

			if (VolumeBounds.IsInside(Center) || VolumeBounds.ComputeSquaredDistanceToPoint(Center) < Radius * Radius)
			{
				GenerateSphereParticles(Center, Radius);
			}
		}

		// Process Boxes
		for (const FKBoxElem& BoxElem : AggGeom.BoxElems)
		{
			const FTransform BoxTransform = BoxElem.GetTransform() * ComponentTransform;
			const FVector Center = BoxTransform.GetLocation();
			const FQuat Rotation = BoxTransform.GetRotation();
			const FVector Scale = ComponentTransform.GetScale3D();
			const FVector Extent(BoxElem.X * 0.5f * Scale.X, BoxElem.Y * 0.5f * Scale.Y, BoxElem.Z * 0.5f * Scale.Z);

			// Check if box overlaps with volume
			if (!VolumeBounds.Intersect(FBox(Center - Extent, Center + Extent)))
			{
				continue;
			}

			// Local axes
			const FVector LocalX = Rotation.RotateVector(FVector::ForwardVector);
			const FVector LocalY = Rotation.RotateVector(FVector::RightVector);
			const FVector LocalZ = Rotation.RotateVector(FVector::UpVector);

			// Generate particles on 6 faces
			GenerateBoxFaceParticles(Center + LocalX * Extent.X, LocalX, LocalY, LocalZ, Extent.Y, Extent.Z);
			GenerateBoxFaceParticles(Center - LocalX * Extent.X, -LocalX, LocalY, LocalZ, Extent.Y, Extent.Z);
			GenerateBoxFaceParticles(Center + LocalY * Extent.Y, LocalY, LocalX, LocalZ, Extent.X, Extent.Z);
			GenerateBoxFaceParticles(Center - LocalY * Extent.Y, -LocalY, LocalX, LocalZ, Extent.X, Extent.Z);
			GenerateBoxFaceParticles(Center + LocalZ * Extent.Z, LocalZ, LocalX, LocalY, Extent.X, Extent.Y);
			GenerateBoxFaceParticles(Center - LocalZ * Extent.Z, -LocalZ, LocalX, LocalY, Extent.X, Extent.Y);
		}

		// Process Convex elements (simplified - generate particles on triangle faces)
		for (const FKConvexElem& ConvexElem : AggGeom.ConvexElems)
		{
			const TArray<FVector>& VertexData = ConvexElem.VertexData;
			if (VertexData.Num() < 4)
			{
				continue;
			}

			// Calculate world-space center and vertices
			FVector CenterSum = FVector::ZeroVector;
			TArray<FVector> WorldVerts;
			WorldVerts.Reserve(VertexData.Num());

			for (const FVector& Vertex : VertexData)
			{
				const FVector WorldVertex = ComponentTransform.TransformPosition(Vertex);
				WorldVerts.Add(WorldVertex);
				CenterSum += WorldVertex;
			}

			const FVector ConvexCenter = CenterSum / static_cast<float>(WorldVerts.Num());

			// Find faces and generate particles on them
			const TArray<int32>& IndexData = ConvexElem.IndexData;
			if (IndexData.Num() >= 3)
			{
				TSet<uint32> ProcessedNormals;

				for (int32 i = 0; i + 2 < IndexData.Num(); i += 3)
				{
					const int32 I0 = IndexData[i];
					const int32 I1 = IndexData[i + 1];
					const int32 I2 = IndexData[i + 2];

					if (!WorldVerts.IsValidIndex(I0) || !WorldVerts.IsValidIndex(I1) || !WorldVerts.IsValidIndex(I2))
					{
						continue;
					}

					const FVector V0 = WorldVerts[I0];
					const FVector V1 = WorldVerts[I1];
					const FVector V2 = WorldVerts[I2];

					FVector Normal = FVector::CrossProduct(V1 - V0, V2 - V0);
					const float NormalLen = Normal.Size();
					if (NormalLen <= KINDA_SMALL_NUMBER)
					{
						continue;
					}

					Normal /= NormalLen;
					if (FVector::DotProduct(Normal, ConvexCenter - V0) > 0.0f)
					{
						Normal = -Normal;
					}

					// Deduplicate normals (same face check)
					const int32 Nx = FMath::RoundToInt(Normal.X * 100.0f);
					const int32 Ny = FMath::RoundToInt(Normal.Y * 100.0f);
					const int32 Nz = FMath::RoundToInt(Normal.Z * 100.0f);
					const uint32 Hash = HashCombine(HashCombine(GetTypeHash(Nx), GetTypeHash(Ny)), GetTypeHash(Nz));
					if (ProcessedNormals.Contains(Hash))
					{
						continue;
					}
					ProcessedNormals.Add(Hash);

					// Add triangle vertices as boundary points (simplified)
					const FVector TriCenter = (V0 + V1 + V2) / 3.0f;
					EditorPreviewBoundaryPositions.Add(TriCenter);
					EditorPreviewBoundaryNormals.Add(Normal);

					// Add edge midpoints for better coverage
					EditorPreviewBoundaryPositions.Add((V0 + V1) * 0.5f);
					EditorPreviewBoundaryNormals.Add(Normal);
					EditorPreviewBoundaryPositions.Add((V1 + V2) * 0.5f);
					EditorPreviewBoundaryNormals.Add(Normal);
					EditorPreviewBoundaryPositions.Add((V2 + V0) * 0.5f);
					EditorPreviewBoundaryNormals.Add(Normal);
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[StaticBoundary Editor Volume] Generated %d preview boundary particles from %d overlapping meshes"),
		EditorPreviewBoundaryPositions.Num(), OverlapResults.Num());
}
#endif

FColor AKawaiiFluidVolume::ComputeDebugDrawColor(int32 ParticleIndex, int32 TotalCount, const FVector& InPosition, float Density) const
{
	if (!VolumeComponent)
	{
		return FColor::White;
	}

	const EFluidDebugVisualization VisualizationType = VolumeComponent->DebugVisualizationType;
	const UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset();

	switch (VisualizationType)
	{
	case EFluidDebugVisualization::ZOrderArrayIndex:
	case EFluidDebugVisualization::ArrayIndex:  // Legacy fallthrough
		{
			// Rainbow gradient based on array index
			// If Z-Order sorted correctly, spatially close particles should have similar colors
			const float T = TotalCount > 1 ? static_cast<float>(ParticleIndex) / static_cast<float>(TotalCount - 1) : 0.0f;
			return FLinearColor::MakeFromHSV8(static_cast<uint8>(T * 255.0f), 255, 255).ToFColor(true);
		}

	case EFluidDebugVisualization::ZOrderMortonCode:
	case EFluidDebugVisualization::MortonCode:  // Legacy fallthrough
		{
			// Compute Morton code from position
			FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
			float MaxRange = FMath::Max3(Range.X, Range.Y, Range.Z);
			if (MaxRange < KINDA_SMALL_NUMBER) MaxRange = 1.0f;

			FVector NormPos = (InPosition - DebugDrawBoundsMin) / MaxRange;
			NormPos.X = FMath::Clamp(NormPos.X, 0.0, 1.0);
			NormPos.Y = FMath::Clamp(NormPos.Y, 0.0, 1.0);
			NormPos.Z = FMath::Clamp(NormPos.Z, 0.0, 1.0);

			// Simple Morton-like hue (not full Morton code, but visually similar)
			float Hue = (NormPos.X * 0.33f + NormPos.Y * 0.33f + NormPos.Z * 0.33f);
			return FLinearColor::MakeFromHSV8(static_cast<uint8>(Hue * 255.0f), 255, 255).ToFColor(true);
		}

	case EFluidDebugVisualization::PositionX:
		{
			const FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
			const float Normalized = (Range.X > KINDA_SMALL_NUMBER) ?
				(InPosition.X - DebugDrawBoundsMin.X) / Range.X : 0.0f;
			return FColor(static_cast<uint8>(FMath::Clamp(Normalized, 0.0f, 1.0f) * 255), 50, 50);
		}

	case EFluidDebugVisualization::PositionY:
		{
			const FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
			const float Normalized = (Range.Y > KINDA_SMALL_NUMBER) ?
				(InPosition.Y - DebugDrawBoundsMin.Y) / Range.Y : 0.0f;
			return FColor(50, static_cast<uint8>(FMath::Clamp(Normalized, 0.0f, 1.0f) * 255), 50);
		}

	case EFluidDebugVisualization::PositionZ:
		{
			const FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
			const float Normalized = (Range.Z > KINDA_SMALL_NUMBER) ?
				(InPosition.Z - DebugDrawBoundsMin.Z) / Range.Z : 0.0f;
			return FColor(50, 50, static_cast<uint8>(FMath::Clamp(Normalized, 0.0f, 1.0f) * 255));
		}

	case EFluidDebugVisualization::Density:
		{
			// Blue (low) to Red (high) based on density
			const float RestDensity = Preset ? Preset->Density : 1000.0f;
			const float NormalizedDensity = FMath::Clamp(Density / (RestDensity * 2.0f), 0.0f, 1.0f);
			return FLinearColor::LerpUsingHSV(FLinearColor::Blue, FLinearColor::Red, NormalizedDensity).ToFColor(true);
		}

	default:
		return FColor::White;
	}
}

//========================================
// Brush API
//========================================

void AKawaiiFluidVolume::AddParticlesInRadius(const FVector& WorldCenter, float Radius, int32 Count,
                                              const FVector& Velocity, float Randomness,
                                              const FVector& SurfaceNormal)
{
	if (!SimulationModule || Count <= 0 || Radius <= 0.0f)
	{
		return;
	}

	// Hemisphere distribution above surface
	for (int32 i = 0; i < Count; ++i)
	{
		// Random point in unit sphere
		FVector RandomDir = FMath::VRand();

		// Ensure above surface (dot with normal > 0)
		if (FVector::DotProduct(RandomDir, SurfaceNormal) < 0)
		{
			RandomDir = -RandomDir;
		}

		// Apply randomness to radius
		const float RandomRadius = Radius * FMath::FRandRange(1.0f - Randomness, 1.0f);
		const FVector SpawnPos = WorldCenter + RandomDir * RandomRadius;

		SimulationModule->SpawnParticle(SpawnPos, Velocity);
	}
}

int32 AKawaiiFluidVolume::RemoveParticlesInRadius(const FVector& WorldCenter, float Radius)
{
	if (!SimulationModule)
	{
		return 0;
	}

	// ID-based despawn: CPU에서 리드백 데이터로 영역 내 ParticleID 수집 후 GPU에서 제거
	FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
	if (!GPUSimulator)
	{
		return 0;
	}

	// Get readback particle data from GPU
	TArray<FGPUFluidParticle> ReadbackParticles;
	if (!GPUSimulator->GetReadbackGPUParticles(ReadbackParticles))
	{
		// No valid readback data yet, skip this frame
		return 0;
	}

	// Find particles within radius and collect their IDs (Volume removes all particles, no SourceID filter)
	const float RadiusSq = Radius * Radius;
	const FVector3f WorldCenterF = FVector3f(WorldCenter);
	TArray<int32> ParticleIDsToRemove;
	TArray<int32> AllReadbackIDs;
	ParticleIDsToRemove.Reserve(128);
	AllReadbackIDs.Reserve(ReadbackParticles.Num());

	for (const FGPUFluidParticle& Particle : ReadbackParticles)
	{
		AllReadbackIDs.Add(Particle.ParticleID);

		const float DistSq = FVector3f::DistSquared(Particle.Position, WorldCenterF);
		if (DistSq <= RadiusSq)
		{
			ParticleIDsToRemove.Add(Particle.ParticleID);
		}
	}

	// Submit ID-based despawn request (CleanupCompletedRequests는 Readback 시 호출됨)
	if (ParticleIDsToRemove.Num() > 0)
	{
		GPUSimulator->AddDespawnByIDRequests(ParticleIDsToRemove);
	}

	return ParticleIDsToRemove.Num();
}

void AKawaiiFluidVolume::ClearAllParticles()
{
	if (SimulationModule)
	{
		SimulationModule->ClearAllParticles();
	}

	// Clear cached shadow data (like UKawaiiFluidComponent does)
	CachedShadowPositions.Empty();
	CachedShadowVelocities.Empty();
	CachedNeighborCounts.Empty();
	CachedAnisotropyAxis1.Empty();
	CachedAnisotropyAxis2.Empty();
	CachedAnisotropyAxis3.Empty();
	PrevNeighborCounts.Empty();

	// Just update rendering - will show 0 particles
	// DO NOT call Cleanup() - that destroys the rendering infrastructure
	// and prevents re-initialization due to guard checks
	if (RenderingModule)
	{
		RenderingModule->UpdateRenderers();
	}
}

//========================================
// Event System
//========================================

void AKawaiiFluidVolume::HandleCollisionEvent(const FKawaiiFluidCollisionEvent& Event)
{
	// Broadcast to VolumeComponent's delegate
	if (VolumeComponent)
	{
		VolumeComponent->OnParticleHit.Broadcast(Event);
	}
}
