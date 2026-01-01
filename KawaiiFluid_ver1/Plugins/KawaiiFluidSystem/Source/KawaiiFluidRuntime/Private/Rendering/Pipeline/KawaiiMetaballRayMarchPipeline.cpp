// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Pipeline/KawaiiMetaballRayMarchPipeline.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"

// Separated shading implementation
#include "Rendering/Shading/KawaiiRayMarchShadingImpl.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ScenePrivate.h"
#include "SceneTextures.h"

bool FKawaiiMetaballRayMarchPipeline::PrepareParticleBuffer(
	FRDGBuilder& GraphBuilder,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers)
{
	// Collect all particle positions from batch
	TArray<FVector3f> AllParticlePositions;
	float AverageParticleRadius = 10.0f;
	float TotalRadius = 0.0f;
	int32 ValidCount = 0;

	for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
	{
		FKawaiiFluidRenderResource* RenderResource = Renderer->GetFluidRenderResource();
		if (RenderResource && RenderResource->IsValid())
		{
			const TArray<FKawaiiRenderParticle>& CachedParticles = RenderResource->GetCachedParticles();
			for (const FKawaiiRenderParticle& Particle : CachedParticles)
			{
				AllParticlePositions.Add(Particle.Position);
			}
		}
		TotalRadius += Renderer->GetCachedParticleRadius();
		ValidCount++;
	}

	if (ValidCount > 0)
	{
		AverageParticleRadius = TotalRadius / ValidCount;
	}

	if (AllParticlePositions.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballRayMarchPipeline: No particles - skipping"));
		return false;
	}

	// Create RDG buffer for particle positions
	const uint32 BufferSize = AllParticlePositions.Num() * sizeof(FVector3f);
	FRDGBufferRef ParticleBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), AllParticlePositions.Num()),
		TEXT("RayMarchParticlePositions"));

	GraphBuilder.QueueBufferUpload(
		ParticleBuffer,
		AllParticlePositions.GetData(),
		BufferSize,
		ERDGInitialDataFlags::None);

	FRDGBufferSRVRef ParticleBufferSRV = GraphBuilder.CreateSRV(ParticleBuffer);

	// Build pipeline data
	CachedPipelineData.ParticleBufferSRV = ParticleBufferSRV;
	CachedPipelineData.ParticleCount = AllParticlePositions.Num();
	CachedPipelineData.ParticleRadius = AverageParticleRadius;

	// Check if SDF Volume optimization is enabled
	const bool bUseSDFVolume = RenderParams.bUseSDFVolumeOptimization;

	if (bUseSDFVolume)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "SDFVolumeBake");

		// Set volume resolution from parameters
		int32 Resolution = FMath::Clamp(RenderParams.SDFVolumeResolution, 32, 256);
		SDFVolumeManager.SetVolumeResolution(FIntVector(Resolution, Resolution, Resolution));

		// Calculate bounding box for volume
		FVector3f VolumeMin, VolumeMax;
		float Margin = AverageParticleRadius * 2.0f;
		CalculateParticleBoundingBox(AllParticlePositions, AverageParticleRadius, Margin, VolumeMin, VolumeMax);

		// Bake SDF volume using compute shader
		FRDGTextureSRVRef SDFVolumeSRV = SDFVolumeManager.BakeSDFVolume(
			GraphBuilder,
			ParticleBufferSRV,
			AllParticlePositions.Num(),
			AverageParticleRadius,
			RenderParams.SDFSmoothness,
			VolumeMin,
			VolumeMax);

		// Store SDF volume data
		CachedPipelineData.SDFVolumeData.SDFVolumeTextureSRV = SDFVolumeSRV;
		CachedPipelineData.SDFVolumeData.VolumeMin = VolumeMin;
		CachedPipelineData.SDFVolumeData.VolumeMax = VolumeMax;
		CachedPipelineData.SDFVolumeData.VolumeResolution = SDFVolumeManager.GetVolumeResolution();
		CachedPipelineData.SDFVolumeData.bUseSDFVolume = true;

		UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: Using SDF Volume optimization (%dx%dx%d)"),
			Resolution, Resolution, Resolution);
	}
	else
	{
		CachedPipelineData.SDFVolumeData.bUseSDFVolume = false;
		UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: Using direct particle iteration (legacy)"));
	}

	return true;
}

