// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Core/FluidParticle.h"

UKawaiiFluidISMRenderer::UKawaiiFluidISMRenderer()
{
	// No component tick needed - UObject doesn't tick
}

void UKawaiiFluidISMRenderer::Initialize(UWorld* InWorld, AActor* InOwner)
{
	CachedWorld = InWorld;
	CachedOwner = InOwner;

	if (!CachedWorld)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer::Initialize - No world context provided"));
	}

	if (!CachedOwner)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer::Initialize - No owner actor provided"));
	}

	InitializeISM();

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidISMRenderer: Initialized (Mesh: %s, MaxParticles: %d)"),
		ISMComponent && ISMComponent->GetStaticMesh() ? *ISMComponent->GetStaticMesh()->GetName() : TEXT("None"),
		MaxRenderParticles);
}

void UKawaiiFluidISMRenderer::Cleanup()
{
	if (ISMComponent)
	{
		ISMComponent->ClearInstances();
		ISMComponent->DestroyComponent(); // Unregister and destroy
		ISMComponent = nullptr;
	}

	// Clear cached references
	CachedWorld = nullptr;
	CachedOwner = nullptr;
	bEnabled = false;
}

void UKawaiiFluidISMRenderer::ApplySettings(const FKawaiiFluidISMRendererSettings& Settings)
{
	bEnabled = Settings.bEnabled;
	ParticleMesh = Settings.ParticleMesh;
	ParticleMaterial = Settings.ParticleMaterial;
	ParticleScale = Settings.ParticleScale;

	// If ISMComponent already exists, update it with new settings
	if (ISMComponent)
	{
		// Update mesh
		if (ParticleMesh)
		{
			ISMComponent->SetStaticMesh(ParticleMesh);
		}

		// Update material
		if (ParticleMaterial)
		{
			ISMComponent->SetMaterial(0, ParticleMaterial);
		}

		UE_LOG(LogTemp, Log, TEXT("ISMRenderer: Applied settings to existing ISMComponent (Mesh: %s, Material: %s)"),
			ParticleMesh ? *ParticleMesh->GetName() : TEXT("None"),
			ParticleMaterial ? *ParticleMaterial->GetName() : TEXT("None"));
	}
}

void UKawaiiFluidISMRenderer::UpdateRendering(const IKawaiiFluidDataProvider* DataProvider, float DeltaTime)
{
	if (!bEnabled || !ISMComponent || !DataProvider)
	{
		return;
	}

	// Get simulation data from DataProvider
	const TArray<FFluidParticle>& SimParticles = DataProvider->GetParticles();

	if (SimParticles.Num() == 0)
	{
		ISMComponent->ClearInstances();
		return;
	}

	// Limit number of particles to render
	int32 NumInstances = FMath::Min(SimParticles.Num(), MaxRenderParticles);

	// Clear existing instances and preallocate memory
	ISMComponent->ClearInstances();
	ISMComponent->PreAllocateInstancesMemory(NumInstances);

	// Get particle radius from simulation
	float ParticleRadius = DataProvider->GetParticleRadius();
	float ScaleFactor = (ParticleRadius / 50.0f) * ParticleScale; // Default Sphere has 50cm radius
	FVector ScaleVec(ScaleFactor, ScaleFactor, ScaleFactor);

	// Add each particle as instance
	for (int32 i = 0; i < NumInstances; ++i)
	{
		const FFluidParticle& Particle = SimParticles[i];

		// Create transform
		FTransform InstanceTransform;
		InstanceTransform.SetLocation(Particle.Position);
		InstanceTransform.SetScale3D(ScaleVec);

		// Velocity-based rotation (optional)
		if (bRotateByVelocity && !Particle.Velocity.IsNearlyZero())
		{
			FRotator Rotation = Particle.Velocity.ToOrientationRotator();
			InstanceTransform.SetRotation(Rotation.Quaternion());
		}

		// Add instance
		ISMComponent->AddInstance(InstanceTransform, false);

		// Velocity-based color (optional)
		if (bColorByVelocity)
		{
			float VelocityMagnitude = Particle.Velocity.Size();
			float T = FMath::Clamp(VelocityMagnitude / MaxVelocityForColor, 0.0f, 1.0f);
			FLinearColor Color = FMath::Lerp(MinVelocityColor, MaxVelocityColor, T);

			// Pass color as custom data (available in material)
			ISMComponent->SetCustomDataValue(i, 0, Color.R, false);
			ISMComponent->SetCustomDataValue(i, 1, Color.G, false);
			ISMComponent->SetCustomDataValue(i, 2, Color.B, false);
			ISMComponent->SetCustomDataValue(i, 3, Color.A, false);
		}
	}

	// Update render state
	ISMComponent->MarkRenderStateDirty();
}

void UKawaiiFluidISMRenderer::InitializeISM()
{
	if (!CachedOwner)
	{
		UE_LOG(LogTemp, Error, TEXT("KawaiiFluidISMRenderer: No owner actor"));
		return;
	}

	// Create ISM component on owner actor
	ISMComponent = NewObject<UInstancedStaticMeshComponent>(
		CachedOwner,
		UInstancedStaticMeshComponent::StaticClass(),
		TEXT("FluidISM_Internal")
	);

	if (!ISMComponent)
	{
		UE_LOG(LogTemp, Error, TEXT("KawaiiFluidISMRenderer: Failed to create ISM component"));
		return;
	}

	// Component setup
	ISMComponent->SetupAttachment(CachedOwner->GetRootComponent());

	// Use absolute coordinates (same as DummyComponent)
	ISMComponent->SetAbsolute(true, true, true);

	ISMComponent->RegisterComponent();
	ISMComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ISMComponent->SetCastShadow(bCastShadow);
	ISMComponent->SetCullDistances(0, CullDistance);

	// Mesh setup
	if (!ParticleMesh)
	{
		ParticleMesh = GetDefaultParticleMesh();
	}

	if (ParticleMesh)
	{
		ISMComponent->SetStaticMesh(ParticleMesh);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer: No particle mesh available"));
	}
	
	// Material setup
	if (!ParticleMaterial)
	{
		ParticleMaterial = GetDefaultParticleMaterial();
	}
	
	if (ParticleMaterial)
	{
		ISMComponent->SetMaterial(0, ParticleMaterial);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer: No particle material available"));
	}

	// Custom data setup (for color variation)
	if (bColorByVelocity)
	{
		ISMComponent->NumCustomDataFloats = 4; // RGBA
	}

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidISMRenderer: ISM component initialized"));
}

UStaticMesh* UKawaiiFluidISMRenderer::GetDefaultParticleMesh()
{
	// Load engine default sphere mesh
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Sphere.Sphere")
	);

	if (!SphereMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer: Failed to load default sphere mesh"));
	}

	return SphereMesh;
}

UMaterialInterface* UKawaiiFluidISMRenderer::GetDefaultParticleMaterial()
{
	// Load engine default material
	UMaterialInterface* DefaultMaterial = LoadObject<UMaterialInterface>(
		nullptr,
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")
	);

	if (!DefaultMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer: Failed to load default material"));
	}

	return DefaultMaterial;
}
