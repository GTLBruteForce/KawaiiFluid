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
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidSSFRRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Rendering/Composite/IFluidCompositePass.h"
#include "Rendering/Composite/FluidRayMarchComposite.h"

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

	// Collect GBuffer mode renderers only
	TMap<FFluidRenderingParameters, TArray<UKawaiiFluidSSFRRenderer*>> GBufferBatches;

	const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidSSFRRenderer* SSFRRenderer = Module->GetSSFRRenderer();
		if (SSFRRenderer && SSFRRenderer->IsRenderingActive())
		{
			const FFluidRenderingParameters& Params = SSFRRenderer->GetLocalParameters();
			if (Params.SSFRMode == ESSFRRenderingMode::GBuffer)
			{
				GBufferBatches.FindOrAdd(Params).Add(SSFRRenderer);
			}
		}
	}

	if (GBufferBatches.Num() == 0)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: PostRenderBasePassDeferred - Processing %d GBuffer batches"), GBufferBatches.Num());

	// Get SceneDepth from RenderTargets
	FRDGTextureRef SceneDepthTexture = RenderTargets.DepthStencil.GetTexture();

	// Process GBuffer batches
	for (auto& Batch : GBufferBatches)
	{
		const FFluidRenderingParameters& BatchParams = Batch.Key;
		const TArray<UKawaiiFluidSSFRRenderer*>& Renderers = Batch.Value;

		// Calculate average particle radius
		float AverageParticleRadius = 10.0f;
		float TotalRadius = 0.0f;
		int ValidCount = 0;

		for (UKawaiiFluidSSFRRenderer* Renderer : Renderers)
		{
			TotalRadius += Renderer->GetCachedParticleRadius();
			ValidCount++;
		}

		if (ValidCount > 0)
		{
			AverageParticleRadius = TotalRadius / ValidCount;
		}

		float BlurRadius = static_cast<float>(BatchParams.BilateralFilterRadius);
		float DepthFalloff = AverageParticleRadius * 0.7f;
		int32 NumIterations = 3;

		// Depth Pass
		FRDGTextureRef BatchDepthTexture = nullptr;
		RenderFluidDepthPass(GraphBuilder, InView, Renderers, SceneDepthTexture, BatchDepthTexture);

		if (BatchDepthTexture)
		{
			// Smoothing Pass
			FRDGTextureRef BatchSmoothedDepthTexture = nullptr;
			RenderFluidSmoothingPass(GraphBuilder, InView, BatchDepthTexture, BatchSmoothedDepthTexture,
									 BlurRadius, DepthFalloff, NumIterations);

			if (BatchSmoothedDepthTexture)
			{
				// Normal Pass
				FRDGTextureRef BatchNormalTexture = nullptr;
				RenderFluidNormalPass(GraphBuilder, InView, BatchSmoothedDepthTexture, BatchNormalTexture);

				// Thickness Pass
				FRDGTextureRef BatchThicknessTexture = nullptr;
				RenderFluidThicknessPass(GraphBuilder, InView, Renderers, SceneDepthTexture, BatchThicknessTexture);

				if (BatchNormalTexture && BatchThicknessTexture)
				{
					if (Renderers.Num() > 0 && Renderers[0]->GetCompositePass())
					{
						FFluidIntermediateTextures IntermediateTextures;
						IntermediateTextures.SmoothedDepthTexture = BatchSmoothedDepthTexture;
						IntermediateTextures.NormalTexture = BatchNormalTexture;
						IntermediateTextures.ThicknessTexture = BatchThicknessTexture;

						// Get GBuffer textures directly from FSceneTextures (UE 5.7)
						const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
						const FSceneTextures& SceneTexturesRef = ViewInfo.GetSceneTextures();
						IntermediateTextures.GBufferATexture = SceneTexturesRef.GBufferA;
						IntermediateTextures.GBufferBTexture = SceneTexturesRef.GBufferB;
						IntermediateTextures.GBufferCTexture = SceneTexturesRef.GBufferC;
						IntermediateTextures.GBufferDTexture = SceneTexturesRef.GBufferD;

						UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: GBuffer textures - A:%s B:%s C:%s D:%s"),
							IntermediateTextures.GBufferATexture ? TEXT("OK") : TEXT("NULL"),
							IntermediateTextures.GBufferBTexture ? TEXT("OK") : TEXT("NULL"),
							IntermediateTextures.GBufferCTexture ? TEXT("OK") : TEXT("NULL"),
							IntermediateTextures.GBufferDTexture ? TEXT("OK") : TEXT("NULL"));

						FScreenPassRenderTarget DummyOutput;

						Renderers[0]->GetCompositePass()->RenderComposite(
							GraphBuilder,
							InView,
							BatchParams,
							IntermediateTextures,
							SceneDepthTexture,
							nullptr,  // SceneColorTexture not available here
							DummyOutput
						);

						UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: GBuffer write complete at correct timing (PostBasePass)"));
					}
				}
			}
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
				// Batch renderers by LocalParameters
				// Separate batches by rendering mode
				// ============================================
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidSSFRRenderer*>> CustomBatches;
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidSSFRRenderer*>> GBufferBatches;
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidSSFRRenderer*>> RayMarchingBatches;

				const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
				for (UKawaiiFluidRenderingModule* Module : Modules)
				{
					if (!Module) continue;

					UKawaiiFluidSSFRRenderer* SSFRRenderer = Module->GetSSFRRenderer();
					if (SSFRRenderer && SSFRRenderer->IsRenderingActive())
					{
						const FFluidRenderingParameters& Params = SSFRRenderer->GetLocalParameters();

						// Route to appropriate batch based on rendering mode
						if (Params.SSFRMode == ESSFRRenderingMode::Custom)
						{
							CustomBatches.FindOrAdd(Params).Add(SSFRRenderer);
						}
						else if (Params.SSFRMode == ESSFRRenderingMode::GBuffer)
						{
							GBufferBatches.FindOrAdd(Params).Add(SSFRRenderer);
						}
						else if (Params.SSFRMode == ESSFRRenderingMode::RayMarching)
						{
							RayMarchingBatches.FindOrAdd(Params).Add(SSFRRenderer);
						}
					}
				}

				// Check if we have any renderers
				if (CustomBatches.Num() == 0 && GBufferBatches.Num() == 0 && RayMarchingBatches.Num() == 0)
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
				// Custom Mode Batched Rendering
				// ============================================
				for (auto& Batch : CustomBatches)
				{
					const FFluidRenderingParameters& BatchParams = Batch.Key;
					const TArray<UKawaiiFluidSSFRRenderer*>& Renderers = Batch.Value;

					RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch");

					// Calculate average particle radius for this batch
					float AverageParticleRadius = 10.0f;
					float TotalRadius = 0.0f;
					int ValidCount = 0;

					for (UKawaiiFluidSSFRRenderer* Renderer : Renderers)
					{
						TotalRadius += Renderer->GetCachedParticleRadius();
						ValidCount++;
					}

					if (ValidCount > 0)
					{
						AverageParticleRadius = TotalRadius / ValidCount;
					}

					// Use BatchParams for rendering parameters
					float BlurRadius = static_cast<float>(BatchParams.BilateralFilterRadius);
					float DepthFalloff = AverageParticleRadius * 0.7f;  // Dynamic calculation
					int32 NumIterations = 3;  // Hardcoded

					// Depth Pass (batched - only render particles from this batch)
					FRDGTextureRef BatchDepthTexture = nullptr;
					RenderFluidDepthPass(GraphBuilder, View, Renderers, SceneDepthTexture, BatchDepthTexture);

					if (BatchDepthTexture)
					{
						// Smoothing Pass
						FRDGTextureRef BatchSmoothedDepthTexture = nullptr;
						RenderFluidSmoothingPass(GraphBuilder, View, BatchDepthTexture, BatchSmoothedDepthTexture,
						                         BlurRadius, DepthFalloff, NumIterations);

						if (BatchSmoothedDepthTexture)
						{
							// Normal Pass
							FRDGTextureRef BatchNormalTexture = nullptr;
							RenderFluidNormalPass(GraphBuilder, View, BatchSmoothedDepthTexture, BatchNormalTexture);

							// Thickness Pass
							FRDGTextureRef BatchThicknessTexture = nullptr;
							RenderFluidThicknessPass(GraphBuilder, View, Renderers, SceneDepthTexture, BatchThicknessTexture);

							if (BatchNormalTexture && BatchThicknessTexture)
							{
								// Composite Pass - Use appropriate composite pass implementation
								// Get composite pass from first renderer (all renderers in batch share same params/mode)
								if (Renderers.Num() > 0 && Renderers[0]->GetCompositePass())
								{
									FFluidIntermediateTextures IntermediateTextures;
									IntermediateTextures.SmoothedDepthTexture = BatchSmoothedDepthTexture;
									IntermediateTextures.NormalTexture = BatchNormalTexture;
									IntermediateTextures.ThicknessTexture = BatchThicknessTexture;

									Renderers[0]->GetCompositePass()->RenderComposite(
										GraphBuilder,
										View,
										BatchParams,
										IntermediateTextures,
										SceneDepthTexture,
										SceneColorInput.Texture,
										Output
									);
								}
							}
						}
					}
				}

				// ============================================
				// Ray Marching SDF Mode Rendering
				// ============================================
				UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: RayMarchingBatches count = %d"), RayMarchingBatches.Num());

				for (auto& Batch : RayMarchingBatches)
				{
					const FFluidRenderingParameters& BatchParams = Batch.Key;
					const TArray<UKawaiiFluidSSFRRenderer*>& Renderers = Batch.Value;

					RDG_EVENT_SCOPE(GraphBuilder, "FluidRayMarchBatch");

					UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: RayMarching batch - Renderers count = %d"), Renderers.Num());

					// Collect all particle positions from batch
					TArray<FVector3f> AllParticlePositions;
					float AverageParticleRadius = 10.0f;
					float TotalRadius = 0.0f;
					int32 ValidCount = 0;

					for (UKawaiiFluidSSFRRenderer* Renderer : Renderers)
					{
						FKawaiiFluidRenderResource* RenderResource = Renderer->GetFluidRenderResource();
						UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: RenderResource = %p, IsValid = %s"),
							RenderResource,
							RenderResource ? (RenderResource->IsValid() ? TEXT("true") : TEXT("false")) : TEXT("N/A"));

						if (RenderResource && RenderResource->IsValid())
						{
							const TArray<FKawaiiRenderParticle>& CachedParticles = RenderResource->GetCachedParticles();
							UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: CachedParticles count = %d"), CachedParticles.Num());
							for (const FKawaiiRenderParticle& Particle : CachedParticles)
							{
								AllParticlePositions.Add(Particle.Position);
							}
						}
						TotalRadius += Renderer->GetCachedParticleRadius();
						ValidCount++;
					}

					if (ValidCount > 0)
					{
						AverageParticleRadius = TotalRadius / ValidCount;
					}

					UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: AllParticlePositions = %d, AverageRadius = %.2f"),
						AllParticlePositions.Num(), AverageParticleRadius);

					if (AllParticlePositions.Num() == 0)
					{
						UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No particles for RayMarching - skipping"));
						continue;
					}

					// Create RDG buffer for particle positions
					const uint32 BufferSize = AllParticlePositions.Num() * sizeof(FVector3f);
					FRDGBufferRef ParticleBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), AllParticlePositions.Num()),
						TEXT("RayMarchParticlePositions"));

					GraphBuilder.QueueBufferUpload(
						ParticleBuffer,
						AllParticlePositions.GetData(),
						BufferSize,
						ERDGInitialDataFlags::NoCopy);

					FRDGBufferSRVRef ParticleBufferSRV = GraphBuilder.CreateSRV(ParticleBuffer);

					// Get RayMarch composite pass from first renderer
					if (Renderers.Num() > 0 && Renderers[0]->GetCompositePass())
					{
						FFluidRayMarchComposite* RayMarchComposite =
							static_cast<FFluidRayMarchComposite*>(Renderers[0]->GetCompositePass().Get());

						// Set particle data
						RayMarchComposite->SetParticleData(
							ParticleBufferSRV,
							AllParticlePositions.Num(),
							AverageParticleRadius);

						// Render (no intermediate textures needed for ray marching)
						FFluidIntermediateTextures DummyIntermediateTextures;

						RayMarchComposite->RenderComposite(
							GraphBuilder,
							View,
							BatchParams,
							DummyIntermediateTextures,
							SceneDepthTexture,
							SceneColorInput.Texture,
							Output);

						UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: Ray Marching rendered %d particles"), AllParticlePositions.Num());
					}
				}

				// ============================================
				// G-Buffer Mode Warning (Not Yet Implemented)
				// ============================================
				if (GBufferBatches.Num() > 0)
				{
					UE_LOG(LogTemp, Warning, TEXT("G-Buffer mode renderers detected but not yet supported in Tonemap pass. Count: %d"), GBufferBatches.Num());
					UE_LOG(LogTemp, Warning, TEXT("G-Buffer mode requires implementation in MotionBlur pass (pre-lighting). See IMPLEMENTATION_GUIDE.md"));
				}

				// TODO (TEAM MEMBER): Implement G-Buffer mode rendering in MotionBlur pass
				// G-Buffer mode should write to GBuffer BEFORE lighting (pre-lighting stage)
				// This allows Lumen/VSM to process the fluid surface
				//
				// Implementation steps:
				// 1. Add MotionBlur pass subscription (similar to Tonemap above)
				// 2. Process GBufferBatches in MotionBlur callback
				// 3. Run Depth/Smoothing/Normal/Thickness passes (same as Custom mode)
				// 4. Call CompositePass->RenderComposite() which writes to GBuffer
				// 5. Test with Lumen reflections and VSM shadows

				// Debug Keep Alive
				GraphBuilder.QueueTextureExtraction(Output.Texture, &GFluidCompositeDebug_KeepAlive);

				return FScreenPassTexture(Output);
			}
		));
	}
}
