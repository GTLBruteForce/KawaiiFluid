// Copyright Epic Games, Inc. All Rights Reserved.

#include "Preview/FluidPreviewScene.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Core/FluidParticle.h"
#include "Core/SpatialHash.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

FFluidPreviewScene::FFluidPreviewScene(FPreviewScene::ConstructionValues CVS)
	: FAdvancedPreviewScene(CVS)
	, CurrentPreset(nullptr)
	, PreviewSettingsObject(nullptr)
	, AccumulatedTime(0.0f)
	, SpawnAccumulator(0.0f)
	, bSimulationActive(false)
	, PreviewActor(nullptr)
	, ParticleMeshComponent(nullptr)
	, FloorMeshComponent(nullptr)
	, CachedParticleRadius(5.0f)
{
	// Create preview settings object
	PreviewSettingsObject = NewObject<UFluidPreviewSettingsObject>(GetTransientPackage(), NAME_None, RF_Transient);

	// Create spatial hash
	SpatialHash = MakeShared<FSpatialHash>(30.0f);

	// Create simulation context
	SimulationContext = NewObject<UKawaiiFluidSimulationContext>(GetTransientPackage(), NAME_None, RF_Transient);

	// Create visualization components
	CreateVisualizationComponents();

	// Setup environment
	SetupFloor();
}

FFluidPreviewScene::~FFluidPreviewScene()
{
	// Clean up
	Particles.Empty();

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
	Collector.AddReferencedObject(PreviewActor);
	Collector.AddReferencedObject(ParticleMeshComponent);
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

	// Create particle ISM component
	ParticleMeshComponent = NewObject<UInstancedStaticMeshComponent>(PreviewActor, TEXT("ParticleMesh"));
	ParticleMeshComponent->SetupAttachment(RootComp);

	// Use engine sphere mesh
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh)
	{
		ParticleMeshComponent->SetStaticMesh(SphereMesh);
	}

	// Create dynamic material for particles
	UMaterial* BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BaseMaterial)
	{
		UMaterialInstanceDynamic* ParticleMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, ParticleMeshComponent);
		ParticleMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.2f, 0.5f, 1.0f, 0.8f));
		ParticleMeshComponent->SetMaterial(0, ParticleMaterial);
	}

	ParticleMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ParticleMeshComponent->SetCastShadow(false);
	ParticleMeshComponent->RegisterComponent();

	// Create floor mesh component
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

	AddComponent(ParticleMeshComponent, FTransform::Identity);
	AddComponent(FloorMeshComponent, FTransform::Identity);
}

