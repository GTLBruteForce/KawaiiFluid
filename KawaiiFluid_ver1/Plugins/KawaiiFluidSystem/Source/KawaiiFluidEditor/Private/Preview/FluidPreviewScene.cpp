// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Preview/FluidPreviewScene.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Core/FluidParticle.h"
#include "Core/SpatialHash.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Core/KawaiiFluidSimulationStats.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "GPU/GPUFluidSimulator.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

FFluidPreviewScene::FFluidPreviewScene(FPreviewScene::ConstructionValues CVS)
	: FAdvancedPreviewScene(CVS)
	, CurrentPreset(nullptr)
	, PreviewSettingsObject(nullptr)
	, SimulationContext(nullptr)
	, SimulationModule(nullptr)
	, SpawnAccumulatedTime(0.0f)
	, TotalSimulationTime(0.0f)
	, bSimulationActive(false)
	, RenderingModule(nullptr)
	, PreviewActor(nullptr)
	, FloorMeshComponent(nullptr)
	, CachedParticleRadius(5.0f)
{
	// Create preview settings object
	PreviewSettingsObject = NewObject<UFluidPreviewSettingsObject>(GetTransientPackage(), NAME_None, RF_Transient);

	// Create simulation context (physics solver)
	// GPU simulator will be initialized in SetPreset() with preset's MaxParticles
	SimulationContext = NewObject<UKawaiiFluidSimulationContext>(GetTransientPackage(), NAME_None, RF_Transient);
	SimulationContext->InitializeRenderResource();

	// Create simulation module (uses same spawn logic as runtime)
	SimulationModule = NewObject<UKawaiiFluidSimulationModule>(GetTransientPackage(), NAME_None, RF_Transient);
	SimulationModule->SetSourceID(0);  // Preview uses fixed source ID

	// Create visualization components (including PreviewActor)
	CreateVisualizationComponents();

	// Create rendering module
	RenderingModule = NewObject<UKawaiiFluidRenderingModule>(GetTransientPackage(), NAME_None, RF_Transient);

	if (RenderingModule && PreviewActor)
	{
		// Initialize with this as DataProvider (ISM attached to PreviewActor's RootComponent)
		// Preset will be set later in SetPreset()
		RenderingModule->Initialize(GetWorld(), PreviewActor->GetRootComponent(), this, nullptr);

		// Metaball settings come from Preset->RenderingParameters (set in SetPreset)

		// Connect MetaballRenderer to SimulationContext for batched rendering
		if (UKawaiiFluidMetaballRenderer* MR = RenderingModule->GetMetaballRenderer())
		{
			MR->SetSimulationContext(SimulationContext);
		}

		// Register to FluidRendererSubsystem (required for Metaball ViewExtension!)
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				RendererSubsystem->RegisterRenderingModule(RenderingModule);
			}
		}
	}

	// Apply default rendering settings
	ApplyPreviewSettings();

	// Setup environment
	SetupFloor();
}

FFluidPreviewScene::~FFluidPreviewScene()
{
	// Unregister from FluidRendererSubsystem first
	if (RenderingModule)
	{
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				RendererSubsystem->UnregisterRenderingModule(RenderingModule);
			}
		}
		RenderingModule->Cleanup();
	}

	// Release GPU simulator
	if (SimulationContext)
	{
		SimulationContext->ReleaseGPUSimulator();
	}

	if (PreviewActor && PreviewActor->IsValidLowLevel())
	{
		PreviewActor->Destroy();
		PreviewActor = nullptr;
	}
}

void FFluidPreviewScene::AddReferencedObjects(FReferenceCollector& Collector)
{
	FAdvancedPreviewScene::AddReferencedObjects(Collector);

	Collector.AddReferencedObject(CurrentPreset);
	Collector.AddReferencedObject(PreviewSettingsObject);
	Collector.AddReferencedObject(SimulationContext);
	Collector.AddReferencedObject(SimulationModule);
	Collector.AddReferencedObject(RenderingModule);
	Collector.AddReferencedObject(PreviewActor);
	Collector.AddReferencedObject(FloorMeshComponent);
	Collector.AddReferencedObjects(WallMeshComponents);
}

