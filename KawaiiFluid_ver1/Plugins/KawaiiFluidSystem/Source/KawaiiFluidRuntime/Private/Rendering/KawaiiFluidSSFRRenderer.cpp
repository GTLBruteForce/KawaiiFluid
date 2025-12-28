// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidSSFRRenderer.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Core/KawaiiRenderParticle.h"
#include "Rendering/Composite/FluidCompositePassFactory.h"

UKawaiiFluidSSFRRenderer::UKawaiiFluidSSFRRenderer()
{
	// No component tick needed - UObject doesn't tick
}

void UKawaiiFluidSSFRRenderer::Initialize(UWorld* InWorld, AActor* InOwner)
{
	CachedWorld = InWorld;
	CachedOwner = InOwner;

	if (!CachedWorld)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidSSFRRenderer::Initialize - No world context provided"));
	}

	if (!CachedOwner)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidSSFRRenderer::Initialize - No owner actor provided"));
	}

	// Cache renderer subsystem for ViewExtension access
	if (CachedWorld)
	{
		RendererSubsystem = CachedWorld->GetSubsystem<UFluidRendererSubsystem>();

		if (!RendererSubsystem)
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidSSFRRenderer: Failed to get FluidRendererSubsystem"));
		}
	}

	// Create GPU render resource
	RenderResource = MakeShared<FKawaiiFluidRenderResource>();

	// Initialize on render thread
	ENQUEUE_RENDER_COMMAND(InitSSFRRenderResource)(
		[RenderResourcePtr = RenderResource.Get()](FRHICommandListImmediate& RHICmdList)
		{
			RenderResourcePtr->InitResource(RHICmdList);
		}
	);

	// Create initial composite pass
	UpdateCompositePass();

	UE_LOG(LogTemp, Log, TEXT("SSFRRenderer: Initialized GPU resources (MaxParticles: %d)"),
		MaxRenderParticles);
}

void UKawaiiFluidSSFRRenderer::Cleanup()
{
	// Release render resource
	if (RenderResource.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(ReleaseSSFRRenderResource)(
			[RenderResource = MoveTemp(RenderResource)](FRHICommandListImmediate& RHICmdList) mutable
			{
				if (RenderResource.IsValid())
				{
					RenderResource->ReleaseResource();
					RenderResource.Reset();
				}
			}
		);
	}

	// Clear cached data
	CachedParticlePositions.Empty();
	RenderParticlesCache.Empty();
	RendererSubsystem = nullptr;
	bIsRenderingActive = false;

	// Clear cached references
	CachedWorld = nullptr;
	CachedOwner = nullptr;
	bEnabled = false;

	UE_LOG(LogTemp, Log, TEXT("SSFRRenderer: Cleanup completed"));
}

void UKawaiiFluidSSFRRenderer::ApplySettings(const FKawaiiFluidSSFRRendererSettings& Settings)
{
	bEnabled = Settings.bEnabled;
	bUseSimulationRadius = Settings.bUseSimulationRadius;
	bRenderSurfaceOnly = Settings.bRenderSurfaceOnly;

	// Map settings to LocalParameters
	LocalParameters.bEnableRendering = Settings.bEnabled;
	LocalParameters.SSFRMode = Settings.SSFRMode;
	LocalParameters.FluidColor = Settings.FluidColor;
	LocalParameters.FresnelStrength = Settings.FresnelStrength;
	LocalParameters.RefractiveIndex = Settings.RefractiveIndex;
	LocalParameters.AbsorptionCoefficient = Settings.AbsorptionCoefficient;
	LocalParameters.SpecularStrength = Settings.SpecularStrength;
	LocalParameters.SpecularRoughness = Settings.SpecularRoughness;
	LocalParameters.ParticleRenderRadius = Settings.ParticleRenderRadius;
	LocalParameters.BilateralFilterRadius = Settings.BilateralFilterRadius;
	LocalParameters.RenderTargetScale = Settings.RenderTargetScale;
	LocalParameters.ThicknessScale = Settings.ThicknessScale;
	LocalParameters.Metallic = Settings.Metallic;
	LocalParameters.Roughness = Settings.Roughness;
	LocalParameters.SubsurfaceOpacity = Settings.SubsurfaceOpacity;

	// MaxRenderParticles stays as member variable (not in LocalParameters)
	MaxRenderParticles = Settings.MaxRenderParticles;

	// Update composite pass if mode changed
	UpdateCompositePass();

	UE_LOG(LogTemp, Log, TEXT("SSFRRenderer: Applied settings (Enabled: %s, UseSimRadius: %s, Color: %s, MaxParticles: %d)"),
		bEnabled ? TEXT("true") : TEXT("false"),
		bUseSimulationRadius ? TEXT("true") : TEXT("false"),
		*LocalParameters.FluidColor.ToString(),
		MaxRenderParticles);
}

