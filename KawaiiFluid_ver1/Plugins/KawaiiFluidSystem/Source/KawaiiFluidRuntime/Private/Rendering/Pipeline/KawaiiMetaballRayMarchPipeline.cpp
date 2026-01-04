// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Pipeline/KawaiiMetaballRayMarchPipeline.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "Core/KawaiiRenderParticle.h"

// Separated shading implementation
#include "Rendering/Shading/KawaiiRayMarchShadingImpl.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "SceneView.h"
#include "ScenePrivate.h"
#include "SceneTextures.h"

void FKawaiiMetaballRayMarchPipeline::ProcessPendingBoundsReadback()
{
	// Process readback from previous frame (if available)
	if (bHasPendingBoundsReadback && PendingBoundsReadbackBuffer.IsValid())
	{
		// Map the pooled buffer and read bounds using RHI command list
		FRHIBuffer* BufferRHI = PendingBoundsReadbackBuffer->GetRHI();
		if (BufferRHI)
		{
			// Get the immediate command list for buffer operations
			FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();

			// Lock buffer for reading
			void* MappedData = RHICmdList.LockBuffer(BufferRHI, 0, sizeof(FVector3f) * 2, RLM_ReadOnly);
			if (MappedData)
			{
				FVector3f* BoundsData = static_cast<FVector3f*>(MappedData);
				FVector3f ReadMin = BoundsData[0];
				FVector3f ReadMax = BoundsData[1];
				RHICmdList.UnlockBuffer(BufferRHI);

				// Validate bounds (check for NaN or extreme values)
				bool bValidBounds = !ReadMin.ContainsNaN() && !ReadMax.ContainsNaN() &&
					ReadMin.X < ReadMax.X && ReadMin.Y < ReadMax.Y && ReadMin.Z < ReadMax.Z;

				if (bValidBounds)
				{
					// Update cached bounds for this frame
					SDFVolumeManager.UpdateCachedBoundsFromReadback(ReadMin, ReadMax);

					// Debug log - every frame for debugging
					FVector3f Size = ReadMax - ReadMin;
					UE_LOG(LogTemp, Log, TEXT("[Bounds Readback] Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f) Size(%.1f, %.1f, %.1f)"),
						ReadMin.X, ReadMin.Y, ReadMin.Z,
						ReadMax.X, ReadMax.Y, ReadMax.Z,
						Size.X, Size.Y, Size.Z);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("RayMarchPipeline: Invalid GPU bounds detected (Min: %.1f,%.1f,%.1f Max: %.1f,%.1f,%.1f)"),
						ReadMin.X, ReadMin.Y, ReadMin.Z,
						ReadMax.X, ReadMax.Y, ReadMax.Z);
				}
			}
		}
		bHasPendingBoundsReadback = false;
	}
}

