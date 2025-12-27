// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidDepthPass.h"
#include "Rendering/FluidDepthShaders.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/IKawaiiFluidRenderable.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidSSFRRenderer.h"
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
// Helper: Collect Active SSFR Renderers (Legacy + New Architecture)
//=============================================================================

namespace
{
	void CollectActiveSSFRRenderersForDepth(
		UFluidRendererSubsystem* Subsystem,
		TArray<FKawaiiFluidRenderResource*>& OutResources,
		TArray<float>& OutRadii,
		TArray<FString>& OutDebugNames)
	{
		if (!Subsystem)
		{
			return;
		}

		// Legacy: Collect from IKawaiiFluidRenderable
		TArray<IKawaiiFluidRenderable*> Renderables = Subsystem->GetAllRenderables();
		for (IKawaiiFluidRenderable* Renderable : Renderables)
		{
			if (Renderable && Renderable->ShouldUseSSFR())
			{
				FKawaiiFluidRenderResource* Resource = Renderable->GetFluidRenderResource();
				if (Resource && Resource->IsValid())
				{
					OutResources.Add(Resource);
					OutRadii.Add(Renderable->GetParticleRadius());
					OutDebugNames.Add(Renderable->GetDebugName());
				}
			}
		}

		// New: Collect from RenderingModules
		const TArray<UKawaiiFluidRenderingModule*>& Modules = Subsystem->GetAllRenderingModules();
		for (UKawaiiFluidRenderingModule* Module : Modules)
		{
			if (!Module) continue;

			UKawaiiFluidSSFRRenderer* SSFRRenderer = Module->GetSSFRRenderer();
			if (SSFRRenderer && SSFRRenderer->IsRenderingActive())
			{
				FKawaiiFluidRenderResource* Resource = SSFRRenderer->GetFluidRenderResource();
				if (Resource && Resource->IsValid())
				{
					OutResources.Add(Resource);
					OutRadii.Add(SSFRRenderer->GetCachedParticleRadius());
					OutDebugNames.Add(TEXT("SSFR_RenderingModule"));
				}
			}
		}
	}
} // anonymous namespace

//=============================================================================
// Depth Pass Implementation
//=============================================================================

void RenderFluidDepthPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutLinearDepthTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidDepthPass");

	// Smoothing 용도의 Depth Texture 생성
	FRDGTextureDesc LinearDepthDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_R32_FLOAT,
		FClearValueBinding(FLinearColor(MAX_flt, 0.0f, 0.0f, 0.0f)),
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutLinearDepthTexture = GraphBuilder.CreateTexture(LinearDepthDesc, TEXT("FluidLinearDepth"));

	// Z-Test 용도의 Depth Texture 생성
	FRDGTextureDesc HardwareDepthDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_DepthStencil, // 하드웨어 깊이 포맷
		FClearValueBinding::DepthFar, // 가장 먼 곳으로 초기화
		TexCreate_DepthStencilTargetable | TexCreate_ShaderResource); // DepthStencilTargetable 필수

	FRDGTextureRef FluidDepthStencil = GraphBuilder.CreateTexture(
		HardwareDepthDesc, TEXT("FluidHardwareDepth"));

	// Linear Depth를 MAX_flt로 초기화
	AddClearRenderTargetPass(GraphBuilder, OutLinearDepthTexture, FLinearColor(MAX_flt, 0, 0, 0));

	// Hardware Depth를 Far로 초기화 (Reversed-Z 기준)
	AddClearDepthStencilPass(GraphBuilder, FluidDepthStencil, true, 0.0f, true, 0);

	// Collect active SSFR renderers (Legacy + New Architecture)
	TArray<FKawaiiFluidRenderResource*> Resources;
	TArray<float> Radii;
	TArray<FString> DebugNames;
	CollectActiveSSFRRenderersForDepth(Subsystem, Resources, Radii, DebugNames);

	if (Resources.Num() == 0)
	{
		return; // No SSFR renderers active
	}

	// Render each SSFR renderer's particles
	for (int32 i = 0; i < Resources.Num(); ++i)
	{
		FKawaiiFluidRenderResource* RR = Resources[i];
		float ParticleRadius = Radii[i];
		const FString& DebugName = DebugNames[i];

		const TArray<FKawaiiRenderParticle>& CachedParticles = RR->GetCachedParticles();

		if (CachedParticles.Num() == 0)
		{
			continue;
		}

		UE_LOG(LogTemp, Log, TEXT("DepthPass (SSFR): %s with %d particles"),
		       *DebugName, CachedParticles.Num());

		// Position만 추출
		TArray<FVector3f> ParticlePositions;
		ParticlePositions.Reserve(CachedParticles.Num());
		for (const FKawaiiRenderParticle& Particle : CachedParticles)
		{
			ParticlePositions.Add(Particle.Position);
		}

		// RDG 버퍼 생성 및 업로드
		const uint32 BufferSize = ParticlePositions.Num() * sizeof(FVector3f);
		FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(
			sizeof(FVector3f), ParticlePositions.Num());
		FRDGBufferRef ParticleBuffer = GraphBuilder.CreateBuffer(
			BufferDesc, TEXT("SSFRParticlePositions"));

		GraphBuilder.QueueBufferUpload(ParticleBuffer, ParticlePositions.GetData(), BufferSize);
		FRDGBufferSRVRef ParticleBufferSRV = GraphBuilder.CreateSRV(ParticleBuffer);

		// View matrices
		FMatrix ViewMatrix = View.ViewMatrices.GetViewMatrix();
		FMatrix ProjectionMatrix = View.ViewMatrices.GetProjectionMatrix();
		FMatrix ViewProjectionMatrix = View.ViewMatrices.GetViewProjectionMatrix();

		// Shader Parameters
		auto* PassParameters = GraphBuilder.AllocParameters<FFluidDepthParameters>();
		PassParameters->ParticlePositions = ParticleBufferSRV; // (위에서 만든 SRV)
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->ViewMatrix = FMatrix44f(ViewMatrix);
		PassParameters->ProjectionMatrix = FMatrix44f(ProjectionMatrix);
		PassParameters->ViewProjectionMatrix = FMatrix44f(ViewProjectionMatrix);
		PassParameters->SceneDepthTexture = SceneDepthTexture; // Scene depth (occlusion test용)
		PassParameters->SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// Color Target (Linear Depth용, R32F)
		// 첫 번째 패스면 Clear, 아니면 Load
		PassParameters->RenderTargets[0] = FRenderTargetBinding(
			OutLinearDepthTexture,
			ERenderTargetLoadAction::ELoad
		);

		// Depth Stencil Target (Hardware Depth용, Z-Test 수행)
		// 첫 번째 패스면 Clear, 아니면 Load
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			FluidDepthStencil,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilWrite // 깊이 버퍼에 쓰기 권한 요청
		);

		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FFluidDepthVS> VertexShader(GlobalShaderMap);
		TShaderMapRef<FFluidDepthPS> PixelShader(GlobalShaderMap);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("DepthDraw_SSFR_%s", *DebugName),
			PassParameters,
			ERDGPassFlags::Raster,
			[VertexShader, PixelShader, PassParameters, ParticleCount = CachedParticles.Num()](
			FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.
					VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;

				// BlendState: Opaque (불투명)
				// 깊이 테스트를 통과한 픽셀은 그냥 덮어씌움
				GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();

				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();

				// PSO DepthStencilState 변경
				// true: 깊이 쓰기 활성화
				// CF_Greater: Reversed-Z에서 더 가까운 값이면 통과 (Far=0.0, Near=1.0)
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

//=============================================================================
// Batched Depth Pass (New Architecture)
//=============================================================================

void RenderFluidDepthPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const TArray<UKawaiiFluidSSFRRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef& OutLinearDepthTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidDepthPass_Batched");

	if (Renderers.Num() == 0)
	{
		return;
	}

	// Smoothing 용도의 Depth Texture 생성
	FRDGTextureDesc LinearDepthDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_R32_FLOAT,
		FClearValueBinding(FLinearColor(MAX_flt, 0.0f, 0.0f, 0.0f)),
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutLinearDepthTexture = GraphBuilder.CreateTexture(LinearDepthDesc, TEXT("FluidLinearDepth"));

	// Z-Test 용도의 Depth Texture 생성
	FRDGTextureDesc HardwareDepthDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_DepthStencil,
		FClearValueBinding::DepthFar,
		TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);

	FRDGTextureRef FluidDepthStencil = GraphBuilder.CreateTexture(
		HardwareDepthDesc, TEXT("FluidHardwareDepth"));

	// Clear render targets
	AddClearRenderTargetPass(GraphBuilder, OutLinearDepthTexture, FLinearColor(MAX_flt, 0, 0, 0));
	AddClearDepthStencilPass(GraphBuilder, FluidDepthStencil, true, 0.0f, true, 0);

	// Get ParticleRenderRadius from first renderer's LocalParameters
	// (all renderers in batch have identical parameters - that's why they're batched)
	float ParticleRadius = Renderers[0]->GetLocalParameters().ParticleRenderRadius;

	// Render each renderer's particles (batch-specific only)
	for (UKawaiiFluidSSFRRenderer* Renderer : Renderers)
	{
		if (!Renderer) continue;

		FKawaiiFluidRenderResource* RR = Renderer->GetFluidRenderResource();
		if (!RR || !RR->IsValid()) continue;
		const TArray<FKawaiiRenderParticle>& CachedParticles = RR->GetCachedParticles();

		if (CachedParticles.Num() == 0)
		{
			continue;
		}

		UE_LOG(LogTemp, Log, TEXT("DepthPass (Batched): Renderer with %d particles"),
		       CachedParticles.Num());

		// Position만 추출
		TArray<FVector3f> ParticlePositions;
		ParticlePositions.Reserve(CachedParticles.Num());
		for (const FKawaiiRenderParticle& Particle : CachedParticles)
		{
			ParticlePositions.Add(Particle.Position);
		}

		// RDG 버퍼 생성 및 업로드
		const uint32 BufferSize = ParticlePositions.Num() * sizeof(FVector3f);
		FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(
			sizeof(FVector3f), ParticlePositions.Num());
		FRDGBufferRef ParticleBuffer = GraphBuilder.CreateBuffer(
			BufferDesc, TEXT("SSFRParticlePositions"));

		GraphBuilder.QueueBufferUpload(ParticleBuffer, ParticlePositions.GetData(), BufferSize);
		FRDGBufferSRVRef ParticleBufferSRV = GraphBuilder.CreateSRV(ParticleBuffer);

		// View matrices
		FMatrix ViewMatrix = View.ViewMatrices.GetViewMatrix();
		FMatrix ProjectionMatrix = View.ViewMatrices.GetProjectionMatrix();
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

		PassParameters->RenderTargets[0] = FRenderTargetBinding(
			OutLinearDepthTexture,
			ERenderTargetLoadAction::ELoad
		);

		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			FluidDepthStencil,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilWrite
		);

		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FFluidDepthVS> VertexShader(GlobalShaderMap);
		TShaderMapRef<FFluidDepthPS> PixelShader(GlobalShaderMap);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("DepthDraw_Batched"),
			PassParameters,
			ERDGPassFlags::Raster,
			[VertexShader, PixelShader, PassParameters, ParticleCount = CachedParticles.Num()](
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
