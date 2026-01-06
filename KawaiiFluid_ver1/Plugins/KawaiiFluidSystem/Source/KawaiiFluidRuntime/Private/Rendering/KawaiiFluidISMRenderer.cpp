// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Core/FluidParticle.h"
#include "Materials/MaterialInstanceDynamic.h"

UKawaiiFluidISMRenderer::UKawaiiFluidISMRenderer()
{
	// No component tick needed - UObject doesn't tick
}

void UKawaiiFluidISMRenderer::Initialize(UWorld* InWorld, USceneComponent* InOwnerComponent)
{
	CachedWorld = InWorld;
	CachedOwnerComponent = InOwnerComponent;

	if (!CachedWorld)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer::Initialize - No world context provided"));
	}

	if (!CachedOwnerComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer::Initialize - No owner component provided"));
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
	CachedOwnerComponent = nullptr;
	bEnabled = false;
}

void UKawaiiFluidISMRenderer::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;

	// Clear instances when disabled
	if (!bEnabled && ISMComponent)
	{
		ISMComponent->ClearInstances();
	}
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
		// Clear instances when disabled
		if (!bEnabled)
		{
			ISMComponent->ClearInstances();
			return;
		}

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

		// Apply color
		SetFluidColor(Settings.ParticleColor);

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
	if (!CachedOwnerComponent)
	{
		UE_LOG(LogTemp, Error, TEXT("KawaiiFluidISMRenderer: No owner component"));
		return;
	}

	// Create ISM component on owner component
	ISMComponent = NewObject<UInstancedStaticMeshComponent>(
		CachedOwnerComponent,
		UInstancedStaticMeshComponent::StaticClass(),
		TEXT("FluidISM_Internal")
	);

	if (!ISMComponent)
	{
		UE_LOG(LogTemp, Error, TEXT("KawaiiFluidISMRenderer: Failed to create ISM component"));
		return;
	}

	// Component setup - FluidComponent에 직접 부착 (안정적인 계층 구조)
	ISMComponent->SetupAttachment(CachedOwnerComponent);

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

void UKawaiiFluidISMRenderer::SetFluidColor(FLinearColor Color)
{
	if (!ISMComponent)
	{
		return;
	}

	// Get or create dynamic material instance
	UMaterialInstanceDynamic* DynMaterial = Cast<UMaterialInstanceDynamic>(ISMComponent->GetMaterial(0));

	if (!DynMaterial)
	{
		// Create dynamic material from current material
		UMaterialInterface* BaseMaterial = ISMComponent->GetMaterial(0);
		if (!BaseMaterial)
		{
			BaseMaterial = GetDefaultParticleMaterial();
		}

		if (BaseMaterial)
		{
			DynMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, ISMComponent);
			ISMComponent->SetMaterial(0, DynMaterial);
		}
	}

	if (DynMaterial)
	{
		// BasicShapeMaterial uses "Color" parameter
		DynMaterial->SetVectorParameterValue(TEXT("Color"), Color);
	}

	// Also update velocity colors for consistency
	MinVelocityColor = Color;
	MaxVelocityColor = Color;
}
