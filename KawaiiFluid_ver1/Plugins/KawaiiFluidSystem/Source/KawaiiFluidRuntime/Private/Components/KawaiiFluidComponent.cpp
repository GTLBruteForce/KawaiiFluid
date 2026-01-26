// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Components/KawaiiFluidComponent.h"
#include "Components/KawaiiFluidVolumeComponent.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Components/FluidInteractionComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiFluidSimulationStats.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "GPU/GPUFluidSimulator.h"
#include "DrawDebugHelpers.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/OverlapResult.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

UKawaiiFluidComponent::UKawaiiFluidComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;  // Subsystem 시뮬레이션 이후 렌더링
	bTickInEditor = true;  // 에디터에서도 Tick 실행 (브러시 렌더링용)

	// 시뮬레이션 모듈 생성
	SimulationModule = CreateDefaultSubobject<UKawaiiFluidSimulationModule>(TEXT("SimulationModule"));

	// 렌더링 모듈 생성`
	RenderingModule = CreateDefaultSubobject<UKawaiiFluidRenderingModule>(TEXT("RenderingModule"));
}

#if WITH_EDITOR
void UKawaiiFluidComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Component's Preset changed - sync to SimulationModule via public API
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidComponent, Preset))
	{
		if (SimulationModule)
		{
			// Use public API to handle preset change (handles delegate rebinding internally)
			SimulationModule->OnPresetChangedExternal(Preset);
		}
	}
	
	if (UKawaiiFluidRenderingModule* RenderingMod = GetRenderingModule())
	{
		if (UKawaiiFluidMetaballRenderer* MR = RenderingMod->GetMetaballRenderer())
		{
			MR->SetPreset(Preset);
		}
	}
}
#endif

void UKawaiiFluidComponent::OnRegister()
{
	Super::OnRegister();

	UWorld* World = GetWorld();
	if (!World || !SimulationModule)
	{
		return;
	}

	// 이미 초기화된 경우 스킵 (property 변경으로 인한 re-register 대응)
	if (SimulationModule->IsInitialized())
	{
		return;
	}

	// Component->Preset 사용 (없으면 기본 생성)
	if (!Preset)
	{
		Preset = NewObject<UKawaiiFluidPresetDataAsset>(this, NAME_None, RF_Transient);
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidComponent [%s]: No Preset assigned, using default values"), *GetName());
	}

	// 시뮬레이션 모듈 초기화
	SimulationModule->Initialize(Preset);

	// 이벤트 콜백 연결
	SimulationModule->SetCollisionEventCallback(
		FOnModuleCollisionEvent::CreateUObject(this, &UKawaiiFluidComponent::HandleCollisionEvent)
	);

	// Volume 등록
	if (UKawaiiFluidVolumeComponent* Volume = GetTargetVolumeComponent())
	{
		Volume->RegisterModule(SimulationModule);
	}

	// 렌더링 모듈 초기화
	if (bEnableRendering && RenderingModule)
	{
		RenderingModule->Initialize(World, this, SimulationModule, Preset);

		// Apply debug draw settings from Component properties
		if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
		{
			ISMRenderer->bEnabled = (DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM);
			ISMRenderer->SetFluidColor(ISMDebugColor);
			ISMRenderer->SetMaxRenderParticles(ISMMaxRenderParticles);
		}
	}

	// Subsystem에 등록
	RegisterToSubsystem();

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s]: OnRegister completed"), *GetName());
}

void UKawaiiFluidComponent::OnComponentDestroyed(bool bDestroyingOK)
{
	// Volume에서 등록 해제
	if (UKawaiiFluidVolumeComponent* Volume = GetTargetVolumeComponent())
	{
		if (SimulationModule)
		{
			Volume->UnregisterModule(SimulationModule);
		}
	}

	// Subsystem에서 등록 해제
	UnregisterFromSubsystem();

	// 이벤트 클리어
	OnParticleHit.Clear();

	// 렌더링 모듈 정리
	if (RenderingModule)
	{
		RenderingModule->Cleanup();
		RenderingModule = nullptr;
	}

	// 시뮬레이션 모듈 정리
	if (SimulationModule)
	{
		SimulationModule->Shutdown();
		SimulationModule = nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s]: OnUnregister cleanup completed"), *GetName());
	
	Super::OnComponentDestroyed(bDestroyingOK);
}

void UKawaiiFluidComponent::BeginPlay()
{
	Super::BeginPlay();

	// 스폰 타이머 초기화
	SpawnAccumulatedTime = 0.0f;

	// Re-initialize rendering in PIE (PostDuplicate cleared everything)
	if (bEnableRendering && RenderingModule && SimulationModule)
	{
		UWorld* World = GetWorld();
		RenderingModule->Initialize(World, this, SimulationModule, Preset);

		// Apply debug draw settings
		bool bISMMode = (DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM);
		if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
		{
			ISMRenderer->bEnabled = bISMMode;
			if (bISMMode)
			{
				ISMRenderer->SetFluidColor(ISMDebugColor);
			}
		}

		// Ensure Metaball is disabled if ISM debug is enabled
		if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
		{
			MetaballRenderer->SetEnabled(!bISMMode);
		}
	}

	// ShapeVolume mode: auto spawn at BeginPlay
	if (SpawnSettings.IsShapeVolumeMode() && SimulationModule)
	{
		ExecuteAutoSpawn();
	}

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidComponent BeginPlay: %s"), *GetName());
}

void UKawaiiFluidComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 정리는 OnUnregister에서 처리됨 (IsBeingDestroyed 체크 후)
	Super::EndPlay(EndPlayReason);
	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidComponent EndPlay: %s"), *GetName());
}

void UKawaiiFluidComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UWorld* World = GetWorld();
	const bool bIsGameWorld = World && World->IsGameWorld();

	// Readback 요청 설정 (GPU 시뮬레이션 전에 호출 필요)
	// Debug Draw, ISM Debug View, 브러시 모드, Recycle 모드에서 readback 필요
	bool bNeedReadback = (DebugDrawMode == EKawaiiFluidDebugDrawMode::DebugDraw) ||
	                     (DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM) ||
#if WITH_EDITORONLY_DATA
	                     bBrushModeActive ||
#endif
	                     SpawnSettings.bContinuousSpawn;
	GetFluidStatsCollector().SetReadbackRequested(bNeedReadback);

#if WITH_EDITOR
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(TICKCOMPONENT_SIMULATION)
		// 에디터에서 시뮬레이션 또는 pending ops 처리
		if (!bIsGameWorld && SimulationModule && SimulationModule->GetSpatialHash())
		{
			if (UKawaiiFluidSimulationContext* Context = SimulationModule->GetSimulationContext())
			{
				// GPU 시뮬레이션 설정 (항상 GPU 사용)
				if (!Context->IsGPUSimulatorReady())
				{
					Context->InitializeGPUSimulator(Preset->MaxParticles);
				}
			if (Context->IsGPUSimulatorReady())
				{
					SimulationModule->SetGPUSimulator(Context->GetGPUSimulatorShared());
					SimulationModule->SetGPUSimulationActive(true);
				}

				if (bBrushModeActive)
				{
					// 브러시 모드: 전체 시뮬레이션 실행
					FKawaiiFluidSimulationParams Params = SimulationModule->BuildSimulationParams();
					Params.ExternalForce += SimulationModule->GetAccumulatedExternalForce();
					if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
					{
						Params.Colliders.Append(Subsystem->GetGlobalColliders());
						Params.InteractionComponents.Append(Subsystem->GetGlobalInteractionComponents());
					}

					// @TODO 이거 풀면 날라가는거 고쳐야함
					Params.bEnableStaticBoundaryParticles = false;
					Params.CollisionChannel = Preset->CollisionChannel;

					float AccumulatedTime = SimulationModule->GetAccumulatedTime();
					Context->Simulate(
						SimulationModule->GetParticlesMutable(),
						Preset,
						Params,
						*SimulationModule->GetSpatialHash(),
						DeltaTime,
						AccumulatedTime
					);
					SimulationModule->SetAccumulatedTime(AccumulatedTime);
					SimulationModule->ResetExternalForce();
				}
				else
				{
					// 브러시 모드 아님: pending spawn/despawn만 처리 (물리 시뮬 없이)
					FGPUFluidSimulator* GPUSim = Context->GetGPUSimulator();
					if (GPUSim && GPUSim->IsReady())
					{
						GPUSim->BeginFrame();
						GPUSim->EndFrame();
					}
				}
			}
		}
	}
