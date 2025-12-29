// Copyright Epic Games, Inc. All Rights Reserved.

#include "Preview/FluidPreviewScene.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Core/FluidParticle.h"
#include "Core/SpatialHash.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/KawaiiFluidSSFRRenderer.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

FFluidPreviewScene::FFluidPreviewScene(FPreviewScene::ConstructionValues CVS)
	: FAdvancedPreviewScene(CVS)
	, CurrentPreset(nullptr)
	, PreviewSettingsObject(nullptr)
	, SimulationModule(nullptr)
	, SimulationContext(nullptr)
	, SpawnAccumulator(0.0f)
	, bSimulationActive(false)
	, RenderingModule(nullptr)
	, CurrentRenderMode(EFluidPreviewRenderMode::SSFR)
	, PreviewActor(nullptr)
	, FloorMeshComponent(nullptr)
	, CachedParticleRadius(5.0f)
{
	// Create preview settings object
	PreviewSettingsObject = NewObject<UFluidPreviewSettingsObject>(GetTransientPackage(), NAME_None, RF_Transient);

	// Create simulation module (owns particles, SpatialHash, etc.)
	SimulationModule = NewObject<UKawaiiFluidSimulationModule>(GetTransientPackage(), NAME_None, RF_Transient);

	// Create simulation context (physics solver)
	SimulationContext = NewObject<UKawaiiFluidSimulationContext>(GetTransientPackage(), NAME_None, RF_Transient);

	// Create visualization components (including PreviewActor)
	CreateVisualizationComponents();

	// Create rendering module (same as runtime!)
	RenderingModule = NewObject<UKawaiiFluidRenderingModule>(GetTransientPackage(), NAME_None, RF_Transient);
	if (RenderingModule && PreviewActor)
	{
		// Initialize with this as DataProvider (IKawaiiFluidDataProvider)
		RenderingModule->Initialize(GetWorld(), PreviewActor, this);

		// Apply default settings to renderers (same as runtime!)
		if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
		{
			FKawaiiFluidISMRendererSettings ISMSettings;
			ISMSettings.bEnabled = true;
			ISMRenderer->ApplySettings(ISMSettings);
		}

		if (UKawaiiFluidSSFRRenderer* SSFRRenderer = RenderingModule->GetSSFRRenderer())
		{
			FKawaiiFluidSSFRRendererSettings SSFRSettings;
			SSFRSettings.bEnabled = true;
			SSFRRenderer->ApplySettings(SSFRSettings);
		}

		// Register to FluidRendererSubsystem (required for SSFR ViewExtension!)
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				RendererSubsystem->RegisterRenderingModule(RenderingModule);
			}
		}
	}

	// Set default render mode
	SetRenderMode(EFluidPreviewRenderMode::SSFR);

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

	// Clean up simulation module
	if (SimulationModule)
	{
		SimulationModule->Shutdown();
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
	Collector.AddReferencedObject(SimulationModule);
	Collector.AddReferencedObject(SimulationContext);
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
		// Initialize simulation module with preset
		if (SimulationModule)
		{
			SimulationModule->Initialize(CurrentPreset);
		}

		// Initialize solvers in context
		if (SimulationContext)
		{
			SimulationContext->InitializeSolvers(CurrentPreset);
		}

		CachedParticleRadius = CurrentPreset->ParticleRadius;

		// Update rendering module with preset settings
		if (RenderingModule)
		{
			if (UKawaiiFluidISMRenderer* ISM = RenderingModule->GetISMRenderer())
			{
				ISM->ParticleScale = CachedParticleRadius / 5.0f;
				ISM->SetFluidColor(CurrentPreset->Color);
			}
			if (UKawaiiFluidSSFRRenderer* SSFR = RenderingModule->GetSSFRRenderer())
			{
				SSFR->LocalParameters.FluidColor = CurrentPreset->Color;
			}
		}
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

	// Update simulation module preset (handles SpatialHash internally)
	if (SimulationModule)
	{
		SimulationModule->SetPreset(CurrentPreset);
	}

	CachedParticleRadius = CurrentPreset->ParticleRadius;

	// Update rendering module with preset settings
	if (RenderingModule)
	{
		if (UKawaiiFluidISMRenderer* ISM = RenderingModule->GetISMRenderer())
		{
			ISM->ParticleScale = CachedParticleRadius / 5.0f;
			ISM->SetFluidColor(CurrentPreset->Color);
		}
		if (UKawaiiFluidSSFRRenderer* SSFR = RenderingModule->GetSSFRRenderer())
		{
			SSFR->LocalParameters.FluidColor = CurrentPreset->Color;
		}
	}
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
	// Clear existing particles via SimulationModule
	if (SimulationModule)
	{
		SimulationModule->ClearAllParticles();
		SimulationModule->SetAccumulatedTime(0.0f);
	}
	SpawnAccumulator = 0.0f;
}

