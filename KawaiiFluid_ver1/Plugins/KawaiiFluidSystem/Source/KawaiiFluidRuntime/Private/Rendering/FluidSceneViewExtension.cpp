// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/FluidSceneViewExtension.h"

#include "FluidRendererSubsystem.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "SceneTexturesConfig.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "PostProcess/PostProcessing.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "EngineUtils.h"

// New Pipeline architecture (ShadingPass removed - Pipeline handles ShadingMode internally)
#include "Rendering/Pipeline/IKawaiiMetaballRenderingPipeline.h"

// Context-based batching
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"  // For FContextCacheKey
#include "Data/KawaiiFluidPresetDataAsset.h"

// 통합 인터페이스
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Core/KawaiiRenderParticle.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"

// ==============================================================================
// Class Implementation
// ==============================================================================

FFluidSceneViewExtension::FFluidSceneViewExtension(const FAutoRegister& AutoRegister,
                                                   UFluidRendererSubsystem* InSubsystem)
	: FSceneViewExtensionBase(AutoRegister), Subsystem(InSubsystem)
{
}

FFluidSceneViewExtension::~FFluidSceneViewExtension()
{
}

/**
 * @brief Called on game thread to setup view family before rendering.
 * Used to cache light direction for render thread access.
 * @param InViewFamily The view family being set up.
 */
void FFluidSceneViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	// World filtering: Only process ViewFamily from our World
	if (InViewFamily.Scene)
	{
		UWorld* ViewWorld = InViewFamily.Scene->GetWorld();
		if (ViewWorld != SubsystemPtr->GetWorld())
		{
			return; // Skip ViewFamily from other World
		}
	}

	// Update cached light direction on game thread (safe to use TActorIterator here)
	SubsystemPtr->UpdateCachedLightDirection();
}

/**
 * @brief Called at the beginning of each frame's view family rendering.
 * @param InViewFamily The view family being rendered.
 */
void FFluidSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	// World filtering: Only process ViewFamily from our World
	// This prevents multiple extensions from competing over the same resources
	if (InViewFamily.Scene)
	{
		UWorld* ViewWorld = InViewFamily.Scene->GetWorld();
		if (ViewWorld != SubsystemPtr->GetWorld())
		{
			return; // Skip ViewFamily from other World
		}
	}

	// Swap VSM buffers through Subsystem (per-world isolation)
	SubsystemPtr->SwapVSMBuffers();
	// Note: Per-frame deduplication is handled by Preset-based TMap batching
}

void FFluidSceneViewExtension::PreRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneViewFamily& InViewFamily)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering)
	{
		return;
	}

	// World filtering
	if (InViewFamily.Scene)
	{
		UWorld* ViewWorld = InViewFamily.Scene->GetWorld();
		if (ViewWorld != SubsystemPtr->GetWorld())
		{
			return;
		}
	}

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluid_PrepareRenderResources");
	

	// Track processed RenderResources to prevent duplicate processing
	TSet<FKawaiiFluidRenderResource*> ProcessedResources;

	const TArray<TObjectPtr<UKawaiiFluidRenderingModule>>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
		if (!MetaballRenderer || !MetaballRenderer->IsRenderingActive())
		{
			continue;
		}

		FKawaiiFluidRenderResource* RenderResource = MetaballRenderer->GetFluidRenderResource();
		if (!RenderResource || !RenderResource->IsValid())
		{
			continue;
		}

		// Skip already processed RenderResources
		if (ProcessedResources.Contains(RenderResource))
		{
			continue;
		}
		ProcessedResources.Add(RenderResource);

		// GPU-only: Extract buffers from GPUSimulator
		FGPUFluidSimulator* GPUSimulator = RenderResource->GetGPUSimulator();
		const float ParticleRadius = RenderResource->GetUnifiedParticleRadius();

		if (!GPUSimulator)
		{
			continue;
		}

		TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer = GPUSimulator->GetPersistentParticleBuffer();
		const int32 ParticleCount = GPUSimulator->GetParticleCount();

		if (!PhysicsPooledBuffer.IsValid() || ParticleCount <= 0)
		{
			continue;
		}

		RDG_EVENT_SCOPE(GraphBuilder, "ExtractToRenderResource_GPU");

		// Register Physics buffer
		FRDGBufferRef PhysicsBuffer = GraphBuilder.RegisterExternalBuffer(
			PhysicsPooledBuffer,
			TEXT("PhysicsParticles_Extract")
		);
		FRDGBufferSRVRef PhysicsBufferSRV = GraphBuilder.CreateSRV(PhysicsBuffer);

		// Get Pooled buffers
		TRefCountPtr<FRDGPooledBuffer> PositionPooledBuffer = RenderResource->GetPooledPositionBuffer();
		TRefCountPtr<FRDGPooledBuffer> VelocityPooledBuffer = RenderResource->GetPooledVelocityBuffer();
		TRefCountPtr<FRDGPooledBuffer> RenderParticlePooled = RenderResource->GetPooledRenderParticleBuffer();
		TRefCountPtr<FRDGPooledBuffer> BoundsPooled = RenderResource->GetPooledBoundsBuffer();

		// Extract RenderParticle + Bounds buffers (for SDF)
		float BoundsMargin = ParticleRadius * 2.0f + 5.0f;

		if (RenderParticlePooled.IsValid() && BoundsPooled.IsValid())
		{
			FRDGBufferRef RenderParticleBuffer = GraphBuilder.RegisterExternalBuffer(
				RenderParticlePooled, TEXT("RenderParticles_Extract"));
			FRDGBufferUAVRef RenderParticleUAV = GraphBuilder.CreateUAV(RenderParticleBuffer);

			FRDGBufferRef BoundsBuffer = GraphBuilder.RegisterExternalBuffer(
				BoundsPooled, TEXT("ParticleBounds_Extract"));
			FRDGBufferUAVRef BoundsBufferUAV = GraphBuilder.CreateUAV(BoundsBuffer);

			FGPUFluidSimulatorPassBuilder::AddExtractRenderDataWithBoundsPass(
				GraphBuilder,
				PhysicsBufferSRV,
				RenderParticleUAV,
				BoundsBufferUAV,
				ParticleCount,
				ParticleRadius,
				BoundsMargin
			);
		}

		// Extract SoA buffers (Position/Velocity)

		if (PositionPooledBuffer.IsValid() && VelocityPooledBuffer.IsValid())
		{
			FRDGBufferRef PositionBuffer = GraphBuilder.RegisterExternalBuffer(
				PositionPooledBuffer, TEXT("RenderPositions_Extract"));
			FRDGBufferUAVRef PositionUAV = GraphBuilder.CreateUAV(PositionBuffer);

			FRDGBufferRef VelocityBuffer = GraphBuilder.RegisterExternalBuffer(
				VelocityPooledBuffer, TEXT("RenderVelocities_Extract"));
			FRDGBufferUAVRef VelocityUAV = GraphBuilder.CreateUAV(VelocityBuffer);

			FGPUFluidSimulatorPassBuilder::AddExtractRenderDataSoAPass(
				GraphBuilder,
				PhysicsBufferSRV,
				PositionUAV,
				VelocityUAV,
				ParticleCount,
				ParticleRadius
			);
		}

		// Buffer preparation complete
		RenderResource->SetBufferReadyForRendering(true);
	}
}

