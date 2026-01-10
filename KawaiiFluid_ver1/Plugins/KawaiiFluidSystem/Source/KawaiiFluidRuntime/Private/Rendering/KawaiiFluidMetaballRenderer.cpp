// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Interfaces/IKawaiiFluidDataProvider.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiRenderParticle.h"
#include "DrawDebugHelpers.h"
#include "RenderGraphResources.h"
#include "RenderingThread.h"
#include "GPU/GPUFluidSimulator.h"

// Pipeline architecture (Pipeline handles ShadingMode internally)
#include "Rendering/Pipeline/IKawaiiMetaballRenderingPipeline.h"
#include "Rendering/Pipeline/KawaiiMetaballScreenSpacePipeline.h"
#include "Rendering/Pipeline/KawaiiMetaballRayMarchPipeline.h"

UKawaiiFluidMetaballRenderer::UKawaiiFluidMetaballRenderer()
{
	// No component tick needed - UObject doesn't tick
}

void UKawaiiFluidMetaballRenderer::Initialize(UWorld* InWorld, USceneComponent* InOwnerComponent, UKawaiiFluidPresetDataAsset* InPreset)
{
	CachedWorld = InWorld;
	CachedOwnerComponent = InOwnerComponent;
	CachedPreset = InPreset;

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


	// Create Pipeline based on Preset
	if (CachedPreset)
	{
		UpdatePipeline();
	}

	UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: Initialized GPU resources (MaxParticles: %d)"),
		MaxRenderParticles);
}

void UKawaiiFluidMetaballRenderer::Cleanup()
{
	// Clear context reference - Context owns the RenderResource
	CachedSimulationContext.Reset();

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
	LocalParameters.SmoothingFilter = Settings.SmoothingFilter;
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
	LocalParameters.bUseSpatialHash = Settings.bUseSpatialHash;

	// Shadow parameters
	LocalParameters.bEnableShadowCasting = Settings.bEnableShadowCasting;
	LocalParameters.VSMResolution = Settings.VSMResolution;
	LocalParameters.VSMBlurRadius = Settings.VSMBlurRadius;
	LocalParameters.VSMBlurIterations = Settings.VSMBlurIterations;
	LocalParameters.ShadowIntensity = Settings.ShadowIntensity;

	// Anisotropy parameters
	LocalParameters.AnisotropyParams = Settings.AnisotropyParams;

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
	}
	else
	{
		RenderRadius = GetLocalParameters().ParticleRenderRadius;
	}

	// =====================================================
	// GPU/CPU 통합 경로: RenderResource에 시뮬레이터 참조 설정
	// 렌더 스레드에서 RenderResource를 통해 일원화된 접근 가능
	// =====================================================
	if (DataProvider->IsGPUSimulationActive())
	{
		FGPUFluidSimulator* Simulator = DataProvider->GetGPUSimulator();

		if (Simulator)
		{
			// Update anisotropy parameters to GPU simulator
			Simulator->SetAnisotropyParams(GetLocalParameters().AnisotropyParams);

			// Get particle count (atomic, thread-safe read)
			const int32 GPUParticleCount = Simulator->GetPersistentParticleCount();

			// 렌더 스레드에서 RenderResource를 통해 GPU 버퍼에 접근
			if (FKawaiiFluidRenderResource* RR = GetFluidRenderResource())
			{
				RR->SetGPUSimulatorReference(Simulator, GPUParticleCount, RenderRadius);
			}

			// Update stats
			LastRenderedParticleCount = FMath::Min(GPUParticleCount, MaxRenderParticles);
			bIsRenderingActive = (GPUParticleCount > 0);

			// Cache radius for shader parameters
			CachedParticleRadius = RenderRadius;

			// Debug logging
			static int32 FrameCounter = 0;
			if (++FrameCounter % 60 == 0)
			{
				UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: GPU mode - RenderResource에 시뮬레이터 참조 설정 (%d particles, radius: %.2f)"),
					GPUParticleCount, RenderRadius);
			}

			DrawDebugVisualization();
			return;
		}
	}

	// CPU 모드: GPU 시뮬레이터 참조 해제
	if (FKawaiiFluidRenderResource* RR = GetFluidRenderResource())
	{
		RR->ClearGPUSimulatorReference();
	}

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

	// 스냅샷 방식: 렌더 스레드로 파티클 데이터 전송 (배칭 렌더링용)
	// ENQUEUE_RENDER_COMMAND로 안전하게 전달, 렌더 스레드에서 Append
	if (CachedSimulationContext.IsValid() && CachedSimulationContext->HasValidRenderResource())
	{
		FKawaiiFluidRenderResource* RR = CachedSimulationContext->GetRenderResource();
		ENQUEUE_RENDER_COMMAND(AppendParticlesSnapshot)(
			[RR, Snapshot = MoveTemp(RenderParticlesCache)](FRHICommandListImmediate& RHICmdList) mutable
			{
				RR->AppendParticlesSnapshot(MoveTemp(Snapshot));
			}
		);
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
	if (FKawaiiFluidRenderResource* RR = GetFluidRenderResource())
	{
		RR->UpdateFromGPUBuffer(PhysicsPooledBuffer, ParticleCount, ParticleRadius);
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
	if (CachedSimulationContext.IsValid())
	{
		return CachedSimulationContext->GetRenderResource();
	}
	return nullptr;
}

void UKawaiiFluidMetaballRenderer::SetSimulationContext(UKawaiiFluidSimulationContext* InContext)
{
	CachedSimulationContext = InContext;

	UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: %s SimulationContext"),
		InContext ? TEXT("Set") : TEXT("Cleared"));
}

