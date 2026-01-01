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

// New Pipeline architecture (ShadingPass removed - Pipeline handles ShadingMode internally)
#include "Rendering/Pipeline/IKawaiiMetaballRenderingPipeline.h"

static TRefCountPtr<IPooledRenderTarget> GFluidCompositeDebug_KeepAlive;

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
	// PostProcess mode is handled entirely in SubscribeToPostProcessingPass
	// - GBuffer: writes to GBuffer
	// - Translucent: writes to GBuffer + Stencil marking
	TMap<FFluidRenderingParameters, TArray<UKawaiiFluidMetaballRenderer*>> GBufferBatches;
	TMap<FFluidRenderingParameters, TArray<UKawaiiFluidMetaballRenderer*>> TranslucentBatches;

	const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
		if (MetaballRenderer && MetaballRenderer->IsRenderingActive())
		{
			const FFluidRenderingParameters& Params = MetaballRenderer->GetLocalParameters();
			// Route based on ShadingMode (PostProcess is handled in SubscribeToPostProcessingPass)
			switch (Params.ShadingMode)
			{
			case EMetaballShadingMode::GBuffer:
			case EMetaballShadingMode::Opaque:
				GBufferBatches.FindOrAdd(Params).Add(MetaballRenderer);
				break;
			case EMetaballShadingMode::Translucent:
				TranslucentBatches.FindOrAdd(Params).Add(MetaballRenderer);
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

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: PostRenderBasePassDeferred - Processing %d GBuffer, %d Translucent batches"),
		GBufferBatches.Num(), TranslucentBatches.Num());

	// Get SceneDepth from RenderTargets
	FRDGTextureRef SceneDepthTexture = RenderTargets.DepthStencil.GetTexture();

	// Process GBuffer batches using new Pipeline architecture
	for (auto& Batch : GBufferBatches)
	{
		const FFluidRenderingParameters& BatchParams = Batch.Key;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_GBuffer");

		// Get Pipeline from first renderer (all renderers in batch share same params)
		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PostBasePass - handles GBuffer write internally based on ShadingMode
			// Note: For GBuffer mode, Output is not used (writes directly to GBuffer textures)
			FScreenPassRenderTarget DummyOutput;

			Pipeline->ExecutePostBasePass(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture,
				DummyOutput);

			UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: GBuffer Pipeline rendered %d renderers at PostBasePass timing"), Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for GBuffer batch"));
		}
	}

	// Process Translucent batches - ExecutePostBasePass for GBuffer write with Stencil marking
	// This writes to GBuffer with Stencil=0x01 marking for TransparencyComposite
	for (auto& Batch : TranslucentBatches)
	{
		const FFluidRenderingParameters& BatchParams = Batch.Key;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_Translucent_GBufferWrite");

		// Get Pipeline from first renderer (all renderers in batch share same params)
		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PostBasePass - handles GBuffer write with Stencil marking internally
			// Note: For Translucent mode, Output is not used (writes to GBuffer + Stencil)
			// Transparency pass runs later in PrePostProcessPass_RenderThread via ExecutePrePostProcess
			FScreenPassRenderTarget DummyOutput;

			Pipeline->ExecutePostBasePass(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture,
				DummyOutput);

			UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: Translucent GBuffer write - %d renderers (Stencil marked)"), Renderers.Num());
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
	// Custom mode: Tonemap pass (ScreenSpace/RayMarching pipelines)
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
				bool bHasAnyModules = SubsystemPtr && SubsystemPtr->GetAllRenderingModules().Num() > 0;

				if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering || !bHasAnyModules)
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluidRendering");

				// ============================================
				// Batch renderers by LocalParameters (new Pipeline-based approach)
				// Separate batches by PipelineType (ScreenSpace vs RayMarching)
				// GBuffer shading is handled in PostRenderBasePassDeferred, not here
				// Translucent: GBuffer write in PostRenderBasePassDeferred, Transparency pass here
				// ============================================
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidMetaballRenderer*>> ScreenSpaceBatches;
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidMetaballRenderer*>> RayMarchingBatches;

				const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
				for (UKawaiiFluidRenderingModule* Module : Modules)
				{
					if (!Module) continue;

					UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
					if (MetaballRenderer && MetaballRenderer->IsRenderingActive())
					{
						const FFluidRenderingParameters& Params = MetaballRenderer->GetLocalParameters();

						// Route based on ShadingMode and PipelineType
						// GBuffer and Translucent modes are handled elsewhere:
						// - GBuffer: PostRenderBasePassDeferred
						// - Translucent: PrePostProcessPass_RenderThread
						if (Params.ShadingMode == EMetaballShadingMode::GBuffer ||
							Params.ShadingMode == EMetaballShadingMode::Translucent)
						{
							// Skip - handled in other callbacks
							continue;
						}
						else if (Params.PipelineType == EMetaballPipelineType::ScreenSpace)
						{
							ScreenSpaceBatches.FindOrAdd(Params).Add(MetaballRenderer);
						}
						else if (Params.PipelineType == EMetaballPipelineType::RayMarching)
						{
							RayMarchingBatches.FindOrAdd(Params).Add(MetaballRenderer);
						}
					}
				}

				// Check if we have any renderers for ScreenSpace/RayMarching
				if (ScreenSpaceBatches.Num() == 0 && RayMarchingBatches.Num() == 0)
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				// Scene Depth 가져오기
				FRDGTextureRef SceneDepthTexture = nullptr;
				if (InInputs.SceneTextures.SceneTextures)
				{
					SceneDepthTexture = InInputs.SceneTextures.SceneTextures->GetContents()->SceneDepthTexture;
				}

				// Composite Setup (공통)
				FScreenPassTexture SceneColorInput = FScreenPassTexture(
					InInputs.GetInput(EPostProcessMaterialInput::SceneColor));
				if (!SceneColorInput.IsValid())
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				// Output Target 결정
				FScreenPassRenderTarget Output = InInputs.OverrideOutput;
				if (!Output.IsValid())
				{
					Output = FScreenPassRenderTarget::CreateFromInput(
						GraphBuilder, SceneColorInput, View.GetOverwriteLoadAction(),
						TEXT("FluidCompositeOutput"));
				}

				// SceneColor 복사
				if (SceneColorInput.Texture != Output.Texture)
				{
					AddDrawTexturePass(GraphBuilder, View, SceneColorInput, Output);
				}

				// ============================================
				// ScreenSpace Pipeline Rendering
				// PostProcess mode is fully handled here:
				// 1. PrepareForTonemap: Generate intermediate textures (depth, normal, thickness)
				// 2. ExecuteTonemap: Apply PostProcess shading using cached textures
				// ============================================
				for (auto& Batch : ScreenSpaceBatches)
				{
					const FFluidRenderingParameters& BatchParams = Batch.Key;
					const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

					RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_ScreenSpace");

					// Get Pipeline from first renderer (all renderers in batch share same params)
					if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
					{
						TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

						// 1. PrepareForTonemap - generate and cache intermediate textures
						Pipeline->PrepareForTonemap(
							GraphBuilder,
							View,
							BatchParams,
							Renderers,
							SceneDepthTexture);

						// 2. ExecuteTonemap - apply PostProcess shading
						Pipeline->ExecuteTonemap(
							GraphBuilder,
							View,
							BatchParams,
							Renderers,
							SceneDepthTexture,
							SceneColorInput.Texture,
							Output);

						UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: ScreenSpace Pipeline rendered %d renderers"), Renderers.Num());
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for ScreenSpace batch"));
					}
				}

				// ============================================
				// RayMarching Pipeline Rendering
				// PostProcess mode is fully handled here:
				// 1. PrepareForTonemap: Prepare particle buffer and optional SDF volume
				// 2. ExecuteTonemap: Apply PostProcess ray march shading
				// ============================================
				for (auto& Batch : RayMarchingBatches)
				{
					const FFluidRenderingParameters& BatchParams = Batch.Key;
					const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

					RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_RayMarching");

					// Get Pipeline from first renderer (all renderers in batch share same params)
					if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
					{
						TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

						// 1. PrepareForTonemap - prepare particle buffer and SDF
						Pipeline->PrepareForTonemap(
							GraphBuilder,
							View,
							BatchParams,
							Renderers,
							SceneDepthTexture);

						// 2. ExecuteTonemap - apply PostProcess ray march shading
						Pipeline->ExecuteTonemap(
							GraphBuilder,
							View,
							BatchParams,
							Renderers,
							SceneDepthTexture,
							SceneColorInput.Texture,
							Output);

						UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: RayMarching Pipeline rendered %d renderers"), Renderers.Num());
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for RayMarching batch"));
					}
				}

				// Debug Keep Alive
				GraphBuilder.QueueTextureExtraction(Output.Texture, &GFluidCompositeDebug_KeepAlive);

				return FScreenPassTexture(Output);
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

	// Collect Translucent mode renderers for TransparencyPass
	TMap<FFluidRenderingParameters, TArray<UKawaiiFluidMetaballRenderer*>> TranslucentBatches;

	const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
		if (MetaballRenderer && MetaballRenderer->IsRenderingActive())
		{
			const FFluidRenderingParameters& Params = MetaballRenderer->GetLocalParameters();
			if (Params.ShadingMode == EMetaballShadingMode::Translucent)
			{
				TranslucentBatches.FindOrAdd(Params).Add(MetaballRenderer);
			}
		}
	}

	if (TranslucentBatches.Num() == 0)
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
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid PrePostProcess: Missing GBuffer or Depth textures"));
		return;
	}

	// Debug log - all should be at internal resolution now
	UE_LOG(LogTemp, Warning, TEXT("=== PrePostProcess TransparencyPass ==="));
	UE_LOG(LogTemp, Warning, TEXT("ViewRect: Min(%d,%d) Size(%d,%d)"),
		ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Width(), ViewRect.Height());
	UE_LOG(LogTemp, Warning, TEXT("SceneColor Size: (%d,%d)"),
		SceneColorTexture->Desc.Extent.X, SceneColorTexture->Desc.Extent.Y);
	UE_LOG(LogTemp, Warning, TEXT("GBufferA Size: (%d,%d)"),
		GBufferATexture->Desc.Extent.X, GBufferATexture->Desc.Extent.Y);

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
	for (auto& Batch : TranslucentBatches)
	{
		const FFluidRenderingParameters& BatchParams = Batch.Key;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PrePostProcess with GBuffer textures for transparency compositing
			Pipeline->ExecutePrePostProcess(
				GraphBuilder,
				View,
				BatchParams,
				Renderers,
				SceneDepthTexture,     // Has Stencil=0x01 marking from GBuffer write
				LitSceneColorCopy,     // Lit scene color (after Lumen/VSM)
				Output,
				GBufferATexture,       // Normals for refraction direction
				GBufferDTexture);      // Thickness for Beer's Law absorption
		}
	}

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: PrePostProcess TransparencyPass rendered %d batches"), TranslucentBatches.Num());
}
