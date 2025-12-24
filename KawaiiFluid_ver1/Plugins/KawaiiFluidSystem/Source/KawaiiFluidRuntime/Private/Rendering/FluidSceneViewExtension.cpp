// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidSceneViewExtension.h"

#include "FluidDepthPass.h"
#include "FluidNormalPass.h"
#include "FluidRendererSubsystem.h"
#include "FluidSmoothingPass.h"
#include "FluidThicknessPass.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "RenderGraphUtils.h"
#include "PostProcess/PostProcessInputs.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "PixelShaderUtils.h"
#include "SceneTextureParameters.h"
#include "Rendering/FluidCompositeShaders.h"

static TRefCountPtr<IPooledRenderTarget> GFluidCompositeDebug_KeepAlive;

static void RenderFluidCompositePass_Internal(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	FRDGTextureRef FluidDepthTexture,
	FRDGTextureRef FluidNormalTexture,
	FRDGTextureRef FluidThicknessTexture,
	FRDGTextureRef SceneDepthTexture,
	FScreenPassRenderTarget Output
)
{
	if (!Subsystem || !FluidDepthTexture || !FluidNormalTexture || !FluidThicknessTexture || !
		SceneDepthTexture)
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
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->InputSampler = TStaticSamplerState<
		SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	PassParameters->InverseProjectionMatrix =
		FMatrix44f(View.ViewMatrices.GetInvProjectionMatrix());
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionMatrix());
	PassParameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetViewMatrix());

	const FFluidRenderingParameters& RenderParams = Subsystem->RenderingParameters;
	PassParameters->FluidColor = RenderParams.FluidColor;
	PassParameters->FresnelStrength = RenderParams.FresnelStrength;
	PassParameters->RefractiveIndex = RenderParams.RefractiveIndex;
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;
	PassParameters->SpecularStrength = RenderParams.SpecularStrength;
	PassParameters->SpecularRoughness = RenderParams.SpecularRoughness;

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

				// 유효성 검사
				if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering ||
					SubsystemPtr->GetAllRenderables().Num() == 0)
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluidRendering");

				// Depth
				FRDGTextureRef DepthTexture = nullptr;
				RenderFluidDepthPass(GraphBuilder, View, SubsystemPtr, DepthTexture);
				if (!DepthTexture) return InInputs.ReturnUntouchedSceneColorForPostProcessing(
					GraphBuilder);

				// Smoothing
				FRDGTextureRef SmoothedDepthTexture = nullptr;
				float BlurRadius = (float)SubsystemPtr->RenderingParameters.BilateralFilterRadius;
				float DepthFalloff = SubsystemPtr->RenderingParameters.DepthThreshold;
				RenderFluidSmoothingPass(GraphBuilder, View, DepthTexture, SmoothedDepthTexture,
				                         BlurRadius, DepthFalloff);
				if (!SmoothedDepthTexture) return InInputs.
					ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);

				// Normal
				FRDGTextureRef NormalTexture = nullptr;
				RenderFluidNormalPass(GraphBuilder, View, SmoothedDepthTexture, NormalTexture);
				if (!NormalTexture) return InInputs.ReturnUntouchedSceneColorForPostProcessing(
					GraphBuilder);

				// Thickness
				FRDGTextureRef ThicknessTexture = nullptr;
				RenderFluidThicknessPass(GraphBuilder, View, SubsystemPtr, ThicknessTexture);
				if (!ThicknessTexture) return InInputs.ReturnUntouchedSceneColorForPostProcessing(
					GraphBuilder);

				// Composite Setup
				FScreenPassTexture SceneColorInput = FScreenPassTexture(
					InInputs.GetInput(EPostProcessMaterialInput::SceneColor));
				if (!SceneColorInput.IsValid()) return InInputs.
					ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);

				FRDGTextureRef SceneDepthTexture = nullptr;
				if (InInputs.SceneTextures.SceneTextures)
				{
					SceneDepthTexture = InInputs.SceneTextures.SceneTextures->GetContents()->
					                             SceneDepthTexture;
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

				RenderFluidCompositePass_Internal(
					GraphBuilder,
					View,
					SubsystemPtr,
					SmoothedDepthTexture,
					NormalTexture,
					ThicknessTexture,
					SceneDepthTexture,
					Output
				);

				// Debug Keep Alive
				GraphBuilder.
					QueueTextureExtraction(Output.Texture, &GFluidCompositeDebug_KeepAlive);

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
	RenderFluidDepthPass(GraphBuilder, View, SubsystemPtr, DepthTexture);
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
	float DepthFalloff = SubsystemPtr->RenderingParameters.DepthThreshold;

	RenderFluidSmoothingPass(GraphBuilder, View, InputDepthTexture, OutSmoothedDepthTexture,
	                         BlurRadius, DepthFalloff);
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
