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
#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "EngineUtils.h"
#include "Async/ParallelFor.h"
#include "PhysicsEngine/BodySetup.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/OverlapResult.h"

AKawaiiFluidVolume::AKawaiiFluidVolume()
{
	// Create the volume component as root
	VolumeComponent = CreateDefaultSubobject<UKawaiiFluidVolumeComponent>(TEXT("KawaiiFluidVolumeComponent"));
	RootComponent = VolumeComponent;

	// Create simulation module (like UKawaiiFluidComponent pattern)
	// This ensures SimulationModule exists in editor mode for Brush functionality
	SimulationModule = CreateDefaultSubobject<UKawaiiFluidSimulationModule>(TEXT("KawaiiFluidSimulationModule"));

	// Create the rendering module
	RenderingModule = CreateDefaultSubobject<UKawaiiFluidRenderingModule>(TEXT("KawaiiFluidRenderingModule"));

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

	// Set readback request for ISM/Point debug modes (required for GPU->CPU data transfer)
	// Without this, ISM renderer won't get updated particle data
	{
		const bool bNeedReadback = RequiresGPUReadback(VolumeComponent->DebugDrawMode)
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
		const bool bPointDebugMode = IsPointDebugMode(CurrentMode);

		// Sync ISM state based on DebugDrawMode (same logic for editor and runtime)
		if (ISMRenderer)
		{
			bool bModeChanged = (CachedDebugDrawMode != CurrentMode);
			bool bColorChanged = !CachedISMDebugColor.Equals(VolumeComponent->ISMDebugColor, 0.001f);

			if (bModeChanged)
			{
				ISMRenderer->SetEnabled(bISMMode);
				CachedDebugDrawMode = CurrentMode;
			}

			if (bISMMode && (bModeChanged || bColorChanged))
			{
				ISMRenderer->SetFluidColor(VolumeComponent->ISMDebugColor);
				CachedISMDebugColor = VolumeComponent->ISMDebugColor;
			}
		}

		// Check Actor-level visibility only (Component's bHiddenInGame is for wireframe, not fluid rendering)
		const bool bIsActorVisible = !IsHidden();

		// Metaball is enabled when Actor is visible AND not in ISM/DebugDraw mode
		// Same logic for editor and runtime - respects user's DebugDrawMode setting
		if (MetaballRenderer)
		{
			MetaballRenderer->SetEnabled(bIsActorVisible && !bISMMode && !bPointDebugMode);
		}

		// UpdateRenderers must be called when Actor is visible OR ISM debug mode is active
		if (bIsActorVisible || bISMMode)
		{
			RenderingModule->UpdateRenderers();
		}
	}

	// Debug visualization (read from VolumeComponent)
	// Only render if Actor is visible and Point debug mode is active
	const bool bIsActorVisibleForDebug = !IsHidden();
	if (bIsActorVisibleForDebug && IsPointDebugMode(VolumeComponent->DebugDrawMode))
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

		// Skip if no particles (no registration = ISM cleared automatically in Subsystem Tick)
		const int32 ActualParticleCount = bGPUActive ? GPUSimulator->GetParticleCount() : SimulationModule->GetParticleCount();
		if (ActualParticleCount <= 0)
		{
			CachedShadowPositions.Empty();
			CachedShadowVelocities.Empty();
			CachedNeighborCounts.Empty();
			return;
		}

		// =====================================================
		// Step 1: Get particle data (Position, Velocity, NeighborCount)
		// This data is used by both VFX and Shadow systems
		// =====================================================
		if (bGPUActive)
		{
			// Determine if readback is needed:
			// 1. ISM Shadow enabled (needs position/anisotropy data for ISM)
			// 2. SplashVFX assigned (needs velocity/neighbor data)
			// 3. Debug visualization enabled (needs particle flags for debug draw)
			UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>();
			bool bNeedShadow = RendererSubsystem &&
			                   RendererSubsystem->bEnableISMShadow &&
			                   VolumeComponent->bEnableShadow;

			// Distance-based shadow culling
			if (bNeedShadow)
			{
				// Determine effective shadow distance
				float EffectiveShadowDistance = VolumeComponent->ShadowCullDistance;
				if (EffectiveShadowDistance <= 0.0f)
				{
					// Auto: Get shadow distance from DirectionalLight
					for (TActorIterator<ADirectionalLight> It(World); It; ++It)
					{
						if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(It->GetLightComponent()))
						{
							if (Light->CastShadows)
							{
								EffectiveShadowDistance = (Light->Mobility == EComponentMobility::Movable)
									? Light->DynamicShadowDistanceMovableLight
									: Light->DynamicShadowDistanceStationaryLight;
								break;
							}
						}
					}
				}

				// Perform distance culling if we have a valid shadow distance
				if (EffectiveShadowDistance > 0.0f)
				{
					if (APlayerCameraManager* CameraManager = World->GetFirstPlayerController() ? World->GetFirstPlayerController()->PlayerCameraManager : nullptr)
					{
						const FVector CameraLocation = CameraManager->GetCameraLocation();
						const FBoxSphereBounds VolumeBounds = VolumeComponent->Bounds;

						// Distance from camera to nearest point of bounds sphere
						const float DistToBounds = FMath::Max(0.0f,
							FVector::Dist(CameraLocation, VolumeBounds.Origin) - VolumeBounds.SphereRadius);

						bNeedShadow = (DistToBounds < EffectiveShadowDistance);
					}
				}
			}
			const bool bNeedVFX = VolumeComponent->SplashVFX != nullptr;
			const bool bNeedDebug = IsPointDebugMode(VolumeComponent->DebugDrawMode);
			const bool bNeedDebugZOrder = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::Point_ZOrderArrayIndex);
			const bool bNeedReadback = bNeedShadow || bNeedVFX || bNeedDebug;
			
			// Only enable readback when actually needed (avoids GPU barrier overhead)
			GPUSimulator->SetShadowReadbackEnabled(bNeedReadback);
			GPUSimulator->SetAnisotropyReadbackEnabled(bNeedShadow); // Anisotropy only for shadow rendering
			GPUSimulator->SetDebugZOrderIndexEnabled(bNeedDebugZOrder); // Enable Z-Order index recording for debug visualization

			TArray<FVector> NewVelocities;
			TArray<FVector4> NewAnisotropyAxis1, NewAnisotropyAxis2, NewAnisotropyAxis3;

			if (bNeedReadback && GPUSimulator->HasReadyShadowPositions())
			{
				// New readback data available - update cache (with anisotropy)
				GPUSimulator->GetShadowDataWithAnisotropy(
					Positions, NewVelocities,
					NewAnisotropyAxis1, NewAnisotropyAxis2, NewAnisotropyAxis3);
				NumParticles = Positions.Num();

				if (NumParticles > 0)
				{
					// Use Move for arrays that won't be used after this (avoids deep copy)
					CachedShadowPositions = Positions;  // Keep copy - needed for RegisterShadowParticles below
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
			else if (bNeedReadback && CachedShadowPositions.Num() > 0 && CachedShadowVelocities.Num() == CachedShadowPositions.Num())
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
			else if (bNeedReadback && CachedShadowPositions.Num() > 0)
			{
				// Fallback: use cached positions without prediction
				Positions = CachedShadowPositions;
				NumParticles = Positions.Num();
			}
			// If !bNeedReadback, NumParticles remains 0 and no readback processing occurs
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
			// Step 2: ISM Shadow Integration (conditional)
			// =====================================================
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				if (RendererSubsystem->bEnableISMShadow && VolumeComponent->bEnableShadow)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE(ISM Shadow Volume)

					// Determine shadow particle radius based on mode:
					// - ISM debug mode ON (with or without shadow): use simulation radius for consistency
					// - Shadow only (no ISM debug): use RenderRadius to match visual rendering
					const bool bISMDebugMode = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM);
					float ShadowParticleRadius = ParticleRadius;  // Default: simulation radius
					if (!bISMDebugMode)
					{
						if (UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset())
						{
							ShadowParticleRadius = Preset->RenderingParameters.ParticleRenderRadius;
						}
					}

					// Register shadow particles for aggregation (will be rendered in Subsystem Tick)
					RendererSubsystem->RegisterShadowParticles(
						Positions.GetData(),
						NumParticles,
						ShadowParticleRadius,
						VolumeComponent->ShadowMeshQuality
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
	if (!VolumeComponent || !RenderingModule || !SimulationModule)
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
	// Rendering parameters are taken from Preset->RenderingParameters
	RenderingModule->Initialize(World, VolumeComponent, SimulationModule, Preset);

	// Configure ISMRenderer based on initial DebugDrawMode (like KawaiiFluidComponent)
	if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
	{
		const bool bISMMode = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM);
		ISMRenderer->SetEnabled(bISMMode);
		if (bISMMode)
		{
			ISMRenderer->SetFluidColor(VolumeComponent->ISMDebugColor);
		}
		UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: ISMRenderer %s"), *GetName(), bISMMode ? TEXT("enabled") : TEXT("disabled"));
	}

	// Configure MetaballRenderer based on preset's RenderingParameters
	// Metaball is disabled when ISM or DebugDraw mode is active (mutual exclusion)
	if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
	{
		const bool bISMMode = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM);
		const bool bPointDebugMode = IsPointDebugMode(VolumeComponent->DebugDrawMode);

		if (Preset)
		{
			// Disable Metaball if ISM or DebugDraw mode is active
			const bool bIsActorVisible = !IsHidden();
			const bool bEnableMetaball = bIsActorVisible && !bISMMode && !bPointDebugMode;
			MetaballRenderer->SetEnabled(bEnableMetaball);

			// Creates the ScreenSpace pipeline
			MetaballRenderer->UpdatePipeline();

			UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: MetaballRenderer configured (Enabled: %s, Pipeline: ScreenSpace)"),
				*GetName(),
				bEnableMetaball ? TEXT("true") : TEXT("false"));
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
	const bool bPointDebugMode = IsPointDebugMode(CurrentMode);

	if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
	{
		ISMRenderer->SetEnabled(bISMMode);
		if (bISMMode)
		{
			ISMRenderer->SetFluidColor(VolumeComponent->ISMDebugColor);
		}
		UE_LOG(LogTemp, Log, TEXT("AKawaiiFluidVolume [%s]: Editor ISMRenderer %s"),
			*GetName(), bISMMode ? TEXT("enabled") : TEXT("disabled"));
	}

	// Metaball is enabled when Actor is visible AND not in ISM/DebugDraw mode
	if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
	{
		const bool bIsActorVisible = !IsHidden();
		const bool bEnableMetaball = bIsActorVisible && !bISMMode && !bPointDebugMode;
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

	// Get particle size from Preset's ParticleRadius (simulation radius for accurate debug visualization)
	float DebugPointSize = 5.0f;  // Default fallback
	if (UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset())
	{
		DebugPointSize = Preset->ParticleRadius;
	}

	if (SimulationModule->IsGPUSimulationActive())
	{
		// GPU mode: Use lightweight cached positions (no sync readback)
		FGPUFluidSimulator* Simulator = SimulationModule->GetGPUSimulator();
		if (!Simulator)
		{
			return;
		}

		TArray<FVector3f> Positions;
		TArray<int32> ParticleIDs;
		TArray<int32> SourceIDs;
		if (!Simulator->GetParticlePositionsAndIDs(Positions, ParticleIDs, SourceIDs))
		{
			return;
		}

		const int32 TotalCount = Positions.Num();
		if (TotalCount == 0)
		{
			return;
		}

		// Get flags for Point_IsAttached debug mode
		const TArray<uint32>* Flags = Simulator->GetParticleFlags();

		// Get Z-Order array indices for Point_ZOrderArrayIndex debug mode
		TArray<int32> ZOrderIndices;
		const bool bHasZOrderIndices = (VolumeComponent->DebugDrawMode == EKawaiiFluidDebugDrawMode::Point_ZOrderArrayIndex) && 
										Simulator->GetZOrderArrayIndices(ZOrderIndices);

		// Update bounds for position-based coloring
		DebugDrawBoundsMin = FVector(Positions[0]);
		DebugDrawBoundsMax = FVector(Positions[0]);
		for (const FVector3f& Pos : Positions)
		{
			FVector P(Pos);
			DebugDrawBoundsMin = DebugDrawBoundsMin.ComponentMin(P);
			DebugDrawBoundsMax = DebugDrawBoundsMax.ComponentMax(P);
		}

		// Draw particles
		for (int32 i = 0; i < TotalCount; ++i)
		{
			FVector Pos(Positions[i]);
			const bool bNearBoundary = Flags && i < Flags->Num() && ((*Flags)[i] & EGPUParticleFlags::NearBoundary);
			
			// Get Z-Order array index if available (ParticleID → ZOrderIndex mapping)
			const int32 ZOrderIndex = (bHasZOrderIndices && ParticleIDs[i] < ZOrderIndices.Num()) ? ZOrderIndices[ParticleIDs[i]] : -1;
			
			FColor Color = ComputeDebugDrawColor(i, TotalCount, Pos, 0.0f, bNearBoundary, ZOrderIndex);
			DrawDebugPoint(World, Pos, DebugPointSize, Color, false, -1.0f, 0);
		}
	}
	else
	{
		// CPU mode: Direct particle array
		const TArray<FFluidParticle>& Particles = SimulationModule->GetParticles();
		const int32 TotalCount = Particles.Num();
		if (TotalCount == 0)
		{
			return;
		}

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

	// Allow both WorldStatic and WorldDynamic as World Collision targets
	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	
	World->OverlapMultiByObjectType(
		OverlapResults,
		VolumeCenter,
		FQuat::Identity,
		ObjectQueryParams,
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
			else
			{
				// If no IndexData, get planes from ChaosConvex
				TArray<FPlane> ChaosPlanes;
				ConvexElem.GetPlanes(ChaosPlanes);
				
				for (const FPlane& ChaosPlane : ChaosPlanes)
				{
					// ChaosPlane is in local space, so transform to world space
					const FVector LocalNormal = FVector(ChaosPlane.X, ChaosPlane.Y, ChaosPlane.Z);
					const FVector WorldNormal = ComponentTransform.TransformVectorNoScale(LocalNormal);

					// Transform a point on the plane to world space
					const FVector LocalPoint = LocalNormal * ChaosPlane.W;
					const FVector WorldPoint = ComponentTransform.TransformPosition(LocalPoint);
					
					// Add plane center as boundary point
					EditorPreviewBoundaryPositions.Add(WorldPoint);
					EditorPreviewBoundaryNormals.Add(WorldNormal);
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[StaticBoundary Editor Volume] Generated %d preview boundary particles from %d overlapping meshes"),
		EditorPreviewBoundaryPositions.Num(), OverlapResults.Num());
}
#endif

FColor AKawaiiFluidVolume::ComputeDebugDrawColor(int32 ParticleIndex, int32 TotalCount, const FVector& InPosition, float Density, bool bNearBoundary, int32 ZOrderArrayIndex) const
{
	if (!VolumeComponent)
	{
		return FColor::White;
	}

	const EKawaiiFluidDebugDrawMode DrawMode = VolumeComponent->DebugDrawMode;
	const UKawaiiFluidPresetDataAsset* Preset = VolumeComponent->GetPreset();

	switch (DrawMode)
	{
	case EKawaiiFluidDebugDrawMode::Point_ZOrderArrayIndex:
	case EKawaiiFluidDebugDrawMode::DebugDraw:  // Legacy fallthrough (default to ZOrderArrayIndex)
		{
			// Rainbow gradient based on Z-Order array index (actual post-sort index)
			// If Z-Order is provided, use it; otherwise fall back to ParticleID (ParticleIndex)
			const int32 IndexToUse = (ZOrderArrayIndex >= 0) ? ZOrderArrayIndex : ParticleIndex;
			const float T = TotalCount > 1 ? static_cast<float>(IndexToUse) / static_cast<float>(TotalCount - 1) : 0.0f;
			return FLinearColor::MakeFromHSV8(static_cast<uint8>(T * 255.0f), 255, 255).ToFColor(true);
		}

	case EKawaiiFluidDebugDrawMode::Point_ZOrderMortonCode:
		{
			// Compute Morton code from position
			FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
			float MaxRange = FMath::Max3(Range.X, Range.Y, Range.Z);
			if (MaxRange < KINDA_SMALL_NUMBER)
			{
				MaxRange = 1.0f;
			}

			FVector NormPos = (InPosition - DebugDrawBoundsMin) / MaxRange;
			NormPos.X = FMath::Clamp(NormPos.X, 0.0, 1.0);
			NormPos.Y = FMath::Clamp(NormPos.Y, 0.0, 1.0);
			NormPos.Z = FMath::Clamp(NormPos.Z, 0.0, 1.0);

			// Simple Morton-like hue (not full Morton code, but visually similar)
			float Hue = (NormPos.X * 0.33f + NormPos.Y * 0.33f + NormPos.Z * 0.33f);
			return FLinearColor::MakeFromHSV8(static_cast<uint8>(Hue * 255.0f), 255, 255).ToFColor(true);
		}

	case EKawaiiFluidDebugDrawMode::Point_PositionX:
		{
			const FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
			const float Normalized = (Range.X > KINDA_SMALL_NUMBER) ?
				(InPosition.X - DebugDrawBoundsMin.X) / Range.X : 0.0f;
			return FColor(static_cast<uint8>(FMath::Clamp(Normalized, 0.0f, 1.0f) * 255), 50, 50);
		}

	case EKawaiiFluidDebugDrawMode::Point_PositionY:
		{
			const FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
			const float Normalized = (Range.Y > KINDA_SMALL_NUMBER) ?
				(InPosition.Y - DebugDrawBoundsMin.Y) / Range.Y : 0.0f;
			return FColor(50, static_cast<uint8>(FMath::Clamp(Normalized, 0.0f, 1.0f) * 255), 50);
		}

	case EKawaiiFluidDebugDrawMode::Point_PositionZ:
		{
			const FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
			const float Normalized = (Range.Z > KINDA_SMALL_NUMBER) ?
				(InPosition.Z - DebugDrawBoundsMin.Z) / Range.Z : 0.0f;
			return FColor(50, 50, static_cast<uint8>(FMath::Clamp(Normalized, 0.0f, 1.0f) * 255));
		}

	case EKawaiiFluidDebugDrawMode::Point_Density:
		{
			// Blue (low) to Red (high) based on density
			const float RestDensity = Preset ? Preset->Density : 1000.0f;
			const float NormalizedDensity = FMath::Clamp(Density / (RestDensity * 2.0f), 0.0f, 1.0f);
			return FLinearColor::LerpUsingHSV(FLinearColor::Blue, FLinearColor::Red, NormalizedDensity).ToFColor(true);
		}

	case EKawaiiFluidDebugDrawMode::Point_IsAttached:
		{
			// Green = near boundary, Blue = free particle
			// This visualizes NEAR_BOUNDARY flag which is dynamically set by GPU BoundaryAdhesion pass
			if (bNearBoundary)
			{
				return FColor(50, 255, 50, 255);  // Bright green for near boundary
			}
			else
			{
				return FColor(50, 100, 255, 255);  // Blue for free particles
			}
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

void AKawaiiFluidVolume::RemoveParticlesInRadiusGPU(const FVector& WorldCenter, float Radius)
{
	if (!SimulationModule)
	{
		return;
	}

	// GPU-driven brush despawn: no readback dependency
	FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
	if (GPUSimulator)
	{
		GPUSimulator->AddGPUDespawnBrushRequest(FVector3f(WorldCenter), Radius);
	}
}

void AKawaiiFluidVolume::RemoveParticlesBySourceGPU(int32 SourceID)
{
	if (!SimulationModule)
	{
		return;
	}

	FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
	if (GPUSimulator)
	{
		GPUSimulator->AddGPUDespawnSourceRequest(SourceID);
	}
}

void AKawaiiFluidVolume::RemoveOldestParticlesGPU(int32 Count)
{
	if (!SimulationModule || Count <= 0)
	{
		return;
	}

	FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
	if (GPUSimulator)
	{
		GPUSimulator->AddGPUExplicitRemoveOldestRequest(Count);
	}
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
