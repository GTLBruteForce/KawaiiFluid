// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Core/KawaiiRenderParticle.h"
#include "DrawDebugHelpers.h"
#include "RenderGraphResources.h"
#include "GPU/GPUFluidSimulator.h"

// Pipeline architecture (Pipeline handles ShadingMode internally)
#include "Rendering/Pipeline/IKawaiiMetaballRenderingPipeline.h"
#include "Rendering/Pipeline/KawaiiMetaballScreenSpacePipeline.h"
#include "Rendering/Pipeline/KawaiiMetaballRayMarchPipeline.h"

UKawaiiFluidMetaballRenderer::UKawaiiFluidMetaballRenderer()
{
	// No component tick needed - UObject doesn't tick
}

void UKawaiiFluidMetaballRenderer::Initialize(UWorld* InWorld, USceneComponent* InOwnerComponent)
{
	CachedWorld = InWorld;
	CachedOwnerComponent = InOwnerComponent;

	if (!CachedWorld)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidMetaballRenderer::Initialize - No world context provided"));
	}

	if (!CachedOwnerComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidMetaballRenderer::Initialize - No owner component provided"));
	}

	// Cache renderer subsystem for ViewExtension access
	if (CachedWorld)
	{
		RendererSubsystem = CachedWorld->GetSubsystem<UFluidRendererSubsystem>();

		if (!RendererSubsystem.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidMetaballRenderer: Failed to get FluidRendererSubsystem"));
		}
	}

	// Create GPU render resource
	RenderResource = MakeShared<FKawaiiFluidRenderResource>();

	// Initialize on render thread
	ENQUEUE_RENDER_COMMAND(InitMetaballRenderResource)(
		[RenderResourcePtr = RenderResource.Get()](FRHICommandListImmediate& RHICmdList)
		{
			RenderResourcePtr->InitResource(RHICmdList);
		}
	);

	// Create initial Pipeline
	UpdatePipeline();

	UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: Initialized GPU resources (MaxParticles: %d)"),
		MaxRenderParticles);
}

void UKawaiiFluidMetaballRenderer::Cleanup()
{
	// Release render resource
	if (RenderResource.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(ReleaseMetaballRenderResource)(
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
	CachedOwnerComponent = nullptr;
	bEnabled = false;

	UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: Cleanup completed"));
}

void UKawaiiFluidMetaballRenderer::ApplySettings(const FKawaiiFluidMetaballRendererSettings& Settings)
{
	bEnabled = Settings.bEnabled;
	bUseSimulationRadius = Settings.bUseSimulationRadius;
	bRenderSurfaceOnly = Settings.bRenderSurfaceOnly;

	// Disable rendering immediately when disabled
	if (!bEnabled)
	{
		bIsRenderingActive = false;
		// Clear GPU resources to stop rendering
		if (RenderResource.IsValid())
		{
			RenderResource->UpdateParticleData(TArray<FKawaiiRenderParticle>());
		}
	}

	// Map settings to LocalParameters
	LocalParameters.bEnableRendering = Settings.bEnabled;
	LocalParameters.PipelineType = Settings.PipelineType;
	LocalParameters.ShadingMode = Settings.ShadingMode;
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

	// Ray Marching SDF parameters
	LocalParameters.SDFSmoothness = Settings.SDFSmoothness;
	LocalParameters.MaxRayMarchSteps = Settings.MaxRayMarchSteps;
	LocalParameters.RayMarchHitThreshold = Settings.RayMarchHitThreshold;
	LocalParameters.RayMarchMaxDistance = Settings.RayMarchMaxDistance;
	LocalParameters.SSSIntensity = Settings.SSSIntensity;
	LocalParameters.SSSColor = Settings.SSSColor;
	LocalParameters.bUseSDFVolumeOptimization = Settings.bUseSDFVolumeOptimization;
	LocalParameters.SDFVolumeResolution = Settings.SDFVolumeResolution;

	// Debug visualization settings
	LocalParameters.bDebugDrawSDFVolume = Settings.bDebugDrawSDFVolume;
	LocalParameters.SDFVolumeDebugColor = Settings.SDFVolumeDebugColor;

	// MaxRenderParticles stays as member variable (not in LocalParameters)
	MaxRenderParticles = Settings.MaxRenderParticles;

	// Update Pipeline (ShadingMode is handled internally by Pipeline)
	UpdatePipeline();

	UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: Applied settings (Enabled: %s, UseSimRadius: %s, Color: %s, MaxParticles: %d)"),
		bEnabled ? TEXT("true") : TEXT("false"),
		bUseSimulationRadius ? TEXT("true") : TEXT("false"),
		*LocalParameters.FluidColor.ToString(),
		MaxRenderParticles);
}

void UKawaiiFluidMetaballRenderer::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;

	if (!bEnabled)
	{
		// Clear rendering state when disabled
		bIsRenderingActive = false;

		// Clear GPU resources to stop rendering
		if (RenderResource.IsValid())
		{
			RenderResource->UpdateParticleData(TArray<FKawaiiRenderParticle>());
		}
	}
}

