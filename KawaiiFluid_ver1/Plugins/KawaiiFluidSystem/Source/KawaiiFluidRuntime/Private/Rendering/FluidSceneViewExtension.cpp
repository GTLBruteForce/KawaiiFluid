// Copyright KawaiiFluid Team. All Rights Reserved.

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

	// 중복 처리 방지를 위해 처리된 RenderResource 추적
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

		// 이미 처리된 RenderResource는 스킵
		if (ProcessedResources.Contains(RenderResource))
		{
			continue;
		}
		ProcessedResources.Add(RenderResource);

		// GPU-only: GPUSimulator에서 버퍼 추출
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

		// Physics 버퍼 등록
		FRDGBufferRef PhysicsBuffer = GraphBuilder.RegisterExternalBuffer(
			PhysicsPooledBuffer,
			TEXT("PhysicsParticles_Extract")
		);
		FRDGBufferSRVRef PhysicsBufferSRV = GraphBuilder.CreateSRV(PhysicsBuffer);

		// Pooled 버퍼 가져오기
		TRefCountPtr<FRDGPooledBuffer> PositionPooledBuffer = RenderResource->GetPooledPositionBuffer();
		TRefCountPtr<FRDGPooledBuffer> VelocityPooledBuffer = RenderResource->GetPooledVelocityBuffer();
		TRefCountPtr<FRDGPooledBuffer> RenderParticlePooled = RenderResource->GetPooledRenderParticleBuffer();
		TRefCountPtr<FRDGPooledBuffer> BoundsPooled = RenderResource->GetPooledBoundsBuffer();

		// RenderParticle + Bounds 버퍼 추출 (SDF용)
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

		// SoA 버퍼 추출 (Position/Velocity)
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

		// 버퍼 준비 완료
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

void FFluidSceneViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// Only render for views from our World
	if (!IsViewFromOurWorld(InView))
	{
		return;
	}

	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluid_PostBasePass");

	// Collect GBuffer/Translucent renderers only
	// Batching by Preset
	// PostProcess mode is handled entirely in SubscribeToPostProcessingPass
	// - GBuffer: writes to GBuffer
	// - Translucent: writes to GBuffer + Stencil marking
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> GBufferBatches;
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> TranslucentBatches;

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

			// Route based on ShadingMode (PostProcess is handled in SubscribeToPostProcessingPass)
			switch (Params.ShadingMode)
			{
			case EMetaballShadingMode::GBuffer:
			case EMetaballShadingMode::Opaque:
				GBufferBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
				break;
			case EMetaballShadingMode::Translucent:
				TranslucentBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
				break;
			case EMetaballShadingMode::PostProcess:
				// Handled in SubscribeToPostProcessingPass
				break;
			}
		}
	}

	if (GBufferBatches.Num() == 0 && TranslucentBatches.Num() == 0)
	{
		return;
	}

	UE_LOG(LogTemp, Log,
	       TEXT(
		       "KawaiiFluid: PostRenderBasePassDeferred - Processing %d GBuffer, %d Translucent batches"
	       ),
	       GBufferBatches.Num(), TranslucentBatches.Num());

	// Get SceneDepth from RenderTargets
	FRDGTextureRef SceneDepthTexture = RenderTargets.DepthStencil.GetTexture();

	// Process GBuffer batches using new Pipeline architecture
	// Batched by (Preset + GPUMode) - same context renders only once
	for (auto& Batch : GBufferBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_GBuffer");

		// Get Pipeline from first renderer (all renderers in batch share same preset)
		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PostBasePass - handles GBuffer write internally based on ShadingMode
			Pipeline->ExecutePostBasePass(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture);

			UE_LOG(LogTemp, Log,
			       TEXT("KawaiiFluid: GBuffer Pipeline rendered %d renderers at PostBasePass timing"
			       ), Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for GBuffer batch"));
		}
	}

	// Process Translucent batches - ExecutePostBasePass for GBuffer write with Stencil marking
	// This writes to GBuffer with Stencil=0x01 marking for TransparencyComposite
	// Batched by (Preset + GPUMode) - same context renders only once
	for (auto& Batch : TranslucentBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_Translucent_GBufferWrite");

		// Get Pipeline from first renderer (all renderers in batch share same preset)
		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PostBasePass - handles GBuffer write with Stencil marking internally
			// Transparency pass runs later in PrePostProcessPass_RenderThread via ExecutePrePostProcess
			Pipeline->ExecutePostBasePass(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture);

			UE_LOG(LogTemp, Log,
			       TEXT("KawaiiFluid: Translucent GBuffer write - %d renderers (Stencil marked)"),
			       Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for Translucent batch"));
		}
	}
}

void FFluidSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	// Custom mode: Tonemap pass (ScreenSpace pipeline)
	// Note: Translucent mode is handled in PrePostProcessPass_RenderThread
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
	// Batching by Preset
	// - Translucent: GBuffer write already done, transparency compositing here
	// - ScreenSpace: Full pipeline (depth/normal/thickness generation + shading)
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> TranslucentBatches;
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> ScreenSpaceBatches;

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

			if (Params.ShadingMode == EMetaballShadingMode::Translucent)
			{
				TranslucentBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
			}
			else if (Params.ShadingMode == EMetaballShadingMode::GBuffer)
			{
				// GBuffer shading is handled in PostRenderBasePassDeferred
				continue;
			}
			else if (Params.PipelineType == EMetaballPipelineType::ScreenSpace)
			{
				ScreenSpaceBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
			}
		}
	}

	// Early return if nothing to render
	if (TranslucentBatches.Num() == 0 && ScreenSpaceBatches.Num() == 0)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluid_TransparencyPass_PrePostProcess");

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

	// Get GBuffer textures
	const FSceneTextures& SceneTexturesRef = ViewInfo.GetSceneTextures();
	FRDGTextureRef GBufferATexture = SceneTexturesRef.GBufferA;
	FRDGTextureRef GBufferDTexture = SceneTexturesRef.GBufferD;

	if (!GBufferATexture || !GBufferDTexture || !SceneDepthTexture)
	{
		UE_LOG(LogTemp, Warning,
		       TEXT("KawaiiFluid PrePostProcess: Missing GBuffer or Depth textures"));
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

	// Apply TransparencyPass for each Translucent batch using Pipeline
	// Batched by (Preset + GPUMode) - same context renders only once
	for (auto& Batch : TranslucentBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_Translucent(%d renderers)", Renderers.Num());

		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PrePostProcess with GBuffer textures for transparency compositing
			Pipeline->ExecutePrePostProcess(
				GraphBuilder,
				View,
				BatchParams,
				Renderers,
				SceneDepthTexture, // Has Stencil=0x01 marking from GBuffer write
				LitSceneColorCopy, // Lit scene color (after Lumen/VSM)
				Output,
				GBufferATexture, // Normals for refraction direction
				GBufferDTexture); // Thickness for Beer's Law absorption
		}
	}

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

	// UE_LOG(LogTemp, Log,
	//        TEXT("KawaiiFluid: PrePostProcess rendered - Translucent:%d ScreenSpace:%d"),
	//        TranslucentBatches.Num(), ScreenSpaceBatches.Num());
}
