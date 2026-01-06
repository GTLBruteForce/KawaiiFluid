// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Pipeline/KawaiiMetaballRayMarchPipeline.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "Core/KawaiiRenderParticle.h"
#include "Rendering/Shaders/FluidSpatialHashShaders.h"
#include "Rendering/Shaders/ExtractRenderPositionsShaders.h"

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
					//UE_LOG(LogTemp, Log, TEXT("[Bounds Readback] Min(%.1f, %.1f, %.1f) Max(%.1f, %.1f, %.1f) Size(%.1f, %.1f, %.1f)"),ReadMin.X, ReadMin.Y, ReadMin.Z,ReadMax.X, ReadMax.Y, ReadMax.Z,Size.X, Size.Y, Size.Z);
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

	//UE_LOG(LogTemp, Warning, TEXT("=== PrepareParticleBuffer [Frame %llu, Call #%d] ==="), CurrentFrame, CallsThisFrame);
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
	FRDGBufferSRVRef CachedGPUBoundsBufferSRV = nullptr;  // GPU bounds buffer for same-frame use
	bool bUsingGPUBuffer = false;
	TArray<FVector3f> AllParticlePositions; // For CPU mode bounding box calculation

	// First pass: check for GPU simulation mode and access simulator buffer directly
	// This runs on RENDER THREAD - safe to access GPU simulator's PersistentParticleBuffer
	for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
	{
		// ========== GPU SIMULATION MODE: Direct buffer access (render thread safe) ==========
		FGPUFluidSimulator* GPUSimulator = Renderer->GetGPUSimulator();

		UE_LOG(LogTemp, Warning, TEXT("[RayMarchPipeline] Renderer=%s, GPUSimulator=%s"),
			*Renderer->GetName(),
			GPUSimulator ? TEXT("VALID") : TEXT("NULL"));

		if (GPUSimulator)
		{
			// Get the persistent particle buffer directly from GPU simulator
			// This is thread-safe: both simulation and rendering run on render thread
			TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer = GPUSimulator->GetPersistentParticleBuffer();
			const int32 PhysicsParticleCount = GPUSimulator->GetPersistentParticleCount();

			UE_LOG(LogTemp, Warning, TEXT("[RayMarchPipeline] GPU Mode: PooledValid=%d, ParticleCount=%d"),
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

				// Create bounds buffer for same-frame GPU bounds (no readback needed)
				FRDGBufferRef GPUBoundsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), 2),
					TEXT("GPUParticleBounds"));
				FRDGBufferUAVRef GPUBoundsBufferUAV = GraphBuilder.CreateUAV(GPUBoundsBuffer);

				// OPTIMIZED: Single merged pass - ExtractRenderData + CalculateBounds
				float ParticleRadius = Renderer->GetCachedParticleRadius();
				float BoundsMargin = ParticleRadius * 2.0f;
				FGPUFluidSimulatorPassBuilder::AddExtractRenderDataWithBoundsPass(
					GraphBuilder,
					PhysicsBufferSRV,
					RenderBufferUAV,
					GPUBoundsBufferUAV,
					PhysicsParticleCount,
					ParticleRadius,
					BoundsMargin);

				// Create SRVs for subsequent passes
				ParticleBufferSRV = GraphBuilder.CreateSRV(RenderBuffer);
				TotalParticleCount = PhysicsParticleCount;
				AverageParticleRadius = ParticleRadius;
				bUsingGPUBuffer = true;

				// Store bounds buffer SRV for SDF baking and ray marching
				CachedGPUBoundsBufferSRV = GraphBuilder.CreateSRV(GPUBoundsBuffer);

				// Create SoA Position buffer and extract directly (GPU mode optimization)
				FRDGBufferRef PositionSoABuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), PhysicsParticleCount),
					TEXT("RenderPositionsSoA"));
				FRDGBufferUAVRef PositionSoAUAV = GraphBuilder.CreateUAV(PositionSoABuffer);

				// Velocity dummy buffer (required by AddExtractRenderDataSoAPass)
				FRDGBufferRef VelocitySoABuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), PhysicsParticleCount),
					TEXT("RenderVelocitiesSoA"));
				FRDGBufferUAVRef VelocitySoAUAV = GraphBuilder.CreateUAV(VelocitySoABuffer);

				// Extract SoA data directly from Physics buffer (62% bandwidth reduction)
				FGPUFluidSimulatorPassBuilder::AddExtractRenderDataSoAPass(
					GraphBuilder,
					PhysicsBufferSRV,
					PositionSoAUAV,
					VelocitySoAUAV,
					PhysicsParticleCount,
					ParticleRadius);

				CachedPipelineData.PositionBufferSRV = GraphBuilder.CreateSRV(PositionSoABuffer);
				CachedPipelineData.bUseSoABuffers = true;

				UE_LOG(LogTemp, Verbose, TEXT("  >>> SoA Buffers extracted directly (GPU mode, %d particles)"),
					PhysicsParticleCount);

				UE_LOG(LogTemp, Verbose, TEXT("  >>> GPU MODE: ExtractRenderData+Bounds MERGED (%d particles, radius: %.2f)"),
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

		// ========== CPU 모드 SoA 버퍼 생성 ==========
		FRDGBufferRef PositionBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), AllParticlePositions.Num()),
			TEXT("CPUModePositionsSoA"));

		GraphBuilder.QueueBufferUpload(
			PositionBuffer,
			AllParticlePositions.GetData(),
			AllParticlePositions.Num() * sizeof(FVector3f),
			ERDGInitialDataFlags::None);

		CachedPipelineData.PositionBufferSRV = GraphBuilder.CreateSRV(PositionBuffer);
		CachedPipelineData.bUseSoABuffers = true;
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

		FRDGTextureSRVRef SDFVolumeSRV = nullptr;
		FVector3f VolumeMin, VolumeMax;
		bool bUseGPUBoundsForRayMarch = false;

		if (bUsingGPUBuffer && CachedGPUBoundsBufferSRV)
		{
			// GPU MODE: Use bounds from GPU buffer directly (no readback latency!)
			// Both SDF Bake and Ray March will read bounds from the same GPU buffer
			SDFVolumeSRV = SDFVolumeManager.BakeSDFVolumeWithGPUBoundsDirect(
				GraphBuilder,
				ParticleBufferSRV,
				TotalParticleCount,
				AverageParticleRadius,
				RenderParams.SDFSmoothness,
				CachedGPUBoundsBufferSRV);

			// For debug visualization, we don't have exact bounds (they're in GPU buffer)
			// Use placeholder values - actual rendering uses GPU buffer
			VolumeMin = FVector3f(-500.0f, -500.0f, -500.0f);
			VolumeMax = FVector3f(500.0f, 500.0f, 500.0f);
			bUseGPUBoundsForRayMarch = true;

			UE_LOG(LogTemp, Verbose, TEXT("GPU Mode: SDF Bake + Ray March using GPU bounds buffer (zero latency)"));
		}
		else
		{
			// CPU mode: Calculate bounds from particle positions
			float Margin = AverageParticleRadius * 2.0f;
			CalculateParticleBoundingBox(AllParticlePositions, AverageParticleRadius, Margin, VolumeMin, VolumeMax);

			// Bake SDF volume using compute shader (CPU bounds path)
			SDFVolumeSRV = SDFVolumeManager.BakeSDFVolume(
				GraphBuilder,
				ParticleBufferSRV,
				TotalParticleCount,
				AverageParticleRadius,
				RenderParams.SDFSmoothness,
				VolumeMin,
				VolumeMax);
		}

		// Store SDF volume data
		CachedPipelineData.SDFVolumeData.SDFVolumeTextureSRV = SDFVolumeSRV;
		CachedPipelineData.SDFVolumeData.VolumeMin = VolumeMin;
		CachedPipelineData.SDFVolumeData.VolumeMax = VolumeMax;
		CachedPipelineData.SDFVolumeData.VolumeResolution = SDFVolumeManager.GetVolumeResolution();
		CachedPipelineData.SDFVolumeData.bUseSDFVolume = true;
		CachedPipelineData.SDFVolumeData.bUseGPUBounds = bUseGPUBoundsForRayMarch;
		CachedPipelineData.SDFVolumeData.BoundsBufferSRV = CachedGPUBoundsBufferSRV;

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

		// ========== HYBRID MODE: Build Spatial Hash for precise final evaluation ==========
		// SDF Volume handles 90% of ray distance (fast O(1) sampling)
		// Spatial Hash handles 10% final evaluation (precise O(k) lookup)
		const bool bUseSpatialHash = RenderParams.bUseSpatialHash;
		if (bUseSpatialHash)
		{
			RDG_EVENT_SCOPE(GraphBuilder, "SpatialHashBuild_Hybrid");

			// Cell size = SearchRadius to ensure 3x3x3 search covers all neighbors
			// SearchRadius = ParticleRadius * 2.0 + Smoothness
			float SearchRadius = AverageParticleRadius * 2.0f + RenderParams.SDFSmoothness;
			float CellSize = SearchRadius;

			// Step 1: Extract float3 positions from FKawaiiRenderParticle buffer
			FRDGBufferRef PositionBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), TotalParticleCount),
				TEXT("SpatialHash.ExtractedPositions"));
			FRDGBufferUAVRef PositionUAV = GraphBuilder.CreateUAV(PositionBuffer);
			FRDGBufferSRVRef PositionSRV = GraphBuilder.CreateSRV(PositionBuffer);

			FExtractRenderPositionsPassBuilder::AddExtractPositionsPass(
				GraphBuilder,
				ParticleBufferSRV,
				PositionUAV,
				TotalParticleCount);

			// Step 2: Build Multi-pass Spatial Hash with extracted positions
			FSpatialHashMultipassResources HashResources;
			bool bHashSuccess = FSpatialHashBuilder::CreateAndBuildHashMultipass(
				GraphBuilder,
				PositionSRV,
				TotalParticleCount,
				CellSize,
				HashResources);

			if (bHashSuccess && HashResources.IsValid())
			{
				CachedPipelineData.SpatialHashData.bUseSpatialHash = true;
				CachedPipelineData.SpatialHashData.CellDataSRV = HashResources.CellDataSRV;
				CachedPipelineData.SpatialHashData.ParticleIndicesSRV = HashResources.ParticleIndicesSRV;
				CachedPipelineData.SpatialHashData.CellSize = CellSize;

				UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: HYBRID MODE - SDF Volume + Spatial Hash (%d particles, CellSize: %.2f)"),
					TotalParticleCount, CellSize);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: Spatial Hash build failed for Hybrid mode, using SDF Volume only"));
				CachedPipelineData.SpatialHashData.bUseSpatialHash = false;
			}
		}
		else
		{
			CachedPipelineData.SpatialHashData.bUseSpatialHash = false;
		}
	}
	else
	{
		// No SDF Volume: Direct O(N) particle iteration (legacy mode)
		CachedPipelineData.SDFVolumeData.bUseSDFVolume = false;
		CachedPipelineData.SpatialHashData.bUseSpatialHash = false;
		UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: Using direct particle iteration (legacy O(N))"));
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

void FKawaiiMetaballRayMarchPipeline::PrepareRender(
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

void FKawaiiMetaballRayMarchPipeline::ExecuteRender(
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

	// Reset intermediate textures from previous frame
	CachedIntermediateTextures.Reset();

	// Check if shadows are enabled (need depth output for VSM shadow projection)
	const bool bOutputDepth = RenderParams.bEnableShadowCasting && RenderParams.ShadowIntensity > 0.0f;
	FRDGTextureRef FluidDepthTexture = nullptr;

	// Delegate to separated shading implementation
	KawaiiRayMarchShading::RenderPostProcessShading(
		GraphBuilder, View, RenderParams, CachedPipelineData,
		SceneDepthTexture, SceneColorTexture, Output,
		bOutputDepth, &FluidDepthTexture);

	// Store depth texture for shadow history (reuse SmoothedDepthTexture field)
	if (bOutputDepth && FluidDepthTexture)
	{
		CachedIntermediateTextures.SmoothedDepthTexture = FluidDepthTexture;
		UE_LOG(LogTemp, Log, TEXT("KawaiiFluid: RayMarching depth output stored for shadow projection"));
	}

	UE_LOG(LogTemp, Verbose, TEXT("KawaiiFluid: RayMarching Tonemap executed"));
}
