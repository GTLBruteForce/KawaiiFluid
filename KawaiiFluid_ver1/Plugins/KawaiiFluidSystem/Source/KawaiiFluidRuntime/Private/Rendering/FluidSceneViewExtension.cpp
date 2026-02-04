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
#include "Rendering/Pipeline/IKawaiiFluidRenderingPipeline.h"

// Context-based batching
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"  // For FContextCacheKey
#include "Data/KawaiiFluidPresetDataAsset.h"

// Unified interface
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Core/KawaiiRenderParticle.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RHIGPUReadback.h"  // FRHIGPUBufferReadback for particle bounds readback
#include "Rendering/FluidDepthPass.h"

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
}

/**
 * @brief Called at the beginning of each frame's view family rendering.
 * This is the LAST game thread callback before render thread starts.
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
	UWorld* OurWorld = SubsystemPtr->GetWorld();
	if (InViewFamily.Scene)
	{
		UWorld* ViewWorld = InViewFamily.Scene->GetWorld();
		if (ViewWorld != OurWorld)
		{
			return; // Skip ViewFamily from other World
		}
	}

	// ============================================
	// NOTE: Bone transform refresh is now done in SimulateSubstep(), NOT here.
	//
	// SimulateSubstep is called from HandlePostActorTick (after animation),
	// and it refreshes bones BEFORE enqueueing the fallback render command.
	// This ensures fallback execution uses current frame's bones.
	//
	// If we also refresh here, the double buffer would swap twice per frame,
	// causing the render thread to potentially read stale data.
	// ============================================

	// Note: Per-frame deduplication is handled by Preset-based TMap batching
}

void FFluidSceneViewExtension::PreRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneViewFamily& InViewFamily)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	// World filtering
	UWorld* OurWorld = SubsystemPtr->GetWorld();
	if (InViewFamily.Scene)
	{
		UWorld* ViewWorld = InViewFamily.Scene->GetWorld();
		if (ViewWorld != OurWorld)
		{
			return;
		}
	}

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluid_PrepareRenderResources");

	// NOTE: Simulation execution is handled by the fallback render command in
	// GPUFluidSimulator::SimulateSubstep, which creates its own RDG graph.
	// Executing simulation here in the same RDG as rendering causes resource
	// lifetime issues with SSFR (GPU buffer direct access).

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

		// Extract RenderParticle + Bounds buffers
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

			// Enqueue particle bounds readback for Unlimited Simulation Range world collision
			// This allows CPU to know particle AABB for expanding collision query bounds
			if (GPUSimulator->IsParticleBoundsReadbackEnabled())
			{
				AddReadbackBufferPass(GraphBuilder,
					RDG_EVENT_NAME("GPUFluid::ParticleBoundsReadback"),
					BoundsBuffer,
					[GPUSimulator, BoundsBuffer](FRHICommandListImmediate& InRHICmdList)
					{
						GPUSimulator->EnqueueParticleBoundsReadback(InRHICmdList, BoundsBuffer->GetRHI());
					});
			}
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

				// Validity check
				bool bHasAnyModules = SubsystemPtr && SubsystemPtr->GetAllRenderingModules().Num() >
					0;

				if (!SubsystemPtr || !bHasAnyModules)
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
	if (!SubsystemPtr)
	{
		return;
	}

	// Collect all renderers for PrePostProcess (before TSR)
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> ScreenSpaceBatches;
	TArray<UKawaiiFluidMetaballRenderer*> AllActiveRenderers;

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

			ScreenSpaceBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
			AllActiveRenderers.Add(MetaballRenderer);
		}
	}

	// Early return if nothing to render
	if (ScreenSpaceBatches.Num() == 0)
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

	// ============================================
	// 1. Unified Hardware Depth (for Shading & Depth Pass Occlusion)
	// This texture tracks (Environment + All Fluid Surfaces rendered so far) in Hardware Depth format.
	// ============================================
	FRDGTextureDesc CombinedDepthDesc = SceneDepthTexture->Desc;
	FRDGTextureRef CombinedHardwareDepth = GraphBuilder.CreateTexture(CombinedDepthDesc, TEXT("KawaiiFluid_CombinedHardwareDepth"));
	AddCopyTexturePass(GraphBuilder, SceneDepthTexture, CombinedHardwareDepth);


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
	// 2. Sort Batches by MAX Distance (Back-to-Front)
	// ============================================
	struct FSortableBatch
	{
		FContextCacheKey Key;
		TArray<UKawaiiFluidMetaballRenderer*> Renderers;
		float MaxDistanceToCamera;
	};

	TArray<FSortableBatch> SortedBatches;
	FVector CameraLocation = View.ViewMatrices.GetViewOrigin();

	for (auto& Pair : ScreenSpaceBatches)
	{
		FSortableBatch NewBatch;
		NewBatch.Key = Pair.Key;
		NewBatch.Renderers = Pair.Value;

		float MaxDist = 0.0f;
		for (UKawaiiFluidMetaballRenderer* Renderer : NewBatch.Renderers)
		{
			float Dist = FVector::Distance(CameraLocation, Renderer->GetSpawnPositionHint());
			MaxDist = FMath::Max(MaxDist, Dist);
		}
		NewBatch.MaxDistanceToCamera = MaxDist;

		SortedBatches.Add(NewBatch);
	}

	SortedBatches.Sort([](const FSortableBatch& A, const FSortableBatch& B) {
		return A.MaxDistanceToCamera > B.MaxDistanceToCamera;
	});

	// ============================================
	// 3. Incremental Rendering Loop
	// ============================================
	for (auto& Batch : SortedBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Renderers;

		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_Incremental(%d)", Renderers.Num());

		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// 1. PrepareRender - CombinedHardwareDepth is passed as REFERENCE to be updated
			{
				RDG_EVENT_SCOPE(GraphBuilder, "PrepareRender");
				Pipeline->PrepareRender(
					GraphBuilder,
					View,
					BatchParams,
					Renderers,
					SceneDepthTexture,       // Original Scene Depth
					CombinedHardwareDepth);  // Incremental Hardware Depth (Updated by Pipeline)
			}

			// 2. ExecuteRender - uses the LATEST CombinedHardwareDepth for shading occlusion
			{
				RDG_EVENT_SCOPE(GraphBuilder, "ExecuteRender");
				Pipeline->ExecuteRender(
					GraphBuilder,
					View,
					BatchParams,
					Renderers,
					SceneDepthTexture,
					CombinedHardwareDepth, // SHADER READS THIS: contains 벽 + 모든 이전 유체 + 현재 유체
					LitSceneColorCopy,
					Output);
			}

			// Chained Rendering: Update LitSceneColorCopy with current Output
			AddCopyTexturePass(GraphBuilder, Output.Texture, LitSceneColorCopy);
		}
	}
}
