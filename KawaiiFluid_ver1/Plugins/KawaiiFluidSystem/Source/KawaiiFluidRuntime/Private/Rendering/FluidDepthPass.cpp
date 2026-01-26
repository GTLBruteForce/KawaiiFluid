// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/FluidDepthPass.h"
#include "Rendering/FluidDepthShaders.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ScenePrivate.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "CommonRenderResources.h"

//=============================================================================
// Batched Depth Pass
//=============================================================================

void RenderFluidDepthPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutLinearDepthTexture,
	FRDGTextureRef& OutVelocityTexture,
	FRDGTextureRef& OutOcclusionMaskTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidDepthPass_Batched");

	if (Renderers.Num() == 0)
	{
		return;
	}

	// Create Depth Texture for smoothing
	FRDGTextureDesc LinearDepthDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_R32_FLOAT,
		FClearValueBinding(FLinearColor(MAX_flt, 0.0f, 0.0f, 0.0f)),
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutLinearDepthTexture = GraphBuilder.CreateTexture(LinearDepthDesc, TEXT("FluidLinearDepth"));

	// Create Velocity Texture (for screen-space flow)
	FRDGTextureDesc VelocityDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_G16R16F,  // RG16F for 2D screen velocity
		FClearValueBinding(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)),
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutVelocityTexture = GraphBuilder.CreateTexture(VelocityDesc, TEXT("FluidVelocity"));

	// OcclusionMask Texture 생성 (1.0 = visible, 0.0 = occluded by scene geometry)
	FRDGTextureDesc OcclusionMaskDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_R8,  // Single channel, 8-bit is sufficient for mask
		FClearValueBinding(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)),
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutOcclusionMaskTexture = GraphBuilder.CreateTexture(OcclusionMaskDesc, TEXT("FluidOcclusionMask"));

	// Create Depth Texture for Z-Test
	FRDGTextureDesc HardwareDepthDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_DepthStencil,
		FClearValueBinding::DepthFar,
		TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);

	FRDGTextureRef FluidDepthStencil = GraphBuilder.CreateTexture(
		HardwareDepthDesc, TEXT("FluidHardwareDepth"));

	// Clear render targets
	AddClearRenderTargetPass(GraphBuilder, OutLinearDepthTexture, FLinearColor(MAX_flt, 0, 0, 0));
	AddClearRenderTargetPass(GraphBuilder, OutVelocityTexture, FLinearColor(0, 0, 0, 0));
	AddClearRenderTargetPass(GraphBuilder, OutOcclusionMaskTexture, FLinearColor(0, 0, 0, 0));  // Default: occluded
	AddClearDepthStencilPass(GraphBuilder, FluidDepthStencil, true, 0.0f, true, 0);

	// Get ParticleRenderRadius from first renderer's LocalParameters
	// (all renderers in batch have identical parameters - that's why they're batched)
	float ParticleRadius = Renderers[0]->GetLocalParameters().ParticleRenderRadius;

	// Track processed RenderResources to prevent duplicate processing
	TSet<FKawaiiFluidRenderResource*> ProcessedResources;

	// Render each renderer's particles (batch-specific only)
	for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
	{
		if (!Renderer) continue;

		FKawaiiFluidRenderResource* RR = Renderer->GetFluidRenderResource();
		if (!RR || !RR->IsValid()) continue;

		// Skip already processed RenderResources
		if (ProcessedResources.Contains(RR))
		{
			continue;
		}
		ProcessedResources.Add(RR);

		FRDGBufferSRVRef ParticleBufferSRV = nullptr;
		int32 ParticleCount = 0;

		// Anisotropy state
		bool bUseAnisotropy = false;
		FRDGBufferSRVRef AnisotropyAxis1SRV = nullptr;
		FRDGBufferSRVRef AnisotropyAxis2SRV = nullptr;
		FRDGBufferSRVRef AnisotropyAxis3SRV = nullptr;

		// =====================================================
		// 통합 경로: RenderResource에서 일원화된 데이터 접근
		// GPU/CPU 모두 동일한 버퍼 사용
		// =====================================================
		ParticleCount = RR->GetUnifiedParticleCount();
		if (ParticleCount <= 0)
		{
			continue;
		}

		// Get Position buffer (GPU/CPU common - always created by ViewExtension)
		TRefCountPtr<FRDGPooledBuffer> PositionPooledBuffer = RR->GetPooledPositionBuffer();
		if (!PositionPooledBuffer.IsValid())
		{
			// Skip if buffer not created by ViewExtension
			continue;
		}

		FRDGBufferRef PositionBuffer = GraphBuilder.RegisterExternalBuffer(
			PositionPooledBuffer,
			TEXT("SSFRParticlePositions"));
		ParticleBufferSRV = GraphBuilder.CreateSRV(PositionBuffer);

		// Get Velocity buffer (for flow texture)
		FRDGBufferSRVRef VelocityBufferSRV = nullptr;
		TRefCountPtr<FRDGPooledBuffer> VelocityPooledBuffer = RR->GetPooledVelocityBuffer();
		if (VelocityPooledBuffer.IsValid())
		{
			FRDGBufferRef VelocityBuffer = GraphBuilder.RegisterExternalBuffer(
				VelocityPooledBuffer,
				TEXT("SSFRParticleVelocities"));
			VelocityBufferSRV = GraphBuilder.CreateSRV(VelocityBuffer);
		}
		else
		{
			// Create dummy velocity buffer (single zero vector) when velocity data not available
			// This prevents shader crashes when flow texture is disabled
			FRDGBufferDesc DummyVelDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), 1);
			FRDGBufferRef DummyVelocityBuffer = GraphBuilder.CreateBuffer(DummyVelDesc, TEXT("DummyVelocityBuffer"));
			// Clear to zero
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyVelocityBuffer), 0);
			VelocityBufferSRV = GraphBuilder.CreateSRV(DummyVelocityBuffer);
		}

		// Anisotropy (valid only in GPU mode)
		bUseAnisotropy = RR->GetAnisotropyBufferSRVs(
			GraphBuilder,
			AnisotropyAxis1SRV,
			AnisotropyAxis2SRV,
			AnisotropyAxis3SRV);

		// Skip if no valid particles
		if (!ParticleBufferSRV || ParticleCount == 0)
		{
			continue;
		}

		// View matrices
		FMatrix ViewMatrix = View.ViewMatrices.GetViewMatrix();
		FMatrix ProjectionMatrix = View.ViewMatrices.GetProjectionNoAAMatrix();
		FMatrix ViewProjectionMatrix = View.ViewMatrices.GetViewProjectionMatrix();
		
		// Shader Parameters
		auto* PassParameters = GraphBuilder.AllocParameters<FFluidDepthParameters>();
		PassParameters->ParticlePositions = ParticleBufferSRV;
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->ViewMatrix = FMatrix44f(ViewMatrix);
		PassParameters->ProjectionMatrix = FMatrix44f(ProjectionMatrix);
		PassParameters->ViewProjectionMatrix = FMatrix44f(ViewProjectionMatrix);
		PassParameters->SceneDepthTexture = SceneDepthTexture;
		PassParameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// Anisotropy buffers (if enabled)
		PassParameters->AnisotropyAxis1 = AnisotropyAxis1SRV;
		PassParameters->AnisotropyAxis2 = AnisotropyAxis2SRV;
		PassParameters->AnisotropyAxis3 = AnisotropyAxis3SRV;

		// Velocity buffer for flow texture
		PassParameters->ParticleVelocities = VelocityBufferSRV;

		// ViewRect and texture size for SceneDepth UV transformation
		// FViewInfo::ViewRect = Valid region of SceneDepth (Screen Percentage applied)
		const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
		PassParameters->SceneViewRect = FVector2f(
			ViewInfo.ViewRect.Width(),
			ViewInfo.ViewRect.Height());
		PassParameters->SceneTextureSize = FVector2f(
			SceneDepthTexture->Desc.Extent.X,
			SceneDepthTexture->Desc.Extent.Y);

		PassParameters->RenderTargets[0] = FRenderTargetBinding(
			OutLinearDepthTexture,
			ERenderTargetLoadAction::ELoad
		);

		// MRT[1]: Screen-space velocity for flow texture
		PassParameters->RenderTargets[1] = FRenderTargetBinding(
			OutVelocityTexture,
			ERenderTargetLoadAction::ELoad
		);

		// MRT[2]: OcclusionMask for composite pass (1.0 = visible, 0.0 = occluded)
		PassParameters->RenderTargets[2] = FRenderTargetBinding(
			OutOcclusionMaskTexture,
			ERenderTargetLoadAction::ELoad
		);

		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			FluidDepthStencil,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilWrite
		);

		// Select shader permutation based on anisotropy
		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		FFluidDepthVS::FPermutationDomain VSPermutationDomain;
		FFluidDepthPS::FPermutationDomain PSPermutationDomain;
		VSPermutationDomain.Set<FUseAnisotropyDim>(bUseAnisotropy);
		PSPermutationDomain.Set<FUseAnisotropyDim>(bUseAnisotropy);

		TShaderMapRef<FFluidDepthVS> VertexShader(GlobalShaderMap, VSPermutationDomain);
		TShaderMapRef<FFluidDepthPS> PixelShader(GlobalShaderMap, PSPermutationDomain);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("DepthDraw_Batched%s", bUseAnisotropy ? TEXT("_Anisotropic") : TEXT("")),
			PassParameters,
			ERDGPassFlags::Raster,
			[VertexShader, PixelShader, PassParameters, ParticleCount](
			FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.
					VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;

				GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
					true, CF_Greater>::GetRHI();

				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(),
				                    *PassParameters);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(),
				                    *PassParameters);

				RHICmdList.DrawPrimitive(0, 2, ParticleCount);
			});
	}
}
