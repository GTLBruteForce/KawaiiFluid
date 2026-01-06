// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidThicknessPass.h"
#include "Rendering/FluidThicknessShaders.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
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

	// 뷰포트 크기로 텍스처 생성
	FIntPoint TextureSize = View.UnscaledViewRect.Size();

	// Thickness Texture 생성
	FRDGTextureDesc ThicknessDesc = FRDGTextureDesc::Create2D(
		TextureSize,
		PF_R16F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutThicknessTexture = GraphBuilder.CreateTexture(ThicknessDesc, TEXT("FluidThicknessTexture"));

	AddClearRenderTargetPass(GraphBuilder, OutThicknessTexture, FLinearColor::Black);

	// Get rendering parameters from first renderer's LocalParameters
	// (all renderers in batch have identical parameters - that's why they're batched)
	float ThicknessScale = Renderers[0]->GetLocalParameters().ThicknessScale;
	float ParticleRadius = Renderers[0]->GetLocalParameters().ParticleRenderRadius;

	// Render each renderer's particles (batch-specific only)
	for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
	{
		if (!Renderer) continue;

		FKawaiiFluidRenderResource* RR = Renderer->GetFluidRenderResource();
		if (!RR || !RR->IsValid()) continue;

		FRDGBufferSRVRef ParticleBufferSRV = nullptr;
		int32 ParticleCount = 0;

		// GPU 모드: GPUSimulator에서 직접 버퍼 접근 후 Position 추출
		FGPUFluidSimulator* GPUSimulator = Renderer->GetGPUSimulator();
		if (GPUSimulator && GPUSimulator->GetPersistentParticleBuffer().IsValid())
		{
			TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer = GPUSimulator->GetPersistentParticleBuffer();
			ParticleCount = GPUSimulator->GetParticleCount();

			if (ParticleCount > 0)
			{
				// Physics 버퍼 등록
				FRDGBufferRef PhysicsBuffer = GraphBuilder.RegisterExternalBuffer(
					PhysicsPooledBuffer,
					TEXT("SSFRThicknessPhysicsParticles_GPU"));
				FRDGBufferSRVRef PhysicsBufferSRV = GraphBuilder.CreateSRV(PhysicsBuffer);

				// Position 전용 버퍼 생성
				FRDGBufferRef PositionBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), ParticleCount),
					TEXT("SSFRThicknessPositions_GPU"));
				FRDGBufferUAVRef PositionBufferUAV = GraphBuilder.CreateUAV(PositionBuffer);

				// Velocity 더미 버퍼 (ExtractRenderDataSoAPass 요구사항)
				FRDGBufferRef VelocityBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), ParticleCount),
					TEXT("SSFRThicknessVelocities_GPU"));
				FRDGBufferUAVRef VelocityBufferUAV = GraphBuilder.CreateUAV(VelocityBuffer);

				// ExtractRenderDataSoAPass로 Position/Velocity 추출
				FGPUFluidSimulatorPassBuilder::AddExtractRenderDataSoAPass(
					GraphBuilder,
					PhysicsBufferSRV,
					PositionBufferUAV,
					VelocityBufferUAV,
					ParticleCount,
					ParticleRadius);

				ParticleBufferSRV = GraphBuilder.CreateSRV(PositionBuffer);
			}
		}
		// CPU 모드: 캐시에서 업로드
		else
		{
			TArray<FKawaiiRenderParticle> CachedParticlesCopy = RR->GetCachedParticles();

			if (CachedParticlesCopy.Num() == 0)
			{
				continue;
			}

			ParticleCount = CachedParticlesCopy.Num();

			UE_LOG(LogTemp, Log, TEXT("ThicknessPass (CPU Mode): Renderer with %d particles"), ParticleCount);

			// Position만 추출
			TArray<FVector3f> ParticlePositions;
			ParticlePositions.Reserve(ParticleCount);
			for (const FKawaiiRenderParticle& Particle : CachedParticlesCopy)
			{
				ParticlePositions.Add(Particle.Position);
			}

			// RDG 버퍼 생성 및 업로드
			const uint32 BufferSize = ParticlePositions.Num() * sizeof(FVector3f);
			FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(
				sizeof(FVector3f), ParticlePositions.Num());
			FRDGBufferRef ParticleBuffer = GraphBuilder.CreateBuffer(
				BufferDesc, TEXT("SSFRThicknessPositions_CPU"));

			GraphBuilder.QueueBufferUpload(ParticleBuffer, ParticlePositions.GetData(), BufferSize);
			ParticleBufferSRV = GraphBuilder.CreateSRV(ParticleBuffer);
		}

		// 유효한 파티클이 없으면 스킵
		if (!ParticleBufferSRV || ParticleCount == 0)
		{
			continue;
		}

		FMatrix ViewMatrix = View.ViewMatrices.GetViewMatrix();
		FMatrix ProjectionMatrix = View.ViewMatrices.GetProjectionNoAAMatrix();

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessParameters>();
		PassParameters->ParticlePositions = ParticleBufferSRV;
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->ViewMatrix = FMatrix44f(ViewMatrix);
		PassParameters->ProjectionMatrix = FMatrix44f(ProjectionMatrix);
		PassParameters->ThicknessScale = ThicknessScale; // Use from LocalParameters

		// Occlusion test를 위한 SceneDepth 파라미터
		PassParameters->SceneDepthTexture = SceneDepthTexture;
		PassParameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// SceneDepth UV 변환을 위한 ViewRect와 텍스처 크기
		const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
		PassParameters->SceneViewRect = FVector2f(
			ViewInfo.ViewRect.Width(),
			ViewInfo.ViewRect.Height());
		PassParameters->SceneTextureSize = FVector2f(
			SceneDepthTexture->Desc.Extent.X,
			SceneDepthTexture->Desc.Extent.Y);

		PassParameters->RenderTargets[0] = FRenderTargetBinding(
			OutThicknessTexture, ERenderTargetLoadAction::ELoad);

		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FFluidThicknessVS> VertexShader(GlobalShaderMap);
		TShaderMapRef<FFluidThicknessPS> PixelShader(GlobalShaderMap);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ThicknessDraw_Batched"),
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

				GraphicsPSOInit.BlendState = TStaticBlendState<
					CW_RED, // R채널만 사용 (R16F)
					BO_Add, BF_One, BF_One, // Color: Add(Src, Dst)
					BO_Add, BF_Zero, BF_One // Alpha: Add(0, 1) -> Alpha는 안 건드림
				>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
					false, CF_Always>::GetRHI();

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