#endif

	// Unified Simulation Bounds - 동적 파라미터 설정 및 CPU 충돌 처리
	// Volume bounds 설정은 SimulationModule에 있으며, Center/Rotation은 Component에서 동적으로 전달
	if (SimulationModule)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SimulationVolume_Setting_Collision)
		// Update volume runtime parameters (size/rotation from editor, center from component)
		// Note: Bounce/Friction parameters are now obtained from Preset internally
		UKawaiiFluidPresetDataAsset* ModulePreset = SimulationModule->GetPreset();
		const float PresetBounce = ModulePreset ? ModulePreset->Restitution : 0.0f;
		const float PresetFriction = ModulePreset ? ModulePreset->Friction : 0.5f;
		SimulationModule->SetSimulationVolume(
			SimulationModule->GetEffectiveVolumeSize(),
			SimulationModule->VolumeRotation,
			PresetBounce,
			PresetFriction
		);
		// CPU-side boundary collision (for CPU simulation mode)
		SimulationModule->ResolveVolumeBoundaryCollisions();
	}

	// Simulation Bounds Wireframe 시각화 (Containment bounds)
	// Always shown in editor mode (not PIE), always uses configured color
	// Selection is indicated by Spawn Shape wireframe turning yellow, not this
#if WITH_EDITOR
	if (SimulationModule && !bIsGameWorld && !SimulationModule->GetTargetSimulationVolume())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SimulationVolume_VISUAL)
		const FVector Center = GetComponentLocation();
		const FQuat Rotation = SimulationModule->VolumeRotation.Quaternion();

		DrawDebugBox(
			World,
			Center,
			SimulationModule->GetVolumeHalfExtent(),  // DrawDebugBox takes half-extent
			Rotation,
			SimulationModule->VolumeWireframeColor,  // Always use configured color (Green by default)
			false,  // bPersistentLines
			-1.0f,  // LifeTime (매 프레임 다시 그림)
			0,      // DepthPriority
			2.0f    // Fixed thickness
		);
	}
#endif

	// Z-Order Space Wireframe Visualization (auto-calculated internal spatial hash bounds)
	// Shows the auto-calculated Z-Order space that contains the simulation volume
	// This is always axis-aligned and may be larger than the containment volume
	// Only shown in editor mode (not PIE), disabled by default
	// Skip if external TargetSimulationVolume is set - the Volume will draw its own bounds instead
#if WITH_EDITOR
	if (SimulationModule && SimulationModule->bShowZOrderSpaceWireframe && !bIsGameWorld && !GetTargetSimulationVolume())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(ZOrderSpace_Visualization)
		// Use BoundsExtent from SimulationModule (calculated from CellSize * GridResolution)
		const float ZOrderBoundsExtent = SimulationModule->BoundsExtent;
		const FVector HalfExtent = FVector(ZOrderBoundsExtent * 0.5f);
		const FVector ComponentLocation = GetComponentLocation();

		// Use configurable color, brighten when selected
		AActor* Owner = GetOwner();
		FColor ZOrderColor = SimulationModule->ZOrderSpaceWireframeColor;
		if (Owner && Owner->IsSelected())
		{
			// Brighten the color when selected
			ZOrderColor = FColor(
				FMath::Min(255, ZOrderColor.R + 80),
				FMath::Min(255, ZOrderColor.G + 80),
				FMath::Min(255, ZOrderColor.B + 80),
				ZOrderColor.A
			);
		}

		DrawDebugBox(
			World,
			ComponentLocation,  // Center at component location
			HalfExtent,
			FQuat::Identity,  // Z-Order bounds are always axis-aligned
			ZOrderColor,
			false,  // bPersistentLines
			-1.0f,  // LifeTime (redraw each frame)
			0,      // DepthPriority
			2.0f    // Thickness
		);
	}
#endif

	// Emitter mode: continuous spawn (Stream, Spray)
	if (bIsGameWorld && SpawnSettings.IsEmitterMode())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Process_Continuous_Spawn)
		ProcessContinuousSpawn(DeltaTime);
	}

	// 렌더링 업데이트 (에디터 + 게임 모두)
	// ISM/Debug Draw 활성화 시 Metaball 비활성화 (디버그 시각화 우선)
	if (RenderingModule)
	{
		UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer();
		UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer();

		bool bISMMode = (DebugDrawMode == EKawaiiFluidDebugDrawMode::ISM);
		bool bDebugDrawMode = (DebugDrawMode == EKawaiiFluidDebugDrawMode::DebugDraw);

		// Sync debug draw settings from Component properties
		if (ISMRenderer)
		{
			bool bModeChanged = (CachedDebugDrawMode != DebugDrawMode);
			bool bColorChanged = !CachedISMDebugColor.Equals(ISMDebugColor, 0.001f);

			// Update enabled state
			if (bModeChanged)
			{
				ISMRenderer->bEnabled = bISMMode;
				CachedDebugDrawMode = DebugDrawMode;
			}

			// Update color if ISM mode and (mode changed OR color changed)
			if (bISMMode && (bModeChanged || bColorChanged))
			{
				ISMRenderer->SetFluidColor(ISMDebugColor);
				CachedISMDebugColor = ISMDebugColor;
			}

			// Update MaxRenderParticles if changed at runtime
			if (ISMRenderer->GetMaxRenderParticles() != ISMMaxRenderParticles)
			{
				ISMRenderer->SetMaxRenderParticles(ISMMaxRenderParticles);
			}
		}

		static int32 RenderLogCounter = 0;
		if (RenderLogCounter++ % 120 == 0)
		{
			UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s] Render State - ISM: %s (Enabled: %s), Metaball: %s (Enabled: %s)"),
				*GetName(),
				ISMRenderer ? TEXT("Valid") : TEXT("Null"),
				(ISMRenderer && ISMRenderer->IsEnabled()) ? TEXT("Yes") : TEXT("No"),
				MetaballRenderer ? TEXT("Valid") : TEXT("Null"),
				(MetaballRenderer && MetaballRenderer->IsEnabled()) ? TEXT("Yes") : TEXT("No"));
		}

		// Disable metaball if ISM or Debug Draw is enabled (mutually exclusive)
		if (bISMMode || bDebugDrawMode)
		{
			if (MetaballRenderer)
			{
				MetaballRenderer->SetEnabled(false);
			}
		}
		// Enable metaball if both are disabled
		else
		{
			if (MetaballRenderer)
			{
				MetaballRenderer->SetEnabled(true);
			}
		}

		RenderingModule->UpdateRenderers();
	}

	// Debug Draw: DrawDebugPoint 기반 Z-Order 시각화
	// Only render if bEnableRendering is true
	if (bEnableRendering && DebugDrawMode == EKawaiiFluidDebugDrawMode::DebugDraw)
	{
		DrawDebugParticles();
	}

	// Static Boundary Debug Draw: 벽/바닥의 boundary particle 시각화
	if (bShowStaticBoundaryParticles)
	{
		DrawDebugStaticBoundaryParticles();
	}

	// VSM Integration: Update shadow proxy with particle data
	if (SimulationModule)
	{
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(VSM Integration)
			if (RendererSubsystem->bEnableVSMIntegration && bEnableShadow)
			{
				// Use member buffer to avoid per-frame allocation
				ShadowPredictionBuffer.Reset();
				TArray<FVector>& Positions = ShadowPredictionBuffer;
				int32 NumParticles = 0;
		
				// Check if GPU simulation is active
				FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
				const bool bGPUActive = SimulationModule->IsGPUSimulationActive() && GPUSimulator != nullptr;
		
				// 파티클 수가 0이면 ISM 클리어하고 스킵
				const int32 ActualParticleCount = bGPUActive ? GPUSimulator->GetParticleCount() : SimulationModule->GetParticleCount();
				if (ActualParticleCount <= 0)
				{
					CachedShadowPositions.Empty();
					CachedShadowVelocities.Empty();
					RendererSubsystem->UpdateShadowInstances(nullptr, 0, 0.0f);
					return;
				}
		
				if (bGPUActive)
				{
					// GPU Mode: Enable async shadow readback and get positions
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

						// 각 청크의 결과를 메인 Bounds에 병합
						for (const FBox& Box : ChunkBounds)
						{
							FluidBounds += Box;
						}
					}
					else
					{
						// 파티클 수가 적으면 단일 스레드에서 처리 (오버헤드 방지)
						for (int32 i = 0; i < NumParticles; ++i)
						{
							FluidBounds += Positions[i];
						}
					}

					// Expand bounds by particle radius
					const float ParticleRadius = SimulationModule->GetParticleRadius();
					FluidBounds = FluidBounds.ExpandBy(ParticleRadius * 2.0f);

					// Update shadow proxy state (creates HISM component if needed)
					RendererSubsystem->UpdateShadowProxyState();

					// Update HISM shadow instances with uniform spheres
					// Pass MaxParticleCount for efficient instance pooling (avoids reallocations)
					RendererSubsystem->UpdateShadowInstances(
						Positions.GetData(), 
						NumParticles, 
						ParticleRadius, 
						SpawnSettings.MaxParticleCount
					);

					// Spawn splash VFX based on condition mode (with state change detection)
					if (SplashVFX)
					{
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
									GetWorld(),
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
				else
				{
					// Clear shadow instances when no particles
					RendererSubsystem->UpdateShadowInstances(nullptr, 0, 0.0f);
				}
			}
			else
			{
				// Shadow disabled (VSM off or component shadow off) - disable GPU readback
				FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
				if (GPUSimulator != nullptr)
				{
					GPUSimulator->SetShadowReadbackEnabled(false);
					GPUSimulator->SetAnisotropyReadbackEnabled(false);
				}
			}
		}
	}