bool FFluidSceneViewExtension::IsViewFromOurWorld(const FSceneView& InView) const
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return false;
	}

	UWorld* OurWorld = SubsystemPtr->GetWorld();
	if (!OurWorld)
	{
		return false;
	}

	// Get World from View's Family Scene
	if (InView.Family && InView.Family->Scene)
	{
		UWorld* ViewWorld = InView.Family->Scene->GetWorld();
		return ViewWorld == OurWorld;
	}

	return false;
}


void FFluidSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	// Tonemap pass - kept for potential future use, currently does nothing
	// All fluid rendering happens in PrePostProcessPass_RenderThread
	if (Pass == EPostProcessingPass::Tonemap)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateLambda(
			[this](FRDGBuilder& GraphBuilder, const FSceneView& View,
			       const FPostProcessMaterialInputs& InInputs)
			{
				// Only render for views from our World
				if (!IsViewFromOurWorld(View))
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();

				// 유효성 검사
				bool bHasAnyModules = SubsystemPtr && SubsystemPtr->GetAllRenderingModules().Num() >
					0;

				if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering || !
					bHasAnyModules)
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				// ============================================
				// All fluid rendering and shadow processing is now in PrePostProcessPass_RenderThread (before TSR)
				// This callback is kept for potential future use but does nothing
				// ============================================
				return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
			}
		));
	}
}

void FFluidSceneViewExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessingInputs& Inputs)
{
	// Only render for views from our World
	if (!IsViewFromOurWorld(View))
	{
		return;
	}

	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering)
	{
		return;
	}

	// Collect all renderers for PrePostProcess (before TSR)
	// Batching by Preset and PipelineType
	// - ScreenSpace: Full pipeline (depth/normal/thickness generation + shading)
	// - RayMarching: Volumetric ray marching pipeline
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> ScreenSpaceBatches;
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> RayMarchingBatches;

	const TArray<TObjectPtr<UKawaiiFluidRenderingModule>>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
		if (MetaballRenderer && MetaballRenderer->IsRenderingActive())
		{
			// Get preset for batching
			UKawaiiFluidPresetDataAsset* Preset = MetaballRenderer->GetPreset();
			if (!Preset)
			{
				continue;
			}

			// GPU-only mode - always use GPU batching key
			FContextCacheKey BatchKey(Preset);

			// Get rendering params from preset
			const FFluidRenderingParameters& Params = Preset->RenderingParameters;

			// Route based on PipelineType only
			if (Params.PipelineType == EMetaballPipelineType::RayMarching)
			{
				RayMarchingBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
			}
			else if (Params.PipelineType == EMetaballPipelineType::ScreenSpace)
			{
				ScreenSpaceBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
			}
		}
	}

	// Early return if nothing to render
	if (ScreenSpaceBatches.Num() == 0 && RayMarchingBatches.Num() == 0)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluid_PrePostProcess");

	// Get textures from Inputs - at this point everything is at internal resolution
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	FIntRect ViewRect = ViewInfo.ViewRect;

	// Get SceneColor and SceneDepth from SceneTextures
	if (!Inputs.SceneTextures)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid PrePostProcess: SceneTextures not available"));
		return;
	}

	FRDGTextureRef SceneColorTexture = (*Inputs.SceneTextures)->SceneColorTexture;
	FRDGTextureRef SceneDepthTexture = (*Inputs.SceneTextures)->SceneDepthTexture;

	if (!SceneColorTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid PrePostProcess: SceneColor not available"));
		return;
	}


	// Debug log - all should be at internal resolution now
	//UE_LOG(LogTemp, Warning, TEXT("=== PrePostProcess TransparencyPass ==="));
	//UE_LOG(LogTemp, Warning, TEXT("ViewRect: Min(%d,%d) Size(%d,%d)"),
	//       ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Width(), ViewRect.Height());
	//UE_LOG(LogTemp, Warning, TEXT("SceneColor Size: (%d,%d)"),
	//       SceneColorTexture->Desc.Extent.X, SceneColorTexture->Desc.Extent.Y);
	//UE_LOG(LogTemp, Warning, TEXT("GBufferA Size: (%d,%d)"),
	//       GBufferATexture->Desc.Extent.X, GBufferATexture->Desc.Extent.Y);

	// Create output render target from SceneColor
	FScreenPassRenderTarget Output(
		FScreenPassTexture(SceneColorTexture, ViewRect),
		ERenderTargetLoadAction::ELoad);

	// Create copy of SceneColor for reading (can't read and write same texture)
	FRDGTextureDesc LitSceneColorDesc = SceneColorTexture->Desc;
	LitSceneColorDesc.Flags |= TexCreate_ShaderResource;
	FRDGTextureRef LitSceneColorCopy = GraphBuilder.CreateTexture(
		LitSceneColorDesc,
		TEXT("LitSceneColorCopy_PrePostProcess"));

	// Copy SceneColor
	AddCopyTexturePass(GraphBuilder, SceneColorTexture, LitSceneColorCopy);


	// ============================================
	// ScreenSpace Pipeline Rendering (before TSR)
	// Batched by (Preset + GPUMode) - same context renders only once
	// ============================================
	for (auto& Batch : ScreenSpaceBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_ScreenSpace(%d)", Renderers.Num());

		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// 1. PrepareRender - generate and cache intermediate textures
			{
				RDG_EVENT_SCOPE(GraphBuilder, "PrepareRender");
				Pipeline->PrepareRender(
					GraphBuilder,
					View,
					BatchParams,
					Renderers,
					SceneDepthTexture);
			}

			// 2. ExecuteRender - apply shading
			{
				RDG_EVENT_SCOPE(GraphBuilder, "ExecuteRender");
				Pipeline->ExecuteRender(
					GraphBuilder,
					View,
					BatchParams,
					Renderers,
					SceneDepthTexture,
					LitSceneColorCopy,
					Output);
			}

			UE_LOG(LogTemp, Verbose,
			       TEXT("KawaiiFluid: ScreenSpace Pipeline rendered %d renderers"),
			       Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for ScreenSpace batch"));
		}
	}

	// ============================================
	// RayMarching Pipeline Rendering (before TSR)
	// Batched by (Preset + GPUMode) - same context renders only once
	// ============================================
	for (auto& Batch : RayMarchingBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_RayMarching(%d)", Renderers.Num());

		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// 1. PrepareRender - build density volumes from Z-Order sorted particles
			{
				RDG_EVENT_SCOPE(GraphBuilder, "PrepareRender");
				Pipeline->PrepareRender(
					GraphBuilder,
					View,
					BatchParams,
					Renderers,
					SceneDepthTexture);
			}

			// 2. ExecuteRender - ray marching and compositing
			{
				RDG_EVENT_SCOPE(GraphBuilder, "ExecuteRender");
				Pipeline->ExecuteRender(
					GraphBuilder,
					View,
					BatchParams,
					Renderers,
					SceneDepthTexture,
					LitSceneColorCopy,
					Output);
			}

			UE_LOG(LogTemp, Log,
			       TEXT("KawaiiFluid: RayMarching Pipeline rendered %d renderers"),
			       Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for RayMarching batch"));
		}
	}

	// UE_LOG(LogTemp, Log,
	//        TEXT("KawaiiFluid: PrePostProcess rendered - Translucent:%d ScreenSpace:%d RayMarching:%d"),
	//        TranslucentBatches.Num(), ScreenSpaceBatches.Num(), RayMarchingBatches.Num());
}