bool FKawaiiMetaballRayMarchPipeline::PrepareParticleBuffer(
	FRDGBuilder& GraphBuilder,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers)
{
	// ========== DEBUG LOGGING ==========
	static uint64 FrameCounter = 0;
	static uint64 LastFrameNumber = 0;
	static int32 CallsThisFrame = 0;

	uint64 CurrentFrame = GFrameCounter;
	if (CurrentFrame != LastFrameNumber)
	{
		LastFrameNumber = CurrentFrame;
		CallsThisFrame = 0;
	}
	CallsThisFrame++;

	UE_LOG(LogTemp, Warning, TEXT("=== PrepareParticleBuffer [Frame %llu, Call #%d] ==="), CurrentFrame, CallsThisFrame);
	// ====================================

	// Process readback from previous frame first
	ProcessPendingBoundsReadback();

	// Phase 2: Try to use GPU buffer directly (no CPU involvement)
	// This ensures GPU simulation results are used for SDF baking

	float AverageParticleRadius = 10.0f;
	float TotalRadius = 0.0f;
	int32 ValidCount = 0;
	int32 TotalParticleCount = 0;
	FRDGBufferSRVRef ParticleBufferSRV = nullptr;
	bool bUsingGPUBuffer = false;
	TArray<FVector3f> AllParticlePositions; // For CPU mode bounding box calculation

	// First pass: check for GPU simulation mode and access simulator buffer directly
	// This runs on RENDER THREAD - safe to access GPU simulator's PersistentParticleBuffer
	for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
	{
		// ========== GPU SIMULATION MODE: Direct buffer access (render thread safe) ==========
		FGPUFluidSimulator* GPUSimulator = Renderer->GetGPUSimulator();
		if (GPUSimulator)
		{
			// Get the persistent particle buffer directly from GPU simulator
			// This is thread-safe: both simulation and rendering run on render thread
			TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer = GPUSimulator->GetPersistentParticleBuffer();
			const int32 PhysicsParticleCount = GPUSimulator->GetPersistentParticleCount();

			UE_LOG(LogTemp, Warning, TEXT("  GPU Simulator: PooledValid=%d, ParticleCount=%d"),
				PhysicsPooledBuffer.IsValid() ? 1 : 0, PhysicsParticleCount);

			if (PhysicsPooledBuffer.IsValid() && PhysicsParticleCount > 0)
			{
				// Register the physics buffer (FGPUFluidParticle format - 64 bytes)
				FRDGBufferRef PhysicsBuffer = GraphBuilder.RegisterExternalBuffer(
					PhysicsPooledBuffer,
					TEXT("GPUPhysicsParticles"));
				FRDGBufferSRVRef PhysicsBufferSRV = GraphBuilder.CreateSRV(PhysicsBuffer);

				// Create render buffer for converted data (FKawaiiRenderParticle format - 32 bytes)
				FRDGBufferRef RenderBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FKawaiiRenderParticle), PhysicsParticleCount),
					TEXT("GPURenderParticles"));
				FRDGBufferUAVRef RenderBufferUAV = GraphBuilder.CreateUAV(RenderBuffer);

				// Add extract render data pass to convert Physics → Render format
				float ParticleRadius = Renderer->GetCachedParticleRadius();
				FGPUFluidSimulatorPassBuilder::AddExtractRenderDataPass(
					GraphBuilder,
					PhysicsBufferSRV,
					RenderBufferUAV,
					PhysicsParticleCount,
					ParticleRadius);

				// Create SRV from converted render buffer for subsequent passes
				ParticleBufferSRV = GraphBuilder.CreateSRV(RenderBuffer);
				TotalParticleCount = PhysicsParticleCount;
				AverageParticleRadius = ParticleRadius;
				bUsingGPUBuffer = true;

				UE_LOG(LogTemp, Warning, TEXT("  >>> CONVERTED GPU PHYSICS → RENDER (%d particles, radius: %.2f)"),
					TotalParticleCount, ParticleRadius);
				break; // Use first valid GPU simulator (batching not supported in GPU mode yet)
			}
			else
			{
				// GPU mode but buffer not ready yet - skip rendering this frame
				UE_LOG(LogTemp, Warning, TEXT("  >>> GPU MODE - BUFFER NOT READY YET"));
				CachedPipelineData.Reset();
				return false;
			}
		}

		// ========== CPU SIMULATION MODE: Fall through to RenderResource path ==========
		FKawaiiFluidRenderResource* RenderResource = Renderer->GetFluidRenderResource();

		// DEBUG: Log buffer state
		if (RenderResource)
		{
			int32 CachedCount = RenderResource->GetCachedParticles().Num();

			UE_LOG(LogTemp, Warning, TEXT("  RenderResource (CPU mode): CachedCount=%d"), CachedCount);

			if (CachedCount > 0)
			{
				const FKawaiiRenderParticle& FirstCached = RenderResource->GetCachedParticles()[0];
				UE_LOG(LogTemp, Warning, TEXT("  -> CachedParticle[0] Position: (%.1f, %.1f, %.1f)"),
					FirstCached.Position.X, FirstCached.Position.Y, FirstCached.Position.Z);
			}
		}

		TotalRadius += Renderer->GetCachedParticleRadius();
		ValidCount++;
	}

	// CPU mode fallback: use CPU cache
	// Note: If we reach here, we're definitely NOT in GPU mode
	// (GPU mode would have returned early with buffer or "not ready")
	if (!bUsingGPUBuffer)
	{
		UE_LOG(LogTemp, Warning, TEXT("  >>> USING CPU CACHE (CPU simulation mode)"));

		TArray<FKawaiiRenderParticle> AllParticles;

		for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
		{
			FKawaiiFluidRenderResource* RenderResource = Renderer->GetFluidRenderResource();
			if (RenderResource && RenderResource->IsValid())
			{
				const TArray<FKawaiiRenderParticle>& CachedParticles = RenderResource->GetCachedParticles();
				AllParticles.Append(CachedParticles);

				// Log first cached particle position
				if (CachedParticles.Num() > 0)
				{
					UE_LOG(LogTemp, Warning, TEXT("  CPU Fallback using %d cached particles, First: (%.1f, %.1f, %.1f)"),
						CachedParticles.Num(),
						CachedParticles[0].Position.X,
						CachedParticles[0].Position.Y,
						CachedParticles[0].Position.Z);
				}
			}
		}

		if (AllParticles.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballRayMarchPipeline: No particles - skipping"));
			CachedPipelineData.Reset();
			return false;
		}

		// Extract positions for bounding box calculation (CPU mode only)
		AllParticlePositions.Reserve(AllParticles.Num());
		for (const FKawaiiRenderParticle& Particle : AllParticles)
		{
			AllParticlePositions.Add(Particle.Position);
		}

		// Create RDG buffer for particles (FKawaiiRenderParticle format)
		const uint32 BufferSize = AllParticles.Num() * sizeof(FKawaiiRenderParticle);
		FRDGBufferRef ParticleBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FKawaiiRenderParticle), AllParticles.Num()),
			TEXT("RayMarchRenderParticles"));

		GraphBuilder.QueueBufferUpload(
			ParticleBuffer,
			AllParticles.GetData(),
			BufferSize,
			ERDGInitialDataFlags::None);

		ParticleBufferSRV = GraphBuilder.CreateSRV(ParticleBuffer);
		TotalParticleCount = AllParticles.Num();
	}

	if (ValidCount > 0)
	{
		AverageParticleRadius = TotalRadius / ValidCount;
	}

	if (TotalParticleCount == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FKawaiiMetaballRayMarchPipeline: No particles - skipping"));
		CachedPipelineData.Reset();
		return false;
	}

	// Build pipeline data
	CachedPipelineData.ParticleBufferSRV = ParticleBufferSRV;
	CachedPipelineData.ParticleCount = TotalParticleCount;
	CachedPipelineData.ParticleRadius = AverageParticleRadius;

	// Check if SDF Volume optimization is enabled
	// GPU mode now supports SDF Volume - ExtractRenderData validates particle positions
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
		FRDGBufferRef BoundsBuffer = nullptr;

		if (bUsingGPUBuffer)
		{
			// GPU mode: Calculate bounds using parallel reduction on GPU
			// This runs as a compute shader and calculates accurate bounds from actual particle positions
			BoundsBuffer = SDFVolumeManager.CalculateGPUBounds(
				GraphBuilder,
				ParticleBufferSRV,
				TotalParticleCount,
				AverageParticleRadius,
				Margin);

			// Queue buffer extraction for readback next frame
			// This uses RDG's safe extraction pattern: extract to pooled buffer,
			// read from pooled buffer next frame (1-frame latency)
			GraphBuilder.QueueBufferExtraction(BoundsBuffer, &PendingBoundsReadbackBuffer);
			bHasPendingBoundsReadback = true;

			// Use cached GPU bounds from previous frame (1-frame latency)
			if (SDFVolumeManager.HasValidGPUBounds())
			{
				SDFVolumeManager.GetLastGPUBounds(VolumeMin, VolumeMax);

				// Debug log - every frame
				FVector3f Size = VolumeMax - VolumeMin;
				UE_LOG(LogTemp, Log, TEXT("[Bounds Used] Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f) Size(%.1f, %.1f, %.1f)"),
					VolumeMin.X, VolumeMin.Y, VolumeMin.Z,
					VolumeMax.X, VolumeMax.Y, VolumeMax.Z,
					Size.X, Size.Y, Size.Z);
			}
			else
			{
				// First frame: use component position as initial bounds center
				// This provides reasonable bounds until GPU readback completes
				FVector3f SpawnCenter = FVector3f::ZeroVector;
				for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
				{
					if (Renderer)
					{
						SpawnCenter = FVector3f(Renderer->GetSpawnPositionHint());
						break;  // Use first renderer's position
					}
				}

				float DefaultExtent = 100.0f;  // Reasonable initial extent around spawn point
				VolumeMin = SpawnCenter - FVector3f(DefaultExtent, DefaultExtent, DefaultExtent);
				VolumeMax = SpawnCenter + FVector3f(DefaultExtent, DefaultExtent, DefaultExtent);

				UE_LOG(LogTemp, Warning, TEXT("[Bounds Used] FIRST FRAME - using spawn position hint: (%.1f, %.1f, %.1f)"),
					SpawnCenter.X, SpawnCenter.Y, SpawnCenter.Z);
			}
		}
		else
		{
			// CPU mode: Calculate bounds from particle positions
			CalculateParticleBoundingBox(AllParticlePositions, AverageParticleRadius, Margin, VolumeMin, VolumeMax);
		}

		// Bake SDF volume using compute shader
		FRDGTextureSRVRef SDFVolumeSRV = SDFVolumeManager.BakeSDFVolume(
			GraphBuilder,
			ParticleBufferSRV,
			TotalParticleCount,
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

		// Notify renderers of SDF volume bounds for debug visualization
		for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
		{
			if (Renderer && Renderer->GetLocalParameters().bDebugDrawSDFVolume)
			{
				Renderer->SetSDFVolumeBounds(FVector(VolumeMin), FVector(VolumeMax));
			}
		}

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
	FRDGTextureRef SceneDepthTexture)
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
