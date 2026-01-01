// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shading/KawaiiGBufferShading.h"
#include "Rendering/Shaders/FluidGBufferWriteShaders.h"
#include "Rendering/FluidRenderingParameters.h"
#include "RenderGraphBuilder.h"
#include "ScreenPass.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "ScenePrivate.h"

void FKawaiiGBufferShading::RenderForScreenSpacePipeline(
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
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiGBufferShading: Missing input textures"));
		return;
	}

	// Validate GBuffer textures
	if (!IntermediateTextures.GBufferATexture ||
		!IntermediateTextures.GBufferBTexture ||
		!IntermediateTextures.GBufferCTexture ||
		!IntermediateTextures.GBufferDTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("FKawaiiGBufferShading: Missing GBuffer textures!"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballShading_ScreenSpace_GBuffer");

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

	UE_LOG(LogTemp, Log, TEXT("FKawaiiGBufferShading: GBuffer write executed successfully"));
}

void FKawaiiGBufferShading::RenderForRayMarchingPipeline(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	FRDGBufferSRVRef ParticleBufferSRV,
	int32 ParticleCount,
	float ParticleRadius,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	// Skeleton: RayMarching + GBuffer not implemented yet
	UE_LOG(LogTemp, Warning, TEXT("FKawaiiGBufferShading::RenderForRayMarchingPipeline - Not implemented (skeleton)"));
}