#if WITH_EDITOR
	// Visualize spawn area in editor (non-game worlds only)
	if (!bIsGameWorld)
	{
		DrawSpawnAreaVisualization();
	}
#endif
}

//========================================
// Continuous Spawn
//========================================

void UKawaiiFluidComponent::ProcessContinuousSpawn(float DeltaTime)
{
	if (!SimulationModule)
	{
		return;
	}
	
	// Hexagonal Stream 모드: Hexagonal Packing 레이어 기반 스폰
	if (SpawnSettings.EmitterType == EFluidEmitterType::HexagonalStream)
	{
		float Spacing = SpawnSettings.StreamParticleSpacing;
		if (Spacing <= 0.0f && SimulationModule && SimulationModule->Preset)
		{
			Spacing = SimulationModule->Preset->SmoothingRadius * 0.5f;
		}
		if (Spacing <= 0.0f)
		{
			Spacing = 10.0f;  // fallback
		}

		const float Speed = FMath::Max(SpawnSettings.SpawnSpeed, 1.0f);
		float LayerInterval;

		if (SpawnSettings.StreamLayerMode == EStreamLayerMode::VelocityBased)
		{
			const float LayerSpacingRatio = FMath::Clamp(SpawnSettings.StreamLayerSpacingRatio, 0.2f, 1.0f);
			LayerInterval = (Spacing * LayerSpacingRatio) / Speed;
		}
		else  // FixedRate
		{
			LayerInterval = 1.0f / FMath::Max(SpawnSettings.StreamLayersPerSecond, 1.0f);
		}

		SpawnAccumulatedTime += DeltaTime;

		// 스폰할 레이어 수 계산
		int32 LayersToSpawn = 0;
		float TempAccumulatedTime = SpawnAccumulatedTime;
		while (TempAccumulatedTime >= LayerInterval)
		{
			++LayersToSpawn;
			TempAccumulatedTime -= LayerInterval;
		}

		if (LayersToSpawn == 0)
		{
			return;
		}
		
		const int32 SourceCountForStream = SimulationModule->GetParticleCountForSource(SimulationModule->GetSourceID());
		if (!SpawnSettings.bContinuousSpawn && SpawnSettings.MaxParticleCount > 0 &&
			SourceCountForStream >= 0 && SourceCountForStream >= SpawnSettings.MaxParticleCount)
		{
			return;
		}

		//========================================
		// 1. 스폰 (실제 개수 반환됨)
		//========================================
		const FQuat Rotation = GetComponentQuat();
		const FVector BaseLocation = GetComponentLocation() + Rotation.RotateVector(SpawnSettings.SpawnOffset);
		const FVector WorldDirection = Rotation.RotateVector(SpawnSettings.SpawnDirection.GetSafeNormal());
		const float ResidualTime = TempAccumulatedTime;

		// Collect all layers into single batch to minimize lock contention
		TArray<FGPUSpawnRequest> AllLayersBatch;
		const float RadiusSq = SpawnSettings.StreamRadius * SpawnSettings.StreamRadius;
		const int32 EstimatedPerLayer = FMath::CeilToInt((PI * RadiusSq) / (Spacing * Spacing));
		AllLayersBatch.Reserve(EstimatedPerLayer * LayersToSpawn);

		int32 TotalSpawned = 0;
		for (int32 i = LayersToSpawn - 1; i >= 0; --i)
		{
			float TimeOffset = static_cast<float>(i) * LayerInterval + ResidualTime;
			float PositionOffset = Speed * TimeOffset;
			FVector OffsetLocation = BaseLocation + WorldDirection * PositionOffset;

			TotalSpawned += SimulationModule->SpawnParticleDirectionalHexLayerBatch(
				OffsetLocation,
				WorldDirection,
				Speed,
				SpawnSettings.StreamRadius,
				Spacing,
				SpawnSettings.StreamJitter,
				AllLayersBatch  // Collect to batch, don't send yet
			);

			SpawnAccumulatedTime -= LayerInterval;
		}

		// Send all layers in single batch
		if (AllLayersBatch.Num() > 0)
		{
			if (FGPUFluidSimulator* GPUSim = SimulationModule->GetGPUSimulator())
			{
				GPUSim->AddSpawnRequests(AllLayersBatch);
			}
		}

		//========================================
		// 2. Recycle: 스폰 후 초과분 Despawn 요청
		//    (GPU 순서: Despawn → Spawn 이므로 정상 작동)
		//========================================
		if (SpawnSettings.bContinuousSpawn && SpawnSettings.MaxParticleCount > 0 && TotalSpawned > 0)
		{
			const int32 CurrentCount = SimulationModule->GetParticleCountForSource(SimulationModule->GetSourceID());
			// -1 = 데이터 미준비 → Recycle 스킵
			if (CurrentCount >= 0 && CurrentCount > SpawnSettings.MaxParticleCount)
			{
				const int32 ToRemove = CurrentCount - SpawnSettings.MaxParticleCount;
				SimulationModule->RemoveOldestParticles(ToRemove);
			}
		}
	}
	// Stream / Spray 모드: ParticlesPerSecond 기반 개별 스폰
	else
	{
		if (SpawnSettings.ParticlesPerSecond <= 0.0f)
		{
			return;
		}

		SpawnAccumulatedTime += DeltaTime;
		const float SpawnInterval = 1.0f / SpawnSettings.ParticlesPerSecond;

		// 스폰할 파티클 수 계산
		int32 SpawnCount = 0;
		float TempAccumulatedTime = SpawnAccumulatedTime;
		while (TempAccumulatedTime >= SpawnInterval)
		{
			++SpawnCount;
			TempAccumulatedTime -= SpawnInterval;
		}

		if (SpawnCount == 0)
		{
			return;
		}

		// 비-Recycle 모드: max 도달 시 스폰 중단 (per-source count 사용)
		// -1 = 데이터 미준비 → 스폰 허용
		const int32 SourceCountForSpray = SimulationModule->GetParticleCountForSource(SimulationModule->GetSourceID());
		if (!SpawnSettings.bContinuousSpawn && SpawnSettings.MaxParticleCount > 0 &&
			SourceCountForSpray >= 0 && SourceCountForSpray >= SpawnSettings.MaxParticleCount)
		{
			return;
		}

		//========================================
		// 1. 스폰
		//========================================
		for (int32 i = 0; i < SpawnCount; ++i)
		{
			SpawnDirectionalParticle();
			SpawnAccumulatedTime -= SpawnInterval;
		}

		//========================================
		// 2. Recycle: 스폰 후 초과분 Despawn 요청
		//========================================
		if (SpawnSettings.bContinuousSpawn && SpawnSettings.MaxParticleCount > 0)
		{
			const int32 CurrentCount = SimulationModule->GetParticleCountForSource(SimulationModule->GetSourceID());
			// -1 = 데이터 미준비 → Recycle 스킵
			if (CurrentCount >= 0 && CurrentCount > SpawnSettings.MaxParticleCount)
			{
				const int32 ToRemove = CurrentCount - SpawnSettings.MaxParticleCount;
				SimulationModule->RemoveOldestParticles(ToRemove);
			}
		}
	}
}

