// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shading/KawaiiRayMarchShadingImpl.h"
#include "Rendering/FluidRenderingParameters.h"
#include "Rendering/MetaballRenderingData.h"
#include "Rendering/Shaders/FluidRayMarchShaders.h"
#include "Rendering/Shaders/FluidRayMarchGBufferShaders.h"
#include "Rendering/Shaders/FluidTransparencyShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "SceneView.h"
#include "ScenePrivate.h"
#include "SceneTextures.h"
#include "GlobalShader.h"
#include "RHIStaticStates.h"

void KawaiiRayMarchShading::RenderGBufferShading(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const FRayMarchingPipelineData& PipelineData,
	FRDGTextureRef SceneDepthTexture)
{
	// Skeleton: RayMarching + GBuffer not fully implemented
	UE_LOG(LogTemp, Warning, TEXT("KawaiiRayMarchShading::RenderGBufferShading - Not fully implemented (skeleton)"));
}

void KawaiiRayMarchShading::RenderTranslucentGBufferWrite(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const FRayMarchingPipelineData& PipelineData,
	FRDGTextureRef SceneDepthTexture)
{
	if (!PipelineData.IsValid() || !SceneDepthTexture)
	{
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
		UE_LOG(LogTemp, Error, TEXT("KawaiiRayMarchShading: Missing GBuffer textures"));
		return;
	}

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidRayMarchGBufferParameters>();

	// Particle data (FKawaiiRenderParticle buffer)
	PassParameters->RenderParticles = PipelineData.ParticleBufferSRV;
	PassParameters->ParticleCount = PipelineData.ParticleCount;
	PassParameters->ParticleRadius = PipelineData.ParticleRadius;

	// Ray marching parameters
	PassParameters->SDFSmoothness = RenderParams.SDFSmoothness;
	PassParameters->MaxRayMarchSteps = RenderParams.MaxRayMarchSteps;
	PassParameters->RayMarchHitThreshold = RenderParams.RayMarchHitThreshold;
	PassParameters->RayMarchMaxDistance = RenderParams.RayMarchMaxDistance;

	// Material parameters
	PassParameters->FluidBaseColor = FVector3f(RenderParams.FluidColor.R, RenderParams.FluidColor.G, RenderParams.FluidColor.B);
	PassParameters->Metallic = RenderParams.Metallic;
	PassParameters->Roughness = RenderParams.Roughness;
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;

	// Scene depth
	PassParameters->FluidSceneDepthTex = SceneDepthTexture;
	PassParameters->FluidSceneTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// SDF Volume (if using optimization)
	const bool bUseSDFVolume = PipelineData.SDFVolumeData.IsValid();
	if (bUseSDFVolume)
	{
		PassParameters->SDFVolumeTexture = PipelineData.SDFVolumeData.SDFVolumeTextureSRV;
		PassParameters->SDFVolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SDFVolumeMin = PipelineData.SDFVolumeData.VolumeMin;
		PassParameters->SDFVolumeMax = PipelineData.SDFVolumeData.VolumeMax;
		PassParameters->SDFVolumeResolution = PipelineData.SDFVolumeData.VolumeResolution;
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

	// Depth/Stencil binding
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneDepthTexture,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ELoad,
		FExclusiveDepthStencil::DepthWrite_StencilWrite);

	// Get shaders
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidRayMarchGBufferVS> VertexShader(GlobalShaderMap);

	FFluidRayMarchGBufferPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FUseSDFVolumeGBufferDim>(bUseSDFVolume);
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

			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

			// Depth test + write, AND stencil write = 0x01
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
				true, CF_DepthNearOrEqual,
				true, CF_Always,
				SO_Keep, SO_Keep, SO_Replace,
				false, CF_Always,
				SO_Keep, SO_Keep, SO_Keep,
				0xFF, 0xFF
			>::GetRHI();

			RHICmdList.SetStencilRef(TranslucentStencilRef);

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);
			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *PassParameters);

			RHICmdList.DrawPrimitive(0, 1, 1);
		});

	UE_LOG(LogTemp, Log, TEXT("KawaiiRayMarchShading: Translucent GBuffer write executed"));
}