void UKawaiiFluidMetaballRenderer::UpdateRendering(const IKawaiiFluidDataProvider* DataProvider, float DeltaTime)
{
	if (!bEnabled || !DataProvider)
	{
		bIsRenderingActive = false;
		return;
	}

	// Get particle radius from simulation
	float ParticleRadius = DataProvider->GetParticleRadius();

	// Determine which radius to use for rendering
	float RenderRadius;
	if (bUseSimulationRadius)
	{
		RenderRadius = ParticleRadius;
		LocalParameters.ParticleRenderRadius = RenderRadius;
	}
	else
	{
		RenderRadius = LocalParameters.ParticleRenderRadius;
	}

	// =====================================================
	// Phase 2: Check if GPU simulation is active
	// If active, store simulator reference for render thread direct access
	// NO game thread buffer access - eliminates race condition!
	// =====================================================
	if (DataProvider->IsGPUSimulationActive())
	{
		FGPUFluidSimulator* Simulator = DataProvider->GetGPUSimulator();

		if (Simulator)
		{
			// Store simulator reference for render thread access
			// The Pipeline (on render thread) will directly access Simulator->GetPersistentParticleBuffer()
			CachedGPUSimulator = Simulator;

			// Get particle count (atomic, thread-safe read)
			const int32 GPUParticleCount = Simulator->GetPersistentParticleCount();

			// Update stats
			LastRenderedParticleCount = FMath::Min(GPUParticleCount, MaxRenderParticles);
			bIsRenderingActive = (GPUParticleCount > 0);

			// Cache radius for shader parameters
			CachedParticleRadius = RenderRadius;

			// Debug logging
			static int32 FrameCounter = 0;
			if (++FrameCounter % 60 == 0)
			{
				UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: GPU mode active - simulator reference set (%d particles, radius: %.2f)"),
					GPUParticleCount, RenderRadius);
			}

			DrawDebugVisualization();
			return;
		}
	}

	// Clear GPU simulator reference when not in GPU mode
	CachedGPUSimulator = nullptr;

	// =====================================================
	// CPU path: Traditional CPU → GPU upload
	// =====================================================
	const TArray<FFluidParticle>& SimParticles = DataProvider->GetParticles();

	if (SimParticles.Num() == 0)
	{
		bIsRenderingActive = false;
		LastRenderedParticleCount = 0;
		return;
	}

	int32 NumParticles = FMath::Min(SimParticles.Num(), MaxRenderParticles);

	// Update GPU resources (ViewExtension will handle rendering automatically)
	UpdateGPUResources(SimParticles, RenderRadius);

	// Update stats
	LastRenderedParticleCount = NumParticles;
	bIsRenderingActive = true;

	// Draw debug visualization if enabled
	DrawDebugVisualization();
}

void UKawaiiFluidMetaballRenderer::UpdateGPUResources(const TArray<FFluidParticle>& Particles, float ParticleRadius)
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
	UE_LOG(LogTemp, Log, TEXT("Metaball: Rendered particles = %d / Total = %d (SurfaceOnly: %s)"),
		RenderParticlesCache.Num(), Particles.Num(), bRenderSurfaceOnly ? TEXT("true") : TEXT("false"));

	// Upload to GPU (via RenderResource)
	if (RenderResource.IsValid())
	{
		RenderResource->UpdateParticleData(RenderParticlesCache);
	}

	// Cache particle radius for ViewExtension access
	CachedParticleRadius = ParticleRadius;
}

void UKawaiiFluidMetaballRenderer::UpdateGPUResourcesFromGPUBuffer(
	FGPUFluidSimulator* Simulator,
	int32 ParticleCount,
	float ParticleRadius)
{
	if (!Simulator || ParticleCount <= 0)
	{
		return;
	}

	// Get physics particle pooled buffer from GPU simulator
	TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer = Simulator->GetPersistentParticleBuffer();
	if (!PhysicsPooledBuffer.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("MetaballRenderer: GPU Simulator has no valid pooled buffer"));
		return;
	}

	// Update render resource using GPU→GPU copy (no CPU involvement)
	if (RenderResource.IsValid())
	{
		RenderResource->UpdateFromGPUBuffer(PhysicsPooledBuffer, ParticleCount, ParticleRadius);
	}

	// Cache particle radius for ViewExtension access
	CachedParticleRadius = ParticleRadius;

	// Log periodically for debugging
	static int32 FrameCounter = 0;
	if (++FrameCounter % 60 == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: GPU→GPU path active (%d particles, radius: %.2f)"),
			ParticleCount, ParticleRadius);
	}
}