void UKawaiiFluidComponent::ExecuteAutoSpawn()
{
	if (!SimulationModule)
	{
		return;
	}

	// Emitter mode does not spawn at BeginPlay
	if (SpawnSettings.SpawnType == EFluidSpawnType::Emitter)
	{
		return;
	}

	// Get ParticleSpacing from Preset (auto-calculated based on SmoothingRadius)
	float ParticleSpacing = 10.0f;  // fallback
	if (SimulationModule->Preset)
	{
		ParticleSpacing = SimulationModule->Preset->ParticleSpacing;
	}

	const FQuat ComponentQuat = GetComponentQuat();
	const FVector Location = GetComponentLocation() + ComponentQuat.RotateVector(SpawnSettings.SpawnOffset);
	const FRotator Rotation = GetComponentRotation();

	if (SpawnSettings.bAutoCalculateParticleCount)
	{
		// Auto-calculate mode: use spacing-based spawn
		const bool bUseHexagonal = (SpawnSettings.GridPattern == ESpawnGridPattern::Hexagonal);

		switch (SpawnSettings.ShapeType)
		{
		case EFluidShapeType::Sphere:
			if (bUseHexagonal)
			{
				SimulationModule->SpawnParticlesSphereHexagonal(
					Location,
					SpawnSettings.SphereRadius,
					ParticleSpacing,
					SpawnSettings.bUseJitter,
					SpawnSettings.JitterAmount,
					SpawnSettings.InitialVelocity,
					Rotation
				);
			}
			else
			{
				SimulationModule->SpawnParticlesSphere(
					Location,
					SpawnSettings.SphereRadius,
					ParticleSpacing,
					SpawnSettings.bUseJitter,
					SpawnSettings.JitterAmount,
					SpawnSettings.InitialVelocity,
					Rotation
				);
			}
			break;

		case EFluidShapeType::Box:
			if (bUseHexagonal)
			{
				SimulationModule->SpawnParticlesBoxHexagonal(
					Location,
					SpawnSettings.BoxExtent,
					ParticleSpacing,
					SpawnSettings.bUseJitter,
					SpawnSettings.JitterAmount,
					SpawnSettings.InitialVelocity,
					Rotation
				);
			}
			else
			{
				SimulationModule->SpawnParticlesBox(
					Location,
					SpawnSettings.BoxExtent,
					ParticleSpacing,
					SpawnSettings.bUseJitter,
					SpawnSettings.JitterAmount,
					SpawnSettings.InitialVelocity,
					Rotation
				);
			}
			break;

		case EFluidShapeType::Cylinder:
			if (bUseHexagonal)
			{
				SimulationModule->SpawnParticlesCylinderHexagonal(
					Location,
					SpawnSettings.CylinderRadius,
					SpawnSettings.CylinderHalfHeight,
					ParticleSpacing,
					SpawnSettings.bUseJitter,
					SpawnSettings.JitterAmount,
					SpawnSettings.InitialVelocity,
					Rotation
				);
			}
			else
			{
				SimulationModule->SpawnParticlesCylinder(
					Location,
					SpawnSettings.CylinderRadius,
					SpawnSettings.CylinderHalfHeight,
					ParticleSpacing,
					SpawnSettings.bUseJitter,
					SpawnSettings.JitterAmount,
					SpawnSettings.InitialVelocity,
					Rotation
				);
			}
			break;
		}
	}
	else
	{
		// Explicit count mode: use count-based spawn
		switch (SpawnSettings.ShapeType)
		{
		case EFluidShapeType::Sphere:
			SimulationModule->SpawnParticlesSphereByCount(
				Location,
				SpawnSettings.SphereRadius,
				SpawnSettings.ParticleCount,
				SpawnSettings.bUseJitter,
				SpawnSettings.JitterAmount,
				SpawnSettings.InitialVelocity,
				Rotation
			);
			break;

		case EFluidShapeType::Box:
			SimulationModule->SpawnParticlesBoxByCount(
				Location,
				SpawnSettings.BoxExtent,
				SpawnSettings.ParticleCount,
				SpawnSettings.bUseJitter,
				SpawnSettings.JitterAmount,
				SpawnSettings.InitialVelocity,
				Rotation
			);
			break;

		case EFluidShapeType::Cylinder:
			SimulationModule->SpawnParticlesCylinderByCount(
				Location,
				SpawnSettings.CylinderRadius,
				SpawnSettings.CylinderHalfHeight,
				SpawnSettings.ParticleCount,
				SpawnSettings.bUseJitter,
				SpawnSettings.JitterAmount,
				SpawnSettings.InitialVelocity,
				Rotation
			);
			break;
		}
	}
}

void UKawaiiFluidComponent::SpawnDirectionalParticle()
{
	if (!SimulationModule)
	{
		return;
	}

	// Transform offset and direction by component rotation
	const FQuat Rotation = GetComponentQuat();
	const FVector Location = GetComponentLocation() + Rotation.RotateVector(SpawnSettings.SpawnOffset);
	const FVector WorldDirection = Rotation.RotateVector(SpawnSettings.SpawnDirection.GetSafeNormal());
	const float ConeAngle = (SpawnSettings.EmitterType == EFluidEmitterType::Spray) ? SpawnSettings.ConeAngle : 0.0f;

	SimulationModule->SpawnParticleDirectional(
		Location,
		WorldDirection,
		SpawnSettings.SpawnSpeed,
		SpawnSettings.StreamRadius,
		ConeAngle
	);
}

