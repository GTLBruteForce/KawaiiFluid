// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidSSFRRenderer.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Core/KawaiiRenderParticle.h"

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

	UE_LOG(LogTemp, Log, TEXT("SSFRRenderer: Initialized GPU resources (FluidColor: %s, MaxParticles: %d)"),
		*FluidColor.ToString(),
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
	FluidColor = Settings.FluidColor;
	Metallic = Settings.Metallic;
	Roughness = Settings.Roughness;
	RefractiveIndex = Settings.RefractiveIndex;
	MaxRenderParticles = Settings.MaxRenderParticles;
	DepthBufferScale = Settings.DepthBufferScale;
	bUseThicknessBuffer = Settings.bUseThicknessBuffer;
	DepthSmoothingIterations = Settings.DepthSmoothingIterations;
	FilterRadius = Settings.FilterRadius;
	SurfaceTension = Settings.SurfaceTension;
	FoamThreshold = Settings.FoamThreshold;
	FoamColor = Settings.FoamColor;
	bShowDebugVisualization = Settings.bShowDebugVisualization;
	bShowRenderTargets = Settings.bShowRenderTargets;
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

	// Get particle radius
	float ParticleRadius = DataProvider->GetParticleRenderRadius();

	// Update GPU resources (ViewExtension will handle rendering automatically)
	UpdateGPUResources(SimParticles, ParticleRadius);

	// Update stats
	LastRenderedParticleCount = NumParticles;
	bIsRenderingActive = true;
}

void UKawaiiFluidSSFRRenderer::UpdateGPUResources(const TArray<FFluidParticle>& Particles, float ParticleRadius)
{
	// Limit particle count
	int32 NumParticles = FMath::Min(Particles.Num(), MaxRenderParticles);

	// Convert FFluidParticle → FKawaiiRenderParticle
	RenderParticlesCache.SetNum(NumParticles);
	for (int32 i = 0; i < NumParticles; ++i)
	{
		RenderParticlesCache[i].Position = FVector3f(Particles[i].Position);
		RenderParticlesCache[i].Velocity = FVector3f(Particles[i].Velocity);
		RenderParticlesCache[i].Radius = ParticleRadius;
		RenderParticlesCache[i].Padding = 0.0f;
	}

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
