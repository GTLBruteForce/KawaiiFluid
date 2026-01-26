// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Core/FluidParticle.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GPU/GPUFluidSimulator.h"
#include "Engine/StaticMesh.h"

UKawaiiFluidISMRenderer::UKawaiiFluidISMRenderer()
{
	// No component tick needed - UObject doesn't tick
}

void UKawaiiFluidISMRenderer::Initialize(UWorld* InWorld, USceneComponent* InOwnerComponent, UKawaiiFluidPresetDataAsset* InPreset)
{
	CachedWorld = InWorld;
	CachedOwnerComponent = InOwnerComponent;
	CachedPreset = InPreset;

	if (!CachedWorld)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer::Initialize - No world context provided"));
	}

	if (!CachedOwnerComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer::Initialize - No owner component provided"));
	}

	if (!CachedPreset)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer::Initialize - No preset provided"));
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
	CachedPreset = nullptr;
	// Note: Do NOT reset bEnabled here - it's controlled by Component's bEnableISMDebugView
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

void UKawaiiFluidISMRenderer::UpdateRendering(const IKawaiiFluidDataProvider* DataProvider, float DeltaTime)
{
	static int32 UpdateLogCounter = 0;
	const bool bShouldLog = (UpdateLogCounter++ % 120 == 0);

	if (!bEnabled)
	{
		return;
	}

	if (!ISMComponent)
	{
		UE_LOG(LogTemp, Error, TEXT("ISMRenderer::UpdateRendering - ISMComponent is NULL!"));
		return;
	}

	if (!DataProvider)
	{
		UE_LOG(LogTemp, Error, TEXT("ISMRenderer::UpdateRendering - DataProvider is NULL!"));
		return;
	}

	// Get simulation data from DataProvider (GPU or CPU)
	const TArray<FFluidParticle>* ParticlesPtr = nullptr;
	TArray<FFluidParticle> GPUParticlesCache;

	if (DataProvider->IsGPUSimulationActive())
	{
		// GPU mode: Use readback data (same as DrawDebugParticles)
		FGPUFluidSimulator* Simulator = DataProvider->GetGPUSimulator();
		if (Simulator && Simulator->GetAllGPUParticles(GPUParticlesCache))
		{
			ParticlesPtr = &GPUParticlesCache;
		}
		else
		{
			// No readback data available
			if (bShouldLog)
			{
				UE_LOG(LogTemp, Warning, TEXT("ISMRenderer: GPU readback not available"));
			}
			return;
		}
	}
	else
	{
		// CPU mode: Direct particle array
		ParticlesPtr = &DataProvider->GetParticles();
	}

	if (!ParticlesPtr || ParticlesPtr->Num() == 0)
	{
		ISMComponent->ClearInstances();
		return;
	}

	const TArray<FFluidParticle>& SimParticles = *ParticlesPtr;

	if (bShouldLog)
	{
		UE_LOG(LogTemp, Warning, TEXT("=== ISM Debug: Particles=%d, Registered=%d, Visible=%d, Mesh=%s, Material=%s, InstanceCount=%d ==="),
			SimParticles.Num(),
			ISMComponent->IsRegistered() ? 1 : 0,
			ISMComponent->IsVisible() ? 1 : 0,
			ISMComponent->GetStaticMesh() ? TEXT("OK") : TEXT("NULL"),
			ISMComponent->GetMaterial(0) ? TEXT("OK") : TEXT("NULL"),
			ISMComponent->GetInstanceCount());
	}

	// Limit number of particles to render
	int32 NumInstances = FMath::Min(SimParticles.Num(), MaxRenderParticles);

	// Clear existing instances and preallocate memory
	ISMComponent->ClearInstances();
	ISMComponent->PreAllocateInstancesMemory(NumInstances);

	// Get ParticleRenderRadius from Preset to match Metaball rendering size
	float ParticleRenderRadius = 15.0f; // Default fallback
	if (CachedPreset)
	{
		ParticleRenderRadius = CachedPreset->RenderingParameters.ParticleRenderRadius;
	}

	// Scale factor based on ParticleRenderRadius (Default Sphere has 50cm radius)
	float ScaleFactor = ParticleRenderRadius / 50.0f;
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

		// Add instance (use local space - SetAbsolute makes local=world)
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

	// Update render state - single call is sufficient in UE5
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

	// Component setup - Attach directly to FluidComponent (stable hierarchy)
	ISMComponent->SetupAttachment(CachedOwnerComponent);

	// Use absolute coordinates (same as DummyComponent)
	ISMComponent->SetAbsolute(true, true, true);

	// Mesh setup - MUST be done before RegisterComponent()
	UStaticMesh* DefaultMesh = GetDefaultParticleMesh();
	if (DefaultMesh)
	{
		ISMComponent->SetStaticMesh(DefaultMesh);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("KawaiiFluidISMRenderer: Failed to load default sphere mesh"));
		return;
	}

	// Material setup - MUST be done before RegisterComponent()
	UMaterialInterface* DefaultMaterial = GetDefaultParticleMaterial();
	if (DefaultMaterial)
	{
		ISMComponent->SetMaterial(0, DefaultMaterial);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidISMRenderer: Failed to load default material"));
	}

	// Set properties before registration
	ISMComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ISMComponent->SetCastShadow(bCastShadow);
	ISMComponent->SetCullDistances(0, CullDistance);
	ISMComponent->SetVisibility(true);
	ISMComponent->SetHiddenInGame(false);

	// Register component AFTER all setup is done
	ISMComponent->RegisterComponent();

	if (!ISMComponent->IsRegistered())
	{
		UE_LOG(LogTemp, Error, TEXT("KawaiiFluidISMRenderer: RegisterComponent() failed!"));
		return;
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