//========================================
// Editor Visualization
//========================================

#if WITH_EDITOR
void UKawaiiFluidComponent::DrawSpawnAreaVisualization()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// 선택 여부에 따라 색상 및 두께 변경
	const bool bIsSelected = Owner->IsSelected();

	const FQuat Rotation = GetComponentQuat();
	const FVector Location = GetComponentLocation() + Rotation.RotateVector(SpawnSettings.SpawnOffset);
	const FColor SpawnColor = bIsSelected ? FColor::Yellow : FColor::Cyan;
	const float Duration = -1.0f;  // 영구
	const uint8 DepthPriority = 0;
	const float Thickness = bIsSelected ? 3.0f : 2.0f;

	if (SpawnSettings.SpawnType == EFluidSpawnType::ShapeVolume)
	{
		// Shape Volume visualization
		switch (SpawnSettings.ShapeType)
		{
		case EFluidShapeType::Sphere:
			// Sphere는 회전에 영향받지 않음
			DrawDebugSphere(World, Location, SpawnSettings.SphereRadius, 24, SpawnColor, false, Duration, DepthPriority, Thickness);
			break;

		case EFluidShapeType::Box:
			// Box는 회전 적용
			DrawDebugBox(World, Location, SpawnSettings.BoxExtent, Rotation, SpawnColor, false, Duration, DepthPriority, Thickness);
			break;

		case EFluidShapeType::Cylinder:
			{
				const float Radius = SpawnSettings.CylinderRadius;
				const float HalfHeight = SpawnSettings.CylinderHalfHeight;

				// 로컬 좌표로 원기둥 꼭짓점 계산 후 회전 적용
				const FVector LocalTopCenter = FVector(0, 0, HalfHeight);
				const FVector LocalBottomCenter = FVector(0, 0, -HalfHeight);

				const int32 NumSegments = 24;
				for (int32 i = 0; i < NumSegments; ++i)
				{
					const float Angle1 = (float)i / NumSegments * 2.0f * PI;
					const float Angle2 = (float)(i + 1) / NumSegments * 2.0f * PI;

					// 로컬 위치 계산
					const FVector LocalTopP1 = LocalTopCenter + FVector(FMath::Cos(Angle1), FMath::Sin(Angle1), 0) * Radius;
					const FVector LocalTopP2 = LocalTopCenter + FVector(FMath::Cos(Angle2), FMath::Sin(Angle2), 0) * Radius;
					const FVector LocalBottomP1 = LocalBottomCenter + FVector(FMath::Cos(Angle1), FMath::Sin(Angle1), 0) * Radius;
					const FVector LocalBottomP2 = LocalBottomCenter + FVector(FMath::Cos(Angle2), FMath::Sin(Angle2), 0) * Radius;

					// 회전 적용 후 월드 위치로 변환
					const FVector TopP1 = Location + Rotation.RotateVector(LocalTopP1);
					const FVector TopP2 = Location + Rotation.RotateVector(LocalTopP2);
					const FVector BottomP1 = Location + Rotation.RotateVector(LocalBottomP1);
					const FVector BottomP2 = Location + Rotation.RotateVector(LocalBottomP2);

					DrawDebugLine(World, TopP1, TopP2, SpawnColor, false, Duration, DepthPriority, Thickness);
					DrawDebugLine(World, BottomP1, BottomP2, SpawnColor, false, Duration, DepthPriority, Thickness);
				}

				for (int32 i = 0; i < 4; ++i)
				{
					const float Angle = (float)i / 4 * 2.0f * PI;
					const FVector LocalTopP = LocalTopCenter + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Radius;
					const FVector LocalBottomP = LocalBottomCenter + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Radius;

					const FVector TopP = Location + Rotation.RotateVector(LocalTopP);
					const FVector BottomP = Location + Rotation.RotateVector(LocalBottomP);
					DrawDebugLine(World, TopP, BottomP, SpawnColor, false, Duration, DepthPriority, Thickness);
				}
			}
			break;
		}
	}
	else // Emitter mode
	{
		// Direction arrow (apply component rotation)
		const FVector WorldDir = Rotation.RotateVector(SpawnSettings.SpawnDirection.GetSafeNormal());
		const float ArrowLength = 100.0f;
		const FVector EndPoint = Location + WorldDir * ArrowLength;

		DrawDebugDirectionalArrow(World, Location, EndPoint, 20.0f, SpawnColor, false, Duration, DepthPriority, Thickness);

		// Stream radius circle
		if (SpawnSettings.StreamRadius > 0.0f)
		{
			FVector Right, Up;
			WorldDir.FindBestAxisVectors(Right, Up);

			const int32 NumSegments = 24;
			for (int32 i = 0; i < NumSegments; ++i)
			{
				const float Angle1 = (float)i / NumSegments * 2.0f * PI;
				const float Angle2 = (float)(i + 1) / NumSegments * 2.0f * PI;

				const FVector P1 = Location + (Right * FMath::Cos(Angle1) + Up * FMath::Sin(Angle1)) * SpawnSettings.StreamRadius;
				const FVector P2 = Location + (Right * FMath::Cos(Angle2) + Up * FMath::Sin(Angle2)) * SpawnSettings.StreamRadius;

				DrawDebugLine(World, P1, P2, SpawnColor, false, Duration, DepthPriority, Thickness);
			}
		}

		// Spray emitter: show cone
		if (SpawnSettings.EmitterType == EFluidEmitterType::Spray && SpawnSettings.ConeAngle > 0.0f)
		{
			const float ConeLength = 80.0f;
			const float HalfAngleRad = FMath::DegreesToRadians(SpawnSettings.ConeAngle * 0.5f);
			const float ConeRadius = ConeLength * FMath::Tan(HalfAngleRad);

			FVector ConeRight, ConeUp;
			WorldDir.FindBestAxisVectors(ConeRight, ConeUp);

			// Cone lines from apex to base
			const int32 NumLines = 8;
			const FVector ConeCenter = Location + WorldDir * ConeLength;

			for (int32 i = 0; i < NumLines; ++i)
			{
				const float Angle = (float)i / NumLines * 2.0f * PI;
				const FVector ConePoint = ConeCenter + (ConeRight * FMath::Cos(Angle) + ConeUp * FMath::Sin(Angle)) * ConeRadius;
				DrawDebugLine(World, Location, ConePoint, FColor::Orange, false, Duration, DepthPriority, Thickness * 0.5f);
			}

			// Cone base circle
			for (int32 i = 0; i < NumLines; ++i)
			{
				const float Angle1 = (float)i / NumLines * 2.0f * PI;
				const float Angle2 = (float)(i + 1) / NumLines * 2.0f * PI;

				const FVector P1 = ConeCenter + (ConeRight * FMath::Cos(Angle1) + ConeUp * FMath::Sin(Angle1)) * ConeRadius;
				const FVector P2 = ConeCenter + (ConeRight * FMath::Cos(Angle2) + ConeUp * FMath::Sin(Angle2)) * ConeRadius;

				DrawDebugLine(World, P1, P2, FColor::Orange, false, Duration, DepthPriority, Thickness * 0.5f);
			}
		}
	}
}
#endif

//========================================
// Event System
//========================================