void FFluidPreviewScene::CreateVisualizationComponents()
{
	// Spawn preview actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	PreviewActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!PreviewActor)
	{
		return;
	}

	// Create root component
	USceneComponent* RootComp = NewObject<USceneComponent>(PreviewActor, TEXT("Root"));
	PreviewActor->SetRootComponent(RootComp);
	RootComp->RegisterComponent();

	// Create floor mesh component
	UMaterial* BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	FloorMeshComponent = NewObject<UStaticMeshComponent>(PreviewActor, TEXT("FloorMesh"));
	FloorMeshComponent->SetupAttachment(RootComp);

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh)
	{
		FloorMeshComponent->SetStaticMesh(CubeMesh);
	}

	// Create floor material
	if (BaseMaterial)
	{
		UMaterialInstanceDynamic* FloorMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, FloorMeshComponent);
		FloorMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.3f, 0.3f, 0.35f, 1.0f));
		FloorMeshComponent->SetMaterial(0, FloorMaterial);
	}

	FloorMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FloorMeshComponent->RegisterComponent();

	AddComponent(FloorMeshComponent, FTransform::Identity);
}

void FFluidPreviewScene::SetPreset(UKawaiiFluidPresetDataAsset* InPreset)
{
	CurrentPreset = InPreset;

	if (CurrentPreset)
	{
		// Initialize GPU simulator with fixed buffer size
		if (SimulationContext)
		{
			SimulationContext->InitializeGPUSimulator(FFluidPreviewSettings::GPUBufferSize);
			SimulationContext->InitializeSolvers(CurrentPreset);
			SimulationContext->SetCachedPreset(CurrentPreset);
		}

		// Initialize simulation module with preset and GPU simulator
		if (SimulationModule)
		{
			SimulationModule->Initialize(CurrentPreset);
			SimulationModule->SetGPUSimulator(SimulationContext->GetGPUSimulatorShared());
			SimulationModule->SetGPUSimulationActive(true);
		}

		CachedParticleRadius = CurrentPreset->ParticleRadius;

		// Update renderers from preset
		if (RenderingModule)
		{
			// Metaball uses Preset->RenderingParameters via SetPreset
			if (UKawaiiFluidMetaballRenderer* Metaball = RenderingModule->GetMetaballRenderer())
			{
				Metaball->SetPreset(CurrentPreset);
			}
		}

		// Apply ISM settings from PreviewSettings
		ApplyPreviewSettings();
	}

	// Reset simulation
	ResetSimulation();
}

void FFluidPreviewScene::RefreshFromPreset()
{
	if (!CurrentPreset)
	{
		return;
	}

	// Ensure GPU simulator is initialized
	if (SimulationContext)
	{
		// Ensure GPU simulator exists with fixed buffer size
		if (!SimulationContext->IsGPUSimulatorReady())
		{
			SimulationContext->InitializeGPUSimulator(FFluidPreviewSettings::GPUBufferSize);
		}

		SimulationContext->InitializeSolvers(CurrentPreset);
		SimulationContext->SetCachedPreset(CurrentPreset);
	}

	// Update simulation module preset
	if (SimulationModule)
	{
		SimulationModule->SetPreset(CurrentPreset);
	}

	CachedParticleRadius = CurrentPreset->ParticleRadius;

	// Update renderers from preset
	if (RenderingModule)
	{
		// Metaball uses Preset->RenderingParameters via SetPreset
		if (UKawaiiFluidMetaballRenderer* Metaball = RenderingModule->GetMetaballRenderer())
		{
			Metaball->SetPreset(CurrentPreset);
		}
	}

	// Apply ISM settings from PreviewSettings
	ApplyPreviewSettings();
}

