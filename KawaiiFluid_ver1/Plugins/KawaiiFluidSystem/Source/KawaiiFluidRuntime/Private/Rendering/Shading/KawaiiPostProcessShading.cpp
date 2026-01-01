// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shading/KawaiiPostProcessShading.h"
#include "Rendering/FluidCompositeShaders.h"
#include "Rendering/Shaders/FluidRayMarchShaders.h"
#include "Rendering/FluidRenderingParameters.h"
#include "RenderGraphBuilder.h"
#include "ScreenPass.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ScenePrivate.h"

void FKawaiiPostProcessShading::RenderForScreenSpacePipeline(
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

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballShading_ScreenSpace_PostProcess");

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
	PassParameters->ProjectionMatrix = FMatrix44f(View.ViewMatrices.GetProjectionNoAAMatrix());
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

void FKawaiiPostProcessShading::RenderForRayMarchingPipeline(
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
	const bool bUseSDFVolume = SDFVolumeData.IsValid();

	UE_LOG(LogTemp, Log, TEXT("FKawaiiPostProcessShading::RenderForRayMarchingPipeline - Particles: %d, Radius: %.2f, UseSDFVolume: %s"),
		ParticleCount, ParticleRadius, bUseSDFVolume ? TEXT("true") : TEXT("false"));

	// Validate based on rendering mode
	if (bUseSDFVolume)
	{
		// SDF Volume mode: need volume texture
		if (!SDFVolumeData.SDFVolumeTextureSRV)
		{
			UE_LOG(LogTemp, Warning, TEXT("FKawaiiPostProcessShading: SDF Volume mode enabled but no volume data set"));
			return;
		}
	}
	else
	{
		// Legacy mode: need particle data
		if (!ParticleBufferSRV || ParticleCount <= 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("FKawaiiPostProcessShading: No particle data set"));
			return;
		}
	}

	if (!SceneDepthTexture || !SceneColorTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiPostProcessShading: Missing scene textures"));
		return;
	}

	if (!Output.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiPostProcessShading: Invalid output target"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballShading_RayMarching_PostProcess");

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidRayMarchPS::FParameters>();

	// Particle data
	PassParameters->ParticlePositions = ParticleBufferSRV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->ParticleRadius = ParticleRadius;

	// SDF Volume data (for optimized mode)
	if (bUseSDFVolume)
	{
		PassParameters->SDFVolumeTexture = SDFVolumeData.SDFVolumeTextureSRV;
		PassParameters->SDFVolumeSampler = TStaticSamplerState<
			SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SDFVolumeMin = SDFVolumeData.VolumeMin;
		PassParameters->SDFVolumeMax = SDFVolumeData.VolumeMax;
		PassParameters->SDFVolumeResolution = SDFVolumeData.VolumeResolution;

		UE_LOG(LogTemp, Log, TEXT("FKawaiiPostProcessShading: SDF Volume - Min:(%.1f,%.1f,%.1f) Max:(%.1f,%.1f,%.1f) Res:(%d,%d,%d)"),
			SDFVolumeData.VolumeMin.X, SDFVolumeData.VolumeMin.Y, SDFVolumeData.VolumeMin.Z,
			SDFVolumeData.VolumeMax.X, SDFVolumeData.VolumeMax.Y, SDFVolumeData.VolumeMax.Z,
			SDFVolumeData.VolumeResolution.X, SDFVolumeData.VolumeResolution.Y, SDFVolumeData.VolumeResolution.Z);
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
	PassParameters->SceneTextureSampler = TStaticSamplerState<
		SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

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
	PassParameters->SceneViewRect = FVector2f(
		ViewInfo.ViewRect.Width(),
		ViewInfo.ViewRect.Height());
	PassParameters->SceneTextureSize = FVector2f(
		SceneDepthTexture->Desc.Extent.X,
		SceneDepthTexture->Desc.Extent.Y);

	// Render target (blend over existing scene)
	PassParameters->RenderTargets[0] = FRenderTargetBinding(
		Output.Texture, ERenderTargetLoadAction::ELoad);

	// Get shaders with appropriate permutation
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidRayMarchVS> VertexShader(GlobalShaderMap);

	// Select pixel shader permutation based on SDF volume usage
	FFluidRayMarchPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FUseSDFVolumeDim>(bUseSDFVolume);
	TShaderMapRef<FFluidRayMarchPS> PixelShader(GlobalShaderMap, PermutationVector);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MetaballPostProcess_RayMarching (%s, Particles: %d)",
			bUseSDFVolume ? TEXT("SDFVolume") : TEXT("Direct"), ParticleCount),
		PassParameters,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, PassParameters, ViewRect](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(
				ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
				ViewRect.Max.X, ViewRect.Max.Y, 1.0f);
			RHICmdList.SetScissorRect(
				true,
				ViewRect.Min.X, ViewRect.Min.Y,
				ViewRect.Max.X, ViewRect.Max.Y);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI =
				GEmptyVertexDeclaration.VertexDeclarationRHI;
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

			// Draw fullscreen triangle
			RHICmdList.DrawPrimitive(0, 1, 1);
		});
}