void UKawaiiFluidComponent::HandleCollisionEvent(const FKawaiiFluidCollisionEvent& Event)
{
	// 테스트 로그
	UE_LOG(LogTemp, Warning, TEXT("[ParticleHit] Particle=%d, HitActor=%s, SourceComp=%s, HitIC=%s, Bone=%d, Speed=%.1f, ColliderOwnerID=%d"),
		Event.ParticleIndex,
		Event.HitActor ? *Event.HitActor->GetName() : TEXT("NULL"),
		Event.SourceComponent ? *Event.SourceComponent->GetName() : TEXT("NULL"),
		Event.HitInteractionComponent ? *Event.HitInteractionComponent->GetName() : TEXT("NULL"),
		Event.BoneIndex,
		Event.HitSpeed,
		Event.ColliderOwnerID);

	// Module에서 필터링 완료 후 호출됨 - 바로 브로드캐스트
	if (OnParticleHit.IsBound())
	{
		OnParticleHit.Broadcast(Event);
	}
}

//========================================
// Subsystem Registration
//========================================

void UKawaiiFluidComponent::RegisterToSubsystem()
{
	if (!SimulationModule)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			Subsystem->RegisterModule(SimulationModule);
		}
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->RegisterRenderingModule(RenderingModule);
		}
	}
}

void UKawaiiFluidComponent::UnregisterFromSubsystem()
{
	if (!SimulationModule)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			// Module 등록 해제
			Subsystem->UnregisterModule(SimulationModule);
		}
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->UnregisterRenderingModule(RenderingModule);
		}
	}
}

//========================================
// Simulation Volume Access (Delegated to SimulationModule)
//========================================

AKawaiiFluidVolume* UKawaiiFluidComponent::GetTargetSimulationVolume() const
{
	return SimulationModule ? SimulationModule->GetTargetSimulationVolume() : nullptr;
}

UKawaiiFluidVolumeComponent* UKawaiiFluidComponent::GetTargetVolumeComponent() const
{
	return SimulationModule ? SimulationModule->GetTargetVolumeComponent() : nullptr;
}

void UKawaiiFluidComponent::SetTargetSimulationVolume(AKawaiiFluidVolume* NewSimulationVolume)
{
	if (SimulationModule)
	{
		SimulationModule->SetTargetSimulationVolume(NewSimulationVolume);
	}
}

//========================================
// Brush API
//========================================

void UKawaiiFluidComponent::AddParticlesInRadius(const FVector& WorldCenter, float Radius,
                                                  int32 Count, const FVector& Velocity,
                                                  float Randomness, const FVector& SurfaceNormal)
{
	if (!SimulationModule || Count <= 0)
	{
		return;
	}

#if WITH_EDITOR
	// 에디터에서 데이터 수정 시 Modify() 호출 - 인스턴스 직렬화에 반영
	// 컴포넌트와 서브오브젝트 모두 마킹해야 Re-instancing 시 데이터 보존됨
	Modify();
	SimulationModule->Modify();
#endif

	// MaxParticleCount 체크 (브러시는 단순하게 - 공간 있으면 스폰, per-source count 사용)
	// -1 = 데이터 미준비 → 제한 없이 스폰 허용
	int32 ActualCount = Count;
	if (SpawnSettings.MaxParticleCount > 0)
	{
		const int32 CurrentCount = SimulationModule->GetParticleCountForSource(SimulationModule->GetSourceID());
		if (CurrentCount >= 0)  // 데이터 준비됨
		{
			const int32 Available = SpawnSettings.MaxParticleCount - CurrentCount;

			if (Available <= 0)
			{
				return;  // 공간 없음 - 스폰 안함
			}
			else if (Available < Count)
			{
				ActualCount = Available;  // 남은 공간만큼만
			}
		}
	}

	// 노말 정규화 (안전)
	const FVector Normal = SurfaceNormal.GetSafeNormal();

	for (int32 i = 0; i < ActualCount; ++i)
	{
		// 랜덤 방향 생성
		FVector RandomDir = FMath::VRand();

		// 반구 분포: 노말과 반대 방향이면 반전 (표면 위로만 생성)
		if (FVector::DotProduct(RandomDir, Normal) < 0.0f)
		{
			RandomDir = -RandomDir;
		}

		FVector RandomOffset = RandomDir * FMath::FRand() * Radius * Randomness;
		FVector SpawnPos = WorldCenter + RandomOffset;
		FVector SpawnVel = Velocity + FMath::VRand() * 20.0f * Randomness;

		SimulationModule->SpawnParticle(SpawnPos, SpawnVel);
	}
}

int32 UKawaiiFluidComponent::RemoveParticlesInRadius(const FVector& WorldCenter, float Radius)
{
	if (!SimulationModule)
	{
		return 0;
	}

#if WITH_EDITOR
	// 에디터에서 데이터 수정 시 Modify() 호출 - 인스턴스 직렬화에 반영
	// 컴포넌트와 서브오브젝트 모두 마킹해야 Re-instancing 시 데이터 보존됨
	Modify();
	SimulationModule->Modify();
#endif

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

	// Find particles within radius and collect their IDs (filtered by SourceID)
	const float RadiusSq = Radius * Radius;
	const FVector3f WorldCenterF = FVector3f(WorldCenter);
	const int32 MySourceID = SimulationModule->GetSourceID();
	TArray<int32> ParticleIDsToRemove;
	TArray<int32> AllReadbackIDs;
	ParticleIDsToRemove.Reserve(128);  // Pre-allocate for typical brush operation
	AllReadbackIDs.Reserve(ReadbackParticles.Num());

	for (const FGPUFluidParticle& Particle : ReadbackParticles)
	{
		AllReadbackIDs.Add(Particle.ParticleID);

		// 내 SourceID의 파티클만 제거 대상으로
		if (Particle.SourceID != MySourceID)
		{
			continue;
		}

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
		UE_LOG(LogTemp, Verbose, TEXT("RemoveParticlesInRadius: Found %d particles to remove by ID"), ParticleIDsToRemove.Num());
	}

	return ParticleIDsToRemove.Num();
}

void UKawaiiFluidComponent::ClearAllParticles()
{
	if (SimulationModule)
	{
		SimulationModule->ClearAllParticles();
	}

	// 캐시된 Shadow 데이터 클리어 (다음 Tick에서 다시 안 그려지게)
	CachedShadowPositions.Empty();
	CachedShadowVelocities.Empty();
	CachedNeighborCounts.Empty();
	CachedAnisotropyAxis1.Empty();
	CachedAnisotropyAxis2.Empty();
	CachedAnisotropyAxis3.Empty();
	PrevNeighborCounts.Empty();

	// 렌더링도 즉시 클리어
	if (RenderingModule)
	{
		RenderingModule->UpdateRenderers();
	}

	// Shadow ISM 클리어
	if (UWorld* World = GetWorld())
	{
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			RendererSubsystem->UpdateShadowInstances(nullptr, 0, 0.0f);
		}
	}
}

//========================================
// Debug Visualization API
//========================================

void UKawaiiFluidComponent::SetDebugDrawMode(EKawaiiFluidDebugDrawMode Mode)
{
	DebugDrawMode = Mode;

	// Reset bounds for recomputation when enabling DebugDraw mode
	if (Mode == EKawaiiFluidDebugDrawMode::DebugDraw)
	{
		DebugDrawBoundsMin = FVector::ZeroVector;
		DebugDrawBoundsMax = FVector::ZeroVector;
	}

	UE_LOG(LogTemp, Log, TEXT("Debug Draw Mode changed: %d"), (int32)Mode);
}

void UKawaiiFluidComponent::SetDebugVisualizationType(EFluidDebugVisualization Type)
{
	DebugVisualizationType = Type;

	// Reset bounds for recomputation
	DebugDrawBoundsMin = FVector::ZeroVector;
	DebugDrawBoundsMax = FVector::ZeroVector;

	UE_LOG(LogTemp, Log, TEXT("Debug Visualization Type changed: %d"), (int32)Type);
}

