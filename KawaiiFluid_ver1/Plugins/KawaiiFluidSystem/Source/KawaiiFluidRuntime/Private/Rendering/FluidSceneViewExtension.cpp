// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidSceneViewExtension.h"

#include "FluidDepthPass.h"
#include "FluidNormalPass.h"
#include "FluidRendererSubsystem.h"
#include "FluidSmoothingPass.h"
#include "FluidThicknessPass.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "SceneTexturesConfig.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Rendering/SDFVolumeManager.h"

// New Pipeline + Shading architecture
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

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluidGBuffer_PostBasePass");

	// Collect GBuffer and Translucent mode renderers (separate batches)
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
			// Route based on ShadingMode - GBuffer and Translucent handled separately
			if (Params.ShadingMode == EMetaballShadingMode::GBuffer)
			{
				GBufferBatches.FindOrAdd(Params).Add(MetaballRenderer);
			}
			else if (Params.ShadingMode == EMetaballShadingMode::Translucent)
			{
				TranslucentBatches.FindOrAdd(Params).Add(MetaballRenderer);
			}
		}
	}

	if (GBufferBatches.Num() == 0 && TranslucentBatches.Num() == 0)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: PostRenderBasePassDeferred - Processing %d GBuffer batches, %d Translucent batches"),
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

			// Execute Pipeline - handles all passes internally
			// Note: For GBuffer mode, Output is not used (writes directly to GBuffer textures)
			FScreenPassRenderTarget DummyOutput;

			Pipeline->Execute(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture,
				nullptr,  // SceneColorTexture not available in PostBasePass
				DummyOutput);

			UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: GBuffer Pipeline rendered %d renderers at PostBasePass timing"), Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for GBuffer batch"));
		}
	}

	// Process Translucent batches - same Pipeline->Execute as GBuffer
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

			// Execute Pipeline - handles GBuffer write with Stencil marking
			// Note: For Translucent mode, Output is not used (writes to GBuffer + Stencil)
			// Transparency pass runs later in Tonemap callback
			FScreenPassRenderTarget DummyOutput;

			Pipeline->Execute(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture,
				nullptr,  // SceneColorTexture not available in PostBasePass
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
	// Custom mode: Tonemap pass (post-lighting)
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
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidMetaballRenderer*>> GBufferBatches;      // For log only
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidMetaballRenderer*>> TranslucentBatches;  // For TransparencyComposite

				const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
				for (UKawaiiFluidRenderingModule* Module : Modules)
				{
					if (!Module) continue;

					UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
					if (MetaballRenderer && MetaballRenderer->IsRenderingActive())
					{
						const FFluidRenderingParameters& Params = MetaballRenderer->GetLocalParameters();

						// Route based on ShadingMode and PipelineType
						if (Params.ShadingMode == EMetaballShadingMode::GBuffer)
						{
							// GBuffer mode is handled in PostRenderBasePassDeferred, skip here
							GBufferBatches.FindOrAdd(Params).Add(MetaballRenderer);
						}
						else if (Params.ShadingMode == EMetaballShadingMode::Translucent)
						{
							// Translucent: GBuffer write in PostRenderBasePassDeferred,
							// TransparencyComposite (Stage 2) runs here
							TranslucentBatches.FindOrAdd(Params).Add(MetaballRenderer);
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

				// Check if we have any renderers
				// GBuffer: handled elsewhere, Translucent: handled below
				if (ScreenSpaceBatches.Num() == 0 && RayMarchingBatches.Num() == 0 && TranslucentBatches.Num() == 0)
				{
					if (GBufferBatches.Num() > 0)
					{
						UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: GBuffer batches are handled in PostRenderBasePassDeferred"));
					}
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
				// ScreenSpace Pipeline Rendering (new Pipeline architecture)
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

						// Execute Pipeline - handles all passes internally
						Pipeline->Execute(
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
				// RayMarching Pipeline Rendering (new Pipeline architecture)
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

						// Execute Pipeline - handles particle collection, SDF baking, and shading internally
						Pipeline->Execute(
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

				// Note: GBuffer shading mode is handled in PostRenderBasePassDeferred,
				// so no warning needed here - just log if any were skipped
				if (GBufferBatches.Num() > 0)
				{
					UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: %d GBuffer batches handled in PostRenderBasePassDeferred"), GBufferBatches.Num());
				}

				// ============================================
				// Translucent Mode: Apply Transparency Pass (Stage 2)
				// GBuffer write was done in PostRenderBasePassDeferred (Stage 1)
				// Now apply refraction/absorption using TransparencyComposite
				// ============================================
				if (TranslucentBatches.Num() > 0)
				{
					RDG_EVENT_SCOPE(GraphBuilder, "FluidTranslucentTransparency");

					// Get GBuffer textures from ViewInfo (same approach as FluidTransparencyComposite)
					const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
					const FSceneTextures& SceneTexturesRef = ViewInfo.GetSceneTextures();

					FRDGTextureRef GBufferATexture = SceneTexturesRef.GBufferA;
					FRDGTextureRef GBufferDTexture = SceneTexturesRef.GBufferD;

					if (GBufferATexture && GBufferDTexture && SceneDepthTexture)
					{
						// Create copy of SceneColor to avoid read-write hazard
						FRDGTextureDesc LitSceneColorDesc = Output.Texture->Desc;
						LitSceneColorDesc.Flags |= TexCreate_ShaderResource;
						FRDGTextureRef LitSceneColorCopy = GraphBuilder.CreateTexture(
							LitSceneColorDesc,
							TEXT("LitSceneColorCopy"));
						AddCopyTexturePass(GraphBuilder, Output.Texture, LitSceneColorCopy);

						for (auto& Batch : TranslucentBatches)
						{
							const FFluidRenderingParameters& BatchParams = Batch.Key;

							// Apply transparency pass using TransparencyComposite
							// This member is owned by FFluidSceneViewExtension
							const_cast<FFluidSceneViewExtension*>(this)->TransparencyComposite.RenderTransparency(
								GraphBuilder,
								View,
								BatchParams,
								SceneDepthTexture,     // Has Stencil=0x01 marking from GBuffer write
								LitSceneColorCopy,     // Lit scene color (after Lumen/VSM)
								GBufferATexture,       // Normals for refraction direction
								GBufferDTexture,       // Thickness for Beer's Law absorption
								Output);
						}

						UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: Translucent TransparencyComposite rendered %d batches"), TranslucentBatches.Num());
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: Missing GBuffer textures for Translucent transparency pass"));
					}
				}

				// Debug Keep Alive
				GraphBuilder.QueueTextureExtraction(Output.Texture, &GFluidCompositeDebug_KeepAlive);

				return FScreenPassTexture(Output);
			}
		));
	}
}
