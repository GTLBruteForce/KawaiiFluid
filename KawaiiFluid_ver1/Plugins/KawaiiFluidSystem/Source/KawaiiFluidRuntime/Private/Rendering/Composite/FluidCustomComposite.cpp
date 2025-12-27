// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Composite/FluidCustomComposite.h"
#include "Rendering/FluidCompositeShaders.h"
#include "Rendering/FluidRenderingParameters.h"
#include "RenderGraphBuilder.h"
#include "ScreenPass.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"

void FFluidCustomComposite::RenderComposite(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const FFluidIntermediateTextures& IntermediateTextures,
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

	RDG_EVENT_SCOPE(GraphBuilder, "FluidComposite_Custom");

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

	// View matrices
	PassParameters->InverseProjectionMatrix =
		FMatrix44f(View.ViewMatrices.GetInvProjectionMatrix());
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionMatrix());
	PassParameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetViewMatrix());

	// Rendering parameters
	PassParameters->FluidColor = RenderParams.FluidColor;
	PassParameters->FresnelStrength = RenderParams.FresnelStrength;
	PassParameters->RefractiveIndex = RenderParams.RefractiveIndex;
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;
	PassParameters->SpecularStrength = RenderParams.SpecularStrength;
	PassParameters->SpecularRoughness = RenderParams.SpecularRoughness;
	PassParameters->EnvironmentLightColor = RenderParams.EnvironmentLightColor;

	// Render target (blend over existing scene)
	PassParameters->RenderTargets[0] = FRenderTargetBinding(
		Output.Texture, ERenderTargetLoadAction::ELoad);

	// Get shaders
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
				VertexDeclarationRHI; // No input layout
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

			// Draw fullscreen triangle (VS generates coords from VertexID)
			RHICmdList.DrawPrimitive(0, 1, 1);
		});
}