void FKawaiiMetaballRayMarchPipeline::ExecutePostBasePass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FScreenPassRenderTarget Output)
{
	if (Renderers.Num() == 0)
	{
		return;
	}

	// PostProcess mode uses PrepareForTonemap + ExecuteTonemap at Tonemap timing
	if (RenderParams.ShadingMode == EMetaballShadingMode::PostProcess)
	{
		// Nothing to do here - PostProcess mode preparation happens in PrepareForTonemap
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_RayMarching_PostBasePass");

	// Prepare particle buffer for GBuffer/Translucent modes
	if (!PrepareParticleBuffer(GraphBuilder, RenderParams, Renderers))
	{
		return;
	}

	// ShadingMode-specific processing at PostBasePass timing
	// Delegate to separated shading implementation
	switch (RenderParams.ShadingMode)
	{
	case EMetaballShadingMode::GBuffer:
	case EMetaballShadingMode::Opaque:
		KawaiiRayMarchShading::RenderGBufferShading(
			GraphBuilder, View, RenderParams, CachedPipelineData, SceneDepthTexture);
		break;

	case EMetaballShadingMode::Translucent:
		KawaiiRayMarchShading::RenderTranslucentGBufferWrite(
			GraphBuilder, View, RenderParams, CachedPipelineData, SceneDepthTexture);
		break;

	default:
		break;
	}

	UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: RayMarching PostBasePass completed (%d particles, ShadingMode=%d)"),
		CachedPipelineData.ParticleCount, static_cast<int32>(RenderParams.ShadingMode));
}

void FKawaiiMetaballRayMarchPipeline::PrepareForTonemap(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture)
{
	if (Renderers.Num() == 0)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_RayMarching_PrepareForTonemap");

	// Prepare particle buffer and optional SDF volume for PostProcess shading
	if (!PrepareParticleBuffer(GraphBuilder, RenderParams, Renderers))
	{
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: RayMarching PrepareForTonemap completed (%d particles)"),
		CachedPipelineData.ParticleCount);
}

void FKawaiiMetaballRayMarchPipeline::ExecutePrePostProcess(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output,
	FRDGTextureRef GBufferATexture,
	FRDGTextureRef GBufferDTexture)
{
	if (Renderers.Num() == 0)
	{
		return;
	}

	// Only Translucent mode uses PrePostProcess timing
	if (RenderParams.ShadingMode != EMetaballShadingMode::Translucent)
	{
		return;
	}

	// Validate cached pipeline data
	if (!CachedPipelineData.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballRayMarchPipeline: Missing cached pipeline data for PrePostProcess"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_RayMarching_PrePostProcess");

	// Delegate to separated shading implementation
	KawaiiRayMarchShading::RenderTranslucentTransparency(
		GraphBuilder, View, RenderParams,
		SceneDepthTexture, SceneColorTexture, Output,
		GBufferATexture, GBufferDTexture);

	UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: RayMarching PrePostProcess executed"));
}

void FKawaiiMetaballRayMarchPipeline::ExecuteTonemap(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	if (Renderers.Num() == 0)
	{
		return;
	}

	// Only PostProcess mode uses Tonemap timing
	if (RenderParams.ShadingMode != EMetaballShadingMode::PostProcess)
	{
		return;
	}

	// Validate cached pipeline data
	if (!CachedPipelineData.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballRayMarchPipeline: Missing cached pipeline data for Tonemap"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_RayMarching_Tonemap");

	// Delegate to separated shading implementation
	KawaiiRayMarchShading::RenderPostProcessShading(
		GraphBuilder, View, RenderParams, CachedPipelineData,
		SceneDepthTexture, SceneColorTexture, Output);

	UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: RayMarching Tonemap executed"));
}