void KawaiiRayMarchShading::RenderTranslucentTransparency(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output,
	FRDGTextureRef GBufferATexture,
	FRDGTextureRef GBufferDTexture)
{
	if (!SceneColorTexture || !SceneDepthTexture || !GBufferATexture || !GBufferDTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiRayMarchShading: Missing textures for transparency"));
		return;
	}

	if (!Output.IsValid())
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "TranslucentShading_Transparency");

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidTransparencyParameters>();

	// Lit scene color
	PassParameters->LitSceneColorTexture = SceneColorTexture;
	PassParameters->SceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// Scene depth
	PassParameters->FluidSceneDepthTex = SceneDepthTexture;
	PassParameters->DepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// GBuffer A/D
	PassParameters->FluidGBufferATex = GBufferATexture;
	PassParameters->FluidGBufferDTex = GBufferDTexture;
	PassParameters->GBufferSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// Transparency parameters
	PassParameters->RefractiveIndex = RenderParams.RefractiveIndex;
	PassParameters->RefractionStrength = 0.05f;
	PassParameters->Opacity = 0.7f;
	PassParameters->FresnelStrength = RenderParams.FresnelStrength;
	PassParameters->TintColor = FVector3f(RenderParams.FluidColor.R, RenderParams.FluidColor.G, RenderParams.FluidColor.B);
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;

	// Viewport info
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	FIntRect ViewRect = Output.ViewRect;

	PassParameters->OutputViewRect = FVector2f(ViewRect.Width(), ViewRect.Height());
	PassParameters->OutputViewRectMin = FVector2f(ViewRect.Min.X, ViewRect.Min.Y);
	PassParameters->OutputTextureSize = FVector2f(Output.Texture->Desc.Extent.X, Output.Texture->Desc.Extent.Y);

	PassParameters->GBufferViewRect = FVector2f(ViewRect.Width(), ViewRect.Height());
	PassParameters->GBufferViewRectMin = FVector2f(ViewRect.Min.X, ViewRect.Min.Y);
	PassParameters->GBufferTextureSize = FVector2f(GBufferATexture->Desc.Extent.X, GBufferATexture->Desc.Extent.Y);

	PassParameters->SceneViewRect = FVector2f(ViewRect.Width(), ViewRect.Height());
	PassParameters->SceneTextureSize = FVector2f(SceneColorTexture->Desc.Extent.X, SceneColorTexture->Desc.Extent.Y);

	PassParameters->ViewportSize = FVector2f(ViewRect.Width(), ViewRect.Height());
	PassParameters->InverseViewportSize = FVector2f(1.0f / ViewRect.Width(), 1.0f / ViewRect.Height());

	// View uniforms
	PassParameters->View = View.ViewUniformBuffer;

	// Render target
	PassParameters->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);

	// Depth/Stencil
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneDepthTexture,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ELoad,
		FExclusiveDepthStencil::DepthRead_StencilRead);

	// Get shaders
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidTransparencyVS> VertexShader(GlobalShaderMap);
	TShaderMapRef<FFluidTransparencyPS> PixelShader(GlobalShaderMap);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("TranslucentShading_Transparency"),
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

			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

			// Stencil test: only render where stencil == TranslucentStencilRef
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
				false, CF_Always,
				true, CF_Equal,
				SO_Keep, SO_Keep, SO_Keep,
				false, CF_Always,
				SO_Keep, SO_Keep, SO_Keep,
				0xFF, 0x00
			>::GetRHI();

			RHICmdList.SetStencilRef(TranslucentStencilRef);

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

			RHICmdList.DrawPrimitive(0, 1, 1);
		});

	UE_LOG(LogTemp, Log, TEXT("KawaiiRayMarchShading: Translucent transparency pass executed"));
}

