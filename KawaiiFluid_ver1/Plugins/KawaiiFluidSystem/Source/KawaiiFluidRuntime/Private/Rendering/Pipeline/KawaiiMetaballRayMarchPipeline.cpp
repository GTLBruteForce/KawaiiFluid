// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Pipeline/KawaiiMetaballRayMarchPipeline.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "SceneView.h"

void FKawaiiMetaballRayMarchPipeline::Execute(
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

	if (!ShadingPass)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballRayMarchPipeline: No ShadingPass set"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "MetaballPipeline_RayMarching");

	// 1. Collect all particle positions from batch
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
		return;
	}

	// 2. Create RDG buffer for particle positions
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

	// 3. Check if SDF Volume optimization is enabled
	const bool bUseSDFVolume = RenderParams.bUseSDFVolumeOptimization;

	if (bUseSDFVolume)
	{
		// ============================================
		// Optimized Path: Bake SDF to 3D Volume Texture
		// ============================================
		RDG_EVENT_SCOPE(GraphBuilder, "SDFVolumeBake");

		// Set volume resolution from parameters
		int32 Resolution = FMath::Clamp(RenderParams.SDFVolumeResolution, 32, 256);
		SDFVolumeManager.SetVolumeResolution(FIntVector(Resolution, Resolution, Resolution));

		// Calculate bounding box for volume
		FVector3f VolumeMin, VolumeMax;
		float Margin = AverageParticleRadius * 2.0f;
		CalculateParticleBoundingBox(AllParticlePositions, AverageParticleRadius, Margin, VolumeMin, VolumeMax);

		UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: SDF Volume Bake - Min:(%.1f,%.1f,%.1f) Max:(%.1f,%.1f,%.1f)"),
			VolumeMin.X, VolumeMin.Y, VolumeMin.Z,
			VolumeMax.X, VolumeMax.Y, VolumeMax.Z);

		// Bake SDF volume using compute shader
		FRDGTextureSRVRef SDFVolumeSRV = SDFVolumeManager.BakeSDFVolume(
			GraphBuilder,
			ParticleBufferSRV,
			AllParticlePositions.Num(),
			AverageParticleRadius,
			RenderParams.SDFSmoothness,
			VolumeMin,
			VolumeMax);

		// Set volume data for shading pass
		FSDFVolumeData SDFVolumeData;
		SDFVolumeData.SDFVolumeTextureSRV = SDFVolumeSRV;
		SDFVolumeData.VolumeMin = VolumeMin;
		SDFVolumeData.VolumeMax = VolumeMax;
		SDFVolumeData.VolumeResolution = SDFVolumeManager.GetVolumeResolution();
		SDFVolumeData.bUseSDFVolume = true;
		ShadingPass->SetSDFVolumeData(SDFVolumeData);

		UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: Using SDF Volume optimization (%dx%dx%d)"),
			SDFVolumeManager.GetVolumeResolution().X,
			SDFVolumeManager.GetVolumeResolution().Y,
			SDFVolumeManager.GetVolumeResolution().Z);
	}
	else
	{
		// ============================================
		// Legacy Path: Direct particle iteration
		// ============================================
		FSDFVolumeData SDFVolumeData;
		SDFVolumeData.bUseSDFVolume = false;
		ShadingPass->SetSDFVolumeData(SDFVolumeData);

		UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: Using direct particle iteration (legacy)"));
	}

	// 4. Delegate to ShadingPass for ray marching
	ShadingPass->RenderForRayMarchingPipeline(
		GraphBuilder,
		View,
		RenderParams,
		ParticleBufferSRV,
		AllParticlePositions.Num(),
		AverageParticleRadius,
		SceneDepthTexture,
		SceneColorTexture,
		Output);

	UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: Ray Marching rendered %d particles"), AllParticlePositions.Num());
}
