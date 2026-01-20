// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shading/KawaiiScreenSpaceShadingImpl.h"
#include "Rendering/FluidRenderingParameters.h"
#include "Rendering/MetaballRenderingData.h"
#include "Rendering/FluidCompositeShaders.h"
#include "Rendering/Shaders/FluidGBufferWriteShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "SceneView.h"
#include "ScenePrivate.h"
#include "GlobalShader.h"
#include "RHIStaticStates.h"

static TAutoConsoleVariable<int32> CVarFluidSSRDebugMode(
	TEXT("r.Fluid.SSRDebugMode"),
	0,
	TEXT("SSR Debug visualization mode for fluid rendering.\n")
	TEXT("0: None (normal rendering)\n")
	TEXT("1: Hit/Miss (Red=hit, Blue=miss)\n")
	TEXT("2: Reflection direction\n")
	TEXT("3: Hit sample color\n")
	TEXT("4: Reflection Z (Green=into scene, Red=toward camera)\n")
	TEXT("5: 3D Hit test (R=XY distance, G=penetrated, B=not penetrated)\n")
	TEXT("6: Exit reason (Red=behind camera, Green=off screen, Blue=max steps)\n")
	TEXT("7: Normal visualization\n")
	TEXT("8: ViewDir visualization\n")
	TEXT("9: ViewPos.z visualization\n")
	TEXT("10: Fresnel value\n")
	TEXT("11: FresnelStrength/ReflectionBlend/Fresnel\n")
	TEXT("12: Final reflection blend factor\n")
	TEXT("13: XY distance detail (Cyan=XY far, Red=close but no penetration)"),
	ECVF_RenderThreadSafe);