void FFluidPreviewScene::StartSimulation()
{
	bSimulationActive = true;
}

void FFluidPreviewScene::StopSimulation()
{
	bSimulationActive = false;
}

void FFluidPreviewScene::ResetSimulation()
{
	// Clear GPU particles
	if (SimulationContext)
	{
		FGPUFluidSimulator* GPUSimulator = SimulationContext->GetGPUSimulator();
		if (GPUSimulator)
		{
			GPUSimulator->ClearAllParticles();
		}
	}
	SpawnAccumulatedTime = 0.0f;
	TotalSimulationTime = 0.0f;

	// Fill mode: spawn particles immediately on reset (like EmitterComponent::SpawnFill)
	if (SimulationModule && CurrentPreset && PreviewSettingsObject)
	{
		const FFluidPreviewSettings& Settings = PreviewSettingsObject->Settings;

		if (Settings.IsFillMode())
		{
			const FVector SpawnCenter = Settings.PreviewSpawnOffset;
			const float Spacing = CurrentPreset->ParticleSpacing;
			const FVector Velocity = Settings.InitialVelocityDirection.GetSafeNormal() * Settings.InitialSpeed;

			// Use SimulationModule's hexagonal spawn functions (same as EmitterComponent)
			switch (Settings.ShapeType)
			{
			case EPreviewEmitterShapeType::Sphere:
				SimulationModule->SpawnParticlesSphereHexagonal(
					SpawnCenter, Settings.SphereRadius, Spacing,
					true, Settings.JitterAmount, Velocity);
				break;

			case EPreviewEmitterShapeType::Cube:
				SimulationModule->SpawnParticlesBoxHexagonal(
					SpawnCenter, Settings.CubeHalfSize, Spacing,
					true, Settings.JitterAmount, Velocity);
				break;

			case EPreviewEmitterShapeType::Cylinder:
				SimulationModule->SpawnParticlesCylinderHexagonal(
					SpawnCenter, Settings.CylinderRadius, Settings.CylinderHalfHeight, Spacing,
					true, Settings.JitterAmount, Velocity);
				break;
			}
		}
	}
}

