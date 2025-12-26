// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidThicknessPass.h"
#include "Rendering/FluidThicknessShaders.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/IKawaiiFluidRenderable.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Rendering/KawaiiFluidRenderController.h"
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

static void CollectActiveSSFRRenderers(
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
				OutRadii.Add(Renderable->GetParticleRenderRadius());
				OutDebugNames.Add(Renderable->GetDebugName());
			}
		}
	}

	// New: Collect from RenderControllers
	const TArray<UKawaiiFluidRenderController*>& Controllers = Subsystem->GetAllRenderControllers();
	for (UKawaiiFluidRenderController* Controller : Controllers)
	{
		if (!Controller) continue;

		UKawaiiFluidSSFRRenderer* SSFRRenderer = Controller->GetSSFRRenderer();
		if (SSFRRenderer && SSFRRenderer->IsRenderingActive())
		{
			FKawaiiFluidRenderResource* Resource = SSFRRenderer->GetFluidRenderResource();
			if (Resource && Resource->IsValid())
			{
				OutResources.Add(Resource);
				OutRadii.Add(SSFRRenderer->GetCachedParticleRadius());
				OutDebugNames.Add(TEXT("SSFR_RenderController"));
			}
		}
	}
}

//=============================================================================
// Thickness Pass Implementation
//=============================================================================

void RenderFluidThicknessPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	FRDGTextureRef& OutThicknessTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "FluidThicknessPass");

	// Thickness Texture 생성
	FRDGTextureDesc ThicknessDesc = FRDGTextureDesc::Create2D(
		View.UnscaledViewRect.Size(),
		PF_R16F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutThicknessTexture = GraphBuilder.CreateTexture(ThicknessDesc, TEXT("FluidThicknessTexture"));

	AddClearRenderTargetPass(GraphBuilder, OutThicknessTexture, FLinearColor::Black);

	// Collect active SSFR renderers (Legacy + New Architecture)
	TArray<FKawaiiFluidRenderResource*> Resources;
	TArray<float> Radii;
	TArray<FString> DebugNames;
	CollectActiveSSFRRenderers(Subsystem, Resources, Radii, DebugNames);

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

		UE_LOG(LogTemp, Log, TEXT("ThicknessPass (SSFR): %s with %d particles"),
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
			BufferDesc, TEXT("SSFRThicknessPositions"));

		GraphBuilder.QueueBufferUpload(ParticleBuffer, ParticlePositions.GetData(), BufferSize);
		FRDGBufferSRVRef ParticleBufferSRV = GraphBuilder.CreateSRV(ParticleBuffer);

		FMatrix ViewMatrix = View.ViewMatrices.GetViewMatrix();
		FMatrix ProjectionMatrix = View.ViewMatrices.GetProjectionMatrix();

		auto* PassParameters = GraphBuilder.AllocParameters<FFluidThicknessParameters>();
		PassParameters->ParticlePositions = ParticleBufferSRV;
		PassParameters->ParticleRadius = ParticleRadius;
		PassParameters->ViewMatrix = FMatrix44f(ViewMatrix);
		PassParameters->ProjectionMatrix = FMatrix44f(ProjectionMatrix);
		PassParameters->ThicknessScale = Subsystem->RenderingParameters.ThicknessScale;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(
			OutThicknessTexture, ERenderTargetLoadAction::ELoad);

		FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FFluidThicknessVS> VertexShader(GlobalShaderMap);
		TShaderMapRef<FFluidThicknessPS> PixelShader(GlobalShaderMap);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ThicknessDraw_SSFR_%s", *DebugName),
			PassParameters,
			ERDGPassFlags::Raster,
			[VertexShader, PixelShader, PassParameters, ParticleCount = ParticlePositions.Num()](
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