void KawaiiScreenSpaceShading::RenderGBufferShading(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const FMetaballIntermediateTextures& IntermediateTextures,
	FRDGTextureRef SceneDepthTexture)
{
	// Validate input textures
	if (!IntermediateTextures.SmoothedDepthTexture ||
		!IntermediateTextures.NormalTexture ||
		!IntermediateTextures.ThicknessTexture ||
		!SceneDepthTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiScreenSpaceShading::RenderGBufferShading: Missing input textures"));
		return;
	}

	// Validate GBuffer textures
	if (!IntermediateTextures.GBufferATexture ||
		!IntermediateTextures.GBufferBTexture ||
		!IntermediateTextures.GBufferCTexture ||
		!IntermediateTextures.GBufferDTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("KawaiiScreenSpaceShading::RenderGBufferShading: Missing GBuffer textures!"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballShading_GBuffer_ScreenSpace");

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidGBufferWriteParameters>();

	// Texture bindings
	PassParameters->SmoothedDepthTexture = IntermediateTextures.SmoothedDepthTexture;
	PassParameters->NormalTexture = IntermediateTextures.NormalTexture;
	PassParameters->ThicknessTexture = IntermediateTextures.ThicknessTexture;
	PassParameters->FluidSceneDepthTexture = SceneDepthTexture;

	// Samplers
	PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->BilinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// Material parameters
	PassParameters->FluidBaseColor = FVector3f(RenderParams.FluidColor.R, RenderParams.FluidColor.G, RenderParams.FluidColor.B);
	PassParameters->Metallic = RenderParams.Metallic;
	PassParameters->Roughness = RenderParams.Roughness;
	PassParameters->SubsurfaceOpacity = RenderParams.SubsurfaceOpacity;
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;

	// View uniforms
	PassParameters->View = View.ViewUniformBuffer;

	// MRT: GBuffer A/B/C/D
	PassParameters->RenderTargets[0] = FRenderTargetBinding(
		IntermediateTextures.GBufferATexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets[1] = FRenderTargetBinding(
		IntermediateTextures.GBufferBTexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets[2] = FRenderTargetBinding(
		IntermediateTextures.GBufferCTexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets[3] = FRenderTargetBinding(
		IntermediateTextures.GBufferDTexture, ERenderTargetLoadAction::ELoad);

	// Depth/Stencil binding (write custom depth)
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneDepthTexture,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ELoad,
		FExclusiveDepthStencil::DepthWrite_StencilWrite);

	// Get shaders
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidGBufferWriteVS> VertexShader(GlobalShaderMap);
	TShaderMapRef<FFluidGBufferWritePS> PixelShader(GlobalShaderMap);

	// Use ViewInfo.ViewRect for GBuffer mode
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	FIntRect ViewRect = ViewInfo.ViewRect;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MetaballGBuffer_ScreenSpace"),
		PassParameters,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, PassParameters, ViewRect](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X,
			                       ViewRect.Max.Y, 1.0f);
			RHICmdList.SetScissorRect(true, ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Max.X,
			                          ViewRect.Max.Y);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			// Opaque blending for GBuffer write
			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

			// Write depth
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
				true, CF_DepthNearOrEqual>::GetRHI();

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(),
			                    *PassParameters);
			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(),
			                    *PassParameters);

			// Draw fullscreen triangle
			RHICmdList.DrawPrimitive(0, 1, 1);
		});

	UE_LOG(LogTemp, Log, TEXT("KawaiiScreenSpaceShading: GBuffer write executed successfully"));
}

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
	if (!IntermediateTextures.SmoothedDepthTexture ||
		!IntermediateTextures.NormalTexture ||
		!IntermediateTextures.ThicknessTexture ||
		!SceneDepthTexture)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballShading_PostProcess_ScreenSpace");

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidCompositePS::FParameters>();

	// Texture bindings
	PassParameters->FluidDepthTexture = IntermediateTextures.SmoothedDepthTexture;
	PassParameters->FluidNormalTexture = IntermediateTextures.NormalTexture;
	PassParameters->FluidThicknessTexture = IntermediateTextures.ThicknessTexture;
	PassParameters->SceneDepthTexture = SceneDepthTexture;
	PassParameters->SceneColorTexture = SceneColorTexture;
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->InputSampler = TStaticSamplerState<
		SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->PointClampSampler = TStaticSamplerState<
		SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// UV scaling for SceneColor/SceneDepth sampling
	// When Screen Percentage < 100%, ViewRect is smaller than texture size
	FVector2f SceneUVScale(1.0f, 1.0f);
	if (SceneColorTexture)
	{
		FIntPoint TextureSize = SceneColorTexture->Desc.Extent;
		SceneUVScale = FVector2f(
			(float)Output.ViewRect.Width() / (float)TextureSize.X,
			(float)Output.ViewRect.Height() / (float)TextureSize.Y);
	}
	PassParameters->SceneUVScale = SceneUVScale;

	// View matrices
	PassParameters->InverseProjectionMatrix =
		FMatrix44f(View.ViewMatrices.GetInvProjectionMatrix());
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionNoAAMatrix());
	PassParameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetViewMatrix());

	// Rendering parameters
	PassParameters->FluidColor = RenderParams.FluidColor;
	PassParameters->F0Override = RenderParams.F0Override;
	PassParameters->FresnelStrength = RenderParams.FresnelStrength;
	PassParameters->RefractiveIndex = RenderParams.RefractiveIndex;
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;
	PassParameters->AbsorptionColorCoefficients = RenderParams.AbsorptionColorCoefficients;
	PassParameters->SpecularStrength = RenderParams.SpecularStrength;
	PassParameters->SpecularRoughness = RenderParams.SpecularRoughness;
	PassParameters->EnvironmentLightColor = RenderParams.EnvironmentLightColor;

	// Lighting scale parameters
	PassParameters->AmbientScale = RenderParams.AmbientScale;
	PassParameters->TransmittanceScale = RenderParams.TransmittanceScale;
	PassParameters->AlphaThicknessScale = RenderParams.AlphaThicknessScale;
	PassParameters->RefractionScale = RenderParams.RefractionScale;
	PassParameters->FresnelReflectionBlend = RenderParams.FresnelReflectionBlend;

	// Subsurface Scattering (SSS)
	PassParameters->SSSIntensity = RenderParams.SSSIntensity;
	PassParameters->SSSColor = RenderParams.SSSColor;

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

	// SSR parameters
	PassParameters->bEnableSSR = RenderParams.bEnableSSR ? 1 : 0;
	PassParameters->SSRMaxSteps = RenderParams.SSRMaxSteps;
	PassParameters->SSRStepSize = RenderParams.SSRStepSize;
	PassParameters->SSRThickness = RenderParams.SSRThickness;
	PassParameters->SSRIntensity = RenderParams.SSRIntensity;
	PassParameters->SSREdgeFade = RenderParams.SSREdgeFade;
	// CVar override takes priority if set (non-zero)
	int32 DebugModeFromCVar = CVarFluidSSRDebugMode.GetValueOnRenderThread();
	PassParameters->SSRDebugMode = (DebugModeFromCVar > 0) ? DebugModeFromCVar : static_cast<int32>(RenderParams.SSRDebugMode);
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