void KawaiiRayMarchShading::RenderPostProcessShading(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const FRayMarchingPipelineData& PipelineData,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	const bool bUseSDFVolume = PipelineData.SDFVolumeData.IsValid();

	if (!bUseSDFVolume && !PipelineData.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiRayMarchShading: No particle data for PostProcess"));
		return;
	}

	if (!SceneDepthTexture || !SceneColorTexture || !Output.IsValid())
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballShading_PostProcess_RayMarching");

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidRayMarchPS::FParameters>();

	// Particle data (FKawaiiRenderParticle buffer)
	PassParameters->RenderParticles = PipelineData.ParticleBufferSRV;
	PassParameters->ParticleCount = PipelineData.ParticleCount;
	PassParameters->ParticleRadius = PipelineData.ParticleRadius;

	// SDF Volume data
	if (bUseSDFVolume)
	{
		PassParameters->SDFVolumeTexture = PipelineData.SDFVolumeData.SDFVolumeTextureSRV;
		PassParameters->SDFVolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SDFVolumeMin = PipelineData.SDFVolumeData.VolumeMin;
		PassParameters->SDFVolumeMax = PipelineData.SDFVolumeData.VolumeMax;
		PassParameters->SDFVolumeResolution = PipelineData.SDFVolumeData.VolumeResolution;
	}

	// Ray marching parameters
	PassParameters->SDFSmoothness = RenderParams.SDFSmoothness;
	PassParameters->MaxRayMarchSteps = RenderParams.MaxRayMarchSteps;
	PassParameters->RayMarchHitThreshold = RenderParams.RayMarchHitThreshold;
	PassParameters->RayMarchMaxDistance = RenderParams.RayMarchMaxDistance;

	// Appearance parameters
	PassParameters->FluidColor = RenderParams.FluidColor;
	PassParameters->FresnelStrength = RenderParams.FresnelStrength;
	PassParameters->RefractiveIndex = RenderParams.RefractiveIndex;
	PassParameters->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;
	PassParameters->SpecularStrength = RenderParams.SpecularStrength;
	PassParameters->SpecularRoughness = RenderParams.SpecularRoughness;
	PassParameters->EnvironmentLightColor = RenderParams.EnvironmentLightColor;

	// SSS parameters
	PassParameters->SSSIntensity = RenderParams.SSSIntensity;
	PassParameters->SSSColor = RenderParams.SSSColor;

	// Scene textures
	PassParameters->SceneDepthTexture = SceneDepthTexture;
	PassParameters->SceneColorTexture = SceneColorTexture;
	PassParameters->SceneTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// View uniforms
	PassParameters->View = View.ViewUniformBuffer;

	// View matrices
	PassParameters->InverseViewMatrix = FMatrix44f(View.ViewMatrices.GetInvViewMatrix());
	PassParameters->InverseProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvProjectionMatrix());
	PassParameters->ViewMatrix = FMatrix44f(View.ViewMatrices.GetViewMatrix());
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionMatrix());

	// Viewport size
	FIntRect ViewRect = Output.ViewRect;
	PassParameters->ViewportSize = FVector2f(ViewRect.Width(), ViewRect.Height());

	// SceneDepth UV transform
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	PassParameters->SceneViewRect = FVector2f(ViewInfo.ViewRect.Width(), ViewInfo.ViewRect.Height());
	PassParameters->SceneTextureSize = FVector2f(SceneDepthTexture->Desc.Extent.X, SceneDepthTexture->Desc.Extent.Y);

	// Render target
	PassParameters->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);

	// Get shaders
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidRayMarchVS> VertexShader(GlobalShaderMap);

	FFluidRayMarchPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FUseSDFVolumeDim>(bUseSDFVolume);
	TShaderMapRef<FFluidRayMarchPS> PixelShader(GlobalShaderMap, PermutationVector);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MetaballPostProcess_RayMarching (%s)", bUseSDFVolume ? TEXT("SDFVolume") : TEXT("Direct")),
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

			// Alpha blending
			GraphicsPSOInit.BlendState = TStaticBlendState<
				CW_RGBA,
				BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
				BO_Add, BF_Zero, BF_One
			>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

			RHICmdList.DrawPrimitive(0, 1, 1);
		});
}
