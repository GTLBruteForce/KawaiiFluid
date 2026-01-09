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

	// Particle data (FKawaiiRenderParticle buffer - Legacy AoS)
	PassParameters->RenderParticles = PipelineData.ParticleBufferSRV;
	PassParameters->ParticleCount = PipelineData.ParticleCount;
	PassParameters->ParticleRadius = PipelineData.ParticleRadius;

	// SoA buffers (62% memory bandwidth reduction for SDF evaluation)
	const bool bUseSoABuffers = PipelineData.HasValidSoABuffers();
	if (bUseSoABuffers)
	{
		PassParameters->RenderPositions = PipelineData.PositionBufferSRV;
	}

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
	const bool bUseGPUBounds = PipelineData.SDFVolumeData.bUseGPUBounds && PipelineData.SDFVolumeData.BoundsBufferSRV != nullptr;

	if (bUseSDFVolume)
	{
		PassParameters->SDFVolumeTexture = PipelineData.SDFVolumeData.SDFVolumeTextureSRV;
		PassParameters->SDFVolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SDFVolumeResolution = PipelineData.SDFVolumeData.VolumeResolution;

		if (bUseGPUBounds)
		{
			// GPU mode: bounds from buffer (no readback latency)
			PassParameters->BoundsBuffer = PipelineData.SDFVolumeData.BoundsBufferSRV;
		}
		else
		{
			// CPU mode: bounds from uniforms
			PassParameters->SDFVolumeMin = PipelineData.SDFVolumeData.VolumeMin;
			PassParameters->SDFVolumeMax = PipelineData.SDFVolumeData.VolumeMax;
		}
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
	PermutationVector.Set<FUseGPUBoundsGBufferDim>(bUseGPUBounds);
	PermutationVector.Set<FUseSoABuffersGBufferDim>(bUseSoABuffers);
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
	FScreenPassRenderTarget Output,
	bool bOutputDepth,
	FRDGTextureRef* OutFluidDepthTexture)
{
	const bool bUseSDFVolume = PipelineData.SDFVolumeData.IsValid();
	// HYBRID MODE: Spatial Hash is only used WITH SDF Volume for precise final evaluation
	// SDF Volume for 90% fast approach + Spatial Hash for 10% precise final
	const bool bUseSpatialHash = bUseSDFVolume && PipelineData.SpatialHashData.IsValid();

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

	//========================================
	// Scaled Resolution Setup
	//========================================
	const FIntRect FullViewRect = Output.ViewRect;
	const float RenderScale = FMath::Clamp(RenderParams.RenderTargetScale, 0.25f, 1.0f);
	const bool bUseScaledRes = RenderScale < 0.99f;

	FIntPoint ScaledSize(
		FMath::Max(1, FMath::RoundToInt(FullViewRect.Width() * RenderScale)),
		FMath::Max(1, FMath::RoundToInt(FullViewRect.Height() * RenderScale))
	);

	// Create intermediate texture for scaled resolution rendering
	FRDGTextureRef IntermediateTarget = nullptr;
	FRDGTextureRef ScaledDepthTexture = nullptr;
	if (bUseScaledRes)
	{
		// Color texture (scaled-res)
		FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
			ScaledSize,
			PF_FloatRGBA,
			FClearValueBinding::Transparent,
			TexCreate_RenderTargetable | TexCreate_ShaderResource
		);
		IntermediateTarget = GraphBuilder.CreateTexture(IntermediateDesc, TEXT("FluidRayMarch_Scaled"));

		// Depth texture (scaled-res) for depth-aware upsampling
		FRDGTextureDesc ScaledDepthDesc = FRDGTextureDesc::Create2D(
			ScaledSize,
			PF_R32_FLOAT,
			FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource
		);
		ScaledDepthTexture = GraphBuilder.CreateTexture(ScaledDepthDesc, TEXT("FluidRayMarch_ScaledDepth"));
	}

	// Select render target based on scaled resolution mode
	FRDGTextureRef RayMarchTarget = bUseScaledRes ? IntermediateTarget : Output.Texture;
	FIntRect RenderViewRect = bUseScaledRes ? FIntRect(0, 0, ScaledSize.X, ScaledSize.Y) : FullViewRect;

	// Enable depth output for scaled-res (needed for depth-aware upsampling) or if explicitly requested
	const bool bNeedDepthOutput = bOutputDepth || bUseScaledRes;

	// Create depth output texture if requested (full-res for shadow, etc.)
	FRDGTextureRef FluidDepthTexture = nullptr;
	if (bOutputDepth)
	{
		FIntRect ViewRect = FullViewRect;
		FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
			FIntPoint(ViewRect.Width(), ViewRect.Height()),
			PF_R32_FLOAT,
			FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource);

		FluidDepthTexture = GraphBuilder.CreateTexture(DepthDesc, TEXT("FluidRayMarchDepth"));

		if (OutFluidDepthTexture)
		{
			// 스케일링 모드면 ScaledDepthTexture 사용 (실제 depth가 출력되는 텍스처)
			*OutFluidDepthTexture = (bUseScaledRes && ScaledDepthTexture) ? ScaledDepthTexture : FluidDepthTexture;
		}
	}

	auto* PassParameters = GraphBuilder.AllocParameters<FFluidRayMarchPS::FParameters>();

	// Particle data (FKawaiiRenderParticle buffer - Legacy AoS)
	PassParameters->RenderParticles = PipelineData.ParticleBufferSRV;
	PassParameters->ParticleCount = PipelineData.ParticleCount;
	PassParameters->ParticleRadius = PipelineData.ParticleRadius;

	// SoA buffers (62% memory bandwidth reduction for SDF evaluation)
	const bool bUseSoABuffers = PipelineData.HasValidSoABuffers();
	if (bUseSoABuffers)
	{
		PassParameters->RenderPositions = PipelineData.PositionBufferSRV;
		PassParameters->RenderVelocities = PipelineData.VelocityBufferSRV;
	}

	// SDF Volume data
	const bool bUseGPUBounds = bUseSDFVolume && PipelineData.SDFVolumeData.bUseGPUBounds && PipelineData.SDFVolumeData.BoundsBufferSRV != nullptr;

	if (bUseSDFVolume)
	{
		PassParameters->SDFVolumeTexture = PipelineData.SDFVolumeData.SDFVolumeTextureSRV;
		PassParameters->SDFVolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SDFVolumeResolution = PipelineData.SDFVolumeData.VolumeResolution;

		if (bUseGPUBounds)
		{
			// GPU mode: bounds from buffer (no readback latency)
			PassParameters->BoundsBuffer = PipelineData.SDFVolumeData.BoundsBufferSRV;
		}
		else
		{
			// CPU mode: bounds from uniforms
			PassParameters->SDFVolumeMin = PipelineData.SDFVolumeData.VolumeMin;
			PassParameters->SDFVolumeMax = PipelineData.SDFVolumeData.VolumeMax;
		}
	}

	// Spatial Hash data for HYBRID mode (used WITH SDF Volume)
	// SDF Volume for 90% fast approach + Spatial Hash for 10% precise final
	if (bUseSpatialHash)
	{
		PassParameters->CellData = PipelineData.SpatialHashData.CellDataSRV;
		PassParameters->SpatialHashParticleIndices = PipelineData.SpatialHashData.ParticleIndicesSRV;
		PassParameters->SpatialHashCellSize = PipelineData.SpatialHashData.CellSize;
	}

	// Anisotropy data for ellipsoid rendering (baduk stone shape)
	const bool bUseAnisotropy = PipelineData.AnisotropyData.IsValid();
	if (bUseAnisotropy)
	{
		PassParameters->AnisotropyAxis1 = PipelineData.AnisotropyData.AnisotropyAxis1SRV;
		PassParameters->AnisotropyAxis2 = PipelineData.AnisotropyData.AnisotropyAxis2SRV;
		PassParameters->AnisotropyAxis3 = PipelineData.AnisotropyData.AnisotropyAxis3SRV;
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
	PassParameters->AbsorptionColorCoefficients = RenderParams.AbsorptionColorCoefficients;
	PassParameters->SpecularStrength = RenderParams.SpecularStrength;
	PassParameters->SpecularRoughness = RenderParams.SpecularRoughness;
	PassParameters->EnvironmentLightColor = RenderParams.EnvironmentLightColor;

	// Lighting scale parameters
	PassParameters->AmbientScale = RenderParams.AmbientScale;
	PassParameters->TransmittanceScale = RenderParams.TransmittanceScale;
	PassParameters->AbsorptionBias = RenderParams.AbsorptionBias;
	PassParameters->FresnelReflectionBlend = RenderParams.FresnelReflectionBlend;

	// Reflection Cubemap
	if (RenderParams.ReflectionCubemap && RenderParams.ReflectionCubemap->GetResource())
	{
		PassParameters->ReflectionCubemap = RenderParams.ReflectionCubemap->GetResource()->TextureRHI;
		PassParameters->ReflectionCubemapSampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
		PassParameters->bUseReflectionCubemap = 1;
	}
	else
	{
		// Fallback: black texture (ignored by bUseReflectionCubemap flag)
		PassParameters->ReflectionCubemap = GBlackTextureCube->TextureRHI;
		PassParameters->ReflectionCubemapSampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
		PassParameters->bUseReflectionCubemap = 0;
	}
	PassParameters->ReflectionIntensity = RenderParams.ReflectionIntensity;
	PassParameters->ReflectionMipLevel = RenderParams.ReflectionMipLevel;

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

	// Viewport size (use scaled size for half-res rendering)
	PassParameters->ViewportSize = FVector2f(RenderViewRect.Width(), RenderViewRect.Height());

	// SceneDepth UV transform
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	PassParameters->SceneViewRect = FVector2f(ViewInfo.ViewRect.Width(), ViewInfo.ViewRect.Height());
	PassParameters->SceneTextureSize = FVector2f(SceneDepthTexture->Desc.Extent.X, SceneDepthTexture->Desc.Extent.Y);

	// Render targets (use intermediate for scaled-res, direct for full-res)
	PassParameters->RenderTargets[0] = FRenderTargetBinding(
		RayMarchTarget,
		bUseScaledRes ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad);

	// Depth output: use scaled depth for upsampling, or full-res depth for shadows
	if (bUseScaledRes && ScaledDepthTexture)
	{
		// Scaled-res mode: output to scaled depth texture for depth-aware upsampling
		PassParameters->RenderTargets[1] = FRenderTargetBinding(ScaledDepthTexture, ERenderTargetLoadAction::EClear);
	}
	else if (bOutputDepth && FluidDepthTexture)
	{
		// Full-res mode with depth output: output to full-res depth texture
		PassParameters->RenderTargets[1] = FRenderTargetBinding(FluidDepthTexture, ERenderTargetLoadAction::EClear);
	}

	// Get shaders
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFluidRayMarchVS> VertexShader(GlobalShaderMap);

	FFluidRayMarchPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FUseSDFVolumeDim>(bUseSDFVolume);
	PermutationVector.Set<FUseSpatialHashDim>(bUseSpatialHash);
	PermutationVector.Set<FOutputDepthDim>(bNeedDepthOutput);  // Enable depth output for scaled-res
	PermutationVector.Set<FUseGPUBoundsDim>(bUseGPUBounds);    // GPU bounds buffer (no readback)
	PermutationVector.Set<FUseSoABuffersDim>(bUseSoABuffers);  // SoA buffers (62% bandwidth reduction)
	PermutationVector.Set<FUseAnisotropyDim>(bUseAnisotropy);  // Anisotropic ellipsoid rendering (baduk stone)
	TShaderMapRef<FFluidRayMarchPS> PixelShader(GlobalShaderMap, PermutationVector);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MetaballPostProcess_RayMarching (%s%s%s)",
			bUseSDFVolume ? (bUseSpatialHash ? TEXT("Hybrid:SDF+Hash") : TEXT("SDFVolume")) : TEXT("Direct"),
			bOutputDepth ? TEXT(", DepthOut") : TEXT(""),
			bUseScaledRes ? TEXT(", ScaledRes") : TEXT("")),
		PassParameters,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, PassParameters, RenderViewRect, bUseScaledRes](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(RenderViewRect.Min.X, RenderViewRect.Min.Y, 0.0f, RenderViewRect.Max.X, RenderViewRect.Max.Y, 1.0f);
			RHICmdList.SetScissorRect(true, RenderViewRect.Min.X, RenderViewRect.Min.Y, RenderViewRect.Max.X, RenderViewRect.Max.Y);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			if (bUseScaledRes)
			{
				// Scaled-res: No blending, write RGBA directly to intermediate target
				// (Alpha blending will be done in upscale pass)
				GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA>::GetRHI();
			}
			else
			{
				// Full-res: Alpha blending directly to scene
				GraphicsPSOInit.BlendState = TStaticBlendState<
					CW_RGBA,
					BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
					BO_Add, BF_Zero, BF_One
				>::GetRHI();
			}
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

			RHICmdList.DrawPrimitive(0, 1, 1);
		});

	//========================================
	// Pass 2: Upscale (Scaled Resolution only)
	//========================================
	if (bUseScaledRes)
	{
		auto* UpscaleParameters = GraphBuilder.AllocParameters<FFluidUpscalePS::FParameters>();

		// Input: Scaled-res color texture
		UpscaleParameters->InputTexture = IntermediateTarget;
		UpscaleParameters->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// Input: Scaled-res fluid depth texture (for depth-aware upsampling)
		UpscaleParameters->FluidDepthTexture = ScaledDepthTexture;
		UpscaleParameters->DepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// Input: Full-res scene depth texture (for depth comparison)
		UpscaleParameters->SceneDepthTexture = SceneDepthTexture;

		// Resolution info
		UpscaleParameters->InputSize = FVector2f(ScaledSize.X, ScaledSize.Y);
		UpscaleParameters->OutputSize = FVector2f(FullViewRect.Width(), FullViewRect.Height());
		UpscaleParameters->SceneDepthSize = FVector2f(SceneDepthTexture->Desc.Extent.X, SceneDepthTexture->Desc.Extent.Y);

		// Output: Full-res scene texture (ELoad to preserve scene)
		UpscaleParameters->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);

		TShaderMapRef<FFluidUpscaleVS> UpscaleVS(GlobalShaderMap);
		TShaderMapRef<FFluidUpscalePS> UpscalePS(GlobalShaderMap);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("FluidUpscale_AlphaBlend"),
			UpscaleParameters,
			ERDGPassFlags::Raster,
			[UpscaleVS, UpscalePS, UpscaleParameters, FullViewRect](FRHICommandList& RHICmdList)
			{
				// Restore viewport to full resolution
				RHICmdList.SetViewport(FullViewRect.Min.X, FullViewRect.Min.Y, 0.0f, FullViewRect.Max.X, FullViewRect.Max.Y, 1.0f);
				RHICmdList.SetScissorRect(true, FullViewRect.Min.X, FullViewRect.Min.Y, FullViewRect.Max.X, FullViewRect.Max.Y);

				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = UpscaleVS.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = UpscalePS.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleList;

				// Alpha blending to composite onto scene
				GraphicsPSOInit.BlendState = TStaticBlendState<
					CW_RGBA,
					BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
					BO_Add, BF_Zero, BF_One
				>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
				SetShaderParameters(RHICmdList, UpscalePS, UpscalePS.GetPixelShader(), *UpscaleParameters);

				RHICmdList.DrawPrimitive(0, 1, 1);
			});
	}
}
