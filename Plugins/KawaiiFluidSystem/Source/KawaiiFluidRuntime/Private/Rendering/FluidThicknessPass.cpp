// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/FluidThicknessPass.h"
#include "Rendering/FluidThicknessShaders.h"
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
// Batched Thickness Pass
//=============================================================================

void RenderFluidThicknessPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutThicknessTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidThicknessPass_Batched");

	if (Renderers.Num() == 0)
	{
		return;
	}

	// Use the exact extent of the SceneDepthTexture for consistency across passes.
	const FIntPoint TextureExtent = SceneDepthTexture->Desc.Extent;

	// Create Thickness Texture
	FRDGTextureDesc ThicknessDesc = FRDGTextureDesc::Create2D(
		TextureExtent,
		PF_R16F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutThicknessTexture = GraphBuilder.CreateTexture(ThicknessDesc, TEXT("FluidThicknessTexture"));

	AddClearRenderTargetPass(GraphBuilder, OutThicknessTexture, FLinearColor::Black);

	// Get rendering parameters from first renderer's LocalParameters
	// (all renderers in batch have identical parameters - that's why they're batched)
	float ThicknessScale = Renderers[0]->GetLocalParameters().ThicknessScale;
	float ParticleRadius = Renderers[0]->GetLocalParameters().ParticleRenderRadius;

	// Pre-compute view parameters
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	const FIntRect ViewRect = ViewInfo.ViewRect;

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

		// =====================================================
		// Unified path: unified data access from RenderResource
		// Both GPU/CPU use same buffer
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
			TEXT("SSFRThicknessPositions"));
		ParticleBufferSRV = GraphBuilder.CreateSRV(PositionBuffer);

		// Skip if no valid particles
		if (!ParticleBufferSRV || ParticleCount == 0)
		{
			continue;
		}

		FMatrix ViewMatrix = View.ViewMatrices.GetViewMatrix();
		FMatrix ProjectionMatrix = View.ViewMatrices.GetProjectionMatrix();

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessParameters>();
		PassParameters->ParticlePositions = ParticleBufferSRV;
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->ViewMatrix = FMatrix44f(ViewMatrix);
		PassParameters->ProjectionMatrix = FMatrix44f(ProjectionMatrix);
		PassParameters->ThicknessScale = ThicknessScale; // Use from LocalParameters

		// SceneDepth parameters for occlusion test
		PassParameters->SceneDepthTexture = SceneDepthTexture;
		PassParameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// ViewRect and texture size for SceneDepth UV transformation
		PassParameters->SceneViewRect = FVector2f(
			static_cast<float>(ViewRect.Width()),
			static_cast<float>(ViewRect.Height()));
		PassParameters->SceneTextureSize = FVector2f(
			static_cast<float>(TextureExtent.X),
			static_cast<float>(TextureExtent.Y));

		PassParameters->RenderTargets[0] = FRenderTargetBinding(
			OutThicknessTexture, ERenderTargetLoadAction::ELoad);

		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FFluidThicknessVS> VertexShader(GlobalShaderMap);
		TShaderMapRef<FFluidThicknessPS> PixelShader(GlobalShaderMap);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ThicknessDraw_Batched"),
			PassParameters,
			ERDGPassFlags::Raster,
			[VertexShader, PixelShader, PassParameters, ParticleCount, ViewRect](
			FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.
					VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;

				GraphicsPSOInit.BlendState = TStaticBlendState<
					CW_RED, // R channel only (R16F)
					BO_Add, BF_One, BF_One, // Color: Add(Src, Dst)
					BO_Add, BF_Zero, BF_One // Alpha: Add(0, 1) - don't modify Alpha
				>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
					false, CF_Always>::GetRHI();

				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				// [FIX] Set Viewport to match the View's area within the texture extent.
				RHICmdList.SetViewport(
					static_cast<float>(ViewRect.Min.X), static_cast<float>(ViewRect.Min.Y), 0.0f,
					static_cast<float>(ViewRect.Max.X), static_cast<float>(ViewRect.Max.Y), 1.0f);

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(),
				                    *PassParameters);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(),
				                    *PassParameters);

				RHICmdList.DrawPrimitive(0, 2, ParticleCount);
			});
	}
}