void FFluidPreviewScene::SetPreset(UKawaiiFluidPresetDataAsset* InPreset)
{
	CurrentPreset = InPreset;

	if (CurrentPreset)
	{
		// Update particle visuals (color)
		if (ParticleMeshComponent)
		{
			UMaterialInstanceDynamic* ParticleMaterial = Cast<UMaterialInstanceDynamic>(ParticleMeshComponent->GetMaterial(0));
			if (ParticleMaterial)
			{
				ParticleMaterial->SetVectorParameterValue(TEXT("Color"), CurrentPreset->Color);
			}
		}

		// Initialize solvers
		SimulationContext->InitializeSolvers(CurrentPreset);

		// Update spatial hash cell size
		SpatialHash->SetCellSize(CurrentPreset->SmoothingRadius);

		CachedParticleRadius = CurrentPreset->ParticleRadius;
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

	// Update particle color
	if (ParticleMeshComponent)
	{
		UMaterialInstanceDynamic* ParticleMaterial = Cast<UMaterialInstanceDynamic>(ParticleMeshComponent->GetMaterial(0));
		if (ParticleMaterial)
		{
			ParticleMaterial->SetVectorParameterValue(TEXT("Color"), CurrentPreset->Color);
		}
	}

	// Update spatial hash cell size if changed
	if (SpatialHash.IsValid() && FMath::Abs(SpatialHash->GetCellSize() - CurrentPreset->SmoothingRadius) > KINDA_SMALL_NUMBER)
	{
		SpatialHash->SetCellSize(CurrentPreset->SmoothingRadius);
	}

	CachedParticleRadius = CurrentPreset->ParticleRadius;

	// Update particle visualization scale
	UpdateParticleVisuals();
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
	// Clear existing particles
	Particles.Empty();
	AccumulatedTime = 0.0f;
	SpawnAccumulator = 0.0f;

	// Clear spatial hash
	if (SpatialHash.IsValid())
	{
		SpatialHash->Clear();
	}

	// Update visuals
	UpdateParticleVisuals();
}

void FFluidPreviewScene::ContinuousSpawn(float DeltaTime)
{
	const FFluidPreviewSettings& Settings = PreviewSettingsObject->Settings;

	// Check if we've reached max particle count
	if (Particles.Num() >= Settings.MaxParticleCount)
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
	const int32 RemainingCapacity = Settings.MaxParticleCount - Particles.Num();
	const int32 ActualSpawnCount = FMath::Min(ParticlesToSpawn, RemainingCapacity);

	// Get the next particle ID
	int32 NextParticleID = Particles.Num();

	for (int32 i = 0; i < ActualSpawnCount; ++i)
	{
		// Random position within sphere
		FVector RandomOffset = FMath::VRand() * FMath::FRand() * SpawnRadius;
		FVector Position = SpawnLocation + RandomOffset;

		FFluidParticle Particle(Position, NextParticleID + i);
		Particle.Velocity = SpawnVelocity;
		Particle.Mass = CurrentPreset ? CurrentPreset->ParticleMass : 1.0f;

		Particles.Add(Particle);
	}
}

void FFluidPreviewScene::TickSimulation(float DeltaTime)
{
	if (!bSimulationActive || !CurrentPreset)
	{
		return;
	}

	// Continuous spawn new particles
	ContinuousSpawn(DeltaTime);

	// Skip simulation if no particles yet
	if (Particles.Num() == 0 || !SimulationContext || !SpatialHash.IsValid())
	{
		return;
	}

	// Build simulation params
	FKawaiiFluidSimulationParams Params;
	Params.ExternalForce = CurrentPreset->Gravity;
	Params.World = GetWorld();
	Params.bUseWorldCollision = false; // We handle collision manually in preview
	Params.ParticleRadius = CurrentPreset->ParticleRadius;

	// Run simulation
	SimulationContext->Simulate(
		Particles,
		CurrentPreset,
		Params,
		*SpatialHash,
		DeltaTime,
		AccumulatedTime
	);

	// Handle floor collision manually
	HandleFloorCollision();

	// Handle wall collision if enabled
	if (PreviewSettingsObject->Settings.bShowWalls)
	{
		HandleWallCollision();
	}

	// Update visuals
	UpdateParticleVisuals();
}

void FFluidPreviewScene::HandleFloorCollision()
{
	if (!PreviewSettingsObject->Settings.bShowFloor)
	{
		return;
	}

	const float FloorY = PreviewSettingsObject->Settings.FloorHeight;
	const float ParticleRadius = CurrentPreset ? CurrentPreset->ParticleRadius : 5.0f;
	const float Restitution = CurrentPreset ? CurrentPreset->Restitution : 0.0f;
	const float Friction = CurrentPreset ? CurrentPreset->Friction : 0.5f;

	for (FFluidParticle& Particle : Particles)
	{
		const float MinZ = FloorY + ParticleRadius;

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

void FFluidPreviewScene::HandleWallCollision()
{
	const FFluidPreviewSettings& Settings = PreviewSettingsObject->Settings;
	const float HalfSize = Settings.FloorSize.X * 0.5f;
	const float ParticleRadius = CurrentPreset ? CurrentPreset->ParticleRadius : 5.0f;
	const float Restitution = CurrentPreset ? CurrentPreset->Restitution : 0.0f;

	for (FFluidParticle& Particle : Particles)
	{
		// X boundaries
		if (Particle.Position.X < -HalfSize + ParticleRadius)
		{
			Particle.Position.X = -HalfSize + ParticleRadius;
			Particle.PredictedPosition.X = Particle.Position.X;
			Particle.Velocity.X *= -Restitution;
		}
		else if (Particle.Position.X > HalfSize - ParticleRadius)
		{
			Particle.Position.X = HalfSize - ParticleRadius;
			Particle.PredictedPosition.X = Particle.Position.X;
			Particle.Velocity.X *= -Restitution;
		}

		// Y boundaries
		if (Particle.Position.Y < -HalfSize + ParticleRadius)
		{
			Particle.Position.Y = -HalfSize + ParticleRadius;
			Particle.PredictedPosition.Y = Particle.Position.Y;
			Particle.Velocity.Y *= -Restitution;
		}
		else if (Particle.Position.Y > HalfSize - ParticleRadius)
		{
			Particle.Position.Y = HalfSize - ParticleRadius;
			Particle.PredictedPosition.Y = Particle.Position.Y;
			Particle.Velocity.Y *= -Restitution;
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

void FFluidPreviewScene::SetupWalls()
{
	// Wall setup is handled in UpdateEnvironment
	UpdateEnvironment();
}

void FFluidPreviewScene::UpdateEnvironment()
{
	if (!FloorMeshComponent)
	{
		return;
	}

	const FFluidPreviewSettings& Settings = PreviewSettingsObject->Settings;

	// Update floor visibility and transform
	FloorMeshComponent->SetVisibility(Settings.bShowFloor);

	if (Settings.bShowFloor)
	{
		FVector FloorScale = Settings.FloorSize / 100.0f; // Cube is 100 units
		FloorMeshComponent->SetWorldLocation(FVector(0.0f, 0.0f, Settings.FloorHeight - Settings.FloorSize.Z * 0.5f));
		FloorMeshComponent->SetWorldScale3D(FloorScale);
	}

	// TODO: Update wall meshes if needed
}

void FFluidPreviewScene::UpdateParticleVisuals()
{
	if (!ParticleMeshComponent)
	{
		return;
	}

	const int32 NumParticles = Particles.Num();

	// Adjust instance count
	const int32 CurrentInstanceCount = ParticleMeshComponent->GetInstanceCount();
	if (CurrentInstanceCount != NumParticles)
	{
		ParticleMeshComponent->ClearInstances();

		TArray<FTransform> Transforms;
		Transforms.SetNum(NumParticles);

		for (int32 i = 0; i < NumParticles; ++i)
		{
			Transforms[i] = FTransform::Identity;
		}

		ParticleMeshComponent->AddInstances(Transforms, false);
	}

	// Update instance transforms
	const float ParticleScale = CachedParticleRadius / 50.0f; // Sphere is 100 units diameter

	for (int32 i = 0; i < NumParticles; ++i)
	{
		const FFluidParticle& Particle = Particles[i];
		FTransform InstanceTransform(
			FRotator::ZeroRotator,
			Particle.Position,
			FVector(ParticleScale)
		);
		ParticleMeshComponent->UpdateInstanceTransform(i, InstanceTransform, false, false, true);
	}

	ParticleMeshComponent->MarkRenderStateDirty();
}

float FFluidPreviewScene::GetAverageDensity() const
{
	if (Particles.Num() == 0)
	{
		return 0.0f;
	}

	float TotalDensity = 0.0f;
	for (const FFluidParticle& Particle : Particles)
	{
		TotalDensity += Particle.Density;
	}

	return TotalDensity / Particles.Num();
}