void UKawaiiFluidSSFRRenderer::UpdateRendering(const IKawaiiFluidDataProvider* DataProvider, float DeltaTime)
{
	if (!bEnabled || !DataProvider)
	{
		bIsRenderingActive = false;
		return;
	}

	// Get simulation data from DataProvider
	const TArray<FFluidParticle>& SimParticles = DataProvider->GetParticles();

	if (SimParticles.Num() == 0)
	{
		bIsRenderingActive = false;
		LastRenderedParticleCount = 0;
		return;
	}

	// Limit number of particles to render
	int32 NumParticles = FMath::Min(SimParticles.Num(), MaxRenderParticles);

	// Get particle radius from simulation
	float ParticleRadius = DataProvider->GetParticleRadius();

	// Determine which radius to use for rendering
	float RenderRadius;
	if (bUseSimulationRadius)
	{
		// Use simulation particle radius (from Preset->ParticleRadius)
		RenderRadius = ParticleRadius;
		LocalParameters.ParticleRenderRadius = RenderRadius;
	}
	else
	{
		// Use manually configured render radius from Settings
		RenderRadius = LocalParameters.ParticleRenderRadius;
	}

	// Update GPU resources (ViewExtension will handle rendering automatically)
	UpdateGPUResources(SimParticles, RenderRadius);

	// Update stats
	LastRenderedParticleCount = NumParticles;
	bIsRenderingActive = true;
}

void UKawaiiFluidSSFRRenderer::UpdateGPUResources(const TArray<FFluidParticle>& Particles, float ParticleRadius)
{
	// Optionally filter to render only surface particles (for slime optimization)
	RenderParticlesCache.Reset();
	RenderParticlesCache.Reserve(FMath::Min(Particles.Num(), MaxRenderParticles));

	for (int32 i = 0; i < Particles.Num() && RenderParticlesCache.Num() < MaxRenderParticles; ++i)
	{
		// If bRenderSurfaceOnly is false, render all particles (normal fluid)
		// If bRenderSurfaceOnly is true, only render surface particles (slime optimization)
		if (!bRenderSurfaceOnly || Particles[i].bIsSurfaceParticle)
		{
			FKawaiiRenderParticle& RenderP = RenderParticlesCache.AddDefaulted_GetRef();
			RenderP.Position = FVector3f(Particles[i].Position);
			RenderP.Velocity = FVector3f(Particles[i].Velocity);
			RenderP.Radius = ParticleRadius;
			RenderP.Padding = 0.0f;
		}
	}

	// Log rendered particle count
	UE_LOG(LogTemp, Log, TEXT("SSFR: Rendered particles = %d / Total = %d (SurfaceOnly: %s)"),
		RenderParticlesCache.Num(), Particles.Num(), bRenderSurfaceOnly ? TEXT("true") : TEXT("false"));

	// Upload to GPU (via RenderResource)
	if (RenderResource.IsValid())
	{
		RenderResource->UpdateParticleData(RenderParticlesCache);
	}

	// Cache particle radius for ViewExtension access
	CachedParticleRadius = ParticleRadius;
}

FKawaiiFluidRenderResource* UKawaiiFluidSSFRRenderer::GetFluidRenderResource() const
{
	return RenderResource.Get();
}

bool UKawaiiFluidSSFRRenderer::IsRenderingActive() const
{
	return bIsRenderingActive && RenderResource.IsValid();
}

void UKawaiiFluidSSFRRenderer::UpdateCompositePass()
{
	// Recreate composite pass if mode changed or pass doesn't exist
	if (!CompositePass || CachedSSFRMode != LocalParameters.SSFRMode)
	{
		CachedSSFRMode = LocalParameters.SSFRMode;
		CompositePass = FFluidCompositePassFactory::Create(CachedSSFRMode);

		UE_LOG(LogTemp, Log, TEXT("SSFR Renderer mode changed to: %s"),
			CachedSSFRMode == ESSFRRenderingMode::Custom ? TEXT("Custom") : TEXT("G-Buffer"));
	}
}
