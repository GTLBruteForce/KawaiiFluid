// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidSceneViewExtension.h"

#include "FluidRendererSubsystem.h"
#include "Rendering/FluidShadowHistoryManager.h"
#include "Rendering/FluidShadowProjection.h"
#include "Rendering/FluidVSMBlur.h"
#include "Rendering/FluidShadowReceiver.h"
#include "Rendering/FluidShadowUtils.h"
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

static TRefCountPtr<IPooledRenderTarget> GFluidCompositeDebug_KeepAlive;

// Double-buffered VSM textures (current frame writes, previous frame reads)
static TRefCountPtr<IPooledRenderTarget> GFluidVSMTexture_Write;  // Written to this frame
static TRefCountPtr<IPooledRenderTarget> GFluidVSMTexture_Read;   // Used for shadow receiving

// Cached Light View Projection matrix for shadow receiving (also double-buffered)
static FMatrix44f GFluidLightViewProjectionMatrix_Write = FMatrix44f::Identity;
static FMatrix44f GFluidLightViewProjectionMatrix_Read = FMatrix44f::Identity;

// ==============================================================================
// Shadow Projection Helper
// ==============================================================================

/**
 * @brief Execute fluid shadow projection pass.
 * @param GraphBuilder RDG builder.
 * @param View Scene view.
 * @param Subsystem Fluid renderer subsystem.
 * @param RenderParams Rendering parameters.
 */
static void ExecuteFluidShadowProjection(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	const FFluidRenderingParameters& RenderParams)
{
	// Debug: Log shadow projection entry
	UE_LOG(LogTemp, Log, TEXT("FluidShadow: ExecuteFluidShadowProjection called - bEnableShadowCasting=%d, Subsystem=%p"),
		RenderParams.bEnableShadowCasting, Subsystem);

	if (!Subsystem || !RenderParams.bEnableShadowCasting)
	{
		UE_LOG(LogTemp, Log, TEXT("FluidShadow: Early exit - Subsystem=%p, bEnableShadowCasting=%d"),
			Subsystem, RenderParams.bEnableShadowCasting);
		return;
	}

	FFluidShadowHistoryManager* HistoryManager = Subsystem->GetShadowHistoryManager();
	const bool bHasValidHistory = HistoryManager ? HistoryManager->HasValidHistory() : false;
	UE_LOG(LogTemp, Log, TEXT("FluidShadow: HistoryManager=%p, HasValidHistory=%d"),
		HistoryManager, bHasValidHistory);

	if (!HistoryManager || !bHasValidHistory)
	{
		UE_LOG(LogTemp, Log, TEXT("FluidShadow: No valid history - waiting for next frame"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "FluidShadowProjection");

	// Get history buffer
	const FFluidShadowHistoryBuffer& HistoryBuffer = HistoryManager->GetPreviousFrameBuffer();

	// Get cached light data from subsystem (updated on game thread in SetupViewFamily)
	if (!Subsystem->HasValidCachedLightData())
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidShadow: No valid cached light data"));
		return;
	}

	FFluidShadowLightParams LightParams;
	LightParams.LightDirection = Subsystem->GetCachedLightDirection();
	LightParams.LightViewProjectionMatrix = Subsystem->GetCachedLightViewProjectionMatrix();
	LightParams.bIsValid = true;

	UE_LOG(LogTemp, Log, TEXT("FluidShadow: LightDir=(%f,%f,%f)"),
		LightParams.LightDirection.X, LightParams.LightDirection.Y, LightParams.LightDirection.Z);

	// Check if history buffer has valid depth texture
	if (!HistoryBuffer.bIsValid || !HistoryBuffer.DepthTexture.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidShadow: History buffer invalid - bIsValid=%d, DepthTexture=%d"),
			HistoryBuffer.bIsValid, HistoryBuffer.DepthTexture.IsValid());
		return;
	}

	// Setup projection parameters
	FFluidShadowProjectionParams ProjectionParams;
	ProjectionParams.VSMResolution = FIntPoint(RenderParams.VSMResolution, RenderParams.VSMResolution);
	ProjectionParams.LightViewProjectionMatrix = LightParams.LightViewProjectionMatrix;

	UE_LOG(LogTemp, Log, TEXT("FluidShadow: Calling RenderFluidShadowProjection VSMRes=%dx%d"),
		ProjectionParams.VSMResolution.X, ProjectionParams.VSMResolution.Y);

	// Execute shadow projection
	FFluidShadowProjectionOutput ProjectionOutput;
	RenderFluidShadowProjection(
		GraphBuilder,
		View,
		HistoryBuffer,
		ProjectionParams,
		ProjectionOutput);

	UE_LOG(LogTemp, Log, TEXT("FluidShadow: ProjectionOutput - bIsValid=%d, VSMTexture=%d"),
		ProjectionOutput.bIsValid, ProjectionOutput.VSMTexture != nullptr);

	if (!ProjectionOutput.bIsValid || !ProjectionOutput.VSMTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidShadow: Projection output invalid"));
		return;
	}

	// Apply VSM blur
	FRDGTextureRef BlurredVSM = nullptr;
	if (RenderParams.VSMBlurIterations > 0 && RenderParams.VSMBlurRadius > 0.0f)
	{
		FFluidVSMBlurParams BlurParams;
		BlurParams.BlurRadius = RenderParams.VSMBlurRadius;
		BlurParams.NumIterations = RenderParams.VSMBlurIterations;

		RenderFluidVSMBlur(
			GraphBuilder,
			ProjectionOutput.VSMTexture,
			BlurParams,
			BlurredVSM);
	}
	else
	{
		BlurredVSM = ProjectionOutput.VSMTexture;
	}

	// Extract VSM to pooled render target for persistence (write buffer)
	if (BlurredVSM)
	{
		GraphBuilder.QueueTextureExtraction(BlurredVSM, &GFluidVSMTexture_Write);

		// Store light matrix for shadow receiving (write buffer)
		GFluidLightViewProjectionMatrix_Write = LightParams.LightViewProjectionMatrix;

		UE_LOG(LogTemp, Log, TEXT("FluidShadow: VSM texture queued for extraction"));
	}
}

