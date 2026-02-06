// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/Shading/FluidCompositeShading.h"
#include "Rendering/FluidRenderingParameters.h"
#include "Rendering/MetaballRenderingData.h"
#include "Rendering/FluidCompositeShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "SceneView.h"
#include "ScenePrivate.h"
#include "GlobalShader.h"
#include "RHIStaticStates.h"
#include "TextureResource.h"
#include "HAL/IConsoleManager.h"
#include "EngineGlobals.h"

void KawaiiScreenSpaceShading::RenderPostProcessShading(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const FMetaballIntermediateTextures& IntermediateTextures,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	// Validate input textures
	if (!IntermediateTextures.SmoothedDepthTexture || !SceneDepthTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiScreenSpaceShading::RenderPostProcessShading: Missing required textures (Depth or SceneDepth)"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballShading_PostProcess_ScreenSpace");

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidCompositePS::FParameters>();

	// Texture bindings
	PassParameters->FluidDepthTexture = IntermediateTextures.SmoothedDepthTexture;

	if (IntermediateTextures.NormalTexture)
	{
		PassParameters->FluidNormalTexture = IntermediateTextures.NormalTexture;
	}
	else
	{
		FRDGTextureDesc DummyDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, 1), PF_A32B32G32R32F, FClearValueBinding(FLinearColor(0, 0, 1, 0)),
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		FRDGTextureRef DummyTexture = GraphBuilder.CreateTexture(DummyDesc, TEXT("DummyNormal"));
		AddClearRenderTargetPass(GraphBuilder, DummyTexture, FLinearColor(0, 0, 1, 0));
		PassParameters->FluidNormalTexture = DummyTexture;
	}

	if (IntermediateTextures.ThicknessTexture)
	{
		PassParameters->FluidThicknessTexture = IntermediateTextures.ThicknessTexture;
	}
	else
	{
		FRDGTextureDesc DummyDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, 1), PF_R32_FLOAT, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		FRDGTextureRef DummyTexture = GraphBuilder.CreateTexture(DummyDesc, TEXT("DummyThickness"));
		AddClearRenderTargetPass(GraphBuilder, DummyTexture, FLinearColor::Black);
		PassParameters->FluidThicknessTexture = DummyTexture;
	}

	if (IntermediateTextures.OcclusionMaskTexture)
	{
		PassParameters->OcclusionMaskTexture = IntermediateTextures.OcclusionMaskTexture;
	}
	else
	{
		FRDGTextureDesc DummyDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, 1), PF_R32_FLOAT, FClearValueBinding::White,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		FRDGTextureRef DummyTexture = GraphBuilder.CreateTexture(DummyDesc, TEXT("DummyOcclusionMask"));
		AddClearRenderTargetPass(GraphBuilder, DummyTexture, FLinearColor::White);
		PassParameters->OcclusionMaskTexture = DummyTexture;
	}

	PassParameters->SceneDepthTexture = SceneDepthTexture;

	if (SceneColorTexture)
	{
		PassParameters->SceneColorTexture = SceneColorTexture;
	}
	else
	{
		FRDGTextureDesc DummyDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		FRDGTextureRef DummyTexture = GraphBuilder.CreateTexture(DummyDesc, TEXT("DummySceneColor"));
		AddClearRenderTargetPass(GraphBuilder, DummyTexture, FLinearColor::Black);
		PassParameters->SceneColorTexture = DummyTexture;
	}

	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->InputSampler = TStaticSamplerState<
		SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->PointClampSampler = TStaticSamplerState<
		SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// UV scaling for SceneColor/SceneDepth sampling
	// When Screen Percentage < 100%, ViewRect is smaller than texture size
	FVector2f SceneUVScale(1.0f, 1.0f);
	FVector2f SceneUVOffset(0.0f, 0.0f);
	if (SceneColorTexture)
	{
		FIntPoint TextureSize = SceneColorTexture->Desc.Extent;
		SceneUVScale = FVector2f(
			static_cast<float>(Output.ViewRect.Width()) / static_cast<float>(TextureSize.X),
			static_cast<float>(Output.ViewRect.Height()) / static_cast<float>(TextureSize.Y));

		SceneUVOffset = FVector2f(
			static_cast<float>(Output.ViewRect.Min.X) / static_cast<float>(TextureSize.X),
			static_cast<float>(Output.ViewRect.Min.Y) / static_cast<float>(TextureSize.Y));
	}
	PassParameters->SceneUVScale = SceneUVScale;
	PassParameters->SceneUVOffset = SceneUVOffset;

	// UV scaling and offset for Fluid textures (Depth, Normal, Thickness)
	// These textures are sized to SceneDepth extent but only contain data in ViewRect.
	FVector2f FluidUVScale(1.0f, 1.0f);
	FVector2f FluidUVOffset(0.0f, 0.0f);
	if (IntermediateTextures.SmoothedDepthTexture)
	{
		FIntPoint TextureSize = IntermediateTextures.SmoothedDepthTexture->Desc.Extent;
		FluidUVScale = FVector2f(
			static_cast<float>(Output.ViewRect.Width()) / static_cast<float>(TextureSize.X),
			static_cast<float>(Output.ViewRect.Height()) / static_cast<float>(TextureSize.Y));
		
		FluidUVOffset = FVector2f(
			static_cast<float>(Output.ViewRect.Min.X) / static_cast<float>(TextureSize.X),
			static_cast<float>(Output.ViewRect.Min.Y) / static_cast<float>(TextureSize.Y));
	}
	PassParameters->FluidUVScale = FluidUVScale;
	PassParameters->FluidUVOffset = FluidUVOffset;

	// View matrices
	PassParameters->InverseProjectionMatrix =
		FMatrix44f(View.ViewMatrices.GetInvProjectionMatrix());
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionMatrix());
	PassParameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetViewMatrix());

	// Rendering parameters
	PassParameters->FluidColor = RenderParams.FluidColor;
	PassParameters->FresnelStrength = RenderParams.FresnelStrength;
	PassParameters->RefractiveIndex = RenderParams.RefractiveIndex;
	PassParameters->Opacity = RenderParams.AbsorptionStrength;
	// Auto-calculate absorption coefficients from FluidColor (Beer's Law)
	// Formula: μ = -log(color) where color is the perceived color at unit thickness
	// Brighter color channels = less absorption of that wavelength
	// Clamped to prevent infinity when color approaches 0
	constexpr float MinColor = 0.001f;
	PassParameters->AbsorptionColorCoefficients = FLinearColor(
		-FMath::Loge(FMath::Max(RenderParams.FluidColor.R, MinColor)),
		-FMath::Loge(FMath::Max(RenderParams.FluidColor.G, MinColor)),
		-FMath::Loge(FMath::Max(RenderParams.FluidColor.B, MinColor)),
		1.0f);
	PassParameters->SpecularStrength = RenderParams.SpecularStrength;
	PassParameters->SpecularRoughness = RenderParams.SpecularRoughness;
	PassParameters->AmbientIntensity = RenderParams.AmbientIntensity;
	PassParameters->LightingScale = RenderParams.LightingScale;

	// Lighting scale parameters
	PassParameters->ThicknessSensitivity = RenderParams.ThicknessSensitivity;
	PassParameters->bEnableThicknessClamping = RenderParams.bEnableThicknessClamping ? 1 : 0;
	PassParameters->ThicknessMin = RenderParams.ThicknessMin;
	PassParameters->ThicknessMax = RenderParams.ThicknessMax;
	PassParameters->FresnelReflectionBlend = RenderParams.FresnelReflectionBlend;

	// Refraction parameters
	PassParameters->bEnableRefraction = RenderParams.bEnableRefraction ? 1 : 0;
	PassParameters->RefractionScale = RenderParams.RefractionScale;

	// Caustic parameters
	PassParameters->bEnableCaustics = RenderParams.bEnableCaustics ? 1 : 0;
	PassParameters->CausticIntensity = RenderParams.CausticIntensity;

	// Reflection Cubemap
	if (RenderParams.ReflectionCubemap && RenderParams.ReflectionCubemap->GetResource())
	{
		PassParameters->ReflectionCubemap = RenderParams.ReflectionCubemap->GetResource()->TextureRHI;
		PassParameters->ReflectionCubemapSampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
		PassParameters->bUseReflectionCubemap = 1;
	}
	else
	{
		// Fallback: black texture (ignored by bUseReflectionCubemap=0 flag)
		PassParameters->ReflectionCubemap = GBlackTextureCube->TextureRHI;
		PassParameters->ReflectionCubemapSampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
		PassParameters->bUseReflectionCubemap = 0;
	}
	PassParameters->ReflectionIntensity = RenderParams.ReflectionIntensity;
	PassParameters->ReflectionMipLevel = RenderParams.ReflectionMipLevel;

	// Multi-Light parameters
	// ========================================================================
	// NumLights = 0 means fallback to View.DirectionalLight (single main light).
	// To use multiple lights, populate the packed float4 arrays from scene or external source.
	// - LightDirectionsAndIntensity[i] = FVector4f(Direction.X, Direction.Y, Direction.Z, Intensity)
	// - LightColors[i] = FVector4f(Color.R, Color.G, Color.B, 0.0f)
	// ========================================================================
	PassParameters->NumLights = 0;  // Default: use View.DirectionalLight fallback

	for (int32 i = 0; i < FLUID_MAX_LIGHTS; ++i)
	{
		PassParameters->LightDirectionsAndIntensity[i] = FVector4f(0, 0, 0, 0);
		PassParameters->LightColors[i] = FVector4f(0, 0, 0, 0);
	}

	// TODO: Populate lights from FScene if needed
	// Example future implementation:
	// const FScene* Scene = static_cast<const FViewInfo&>(View).Family->Scene->GetRenderScene();
	// for (int32 i = 0; i < FMath::Min(NumSceneLights, FLUID_MAX_LIGHTS); i++)
	// {
	//     FVector3f Dir = GetLightDirection(i);
	//     float Intensity = GetLightIntensity(i);
	//     FLinearColor Color = GetLightColor(i);
	//     PassParameters->LightDirectionsAndIntensity[i] = FVector4f(Dir.X, Dir.Y, Dir.Z, Intensity);
	//     PassParameters->LightColors[i] = FVector4f(Color.R, Color.G, Color.B, 0.0f);
	//     PassParameters->NumLights++;
	// }

	// Reflection mode parameter (0=None, 1=Cubemap, 2=SSR, 3=SSR+Cubemap)
	PassParameters->ReflectionMode = static_cast<int32>(RenderParams.ReflectionMode);
	PassParameters->ScreenSpaceReflectionMaxSteps = RenderParams.ScreenSpaceReflectionMaxSteps;
	PassParameters->ScreenSpaceReflectionStepSize = RenderParams.ScreenSpaceReflectionStepSize;
	PassParameters->ScreenSpaceReflectionThickness = RenderParams.ScreenSpaceReflectionThickness;
	PassParameters->ScreenSpaceReflectionIntensity = RenderParams.ScreenSpaceReflectionIntensity;
	PassParameters->ScreenSpaceReflectionEdgeFade = RenderParams.ScreenSpaceReflectionEdgeFade;
	PassParameters->ViewportSize = FVector2f(Output.ViewRect.Width(), Output.ViewRect.Height());

	// Render target (blend over existing scene)
	PassParameters->RenderTargets[0] = FRenderTargetBinding(
		Output.Texture, ERenderTargetLoadAction::ELoad);

	// Get shaders
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidCompositeVS> VertexShader(GlobalShaderMap);
	TShaderMapRef<FFluidCompositePS> PixelShader(GlobalShaderMap);

	// Use Output.ViewRect instead of View.UnscaledViewRect
	FIntRect ViewRect = Output.ViewRect;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MetaballPostProcess_ScreenSpace"),
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
				VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			// Alpha blending
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

			// Draw fullscreen triangle
			RHICmdList.DrawPrimitive(0, 1, 1);
		});
}