void UKawaiiFluidComponent::DrawDebugParticles()
{
	if (DebugDrawMode != EKawaiiFluidDebugDrawMode::DebugDraw || !SimulationModule)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Get point size from ParticleRadius (in Preset)
	float PointSize = 8.0f;  // Default fallback
	if (Preset && Preset->ParticleRadius > 0.0f)
	{
		PointSize = Preset->ParticleRadius;
	}

	// Get particle data (GPU or CPU)
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
	const int32 NumParticles = Particles.Num();

	// Auto-compute bounds if not set
	if (DebugDrawBoundsMin.IsNearlyZero() && DebugDrawBoundsMax.IsNearlyZero())
	{
		FVector MinBounds(FLT_MAX);
		FVector MaxBounds(-FLT_MAX);
		for (const FFluidParticle& P : Particles)
		{
			MinBounds = MinBounds.ComponentMin(P.Position);
			MaxBounds = MaxBounds.ComponentMax(P.Position);
		}
		DebugDrawBoundsMin = MinBounds;
		DebugDrawBoundsMax = MaxBounds;
	}

	// Verify Z-Order sorting by computing cell-based Morton codes (matching GPU shader)
	static int32 SortVerifyCounter = 0;
	if (++SortVerifyCounter % 120 == 1)  // Log every 2 seconds at 60fps
	{
		// Get CellSize from simulation module
		// CellSize = SmoothingRadius typically, and SmoothingRadius ≈ 4 * ParticleRadius
		float CellSize = 100.0f;  // Default fallback
		if (SimulationModule)
		{
			// CellSize is typically smoothing radius (≈ 4 * particle radius for SPH)
			CellSize = SimulationModule->GetParticleRadius() * 4.0f;
		}

		// Compute CELL-BASED Morton code (MUST match GPU FluidMortonCode.usf)
		auto ComputeCellBasedMortonCode = [CellSize](const FVector& Pos, const FVector& BoundsMin) -> uint32
		{
			// GPU: cellCoord = floor(pos / CellSize)
			FIntVector CellCoord(
				FMath::FloorToInt(Pos.X / CellSize),
				FMath::FloorToInt(Pos.Y / CellSize),
				FMath::FloorToInt(Pos.Z / CellSize)
			);

			// GPU: gridMin = floor(BoundsMin / CellSize)
			FIntVector GridMin(
				FMath::FloorToInt(BoundsMin.X / CellSize),
				FMath::FloorToInt(BoundsMin.Y / CellSize),
				FMath::FloorToInt(BoundsMin.Z / CellSize)
			);

			// GPU: offset = cellCoord - gridMin, clamped to [0, 1023]
			FIntVector Offset = CellCoord - GridMin;
			uint32 ux = FMath::Clamp(Offset.X, 0, 1023);
			uint32 uy = FMath::Clamp(Offset.Y, 0, 1023);
			uint32 uz = FMath::Clamp(Offset.Z, 0, 1023);

			// Morton3D expansion
			auto ExpandBits = [](uint32 v) -> uint32 {
				v = (v * 0x00010001u) & 0xFF0000FFu;
				v = (v * 0x00000101u) & 0x0F00F00Fu;
				v = (v * 0x00000011u) & 0xC30C30C3u;
				v = (v * 0x00000005u) & 0x49249249u;
				return v;
			};
			uint32 MortonCode = (ExpandBits(uz) << 2) | (ExpandBits(uy) << 1) | ExpandBits(ux);

			// GPU: cellID = mortonCode & (MAX_CELLS - 1)  // MAX_CELLS = 65536
			return MortonCode & 0xFFFF;
		};

		// Count unique CellIDs and find the largest cell
		TMap<uint32, int32> CellIDCounts;
		for (int32 i = 0; i < NumParticles; ++i)
		{
			uint32 CellID = ComputeCellBasedMortonCode(Particles[i].Position, DebugDrawBoundsMin);
			CellIDCounts.FindOrAdd(CellID, 0)++;
		}

		// Find the cell with most particles
		uint32 MaxCellID = 0;
		int32 MaxCount = 0;
		for (const auto& Pair : CellIDCounts)
		{
			if (Pair.Value > MaxCount)
			{
				MaxCount = Pair.Value;
				MaxCellID = Pair.Key;
			}
		}

		// Check Cell 0 specifically
		int32 Cell0Count = CellIDCounts.FindRef(0);

		// Log diagnostic info
		UE_LOG(LogTemp, Warning, TEXT("Z-Order CellID Analysis: TotalParticles=%d, UniqueCells=%d, LargestCell=%u has %d particles (%.1f%%), Cell0 has %d particles"),
			NumParticles, CellIDCounts.Num(), MaxCellID, MaxCount,
			(NumParticles > 0) ? (100.0f * MaxCount / NumParticles) : 0.0f,
			Cell0Count);

		// Warn if too many particles in one cell (Black Hole Cell issue!)
		if (MaxCount > NumParticles / 4)  // More than 25% in one cell
		{
			UE_LOG(LogTemp, Error, TEXT("Z-Order BLACK HOLE DETECTED! CellID %u has %d/%d particles (%.1f%%). This will cause severe performance issues!"),
				MaxCellID, MaxCount, NumParticles, 100.0f * MaxCount / NumParticles);
		}
	}

	// Draw each particle
	for (int32 i = 0; i < NumParticles; ++i)
	{
		const FFluidParticle& Particle = Particles[i];
		FColor Color = ComputeDebugDrawColor(i, NumParticles, Particle.Position, Particle.Density);

		DrawDebugPoint(World, Particle.Position, PointSize, Color, false, -1.0f, 0);
	}
}

FColor UKawaiiFluidComponent::ComputeDebugDrawColor(int32 ParticleIndex, int32 TotalCount, const FVector& Position, float Density) const
{
	switch (DebugVisualizationType)
	{
	case EFluidDebugVisualization::ZOrderArrayIndex:
	case EFluidDebugVisualization::ArrayIndex:  // Legacy
	{
		// Rainbow gradient based on array index
		// If Z-Order sorted correctly, spatially close particles should have similar colors
		float T = (float)ParticleIndex / FMath::Max(TotalCount - 1, 1);
		return FLinearColor::MakeFromHSV8((uint8)(T * 255.0f), 255, 255).ToFColor(true);
	}

	case EFluidDebugVisualization::ZOrderMortonCode:
	case EFluidDebugVisualization::MortonCode:  // Legacy
	{
		// Compute Morton code from position
		FVector Range = DebugDrawBoundsMax - DebugDrawBoundsMin;
		float MaxRange = FMath::Max3(Range.X, Range.Y, Range.Z);
		if (MaxRange < KINDA_SMALL_NUMBER) MaxRange = 1.0f;

		FVector NormPos = (Position - DebugDrawBoundsMin) / MaxRange;
		NormPos.X = FMath::Clamp(NormPos.X, 0.0, 1.0);
		NormPos.Y = FMath::Clamp(NormPos.Y, 0.0, 1.0);
		NormPos.Z = FMath::Clamp(NormPos.Z, 0.0, 1.0);

		// Simple Morton-like hue (not full Morton code, but visually similar)
		float Hue = (NormPos.X * 0.33f + NormPos.Y * 0.33f + NormPos.Z * 0.33f);
		return FLinearColor::MakeFromHSV8((uint8)(Hue * 255.0f), 255, 255).ToFColor(true);
	}

	case EFluidDebugVisualization::PositionX:
	{
		float Range = DebugDrawBoundsMax.X - DebugDrawBoundsMin.X;
		if (Range < KINDA_SMALL_NUMBER) Range = 1.0f;
		float T = FMath::Clamp((Position.X - DebugDrawBoundsMin.X) / Range, 0.0f, 1.0f);
		return FColor((uint8)(T * 255), 50, 50, 255);
	}

	case EFluidDebugVisualization::PositionY:
	{
		float Range = DebugDrawBoundsMax.Y - DebugDrawBoundsMin.Y;
		if (Range < KINDA_SMALL_NUMBER) Range = 1.0f;
		float T = FMath::Clamp((Position.Y - DebugDrawBoundsMin.Y) / Range, 0.0f, 1.0f);
		return FColor(50, (uint8)(T * 255), 50, 255);
	}

	case EFluidDebugVisualization::PositionZ:
	{
		float Range = DebugDrawBoundsMax.Z - DebugDrawBoundsMin.Z;
		if (Range < KINDA_SMALL_NUMBER) Range = 1.0f;
		float T = FMath::Clamp((Position.Z - DebugDrawBoundsMin.Z) / Range, 0.0f, 1.0f);
		return FColor(50, 50, (uint8)(T * 255), 255);
	}

	case EFluidDebugVisualization::Density:
	{
		// Blue (low) -> Green (normal) -> Red (high)
		float NormDensity = FMath::Clamp(Density / 2000.0f, 0.0f, 1.0f);
		if (NormDensity < 0.5f)
		{
			float T = NormDensity * 2.0f;
			return FColor(0, (uint8)(T * 255), (uint8)((1.0f - T) * 255), 255);
		}
		else
		{
			float T = (NormDensity - 0.5f) * 2.0f;
			return FColor((uint8)(T * 255), (uint8)((1.0f - T) * 255), 0, 255);
		}
	}

	default:
		return FColor::White;
	}
}

