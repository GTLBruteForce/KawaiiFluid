// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shading/KawaiiTranslucentShading.h"
#include "Rendering/Shaders/FluidRayMarchGBufferShaders.h"
#include "Rendering/FluidRenderingParameters.h"
#include "RenderGraphBuilder.h"
#include "ScreenPass.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "ScenePrivate.h"

// Stencil reference value for Translucent mode (same as FluidTransparencyComposite::SlimeStencilRef)
static constexpr uint8 TranslucentStencilRef = 0x01;

void FKawaiiTranslucentShading::RenderForScreenSpacePipeline(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const FMetaballIntermediateTextures& IntermediateTextures,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	// Translucent mode only supports RayMarching pipeline
	UE_LOG(LogTemp, Warning, TEXT("FKawaiiTranslucentShading::RenderForScreenSpacePipeline - ScreenSpace not supported for Translucent mode"));
}

void FKawaiiTranslucentShading::RenderForRayMarchingPipeline(
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
	// Validate inputs
	if (!ParticleBufferSRV || ParticleCount <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiTranslucentShading: No particles to render"));
		return;
	}

	if (!SceneDepthTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiTranslucentShading: Missing scene depth texture"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballShading_RayMarching_Translucent_GBufferWrite");

	// Get GBuffer textures from the view
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	const FSceneTextures& SceneTextures = ViewInfo.GetSceneTextures();

	FRDGTextureRef GBufferATexture = SceneTextures.GBufferA;
	FRDGTextureRef GBufferBTexture = SceneTextures.GBufferB;
	FRDGTextureRef GBufferCTexture = SceneTextures.GBufferC;
	FRDGTextureRef GBufferDTexture = SceneTextures.GBufferD;

	if (!GBufferATexture || !GBufferBTexture || !GBufferCTexture || !GBufferDTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("FKawaiiTranslucentShading: Missing GBuffer textures"));
		return;
	}

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidRayMarchGBufferParameters>();

	// Particle data
	PassParameters->ParticlePositions = ParticleBufferSRV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;

	// Ray marching parameters
	PassParameters->SDFSmoothness = RenderParams.SDFSmoothness;
	PassParameters->MaxRayMarchSteps = RenderParams.MaxRayMarchSteps;
	PassParameters->RayMarchHitThreshold = RenderParams.RayMarchHitThreshold;
	PassParameters->RayMarchMaxDistance = RenderParams.RayMarchMaxDistance;

	// Material parameters for GBuffer
	PassParameters->FluidBaseColor = FVector3f(RenderParams.FluidColor.R, RenderParams.FluidColor.G, RenderParams.FluidColor.B);
	PassParameters->Metallic = RenderParams.Metallic;
	PassParameters->Roughness = RenderParams.Roughness;
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;

	// Scene depth texture
	PassParameters->FluidSceneDepthTex = SceneDepthTexture;
	PassParameters->FluidSceneTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// SDF Volume (if using optimization)
	if (IsUsingSDFVolume())
	{
		PassParameters->SDFVolumeTexture = SDFVolumeData.SDFVolumeTextureSRV;
		PassParameters->SDFVolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SDFVolumeMin = SDFVolumeData.VolumeMin;
		PassParameters->SDFVolumeMax = SDFVolumeData.VolumeMax;
		PassParameters->SDFVolumeResolution = SDFVolumeData.VolumeResolution;
	}

	// SceneDepth UV mapping
	FIntRect ViewRect = ViewInfo.ViewRect;
	PassParameters->SceneViewRect = FVector2f(ViewRect.Width(), ViewRect.Height());
	PassParameters->SceneTextureSize = FVector2f(SceneDepthTexture->Desc.Extent.X, SceneDepthTexture->Desc.Extent.Y);

	// View matrices
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->InverseViewMatrix = FMatrix44f(View.ViewMatrices.GetInvViewMatrix());
	PassParameters->InverseProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvProjectionMatrix());
	PassParameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetViewMatrix());
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionMatrix());
	PassParameters->ViewportSize = FVector2f(ViewRect.Width(), ViewRect.Height());

	// MRT: GBuffer A/B/C/D
	PassParameters->RenderTargets[0] = FRenderTargetBinding(GBufferATexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets[1] = FRenderTargetBinding(GBufferBTexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets[2] = FRenderTargetBinding(GBufferCTexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets[3] = FRenderTargetBinding(GBufferDTexture, ERenderTargetLoadAction::ELoad);

	// Depth/Stencil binding - CRITICAL: Write stencil = 0x01 for Transparency pass
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneDepthTexture,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ELoad,
		FExclusiveDepthStencil::DepthWrite_StencilWrite);

	// Get shaders with SDF Volume permutation
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidRayMarchGBufferVS> VertexShader(GlobalShaderMap);

	FFluidRayMarchGBufferPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FUseSDFVolumeGBufferDim>(IsUsingSDFVolume());
	TShaderMapRef<FFluidRayMarchGBufferPS> PixelShader(GlobalShaderMap, PermutationVector);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MetaballTranslucent_RayMarch_GBufferWrite"),
		PassParameters,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, PassParameters, ViewRect](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);
			RHICmdList.SetScissorRect(true, ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Max.X, ViewRect.Max.Y);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			// Opaque blending for GBuffer write
			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

			// Depth test + write, AND stencil write = 0x01 for Transparency pass
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
				true, CF_DepthNearOrEqual,                 // Depth: write enabled, pass if near or equal
				true, CF_Always,                           // Front stencil: enabled, always pass
				SO_Keep, SO_Keep, SO_Replace,              // Stencil ops: keep/keep/replace (write on depth pass)
				false, CF_Always,                          // Back stencil: disabled
				SO_Keep, SO_Keep, SO_Keep,
				0xFF, 0xFF                                 // Read/write masks: full
			>::GetRHI();

			// Set stencil reference to mark translucent regions
			RHICmdList.SetStencilRef(TranslucentStencilRef);

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);
			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *PassParameters);

			// Draw fullscreen triangle
			RHICmdList.DrawPrimitive(0, 1, 1);
		});

	UE_LOG(LogTemp, Log, TEXT("FKawaiiTranslucentShading: RayMarching GBuffer write executed (Stencil=0x%02X), ParticleCount=%d"),
		TranslucentStencilRef, ParticleCount);
}