void FFluidPreviewScene::SpawnParticles(float DeltaTime)
{
	if (!SimulationModule || !CurrentPreset || !PreviewSettingsObject)
	{
		return;
	}

	FGPUFluidSimulator* GPUSimulator = SimulationModule->GetGPUSimulator();
	if (!GPUSimulator)
	{
		return;
	}

	const FFluidPreviewSettings& Settings = PreviewSettingsObject->Settings;

	// Only process Stream mode (Fill mode spawns on reset)
	if (!Settings.IsStreamMode())
	{
		return;
	}

	// Check max particle count (skip if Recycle mode - let recycle handle overflow)
	if (Settings.MaxParticleCount > 0 && !Settings.bContinuousSpawn)
	{
		const int32 CurrentCount = GPUSimulator->GetParticleCount() + GPUSimulator->GetPendingSpawnCount();
		if (CurrentCount >= Settings.MaxParticleCount)
		{
			return;
		}
	}

	// === Stream mode: velocity-based layer spawning (matches EmitterComponent::ProcessStreamEmitter) ===

	// Calculate effective spacing
	float EffectiveSpacing = CurrentPreset->SmoothingRadius * 0.5f;
	if (EffectiveSpacing <= 0.0f)
	{
		EffectiveSpacing = 10.0f;
	}

	const float LayerSpacing = EffectiveSpacing * Settings.StreamLayerSpacingRatio;

	// Velocity-based layer spawning (accumulate distance traveled)
	const float DistanceThisFrame = Settings.InitialSpeed * DeltaTime;
	SpawnAccumulatedTime += DistanceThisFrame;  // Reuse as LayerDistanceAccumulator

	if (SpawnAccumulatedTime < LayerSpacing)
	{
		return;  // Not enough distance accumulated for a layer
	}

	// Calculate number of layers to spawn and residual distance
	const int32 LayerCount = FMath::FloorToInt(SpawnAccumulatedTime / LayerSpacing);
	const float ResidualDistance = FMath::Fmod(SpawnAccumulatedTime, LayerSpacing);

	if (LayerCount <= 0)
	{
		return;
	}

	// === Direction calculation ===
	const FVector BaseLocation = Settings.PreviewSpawnOffset;
	const FVector VelocityDir = Settings.InitialVelocityDirection.GetSafeNormal();
	const FVector OffsetDir = VelocityDir;  // Offset along velocity direction

	// === Spawn layers with position offset (reverse order - oldest first) ===
	// Like EmitterComponent: apply position offset to each layer to prevent overlap
	for (int32 i = LayerCount - 1; i >= 0; --i)
	{
		// Calculate position offset for each layer
		// i = LayerCount-1: oldest layer (farthest from spawn point)
		// i = 0: newest layer (closest to spawn point)
		const float PositionOffset = static_cast<float>(i) * LayerSpacing + ResidualDistance;
		const FVector OffsetLocation = BaseLocation + OffsetDir * PositionOffset;

		SimulationModule->SpawnParticleDirectionalHexLayer(
			OffsetLocation,
			VelocityDir,
			Settings.InitialSpeed,
			Settings.StreamRadius,
			EffectiveSpacing,
			Settings.StreamJitter
		);
	}

	// Update accumulator with residual distance
	SpawnAccumulatedTime = ResidualDistance;

	// Recycle (Stream mode only): After spawning, remove oldest particles if over MaxParticleCount
	if (Settings.bContinuousSpawn && Settings.MaxParticleCount > 0)
	{
		const int32 CurrentCount = GPUSimulator->GetParticleCount();
		if (CurrentCount > Settings.MaxParticleCount)
		{
			const int32 ToRemove = CurrentCount - Settings.MaxParticleCount;
			SimulationModule->RemoveOldestParticlesForSource(0, ToRemove);
		}
	}
}

void FFluidPreviewScene::TickSimulation(float DeltaTime)
{
	if (!bSimulationActive || !CurrentPreset || !SimulationContext)
	{
		return;
	}

	FGPUFluidSimulator* GPUSimulator = SimulationContext->GetGPUSimulator();
	if (!GPUSimulator)
	{
		return;
	}

	// Accumulate simulation time
	TotalSimulationTime += DeltaTime;

	// Request stats readback for density display in preview stats overlay
	GetFluidStatsCollector().SetReadbackRequested(true);

	// Spawn new particles (GPU spawn requests)
	SpawnParticles(DeltaTime);

	// Build simulation params
	FKawaiiFluidSimulationParams Params;
	Params.World = GetWorld();
	Params.bUseWorldCollision = false;
	Params.ParticleRadius = CurrentPreset->ParticleRadius;

	// Set simulation bounds for GPU collision (floor at Z=0)
	const FVector BoundsMin(-500.0, -500.0, 0.0);
	const FVector BoundsMax(500.0, 500.0, 500.0);
	Params.WorldBounds = FBox(BoundsMin, BoundsMax);
	GPUSimulator->SetSimulationBounds(FVector3f(BoundsMin), FVector3f(BoundsMax));

	// Run GPU simulation
	static TArray<FFluidParticle> DummyParticles;  // GPU mode doesn't use CPU particles
	static FSpatialHash DummySpatialHash(CurrentPreset->SmoothingRadius);
	float AccumulatedTime = 0.0f;

	SimulationContext->Simulate(
		DummyParticles,
		CurrentPreset,
		Params,
		DummySpatialHash,
		DeltaTime,
		AccumulatedTime
	);

	// Update rendering module
	if (RenderingModule)
	{
		RenderingModule->UpdateRenderers();
	}
}