void FFluidPreviewScene::ContinuousSpawn(float DeltaTime)
{
	if (!SimulationModule)
	{
		return;
	}

	const FFluidPreviewSettings& Settings = PreviewSettingsObject->Settings;

	// Check if we've reached max particle count
	if (SimulationModule->GetParticleCount() >= Settings.MaxParticleCount)
	{
		return;
	}

	// Accumulate spawn time
	SpawnAccumulator += DeltaTime * Settings.ParticlesPerSecond;

	// Spawn particles based on accumulator
	const int32 ParticlesToSpawn = FMath::FloorToInt(SpawnAccumulator);
	if (ParticlesToSpawn <= 0)
	{
		return;
	}

	// Subtract spawned particles from accumulator
	SpawnAccumulator -= ParticlesToSpawn;

	const FVector SpawnLocation = Settings.SpawnLocation;
	const FVector SpawnVelocity = Settings.SpawnVelocity;
	const float SpawnRadius = Settings.SpawnRadius;

	// Calculate how many we can actually spawn
	const int32 RemainingCapacity = Settings.MaxParticleCount - SimulationModule->GetParticleCount();
	const int32 ActualSpawnCount = FMath::Min(ParticlesToSpawn, RemainingCapacity);

	// Spawn particles via SimulationModule
	for (int32 i = 0; i < ActualSpawnCount; ++i)
	{
		// Random position within sphere
		FVector RandomOffset = FMath::VRand() * FMath::FRand() * SpawnRadius;
		FVector Position = SpawnLocation + RandomOffset;

		SimulationModule->SpawnParticle(Position, SpawnVelocity);
	}
}

void FFluidPreviewScene::TickSimulation(float DeltaTime)
{
	if (!bSimulationActive || !CurrentPreset || !SimulationModule)
	{
		return;
	}

	// Continuous spawn new particles
	ContinuousSpawn(DeltaTime);

	// Skip simulation if no particles yet
	FSpatialHash* SpatialHash = SimulationModule->GetSpatialHash();
	if (SimulationModule->GetParticleCount() == 0 || !SimulationContext || !SpatialHash)
	{
		return;
	}

	// Build simulation params
	FKawaiiFluidSimulationParams Params;
	Params.ExternalForce = CurrentPreset->Gravity;
	Params.World = GetWorld();
	Params.bUseWorldCollision = false; // We handle collision manually in preview
	Params.ParticleRadius = CurrentPreset->ParticleRadius;

	// Get accumulated time from module
	float AccumulatedTime = SimulationModule->GetAccumulatedTime();

	// Run simulation using module's particle array
	SimulationContext->Simulate(
		SimulationModule->GetParticlesMutable(),
		CurrentPreset,
		Params,
		*SpatialHash,
		DeltaTime,
		AccumulatedTime
	);

	// Update accumulated time back to module
	SimulationModule->SetAccumulatedTime(AccumulatedTime);

	// Handle floor collision manually
	HandleFloorCollision();

	// Update rendering module (SSFR or ISM based on mode)
	if (RenderingModule)
	{
		RenderingModule->UpdateRenderers();
	}
}

