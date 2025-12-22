// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidDepthPass.h"
#include "Rendering/FluidDepthShaders.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Core/FluidSimulator.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "CommonRenderResources.h"

//=============================================================================
// Depth Pass Implementation
//=============================================================================

void RenderFluidDepthPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	FRDGTextureRef& OutDepthTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidDepthPass_InstancedMesh");

	// 등록된 시뮬레이터 가져오기
	const TArray<AFluidSimulator*>& Simulators = Subsystem->GetRegisteredSimulators();
	if (Simulators.Num() == 0)
	{
		return;
	}

	// Depth Texture 생성
	FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_R32_FLOAT,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutDepthTexture = GraphBuilder.CreateTexture(DepthDesc, TEXT("FluidDepthTexture"));

	// 각 시뮬레이터의 InstancedMesh 렌더링
	for (AFluidSimulator* Simulator : Simulators)
	{
		if (!Simulator || Simulator->GetParticleCount() == 0)
		{
			continue;
		}

		// DebugMeshComponent (InstancedStaticMeshComponent) 가져오기
		UInstancedStaticMeshComponent* MeshComp = Simulator->DebugMeshComponent;
		if (!MeshComp || !MeshComp->IsVisible())
		{
			continue;
		}

		// 파티클 개수 확인
		int32 InstanceCount = MeshComp->GetInstanceCount();
		if (InstanceCount == 0)
		{
			continue;
		}

		// UE_LOG(LogTemp, Log, TEXT("FluidDepthPass: Rendering %s with %d instances"),
		// 	*Simulator->GetName(), InstanceCount);

		// Instance transforms에서 파티클 위치 추출
		TArray<FVector3f> ParticlePositions;
		ParticlePositions.Reserve(InstanceCount);

		for (int32 i = 0; i < InstanceCount; ++i)
		{
			FTransform InstanceTransform;
			if (MeshComp->GetInstanceTransform(i, InstanceTransform, true)) // World space
			{
				FVector WorldPos = InstanceTransform.GetLocation();
				ParticlePositions.Add(FVector3f(WorldPos));
			}
		}

		if (ParticlePositions.Num() == 0)
		{
			continue;
		}

		// GPU 버퍼 생성 및 업로드
		const uint32 BufferSize = ParticlePositions.Num() * sizeof(FVector3f);
		FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), ParticlePositions.Num());
		FRDGBufferRef ParticleBuffer = GraphBuilder.CreateBuffer(BufferDesc, TEXT("FluidParticlePositions"));

		// 데이터 업로드
		GraphBuilder.QueueBufferUpload(ParticleBuffer, ParticlePositions.GetData(), BufferSize);

		FRDGBufferSRVRef ParticleBufferSRV = GraphBuilder.CreateSRV(ParticleBuffer);

		// View matrices
		FMatrix ViewMatrix = View.ViewMatrices.GetViewMatrix();
		FMatrix ProjectionMatrix = View.ViewMatrices.GetProjectionMatrix();
		FMatrix ViewProjectionMatrix = View.ViewMatrices.GetViewProjectionMatrix();

		float ParticleRadius = Subsystem->RenderingParameters.ParticleRenderRadius;

		// Depth 렌더링 패스
		auto* PassParameters = GraphBuilder.AllocParameters<FFluidDepthParameters>();
		PassParameters->ParticlePositions = ParticleBufferSRV;
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->ViewMatrix = FMatrix44f(ViewMatrix);
		PassParameters->ProjectionMatrix = FMatrix44f(ProjectionMatrix);
		PassParameters->ViewProjectionMatrix = FMatrix44f(ViewProjectionMatrix);
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutDepthTexture, ERenderTargetLoadAction::EClear);

		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FFluidDepthVS> VertexShader(GlobalShaderMap);
		TShaderMapRef<FFluidDepthPS> PixelShader(GlobalShaderMap);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("FluidDepthDraw_%s", *Simulator->GetName()),
			PassParameters,
			ERDGPassFlags::Raster,
			[VertexShader, PixelShader, PassParameters, InstanceCount](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
				GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *PassParameters);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

				// Draw instanced quads (4 vertices per instance)
				RHICmdList.DrawPrimitive(0, 2, InstanceCount);
			});
	}
}
