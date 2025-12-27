// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidSceneViewExtension.h"

#include "FluidDepthPass.h"
#include "FluidNormalPass.h"
#include "FluidRendererSubsystem.h"
#include "FluidSmoothingPass.h"
#include "FluidThicknessPass.h"
#include "IKawaiiFluidRenderable.h"
#include "Core/FluidSimulator.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "RenderGraphUtils.h"
#include "PostProcess/PostProcessInputs.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "PixelShaderUtils.h"
#include "SceneTextureParameters.h"
#include "Rendering/FluidCompositeShaders.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidSSFRRenderer.h"
#include "Rendering/Composite/IFluidCompositePass.h"

static TRefCountPtr<IPooledRenderTarget> GFluidCompositeDebug_KeepAlive;

static void RenderFluidCompositePass_Internal(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	FRDGTextureRef FluidDepthTexture,
	FRDGTextureRef FluidNormalTexture,
	FRDGTextureRef FluidThicknessTexture,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output
)
{
	if (!FluidDepthTexture || !FluidNormalTexture || !FluidThicknessTexture || !SceneDepthTexture)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "FluidCompositePass");

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidCompositePS::FParameters>();

	// 텍스처 바인딩
	PassParameters->FluidDepthTexture = FluidDepthTexture;
	PassParameters->FluidNormalTexture = FluidNormalTexture;
	PassParameters->FluidThicknessTexture = FluidThicknessTexture;
	PassParameters->SceneDepthTexture = SceneDepthTexture;
	PassParameters->SceneColorTexture = SceneColorTexture;
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->InputSampler = TStaticSamplerState<
		SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	PassParameters->InverseProjectionMatrix =
		FMatrix44f(View.ViewMatrices.GetInvProjectionMatrix());
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionMatrix());
	PassParameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetViewMatrix());

	// Use RenderParams directly (passed from caller)
	PassParameters->FluidColor = RenderParams.FluidColor;
	PassParameters->FresnelStrength = RenderParams.FresnelStrength;
	PassParameters->RefractiveIndex = RenderParams.RefractiveIndex;
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;
	PassParameters->SpecularStrength = RenderParams.SpecularStrength;
	PassParameters->SpecularRoughness = RenderParams.SpecularRoughness;
	PassParameters->EnvironmentLightColor = RenderParams.EnvironmentLightColor;

	// 배경 위에 그리기
	PassParameters->RenderTargets[0] = FRenderTargetBinding(
		Output.Texture, ERenderTargetLoadAction::ELoad);

	// 쉐이더 가져오기
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidCompositeVS> VertexShader(GlobalShaderMap);
	TShaderMapRef<FFluidCompositePS> PixelShader(GlobalShaderMap);

	FIntRect ViewRect = View.UnscaledViewRect;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("FluidCompositeDraw"),
		PassParameters,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, PassParameters, ViewRect](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X,
			                       ViewRect.Max.Y, 1.0f);
			RHICmdList.SetScissorRect(true, ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Max.X,
			                          ViewRect.Max.Y);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.
				VertexDeclarationRHI; // [핵심] Input Layout 없음!
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			// Alpha Blending
			GraphicsPSOInit.BlendState = TStaticBlendState<
				CW_RGBA,
				BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
				BO_Add, BF_Zero, BF_One
			>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
				false, CF_Always>::GetRHI();

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(),
			                    *PassParameters);

			// 삼각형 1개로 화면 채우기 (VS에서 VertexID로 좌표 생성)
			RHICmdList.DrawPrimitive(0, 1, 1);
		});
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

void FFluidSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::Tonemap)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateLambda(
			[this](FRDGBuilder& GraphBuilder, const FSceneView& View,
			       const FPostProcessMaterialInputs& InInputs)
			{
				UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();

				// 유효성 검사 (Legacy + New Architecture 모두 지원)
				bool bHasAnyRenderables = SubsystemPtr && SubsystemPtr->GetAllRenderables().Num() > 0;
				bool bHasAnyModules = SubsystemPtr && SubsystemPtr->GetAllRenderingModules().Num() > 0;

				if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering ||
					(!bHasAnyRenderables && !bHasAnyModules))
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluidRendering");

				// ============================================
				// Batch renderers by LocalParameters (New Architecture only)
				// Separate batches by rendering mode
				// ============================================
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidSSFRRenderer*>> CustomBatches;
				TMap<FFluidRenderingParameters, TArray<UKawaiiFluidSSFRRenderer*>> GBufferBatches;

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
					}
				}

				// Check if we have any renderers (Legacy or Batched)
				TArray<IKawaiiFluidRenderable*> LegacyRenderables = SubsystemPtr->GetAllRenderables();
				bool bHasLegacySSFR = false;
				for (IKawaiiFluidRenderable* Renderable : LegacyRenderables)
				{
					if (Renderable && Renderable->ShouldUseSSFR())
					{
						bHasLegacySSFR = true;
						break;
					}
				}

				if (!bHasLegacySSFR && CustomBatches.Num() == 0 && GBufferBatches.Num() == 0)
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
				// LEGACY PATH: IKawaiiFluidRenderable (AFluidSimulator)
				// ============================================
				if (bHasLegacySSFR)
				{
					RDG_EVENT_SCOPE(GraphBuilder, "LegacyFluidRendering");

					// Depth Pass
					FRDGTextureRef DepthTexture = nullptr;
					RenderFluidDepthPass(GraphBuilder, View, SubsystemPtr, SceneDepthTexture, DepthTexture);

					if (DepthTexture)
					{
						// Calculate legacy parameters
						float AverageParticleRadius = 10.0f;
						float TotalRadius = 0.0f;
						int ValidCount = 0;

						for (IKawaiiFluidRenderable* Renderable : LegacyRenderables)
						{
							if (Renderable && Renderable->ShouldUseSSFR())
							{
								TotalRadius += Renderable->GetParticleRadius();
								ValidCount++;
							}
						}

						if (ValidCount > 0)
						{
							AverageParticleRadius = TotalRadius / ValidCount;
						}

						// SSFR 파라미터 (FluidSimulator에서 가져오기)
						float BlurRadius = 40.0f;
						float DepthFalloffMultiplier = 8.0f;
						int32 NumIterations = 3;

						for (IKawaiiFluidRenderable* Renderable : LegacyRenderables)
						{
							if (Renderable && Renderable->ShouldUseSSFR())
							{
								if (AFluidSimulator* Simulator = Cast<AFluidSimulator>(Renderable))
								{
									BlurRadius = Simulator->BlurRadiusPixels * Simulator->SmoothingStrength;
									DepthFalloffMultiplier = Simulator->DepthFalloffMultiplier;
									NumIterations = Simulator->SmoothingIterations;
									break;
								}
							}
						}

						const float DepthFalloff = AverageParticleRadius * DepthFalloffMultiplier;

						// Smoothing Pass
						FRDGTextureRef SmoothedDepthTexture = nullptr;
						RenderFluidSmoothingPass(GraphBuilder, View, DepthTexture, SmoothedDepthTexture,
						                         BlurRadius, DepthFalloff, NumIterations);

						if (SmoothedDepthTexture)
						{
							// Normal Pass
							FRDGTextureRef NormalTexture = nullptr;
							RenderFluidNormalPass(GraphBuilder, View, SmoothedDepthTexture, NormalTexture);

							// Thickness Pass
							FRDGTextureRef ThicknessTexture = nullptr;
							RenderFluidThicknessPass(GraphBuilder, View, SubsystemPtr, ThicknessTexture);

							if (NormalTexture && ThicknessTexture)
							{
								// Composite Pass (Legacy uses Subsystem->RenderingParameters)
								RenderFluidCompositePass_Internal(
									GraphBuilder,
									View,
									SubsystemPtr->RenderingParameters,
									SmoothedDepthTexture,
									NormalTexture,
									ThicknessTexture,
									SceneDepthTexture,
									SceneColorInput.Texture,
									Output
								);
							}
						}
					}
				}

				// ============================================
				// NEW PATH: Custom Mode Batched Rendering
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

							// Thickness Pass (batched - only render particles from this batch)
							FRDGTextureRef BatchThicknessTexture = nullptr;
							RenderFluidThicknessPass(GraphBuilder, View, Renderers, BatchThicknessTexture);

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

void FFluidSceneViewExtension::RenderDepthPass(FRDGBuilder& GraphBuilder, const FSceneView& View)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	FRDGTextureRef DepthTexture = nullptr;
	RenderFluidDepthPass(GraphBuilder, View, SubsystemPtr, nullptr, DepthTexture);
}

void FFluidSceneViewExtension::RenderSmoothingPass(FRDGBuilder& GraphBuilder,
                                                   const FSceneView& View,
                                                   FRDGTextureRef InputDepthTexture,
                                                   FRDGTextureRef& OutSmoothedDepthTexture)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr || !InputDepthTexture)
	{
		return;
	}

	float BlurRadius = static_cast<float>(SubsystemPtr->RenderingParameters.BilateralFilterRadius);

	// Calculate DepthFalloff based on average ParticleRenderRadius
	float AverageParticleRadius = 10.0f; // Default fallback
	float TotalRadius = 0.0f;
	int ValidCount = 0;

	// Legacy: Collect from IKawaiiFluidRenderable
	TArray<IKawaiiFluidRenderable*> Renderables = SubsystemPtr->GetAllRenderables();
	for (IKawaiiFluidRenderable* Renderable : Renderables)
	{
		if (Renderable && Renderable->ShouldUseSSFR())
		{
			TotalRadius += Renderable->GetParticleRadius();
			ValidCount++;
		}
	}

	// New: Collect from RenderingModules
	const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidSSFRRenderer* SSFRRenderer = Module->GetSSFRRenderer();
		if (SSFRRenderer && SSFRRenderer->IsRenderingActive())
		{
			TotalRadius += SSFRRenderer->GetCachedParticleRadius();
			ValidCount++;
		}
	}

	if (ValidCount > 0)
	{
		AverageParticleRadius = TotalRadius / ValidCount;
	}

	// Dynamic calculation: DepthFalloff = ParticleRadius * 0.7
	float DepthFalloff = AverageParticleRadius * 0.7f;

	// Use default iterations (3)
	int32 NumIterations = 3;

	RenderFluidSmoothingPass(GraphBuilder, View, InputDepthTexture, OutSmoothedDepthTexture,
	                         BlurRadius, DepthFalloff, NumIterations);
}

void FFluidSceneViewExtension::RenderNormalPass(FRDGBuilder& GraphBuilder, const FSceneView& View,
                                                FRDGTextureRef SmoothedDepthTexture,
                                                FRDGTextureRef& OutNormalTexture)
{
	if (!SmoothedDepthTexture)
	{
		return;
	}

	RenderFluidNormalPass(GraphBuilder, View, SmoothedDepthTexture, OutNormalTexture);
}

void FFluidSceneViewExtension::RenderThicknessPass(FRDGBuilder& GraphBuilder,
                                                   const FSceneView& View,
                                                   FRDGTextureRef& OutThicknessTexture)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	RenderFluidThicknessPass(GraphBuilder, View, SubsystemPtr, OutThicknessTexture);
}