void FFluidPreviewScene::HandleFloorCollision()
{
	if (!SimulationModule)
	{
		return;
	}

	constexpr float FloorZ = 0.0f;
	const float ParticleRadius = CurrentPreset ? CurrentPreset->ParticleRadius : 5.0f;
	const float Restitution = CurrentPreset ? CurrentPreset->Restitution : 0.0f;
	const float Friction = CurrentPreset ? CurrentPreset->Friction : 0.5f;

	for (FFluidParticle& Particle : SimulationModule->GetParticlesMutable())
	{
		const float MinZ = FloorZ + ParticleRadius;

		if (Particle.Position.Z < MinZ)
		{
			Particle.Position.Z = MinZ;
			Particle.PredictedPosition.Z = MinZ;

			// Apply friction and restitution
			if (Particle.Velocity.Z < 0)
			{
				Particle.Velocity.Z *= -Restitution;

				// Apply friction to horizontal velocity
				FVector HorizontalVel(Particle.Velocity.X, Particle.Velocity.Y, 0.0f);
				HorizontalVel *= (1.0f - Friction);
				Particle.Velocity.X = HorizontalVel.X;
				Particle.Velocity.Y = HorizontalVel.Y;
			}
		}
	}
}

FFluidPreviewSettings& FFluidPreviewScene::GetPreviewSettings()
{
	return PreviewSettingsObject->Settings;
}

void FFluidPreviewScene::ApplyPreviewSettings()
{
	UpdateEnvironment();
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
	static TArray<FFluidParticle> EmptyArray;
	if (SimulationModule)
	{
		return SimulationModule->GetParticles();
	}
	return EmptyArray;
}

int32 FFluidPreviewScene::GetParticleCount() const
{
	return SimulationModule ? SimulationModule->GetParticleCount() : 0;
}

float FFluidPreviewScene::GetParticleRadius() const
{
	return CachedParticleRadius;
}

bool FFluidPreviewScene::IsDataValid() const
{
	return SimulationModule != nullptr && SimulationModule->GetParticleCount() > 0;
}

//========================================
// Render Mode Control
//========================================

void FFluidPreviewScene::SetRenderMode(EFluidPreviewRenderMode NewMode)
{
	CurrentRenderMode = NewMode;

	// Update rendering module mode
	if (RenderingModule)
	{
		const bool bUseSSFR = (NewMode == EFluidPreviewRenderMode::SSFR);
		if (UKawaiiFluidISMRenderer* ISM = RenderingModule->GetISMRenderer())
		{
			ISM->SetEnabled(!bUseSSFR);
		}
		if (UKawaiiFluidSSFRRenderer* SSFR = RenderingModule->GetSSFRRenderer())
		{
			SSFR->SetEnabled(bUseSSFR);
		}
	}
}

//========================================
// Particle Access (via SimulationModule)
//========================================

TArray<FFluidParticle>& FFluidPreviewScene::GetParticlesMutable()
{
	static TArray<FFluidParticle> EmptyArray;
	if (SimulationModule)
	{
		return SimulationModule->GetParticlesMutable();
	}
	return EmptyArray;
}

float FFluidPreviewScene::GetSimulationTime() const
{
	return SimulationModule ? SimulationModule->GetAccumulatedTime() : 0.0f;
}

float FFluidPreviewScene::GetAverageDensity() const
{
	if (!SimulationModule || SimulationModule->GetParticleCount() == 0)
	{
		return 0.0f;
	}

	const TArray<FFluidParticle>& Particles = SimulationModule->GetParticles();
	float TotalDensity = 0.0f;
	for (const FFluidParticle& Particle : Particles)
	{
		TotalDensity += Particle.Density;
	}

	return TotalDensity / Particles.Num();
}