FKawaiiFluidRenderResource* UKawaiiFluidMetaballRenderer::GetFluidRenderResource() const
{
	return RenderResource.Get();
}

bool UKawaiiFluidMetaballRenderer::IsRenderingActive() const
{
	return bIsRenderingActive && RenderResource.IsValid();
}

void UKawaiiFluidMetaballRenderer::UpdatePipeline()
{
	// Check if Pipeline needs to be recreated
	bool bPipelineChanged = !Pipeline || CachedPipelineType != LocalParameters.PipelineType;

	// Create/recreate Pipeline if needed
	// Note: Pipeline now handles ShadingMode internally via switch statements
	if (bPipelineChanged)
	{
		switch (LocalParameters.PipelineType)
		{
		case EMetaballPipelineType::ScreenSpace:
			Pipeline = MakeShared<FKawaiiMetaballScreenSpacePipeline>();
			UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: Created ScreenSpace Pipeline"));
			break;

		case EMetaballPipelineType::RayMarching:
			Pipeline = MakeShared<FKawaiiMetaballRayMarchPipeline>();
			UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: Created RayMarching Pipeline"));
			break;
		}

		CachedPipelineType = LocalParameters.PipelineType;
	}

	UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: Pipeline=%s, Shading=%s"),
		LocalParameters.PipelineType == EMetaballPipelineType::ScreenSpace ? TEXT("ScreenSpace") : TEXT("RayMarching"),
		LocalParameters.ShadingMode == EMetaballShadingMode::PostProcess ? TEXT("PostProcess") :
		LocalParameters.ShadingMode == EMetaballShadingMode::GBuffer ? TEXT("GBuffer") :
		LocalParameters.ShadingMode == EMetaballShadingMode::Opaque ? TEXT("Opaque") : TEXT("Translucent"));
}

void UKawaiiFluidMetaballRenderer::SetSDFVolumeBounds(const FVector& VolumeMin, const FVector& VolumeMax)
{
	// Called from render thread - use atomic or game thread task for thread safety
	AsyncTask(ENamedThreads::GameThread, [this, VolumeMin, VolumeMax]()
	{
		if (IsValid(this))
		{
			CachedSDFVolumeMin = VolumeMin;
			CachedSDFVolumeMax = VolumeMax;
			bHasValidSDFVolumeBounds = true;
		}
	});
}

void UKawaiiFluidMetaballRenderer::DrawDebugVisualization()
{
	if (!LocalParameters.bDebugDrawSDFVolume || !bHasValidSDFVolumeBounds)
	{
		return;
	}

	if (!CachedWorld)
	{
		return;
	}

	// Calculate box center and extent from min/max
	FVector BoxCenter = (CachedSDFVolumeMin + CachedSDFVolumeMax) * 0.5f;
	FVector BoxExtent = (CachedSDFVolumeMax - CachedSDFVolumeMin) * 0.5f;

	// Draw debug box
	DrawDebugBox(
		CachedWorld,
		BoxCenter,
		BoxExtent,
		LocalParameters.SDFVolumeDebugColor,
		false,  // bPersistentLines
		-1.0f,  // LifeTime (negative = one frame)
		0,      // DepthPriority
		2.0f    // Thickness
	);

	// Optional: Draw corner markers for better visibility
	const FColor CornerColor = FColor::Yellow;
	const float MarkerSize = 5.0f;

	// Draw small crosses at corners
	TArray<FVector> Corners = {
		FVector(CachedSDFVolumeMin.X, CachedSDFVolumeMin.Y, CachedSDFVolumeMin.Z),
		FVector(CachedSDFVolumeMax.X, CachedSDFVolumeMin.Y, CachedSDFVolumeMin.Z),
		FVector(CachedSDFVolumeMin.X, CachedSDFVolumeMax.Y, CachedSDFVolumeMin.Z),
		FVector(CachedSDFVolumeMax.X, CachedSDFVolumeMax.Y, CachedSDFVolumeMin.Z),
		FVector(CachedSDFVolumeMin.X, CachedSDFVolumeMin.Y, CachedSDFVolumeMax.Z),
		FVector(CachedSDFVolumeMax.X, CachedSDFVolumeMin.Y, CachedSDFVolumeMax.Z),
		FVector(CachedSDFVolumeMin.X, CachedSDFVolumeMax.Y, CachedSDFVolumeMax.Z),
		FVector(CachedSDFVolumeMax.X, CachedSDFVolumeMax.Y, CachedSDFVolumeMax.Z)
	};

	for (const FVector& Corner : Corners)
	{
		DrawDebugPoint(CachedWorld, Corner, MarkerSize, CornerColor, false, -1.0f, 0);
	}
}