void FFluidPreviewScene::HandleFloorCollision()
{
	// Floor collision is handled by GPU bounds collision
	// See TickSimulation() -> Params.WorldBounds
}

FFluidPreviewSettings& FFluidPreviewScene::GetPreviewSettings()
{
	return PreviewSettingsObject->Settings;
}

void FFluidPreviewScene::ApplyPreviewSettings()
{
	UpdateEnvironment();

	// Metaball settings come from Preset->RenderingParameters (set in SetPreset/RefreshFromPreset)
}

void FFluidPreviewScene::SetupFloor()
{
	UpdateEnvironment();
}

void FFluidPreviewScene::UpdateEnvironment()
{
	if (!FloorMeshComponent)
	{
		return;
	}

	// Floor always visible with fixed size
	const FVector FloorSize(500.0f, 500.0f, 10.0f);
	const float FloorHeight = 0.0f;

	FloorMeshComponent->SetVisibility(true);
	FVector FloorScale = FloorSize / 100.0f; // Cube is 100 units
	FloorMeshComponent->SetWorldLocation(FVector(0.0f, 0.0f, FloorHeight - FloorSize.Z * 0.5f));
	FloorMeshComponent->SetWorldScale3D(FloorScale);
}

//========================================
// IKawaiiFluidDataProvider Interface
//========================================

const TArray<FFluidParticle>& FFluidPreviewScene::GetParticles() const
{
	// GPU mode - no CPU particles available
	static TArray<FFluidParticle> EmptyArray;
	return EmptyArray;
}

int32 FFluidPreviewScene::GetParticleCount() const
{
	// Return GPU particle count
	return GetGPUParticleCount();
}

float FFluidPreviewScene::GetParticleRadius() const
{
	return CachedParticleRadius;
}

bool FFluidPreviewScene::IsDataValid() const
{
	// Valid if GPU simulation is active (particles may be 0 but still valid for rendering)
	return IsGPUSimulationActive();
}

//========================================
// Particle Access (GPU mode - limited access)
//========================================

TArray<FFluidParticle>& FFluidPreviewScene::GetParticlesMutable()
{
	// GPU mode - no mutable CPU particles available
	static TArray<FFluidParticle> EmptyArray;
	return EmptyArray;
}

float FFluidPreviewScene::GetSimulationTime() const
{
	return TotalSimulationTime;
}

float FFluidPreviewScene::GetAverageDensity() const
{
	// Read density from GPU readback data
	if (!SimulationContext)
	{
		return 0.0f;
	}

	FGPUFluidSimulator* GPUSimulator = SimulationContext->GetGPUSimulator();
	if (!GPUSimulator)
	{
		return 0.0f;
	}

	// Get readback GPU particles
	TArray<FGPUFluidParticle> GPUParticles;
	if (!GPUSimulator->GetReadbackGPUParticles(GPUParticles) || GPUParticles.Num() == 0)
	{
		return 0.0f;
	}

	// Calculate average density
	double TotalDensity = 0.0;
	for (const FGPUFluidParticle& Particle : GPUParticles)
	{
		TotalDensity += Particle.Density;
	}

	return static_cast<float>(TotalDensity / GPUParticles.Num());
}

//========================================
// GPU Simulation Interface
//========================================

bool FFluidPreviewScene::IsGPUSimulationActive() const
{
	return SimulationContext && SimulationContext->IsGPUSimulatorReady();
}

int32 FFluidPreviewScene::GetGPUParticleCount() const
{
	if (SimulationContext)
	{
		FGPUFluidSimulator* GPUSimulator = SimulationContext->GetGPUSimulator();
		if (GPUSimulator)
		{
			return GPUSimulator->GetParticleCount();
		}
	}
	return 0;
}

FGPUFluidSimulator* FFluidPreviewScene::GetGPUSimulator() const
{
	return SimulationContext ? SimulationContext->GetGPUSimulator() : nullptr;
}
