// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/KawaiiFluidComponent.h"
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

void UKawaiiFluidComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITOR
	// 에디터 모드에서 렌더링 모듈 초기화 (PIE가 아닐 때만)
	UWorld* World = GetWorld();
	if (World && !World->IsGameWorld() && bEnableRendering && RenderingModule && SimulationModule)
	{
		// Component->Preset 사용 (없으면 기본 생성)
		if (!Preset)
		{
			Preset = NewObject<UKawaiiFluidPresetDataAsset>(this, NAME_None, RF_Transient);
		}
		// Module에 Preset 설정 후 초기화
		SimulationModule->Initialize(Preset);

		// SourceID 설정 (에디터 모드) - 충돌 피드백에서 파티클 소속 식별용
		SimulationModule->SetSourceID(GetUniqueID());

		// RenderingModule 초기화 (Preset 포함)
		RenderingModule->Initialize(World, this, SimulationModule, Preset);

		// ISM 설정 적용
		if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
		{
			ISMRenderer->ApplySettings(ISMSettings);
		}
		
		RegisterToSubsystem();
	}
#endif
}

void UKawaiiFluidComponent::OnUnregister()
{
#if WITH_EDITOR
	// 에디터 모드에서 정리
	UnregisterFromSubsystem();
#endif

	Super::OnUnregister();
}

void UKawaiiFluidComponent::BeginPlay()
{
	Super::BeginPlay();

	// 시뮬레이션 모듈 초기화
	if (SimulationModule)
	{
		// Component->Preset 사용 (없으면 기본 생성)
		if (!Preset)
		{
			Preset = NewObject<UKawaiiFluidPresetDataAsset>(this, NAME_None, RF_Transient);
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidComponent [%s]: No Preset assigned, using default values"), *GetName());
		}
		// Module에 Preset 설정 후 초기화
		SimulationModule->Initialize(Preset);

		// SourceID 설정 - 충돌 피드백에서 파티클 소속 식별용
		SimulationModule->SetSourceID(GetUniqueID());

		// 이벤트 콜백 항상 연결 (Module에서 bEnableCollisionEvents 체크)
		SimulationModule->SetCollisionEventCallback(
			FOnModuleCollisionEvent::CreateUObject(this, &UKawaiiFluidComponent::HandleCollisionEvent)
		);
	}

	// 렌더링 모듈 초기화 (중복 초기화 방지)
	if (bEnableRendering && RenderingModule && SimulationModule)
	{
		if (RenderingModule->IsInitialized())
		{
			// 이미 초기화됨 (에디터에서 복제된 경우 등)
			UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s]: RenderingModule already initialized, skipping"), *GetName());
		}
		else
		{
			// 1. RenderingModule 초기화 (Preset 포함)
			RenderingModule->Initialize(GetWorld(), this, SimulationModule, Preset);

			// 2. ISM 렌더러 설정 적용
			if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
			{
				ISMRenderer->ApplySettings(ISMSettings);
			}

			UE_LOG(LogTemp, Log, TEXT("KawaiiFluidComponent [%s]: Rendering initialized (ISM: %s, Metaball: from Preset)"),
				*GetName(),
				ISMSettings.bEnabled ? TEXT("Enabled") : TEXT("Disabled"));
		}
	}

	// Module을 Subsystem에 등록 (Component가 아닌 Module!)
	RegisterToSubsystem();

	// ShapeVolume mode: auto spawn at BeginPlay
	if (SpawnSettings.IsShapeVolumeMode() && SimulationModule)
	{
		ExecuteAutoSpawn();
	}

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidComponent BeginPlay: %s"), *GetName());
}

void UKawaiiFluidComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Subsystem에서 등록 해제
	UnregisterFromSubsystem();

	// 이벤트 클리어
	OnParticleHit.Clear();

	// 렌더링 모듈 정리
	if (RenderingModule)
	{
		// RenderingModule 정리
		RenderingModule->Cleanup();
		RenderingModule = nullptr;
	}
	// 시뮬레이션 모듈 정리
	if (SimulationModule)
	{
		SimulationModule->Shutdown();
		SimulationModule = nullptr;
	}

	Super::EndPlay(EndPlayReason);
	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidComponent EndPlay: %s"), *GetName());
}

void UKawaiiFluidComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UWorld* World = GetWorld();
	const bool bIsGameWorld = World && World->IsGameWorld();