void UKawaiiFluidComponent::DrawDebugStaticBoundaryParticles()
{
	if (!bShowStaticBoundaryParticles)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const bool bIsGameWorld = World->IsGameWorld();

	// Game mode: GPU 시뮬레이션에서 데이터 사용
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
	// Editor mode: 에디터 미리보기 데이터 사용
	else
	{
		// 주기적으로 boundary particle 재생성 (30프레임마다)
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
			UE_LOG(LogTemp, Log, TEXT("[StaticBoundary Editor] Drawing %d boundary particles"), NumParticles);
		}
	}
#endif
}

#if WITH_EDITOR
void UKawaiiFluidComponent::GenerateEditorBoundaryParticlesPreview()
{
	EditorPreviewBoundaryPositions.Reset();
	EditorPreviewBoundaryNormals.Reset();

	UWorld* World = GetWorld();
	if (!World || !SimulationModule)
	{
		return;
	}

	// Get smoothing radius from preset
	float SmoothingRadius = 20.0f;
	if (Preset)
	{
		SmoothingRadius = Preset->SmoothingRadius;
	}
	const float Spacing = SmoothingRadius * 0.5f;

	// Get volume bounds
	const FVector VolumeCenter = GetComponentLocation();
	float BoundsExtent = SimulationModule->BoundsExtent;
	if (BoundsExtent <= 0.0f)
	{
		BoundsExtent = 500.0f;  // Default fallback
	}
	const FVector HalfExtent(BoundsExtent * 0.5f);
	const FBox VolumeBounds(VolumeCenter - HalfExtent, VolumeCenter + HalfExtent);

	// Find overlapping static mesh actors
	TArray<FOverlapResult> OverlapResults;
	FCollisionQueryParams QueryParams;
	QueryParams.bReturnPhysicalMaterial = false;
	QueryParams.AddIgnoredActor(GetOwner());

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

	// Process each overlapping static mesh
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

		// Process Convex elements (simplified - just use center point with bounding sphere)
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

	UE_LOG(LogTemp, Log, TEXT("[StaticBoundary Editor] Generated %d preview boundary particles from %d overlapping meshes"),
		EditorPreviewBoundaryPositions.Num(), OverlapResults.Num());
}
#endif

//========================================
// InstanceData (Re-instancing 시 파티클 데이터 보존)
//========================================

FKawaiiFluidComponentInstanceData::FKawaiiFluidComponentInstanceData(const UKawaiiFluidComponent* SourceComponent)
	: FActorComponentInstanceData(SourceComponent)
{
	if (SourceComponent && SourceComponent->GetSimulationModule())
	{
		// GPU 모드 시 CPU 배열로 동기화 후 저장
		SourceComponent->GetSimulationModule()->SyncGPUParticlesToCPU();

		SavedParticles = SourceComponent->GetSimulationModule()->GetParticles();

		UE_LOG(LogTemp, Log, TEXT("InstanceData: Saved %d particles from %s"),
			SavedParticles.Num(), *SourceComponent->GetName());
	}
}

void FKawaiiFluidComponentInstanceData::ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase)
{
	Super::ApplyToComponent(Component, CacheApplyPhase);

	if (CacheApplyPhase == ECacheApplyPhase::PostUserConstructionScript)
	{
		if (UKawaiiFluidComponent* FluidComponent = Cast<UKawaiiFluidComponent>(Component))
		{
			if (FluidComponent->GetSimulationModule() && SavedParticles.Num() > 0)
			{
				FluidComponent->GetSimulationModule()->GetParticlesMutable() = SavedParticles;

				// GPU 활성 시 복원된 파티클을 GPU로 업로드
				FluidComponent->GetSimulationModule()->UploadCPUParticlesToGPU();

				UE_LOG(LogTemp, Log, TEXT("InstanceData: Restored %d particles to %s"),
					SavedParticles.Num(), *FluidComponent->GetName());
			}
		}
	}
}

TStructOnScope<FActorComponentInstanceData> UKawaiiFluidComponent::GetComponentInstanceData() const
{
	// 에디터에서만 + 파티클이 있을 때만 저장
	if (GetSimulationModule() && GetSimulationModule()->GetParticleCount() > 0)
	{
		return MakeStructOnScope<FActorComponentInstanceData, FKawaiiFluidComponentInstanceData>(this);
	}

	return Super::GetComponentInstanceData();
}

//========================================
// FFluidSpawnSettings
//========================================

int32 FFluidSpawnSettings::CalculateExpectedParticleCount(float InParticleSpacing) const
{
	// Emitter mode doesn't have a predictable count
	if (SpawnType == EFluidSpawnType::Emitter)
	{
		return 0;
	}

	if (!bAutoCalculateParticleCount)
	{
		// Explicit count mode: return specified value
		return ParticleCount;
	}

	if (InParticleSpacing <= 0.0f)
	{
		return 0;
	}

	// Auto-calculate mode: calculate from shape volume and spacing
	float Volume = 0.0f;

	switch (ShapeType)
	{
	case EFluidShapeType::Sphere:
		// Sphere volume: (4/3)πr³
		Volume = (4.0f / 3.0f) * PI * FMath::Pow(SphereRadius, 3.0f);
		break;

	case EFluidShapeType::Box:
		// Box volume: 8 * Extent.X * Extent.Y * Extent.Z
		Volume = 8.0f * BoxExtent.X * BoxExtent.Y * BoxExtent.Z;
		break;

	case EFluidShapeType::Cylinder:
		// Cylinder volume: πr² * 2h
		Volume = PI * FMath::Pow(CylinderRadius, 2.0f) * CylinderHalfHeight * 2.0f;
		break;
	}

	// Volume per particle: spacing³
	const float ParticleVolume = FMath::Pow(InParticleSpacing, 3.0f);

	if (ParticleVolume <= 0.0f)
	{
		return 0;
	}

	return FMath::CeilToInt(Volume / ParticleVolume);
}