// ==============================================================================
// Shadow Receiver Helper
// ==============================================================================

/**
 * @brief Apply fluid shadows to the scene using the cached VSM.
 * @param GraphBuilder RDG builder.
 * @param View Scene view.
 * @param RenderParams Rendering parameters.
 * @param SceneColorTexture Input scene color.
 * @param SceneDepthTexture Scene depth texture.
 * @param Output Output render target.
 */
static void ApplyFluidShadowReceiver(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef SceneDepthTexture,
	FScreenPassRenderTarget& Output)
{
	UE_LOG(LogTemp, Log, TEXT("FluidShadow: ApplyFluidShadowReceiver called - VSMTexture_Read=%d, ShadowCasting=%d"),
		GFluidVSMTexture_Read.IsValid(), RenderParams.bEnableShadowCasting);

	// Check if we have valid VSM from previous frame (read buffer)
	// Need to check both TRefCountPtr validity AND internal RHI resource
	if (!GFluidVSMTexture_Read.IsValid() ||
		!GFluidVSMTexture_Read->GetRHI() ||
		!RenderParams.bEnableShadowCasting)
	{
		UE_LOG(LogTemp, Log, TEXT("FluidShadow: ApplyFluidShadowReceiver early exit - waiting for VSM from previous frame"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "FluidShadowReceiver");

	// Import cached VSM texture into RDG (from read buffer - previous frame)
	FRDGTextureRef VSMTexture = GraphBuilder.RegisterExternalTexture(
		GFluidVSMTexture_Read,
		TEXT("FluidVSMTexture"));

	if (!VSMTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidShadow: Failed to register external VSM texture"));
		return;
	}

	// Setup receiver parameters
	FFluidShadowReceiverParams ReceiverParams;
	ReceiverParams.ShadowIntensity = RenderParams.ShadowIntensity;
	ReceiverParams.ShadowBias = 0.001f;
	ReceiverParams.MinVariance = 0.00001f;
	ReceiverParams.LightBleedReduction = 0.2f;
	ReceiverParams.bDebugVisualization = false;

	// Apply shadow receiver pass (use read buffer's light matrix)
	RenderFluidShadowReceiver(
		GraphBuilder,
		View,
		SceneColorTexture,
		SceneDepthTexture,
		VSMTexture,
		GFluidLightViewProjectionMatrix_Read,
		ReceiverParams,
		Output);

	UE_LOG(LogTemp, Verbose, TEXT("FluidShadow: Shadow receiver applied"));
}

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
	if (SubsystemPtr)
	{
		// Update cached light direction on game thread (safe to use TActorIterator here)
		SubsystemPtr->UpdateCachedLightDirection();
	}
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

	// Swap VSM buffers: previous frame's write buffer becomes current frame's read buffer
	// This ensures ApplyFluidShadowReceiver uses the fully extracted texture from last frame
	GFluidVSMTexture_Read = GFluidVSMTexture_Write;
	GFluidLightViewProjectionMatrix_Read = GFluidLightViewProjectionMatrix_Write;

	// Swap history buffers at the start of each frame
	if (FFluidShadowHistoryManager* HistoryManager = SubsystemPtr->GetShadowHistoryManager())
	{
		HistoryManager->BeginFrame();
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
			Pipeline->ExecutePostBasePass(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture);

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
			// Transparency pass runs later in PrePostProcessPass_RenderThread via ExecutePrePostProcess
			Pipeline->ExecutePostBasePass(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture);

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
				// Execute Shadow Projection using previous frame's depth
				// This generates VSM texture from history buffer
				// Find the first renderer with shadow casting enabled
				// ============================================
				const FFluidRenderingParameters* ShadowRenderParams = nullptr;
				const TArray<UKawaiiFluidRenderingModule*>& AllModules = SubsystemPtr->GetAllRenderingModules();
				for (UKawaiiFluidRenderingModule* Module : AllModules)
				{
					if (!Module) continue;
					UKawaiiFluidMetaballRenderer* Renderer = Module->GetMetaballRenderer();
					if (Renderer && Renderer->IsRenderingActive())
					{
						const FFluidRenderingParameters& Params = Renderer->GetLocalParameters();
						if (Params.bEnableShadowCasting)
						{
							ShadowRenderParams = &Params;
							break;
						}
					}
				}

				if (ShadowRenderParams)
				{
					ExecuteFluidShadowProjection(
						GraphBuilder,
						View,
						SubsystemPtr,
						*ShadowRenderParams);
				}

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
				// Apply Fluid Shadow Receiver
				// Uses VSM from previous frame to cast shadows on scene
				// Applied before fluid rendering so fluid appears on top of shadows
				// ============================================
				if (ShadowRenderParams)
				{
					// Create a copy for shadow receiver input (can't read and write same texture)
					FRDGTextureDesc ShadowInputDesc = Output.Texture->Desc;
					ShadowInputDesc.Flags |= TexCreate_ShaderResource;
					FRDGTextureRef ShadowInputCopy = GraphBuilder.CreateTexture(
						ShadowInputDesc,
						TEXT("FluidShadowReceiverInput"));
					AddCopyTexturePass(GraphBuilder, Output.Texture, ShadowInputCopy);

					ApplyFluidShadowReceiver(
						GraphBuilder,
						View,
						*ShadowRenderParams,
						ShadowInputCopy,
						SceneDepthTexture,
						Output);
				}

				// ============================================
				// ScreenSpace Pipeline Rendering
				// PostProcess mode is fully handled here:
				// 1. PrepareForTonemap: Generate intermediate textures (depth, normal, thickness)
				// 2. ExecuteTonemap: Apply PostProcess shading using cached textures
				// 3. Store depth to history for shadow projection (next frame)
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

						// 3. Store smoothed depth to history for shadow projection
						if (FFluidShadowHistoryManager* HistoryManager = SubsystemPtr->GetShadowHistoryManager())
						{
							if (const FMetaballIntermediateTextures* IntermediateTextures = Pipeline->GetCachedIntermediateTextures())
							{
								if (IntermediateTextures->SmoothedDepthTexture)
								{
									HistoryManager->StoreCurrentFrame(
										GraphBuilder,
										IntermediateTextures->SmoothedDepthTexture,
										View);
								}
							}
						}

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
				// 3. Store depth to history for shadow projection (next frame)
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

						// 3. Store fluid depth to history for shadow projection
						if (FFluidShadowHistoryManager* HistoryManager = SubsystemPtr->GetShadowHistoryManager())
						{
							if (const FMetaballIntermediateTextures* IntermediateTextures = Pipeline->GetCachedIntermediateTextures())
							{
								// RayMarching stores depth in SmoothedDepthTexture field
								if (IntermediateTextures->SmoothedDepthTexture)
								{
									HistoryManager->StoreCurrentFrame(
										GraphBuilder,
										IntermediateTextures->SmoothedDepthTexture,
										View);

									UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: RayMarching depth stored to shadow history"));
								}
							}
						}

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