#if WITH_EDITOR
	// 브러시 모드에서 에디터 시뮬레이션 실행
	if (!bIsGameWorld && bBrushModeActive && SimulationModule && SimulationModule->GetSpatialHash())
	{
		if (UKawaiiFluidSimulationContext* Context = SimulationModule->GetSimulationContext())
		{
			FKawaiiFluidSimulationParams Params = SimulationModule->BuildSimulationParams();
			Params.ExternalForce += SimulationModule->GetAccumulatedExternalForce();
			if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
			{
				Params.Colliders.Append(Subsystem->GetGlobalColliders());
				Params.InteractionComponents.Append(Subsystem->GetGlobalInteractionComponents());
			}

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
	}
#endif

	// Containment 설정 및 충돌 처리 (시뮬레이션 후에 적용)
	// Containment 프로퍼티는 SimulationModule에 있음
	if (SimulationModule && SimulationModule->bEnableContainment)
	{
		// Center와 Rotation은 동적으로 설정 (다른 값들은 SimulationModule의 UPROPERTY)
		SimulationModule->SetContainment(
			SimulationModule->bEnableContainment,
			GetComponentLocation(),
			SimulationModule->ContainmentExtent,
			GetComponentQuat(),  // Component의 회전 전달
			SimulationModule->ContainmentRestitution,
			SimulationModule->ContainmentFriction
		);
		SimulationModule->ResolveContainmentCollisions();
	}

	// Containment Wireframe 시각화
	if (SimulationModule && SimulationModule->bEnableContainment && SimulationModule->bShowContainmentWireframe)
	{
		const FVector Center = GetComponentLocation();
		const FQuat Rotation = GetComponentQuat();
		DrawDebugBox(
			World,
			Center,
			SimulationModule->ContainmentExtent,
			Rotation,  // Component의 회전 적용
			SimulationModule->ContainmentWireframeColor,
			false,  // bPersistentLines
			-1.0f,  // LifeTime (매 프레임 다시 그림)
			0,      // DepthPriority
			2.0f    // Thickness
		);
	}

	// Emitter mode: continuous spawn (Stream, Spray)
	if (bIsGameWorld && SpawnSettings.IsEmitterMode())
	{
		ProcessContinuousSpawn(DeltaTime);
	}

	// 렌더링 업데이트 (에디터 + 게임 모두)
	// Debug Draw 활성화 시 Metaball 비활성화 (디버그 포인트가 보이도록)
	if (RenderingModule)
	{
		if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
		{
			// Debug Draw 상태에 따라 Metaball 활성화/비활성화
			MetaballRenderer->SetEnabled(!bEnableDebugDraw);
		}
		RenderingModule->UpdateRenderers();
	}

	// Debug Draw: DrawDebugPoint 기반 Z-Order 시각화
	// GPU 모드에서 readback 필요 - Stats Collector에 요청
	GetFluidStatsCollector().SetDebugReadbackRequested(bEnableDebugDraw);
	if (bEnableDebugDraw)
	{
		DrawDebugParticles();
	}

	// VSM Integration: Update shadow proxy with particle data
	if (SimulationModule)
	{
		if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
		{
			if (RendererSubsystem->bEnableVSMIntegration)
			{
				// Get particle positions
				const TArray<FFluidParticle>& Particles = SimulationModule->GetParticles();
				const int32 NumParticles = Particles.Num();

				if (NumParticles > 0)
				{
					// Calculate fluid bounds from particles
					FBox FluidBounds(ForceInit);
					TArray<FVector> Positions;
					Positions.SetNum(NumParticles);

					for (int32 i = 0; i < NumParticles; ++i)
					{
						Positions[i] = Particles[i].Position;
						FluidBounds += Particles[i].Position;
					}

					// Expand bounds by particle radius
					const float ParticleRadius = SimulationModule->GetParticleRadius();
					FluidBounds = FluidBounds.ExpandBy(ParticleRadius * 2.0f);

					// Update shadow proxy state (creates HISM if needed)
					RendererSubsystem->UpdateShadowProxyState();

					// Update HISM shadow instances from particle positions
					RendererSubsystem->UpdateShadowInstances(Positions.GetData(), NumParticles);

					// Legacy: Update density grid (for future use)
					RendererSubsystem->UpdateDensityGrid(Positions.GetData(), NumParticles, FluidBounds);
				}
				else
				{
					// 파티클 0개일 때 shadow ISM 클리어
					RendererSubsystem->UpdateShadowInstances(nullptr, 0);
				}
			}
		}
	}

#if WITH_EDITOR
	// 에디터에서 스폰 영역 시각화 (게임 월드가 아닐 때만)
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

	// 최대 파티클 수 체크
	if (SpawnSettings.MaxParticleCount > 0 && SimulationModule->GetParticleCount() >= SpawnSettings.MaxParticleCount)
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
			// Velocity-based: LayerInterval = (Spacing * LayerSpacingRatio) / Speed
			// LayerSpacingRatio가 작을수록 레이어가 촘촘하게 스폰되어 연속적인 스트림 형성
			// 1.0 = 레이어 간격이 파티클 간격과 동일 (끊겨 보일 수 있음)
			// 0.5 = 레이어가 2배 촘촘하게 (연속적인 모양)
			const float LayerSpacingRatio = FMath::Clamp(SpawnSettings.StreamLayerSpacingRatio, 0.2f, 1.0f);
			LayerInterval = (Spacing * LayerSpacingRatio) / Speed;
		}
		else  // FixedRate
		{
			// 고정 레이어/초 모드
			LayerInterval = 1.0f / FMath::Max(SpawnSettings.StreamLayersPerSecond, 1.0f);
		}

		SpawnAccumulatedTime += DeltaTime;

		// 연속적인 스트림을 위해 여러 레이어 스폰 시 위치 오프셋 적용
		// 각 레이어는 스폰 시점 차이만큼 유체 진행 방향으로 오프셋
		const FQuat Rotation = GetComponentQuat();
		const FVector BaseLocation = GetComponentLocation() + Rotation.RotateVector(SpawnSettings.SpawnOffset);
		const FVector WorldDirection = Rotation.RotateVector(SpawnSettings.SpawnDirection.GetSafeNormal());

		// 스폰해야 할 레이어 수 계산
		int32 LayersToSpawn = 0;
		float TempAccumulatedTime = SpawnAccumulatedTime;
		while (TempAccumulatedTime >= LayerInterval)
		{
			++LayersToSpawn;
			TempAccumulatedTime -= LayerInterval;
		}

		// 잔여 시간: 이전 프레임의 마지막 스폰 이후 경과한 추가 시간
		// 이 시간만큼 이전 레이어가 더 이동했으므로, 새 레이어들도 그만큼 오프셋 필요
		const float ResidualTime = TempAccumulatedTime;

		// 각 레이어를 시간 오프셋에 맞는 위치에 스폰
		// 가장 먼저 스폰됐어야 할 레이어(가장 멀리 이동한)부터 순서대로
		for (int32 i = LayersToSpawn - 1; i >= 0; --i)
		{
			// i번째 레이어는 i * LayerInterval 전에 스폰됐어야 함
			// 그동안 Speed * time만큼 이동했을 것
			// i = LayersToSpawn-1 (oldest): 가장 멀리 이동
			// i = 0 (newest, now): 이동 없음
			// 추가로 잔여 시간만큼 모든 레이어를 오프셋하여 프레임 간 연속성 보장
			float TimeOffset = static_cast<float>(i) * LayerInterval + ResidualTime;
			float PositionOffset = Speed * TimeOffset;

			// 유체 진행 방향으로 오프셋된 위치에서 스폰
			FVector OffsetLocation = BaseLocation + WorldDirection * PositionOffset;

			SimulationModule->SpawnParticleDirectionalHexLayer(
				OffsetLocation,
				WorldDirection,
				Speed,
				SpawnSettings.StreamRadius,
				Spacing,
				SpawnSettings.StreamJitter
			);

			SpawnAccumulatedTime -= LayerInterval;

			// 최대 파티클 수 체크
			if (SpawnSettings.MaxParticleCount > 0 && SimulationModule->GetParticleCount() >= SpawnSettings.MaxParticleCount)
			{
				SpawnAccumulatedTime = 0.0f;
				break;
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

		while (SpawnAccumulatedTime >= SpawnInterval)
		{
			SpawnDirectionalParticle();
			SpawnAccumulatedTime -= SpawnInterval;

			// 최대 파티클 수 체크
			if (SpawnSettings.MaxParticleCount > 0 && SimulationModule->GetParticleCount() >= SpawnSettings.MaxParticleCount)
			{
				SpawnAccumulatedTime = 0.0f;
				break;
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
		switch (SpawnSettings.ShapeType)
		{
		case EFluidShapeType::Sphere:
			SimulationModule->SpawnParticlesSphere(
				Location,
				SpawnSettings.SphereRadius,
				ParticleSpacing,
				SpawnSettings.bUseJitter,
				SpawnSettings.JitterAmount,
				SpawnSettings.InitialVelocity,
				Rotation
			);
			break;

		case EFluidShapeType::Box:
			SimulationModule->SpawnParticlesBox(
				Location,
				SpawnSettings.BoxExtent,
				ParticleSpacing,
				SpawnSettings.bUseJitter,
				SpawnSettings.JitterAmount,
				SpawnSettings.InitialVelocity,
				Rotation
			);
			break;

		case EFluidShapeType::Cylinder:
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

	// 액터가 선택된 상태에서만 시각화 표시
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->IsSelected())
	{
		return;
	}

	const FQuat Rotation = GetComponentQuat();
	const FVector Location = GetComponentLocation() + Rotation.RotateVector(SpawnSettings.SpawnOffset);
	const FColor SpawnColor = FColor::Cyan;
	const float Duration = -1.0f;  // 영구
	const uint8 DepthPriority = 0;
	const float Thickness = 2.0f;

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
// Brush API
//========================================

void UKawaiiFluidComponent::AddParticlesInRadius(const FVector& WorldCenter, float Radius,
                                                  int32 Count, const FVector& Velocity,
                                                  float Randomness, const FVector& SurfaceNormal)
{
	if (!SimulationModule)
	{
		return;
	}

#if WITH_EDITOR
	// 에디터에서 데이터 수정 시 Modify() 호출 - 인스턴스 직렬화에 반영
	// 컴포넌트와 서브오브젝트 모두 마킹해야 Re-instancing 시 데이터 보존됨
	Modify();
	SimulationModule->Modify();
#endif

	// 노말 정규화 (안전)
	const FVector Normal = SurfaceNormal.GetSafeNormal();

	for (int32 i = 0; i < Count; ++i)
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

	float RadiusSq = Radius * Radius;

	TArray<FFluidParticle>& Particles = SimulationModule->GetParticlesMutable();
	int32 RemovedCount = 0;

	for (int32 i = Particles.Num() - 1; i >= 0; --i)
	{
		if (FVector::DistSquared(Particles[i].Position, WorldCenter) <= RadiusSq)
		{
			Particles.RemoveAtSwap(i);
			++RemovedCount;
		}
	}

	return RemovedCount;
}

void UKawaiiFluidComponent::ClearAllParticles()
{
	if (SimulationModule)
	{
		SimulationModule->ClearAllParticles();
	}

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
			RendererSubsystem->UpdateShadowInstances(nullptr, 0);
		}
	}
}

//========================================
// Debug Visualization (Z-Order Sorting)
//========================================

void UKawaiiFluidComponent::SetDebugVisualization(EFluidDebugVisualization Mode)
{
	// Debug visualization now uses DrawDebugPoint system
	if (Mode != EFluidDebugVisualization::None)
	{
		EnableDebugDraw(Mode, DebugPointSize);
	}
	else
	{
		DisableDebugDraw();
	}
}

EFluidDebugVisualization UKawaiiFluidComponent::GetDebugVisualization() const
{
	// Debug visualization now uses DrawDebugPoint system
	return bEnableDebugDraw ? DebugDrawMode : EFluidDebugVisualization::None;
}

//========================================
// DrawDebugPoint Visualization
//========================================

void UKawaiiFluidComponent::EnableDebugDraw(EFluidDebugVisualization Mode, float PointSize)
{
	bEnableDebugDraw = true;
	DebugDrawMode = Mode;
	DebugPointSize = PointSize;

	// Reset bounds for recomputation
	DebugDrawBoundsMin = FVector::ZeroVector;
	DebugDrawBoundsMax = FVector::ZeroVector;

	UE_LOG(LogTemp, Log, TEXT("Debug Draw enabled: Mode=%d, PointSize=%.1f"), (int32)Mode, PointSize);
}

void UKawaiiFluidComponent::DisableDebugDraw()
{
	bEnableDebugDraw = false;
	UE_LOG(LogTemp, Log, TEXT("Debug Draw disabled"));
}

void UKawaiiFluidComponent::DrawDebugParticles()
{
	if (!bEnableDebugDraw || !SimulationModule)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
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

		DrawDebugPoint(World, Particle.Position, DebugPointSize, Color, false, -1.0f, 0);
	}
}

FColor UKawaiiFluidComponent::ComputeDebugDrawColor(int32 ParticleIndex, int32 TotalCount, const FVector& Position, float Density) const
{
	switch (DebugDrawMode)
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

//========================================
// InstanceData (Re-instancing 시 파티클 데이터 보존)
//========================================

FKawaiiFluidComponentInstanceData::FKawaiiFluidComponentInstanceData(const UKawaiiFluidComponent* SourceComponent)
	: FActorComponentInstanceData(SourceComponent)
{
	if (SourceComponent && SourceComponent->GetSimulationModule())
	{
		SavedParticles = SourceComponent->GetSimulationModule()->GetParticles();
		SavedNextParticleID = SourceComponent->GetSimulationModule()->GetNextParticleID();

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
				FluidComponent->GetSimulationModule()->SetNextParticleID(SavedNextParticleID);

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