bool UKawaiiFluidMetaballRenderer::IsRenderingActive() const
{
	return bIsRenderingActive && CachedSimulationContext.IsValid() && CachedSimulationContext->HasValidRenderResource();
}

void UKawaiiFluidMetaballRenderer::UpdatePipeline()
{
	const FFluidRenderingParameters& Params = GetLocalParameters();

	// Check if Pipeline needs to be recreated
	bool bPipelineChanged = !Pipeline || CachedPipelineType != Params.PipelineType;

	// Create/recreate Pipeline if needed
	// Note: Pipeline now handles ShadingMode internally via switch statements
	if (bPipelineChanged)
	{
		switch (Params.PipelineType)
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

		CachedPipelineType = Params.PipelineType;
	}

	UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: Pipeline=%s, Shading=%s"),
		Params.PipelineType == EMetaballPipelineType::ScreenSpace ? TEXT("ScreenSpace") : TEXT("RayMarching"),
		Params.ShadingMode == EMetaballShadingMode::PostProcess ? TEXT("PostProcess") :
		Params.ShadingMode == EMetaballShadingMode::GBuffer ? TEXT("GBuffer") :
		Params.ShadingMode == EMetaballShadingMode::Opaque ? TEXT("Opaque") : TEXT("Translucent"));
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
	const FFluidRenderingParameters& Params = GetLocalParameters();

	if (!Params.bDebugDrawSDFVolume || !bHasValidSDFVolumeBounds)
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
		Params.SDFVolumeDebugColor,
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

void UKawaiiFluidMetaballRenderer::SetPreset(UKawaiiFluidPresetDataAsset* InPreset)
{
	CachedPreset = InPreset;

	if (CachedPreset)
	{
		// Update Pipeline based on Preset's PipelineType
		UpdatePipeline();

		UE_LOG(LogTemp, Log, TEXT("MetaballRenderer: SetPreset - PipelineType=%s"),
			GetLocalParameters().PipelineType == EMetaballPipelineType::ScreenSpace ? TEXT("ScreenSpace") : TEXT("RayMarching"));
	}
}
