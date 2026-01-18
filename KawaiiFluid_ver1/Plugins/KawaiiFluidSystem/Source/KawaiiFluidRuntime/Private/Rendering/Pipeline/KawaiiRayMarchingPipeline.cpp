// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Pipeline/KawaiiRayMarchingPipeline.h"
#include "Rendering/RayMarching/FluidRayMarchingShaders.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "GPU/GPUFluidSimulator.h"
#include "Core/KawaiiFluidSimulationContext.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessing.h"
#include "ScenePrivate.h"

//=============================================================================
// Shader Implementations
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FTileCullingCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidTileCulling.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRayMarchingVS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidRayMarching.usf", "RayMarchingVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FRayMarchingMainPS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidRayMarching.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FRayMarchingSDFPS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidRayMarchingSDF.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FRayMarchingCompositePS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidRayMarching.usf", "CompositePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FTemporalBlendPS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidTemporalBlend.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGenerateMotionVectorsCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidGenerateMotionVectors.usf", "MainCS", SF_Compute);

//=============================================================================
// FKawaiiRayMarchingPipeline Implementation
//=============================================================================

FKawaiiRayMarchingPipeline::FKawaiiRayMarchingPipeline()
{
	VolumeBuilder = MakeUnique<FFluidVolumeBuilder>();
}

FKawaiiRayMarchingPipeline::~FKawaiiRayMarchingPipeline()
{
}

void FKawaiiRayMarchingPipeline::ExecutePostBasePass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture)
{
	// Ray marching doesn't use PostBasePass for GBuffer writes
	// All rendering happens at PrePostProcess/Tonemap timing

	static int32 LogFrameCounter = 0;
	if (++LogFrameCounter % 60 == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] ExecutePostBasePass - Renderers: %d"), Renderers.Num());
	}
}

void FKawaiiRayMarchingPipeline::PrepareRender(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RayMarching.PrepareRender");

	static int32 LogFrameCounter = 0;
	bool bShouldLog = (++LogFrameCounter % 60 == 0);

	if (bShouldLog)
	{
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] PrepareRender - Renderers: %d"), Renderers.Num());
	}

	if (Renderers.Num() == 0)
	{
		if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] PrepareRender - No renderers, returning early"));
		return;
	}

	GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	// Collect particle data from all renderers
	// For now, use the first renderer with valid Z-Order data
	FFluidVolumeInput VolumeInput;
	bool bFoundValidData = false;

	for (UKawaiiFluidMetaballRenderer* Renderer : Renderers)
	{
		if (!Renderer)
		{
			if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] PrepareRender - Null renderer"));
			continue;
		}

		if (!Renderer->IsRenderingActive())
		{
			if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] PrepareRender - Renderer not active (ParticleCount: %d)"),
				Renderer->LastRenderedParticleCount);
			continue;
		}

		// Get RenderResource which has access to Z-Order buffers
		FKawaiiFluidRenderResource* RenderResource = Renderer->GetFluidRenderResource();
		if (!RenderResource)
		{
			if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] PrepareRender - No RenderResource available"));
			continue;
		}

		// Get GPUSimulator for simulation bounds
		FGPUFluidSimulator* GPUSimulator = RenderResource->GetGPUSimulator();
		if (!GPUSimulator)
		{
			if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] PrepareRender - No GPUSimulator"));
			continue;
		}

		// Ensure Z-Order buffer extraction is enabled for next simulation
		// This must be done before checking HasValidZOrderBuffers
		if (!GPUSimulator->IsExtractZOrderBuffersEnabled())
		{
			GPUSimulator->SetExtractZOrderBuffersForRayMarching(true);
			if (bShouldLog) UE_LOG(LogTemp, Log, TEXT("[RayMarching] PrepareRender - Enabled Z-Order buffer extraction, will be available next frame"));
		}

		// Check if Z-Order buffers are available (may not be on first frame after enabling)
		if (!RenderResource->HasValidZOrderBuffers())
		{
			if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] PrepareRender - Z-Order buffers not valid yet, waiting for next frame"));
			continue;
		}

		// Get pooled buffers
		TRefCountPtr<FRDGPooledBuffer> ParticlePooled = GPUSimulator->GetPersistentParticleBuffer();
		TRefCountPtr<FRDGPooledBuffer> CellStartPooled = RenderResource->GetPooledCellStartBuffer();
		TRefCountPtr<FRDGPooledBuffer> CellEndPooled = RenderResource->GetPooledCellEndBuffer();

		if (!ParticlePooled.IsValid() || !CellStartPooled.IsValid() || !CellEndPooled.IsValid())
		{
			if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] PrepareRender - One or more Z-Order buffers invalid"));
			continue;
		}

		// Store pooled buffers for cross-GraphBuilder usage (Hybrid Mode fix)
		// RDG buffers are only valid within the same GraphBuilder instance
		// Pooled buffers persist across frames and can be re-registered
		VolumeInput.SortedParticlesPooled = ParticlePooled;
		VolumeInput.CellStartPooled = CellStartPooled;
		VolumeInput.CellEndPooled = CellEndPooled;

		// Register external buffers with RDG (valid for this GraphBuilder only)
		VolumeInput.SortedParticles = GraphBuilder.RegisterExternalBuffer(ParticlePooled, TEXT("RayMarching.SortedParticles"));
		VolumeInput.CellStart = GraphBuilder.RegisterExternalBuffer(CellStartPooled, TEXT("RayMarching.CellStart"));
		VolumeInput.CellEnd = GraphBuilder.RegisterExternalBuffer(CellEndPooled, TEXT("RayMarching.CellEnd"));

		// Get particle data
		VolumeInput.ParticleCount = GPUSimulator->GetParticleCount();
		VolumeInput.ParticleRadius = Renderer->GetCachedParticleRadius();
		VolumeInput.SmoothingRadius = VolumeInput.ParticleRadius * 2.0f;  // Default: 2x radius

		// CRITICAL: CellSize must match simulation's Z-Order sorting CellSize
		// Otherwise Morton code computation will produce different cell IDs
		VolumeInput.CellSize = GPUSimulator->GetCellSize();
		if (VolumeInput.CellSize <= 0.0f)
		{
			// Fallback: derive from SmoothingRadius (may not match simulation)
			VolumeInput.CellSize = VolumeInput.SmoothingRadius;
			if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] CellSize from GPUSimulator is 0, using fallback: %.2f"), VolumeInput.CellSize);
		}

		// Get simulation bounds from GPUSimulator
		FVector3f SimBoundsMin, SimBoundsMax;
		GPUSimulator->GetSimulationBounds(SimBoundsMin, SimBoundsMax);
		VolumeInput.BoundsMin = SimBoundsMin;
		VolumeInput.BoundsMax = SimBoundsMax;

		// Morton bounds for Z-Order cell ID calculation (same as simulation bounds)
		VolumeInput.MortonBoundsMin = SimBoundsMin;

		// DEBUG: Log bounds info to diagnose particle size issue (first 10 frames only)
		{
			static int32 DebugLogCount = 0;
			if (DebugLogCount < 10)
			{
				++DebugLogCount;
				const FVector3f VolumeSize = SimBoundsMax - SimBoundsMin;
				const float VoxelSize = VolumeSize.GetMax() / 256.0f;  // Assuming VolumeResolution=256
				UE_LOG(LogTemp, Warning, TEXT("[RayMarching DEBUG] Bounds: (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f), VolumeSize: %.1f, VoxelSize: %.2f cm, ParticleRadius: %.2f cm"),
					SimBoundsMin.X, SimBoundsMin.Y, SimBoundsMin.Z,
					SimBoundsMax.X, SimBoundsMax.Y, SimBoundsMax.Z,
					VolumeSize.GetMax(),
					VoxelSize,
					VolumeInput.ParticleRadius);
			}
		}

		// Pre-compute Poly6 kernel coefficient
		// Poly6Coeff = 315 / (64 * PI * h^9)  [in meters]
		const float h_m = VolumeInput.SmoothingRadius * 0.01f;  // cm to m
		VolumeInput.Poly6Coeff = 315.0f / (64.0f * PI * FMath::Pow(h_m, 9.0f));

		if (bShouldLog)
		{
			// Log basic info
			UE_LOG(LogTemp, Log, TEXT("[RayMarching] PrepareRender - Found Z-Order data: ParticleCount=%d, Bounds(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f), CellSize=%.2f"),
				VolumeInput.ParticleCount,
				VolumeInput.BoundsMin.X, VolumeInput.BoundsMin.Y, VolumeInput.BoundsMin.Z,
				VolumeInput.BoundsMax.X, VolumeInput.BoundsMax.Y, VolumeInput.BoundsMax.Z,
				VolumeInput.CellSize);

			// Log buffer sizes for debugging
			UE_LOG(LogTemp, Log, TEXT("[RayMarching] Buffer sizes - Particles: %d bytes, CellStart: %d bytes, CellEnd: %d bytes"),
				ParticlePooled->GetSize(),
				CellStartPooled->GetSize(),
				CellEndPooled->GetSize());

			// Log Morton grid info
			UE_LOG(LogTemp, Log, TEXT("[RayMarching] Morton Grid - AXIS_BITS=%d, GRID_SIZE=%d, MAX_CELLS=%d"),
				GPU_MORTON_GRID_AXIS_BITS,
				GPU_MORTON_GRID_SIZE,
				GPU_MORTON_GRID_SIZE * GPU_MORTON_GRID_SIZE * GPU_MORTON_GRID_SIZE);

			// Calculate expected GridMin for Morton code
			FVector3f GridMin = FVector3f(
				FMath::Floor(VolumeInput.BoundsMin.X / VolumeInput.CellSize),
				FMath::Floor(VolumeInput.BoundsMin.Y / VolumeInput.CellSize),
				FMath::Floor(VolumeInput.BoundsMin.Z / VolumeInput.CellSize));
			UE_LOG(LogTemp, Log, TEXT("[RayMarching] Expected GridMin for Morton: (%.0f, %.0f, %.0f)"),
				GridMin.X, GridMin.Y, GridMin.Z);
		}

		bFoundValidData = true;
		break;
	}

	if (!bFoundValidData)
	{
		if (bShouldLog) UE_LOG(LogTemp, Warning, TEXT("[RayMarching] PrepareRender - No valid Z-Order data found"));
		// Clear cached Z-Order input when no valid data
		CachedZOrderInput = FFluidVolumeInput();
		return;
	}

	// Cache Z-Order input for Hybrid Mode in ExecuteRayMarching
	CachedZOrderInput = VolumeInput;

	// Build volume configuration
	FFluidVolumeConfig VolumeConfig;
	VolumeConfig.VolumeResolution = RenderParams.VolumeResolution;
	VolumeConfig.DensityThreshold = RenderParams.DensityThreshold;
	VolumeConfig.bBuildOccupancyMask = RenderParams.bEnableOccupancyMask;
	VolumeConfig.bBuildMinMaxMipmap = RenderParams.bEnableMinMaxMipmap;

	// SDF mode configuration (default to SDF for better quality)
	VolumeConfig.bBuildSDF = RenderParams.bUseSDF;
	VolumeConfig.SmoothK = RenderParams.SDFSmoothK;
	VolumeConfig.SurfaceOffset = RenderParams.SDFSurfaceOffset;

	// SDF Optimization options
	VolumeConfig.bUseTightAABB = RenderParams.bUseTightAABB;
	VolumeConfig.AABBPaddingMultiplier = RenderParams.AABBPaddingMultiplier;
	VolumeConfig.bUseSparseVoxel = RenderParams.bUseSparseVoxel;
	VolumeConfig.bUseTemporalCoherence = RenderParams.bUseTemporalCoherence;
	VolumeConfig.TemporalDirtyThreshold = RenderParams.TemporalDirtyThreshold;

	if (bShouldLog)
	{
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] PrepareRender - VolumeConfig: Resolution=%d, Threshold=%.4f, OccupancyMask=%d, MinMaxMip=%d"),
			VolumeConfig.VolumeResolution, VolumeConfig.DensityThreshold,
			VolumeConfig.bBuildOccupancyMask, VolumeConfig.bBuildMinMaxMipmap);
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] SDF Params: SmoothK=%.2f, SurfaceOffset=%.2f, bUseSDF=%d"),
			VolumeConfig.SmoothK, VolumeConfig.SurfaceOffset, VolumeConfig.bBuildSDF ? 1 : 0);
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] Tight AABB Config: bUseTightAABB=%d, AABBPadding=%.2f, ParticleCount=%d, ParticleRadius=%.2f"),
			VolumeConfig.bUseTightAABB ? 1 : 0, VolumeConfig.AABBPaddingMultiplier,
			VolumeInput.ParticleCount, VolumeInput.ParticleRadius);
	}

	// Build density volumes from Z-Order sorted particles
	CachedVolumeTextures = VolumeBuilder->BuildVolumes(GraphBuilder, VolumeInput, VolumeConfig);

	if (bShouldLog)
	{
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] PrepareRender - Volume building complete. SDFVolume=%d, AABBBuffer=%d"),
			CachedVolumeTextures.SDFVolume != nullptr ? 1 : 0,
			CachedVolumeTextures.AABBBuffer != nullptr ? 1 : 0);
	}
}

void FKawaiiRayMarchingPipeline::ExecutePrePostProcess(
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
	RDG_EVENT_SCOPE(GraphBuilder, "RayMarching.PrePostProcess");

	static int32 LogFrameCounter = 0;
	if (++LogFrameCounter % 60 == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] ExecutePrePostProcess - Renderers: %d, VolumeValid: %d"),
			Renderers.Num(), CachedVolumeTextures.IsValid() ? 1 : 0);
	}

	// Ray marching doesn't use PrePostProcess
	// Main rendering happens at Tonemap timing
}

void FKawaiiRayMarchingPipeline::ExecuteRender(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const TArray<UKawaiiFluidMetaballRenderer*>& Renderers,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RayMarching.ExecuteRender");

	static int32 LogFrameCounter = 0;
	bool bShouldLog = (++LogFrameCounter % 60 == 0);

	if (bShouldLog)
	{
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] ExecuteRender - Renderers: %d, VolumeValid: %d"),
			Renderers.Num(), CachedVolumeTextures.IsValid() ? 1 : 0);
	}

	if (Renderers.Num() == 0 || !CachedVolumeTextures.IsValid())
	{
		if (bShouldLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[RayMarching] ExecuteRender - Early return! Renderers=%d, VolumeValid=%d"),
				Renderers.Num(), CachedVolumeTextures.IsValid() ? 1 : 0);
		}
		return;
	}

	GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	// 1. Tile Culling
	FRDGBufferRef TileVisibility = nullptr;
	FRDGBufferRef IndirectArgs = nullptr;

	if (RenderParams.bEnableTileCulling)
	{
		ExecuteTileCulling(GraphBuilder, View, RenderParams, SceneDepthTexture,
			TileVisibility, IndirectArgs);
	}

	// 2. Ray Marching
	FRDGTextureRef FluidColor = nullptr;
	FRDGTextureRef FluidDepth = nullptr;

	ExecuteRayMarching(GraphBuilder, View, RenderParams, SceneDepthTexture, SceneColorTexture,
		TileVisibility, FluidColor, FluidDepth);

	// 3. Temporal Blending and Composite
	if (RenderParams.bEnableTemporalReprojection)
	{
		ExecuteTemporalBlend(GraphBuilder, View, RenderParams, FluidColor, FluidDepth, Output);
	}
	else
	{
		// Direct composite without temporal - blend FluidColor onto Output
		ExecuteDirectComposite(GraphBuilder, View, FluidColor, SceneColorTexture, Output);
	}

	// Update history for next frame
	PrevViewProjectionMatrix = View.ViewMatrices.GetViewProjectionMatrix();
	bHasHistoryData = true;
}

void FKawaiiRayMarchingPipeline::ExecuteTileCulling(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	FRDGTextureRef SceneDepthTexture,
	FRDGBufferRef& OutTileVisibility,
	FRDGBufferRef& OutIndirectArgs)
{
	RDG_EVENT_SCOPE(GraphBuilder, "TileCulling");

	const FIntPoint ViewportSize = View.UnscaledViewRect.Size();
	const int32 TilesX = FMath::DivideAndRoundUp(ViewportSize.X, TILE_SIZE);
	const int32 TilesY = FMath::DivideAndRoundUp(ViewportSize.Y, TILE_SIZE);
	const int32 TotalTiles = TilesX * TilesY;
	const int32 TileVisibilityUints = FMath::DivideAndRoundUp(TotalTiles, 32);

	// Create tile visibility buffer
	FRDGBufferDesc TileVisDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TileVisibilityUints);
	OutTileVisibility = GraphBuilder.CreateBuffer(TileVisDesc, TEXT("TileVisibility"));

	// Create indirect args buffer
	FRDGBufferDesc IndirectDesc = FRDGBufferDesc::CreateIndirectDesc(4);  // DispatchX, DispatchY, DispatchZ, (padding)
	OutIndirectArgs = GraphBuilder.CreateBuffer(IndirectDesc, TEXT("TileIndirectArgs"));

	// Clear visibility to zero
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutTileVisibility, PF_R32_UINT), 0u);

	// Execute tile culling
	TShaderMapRef<FTileCullingCS> ComputeShader(GlobalShaderMap);

	FTileCullingCS::FParameters* PassParams = GraphBuilder.AllocParameters<FTileCullingCS::FParameters>();
	// PassParams->FluidAABB = ...; // Would come from AABB compute
	PassParams->SceneDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepthTexture));
	PassParams->DepthSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParams->TileVisibility = GraphBuilder.CreateUAV(OutTileVisibility, PF_R32_UINT);
	PassParams->IndirectArgs = GraphBuilder.CreateUAV(OutIndirectArgs, PF_R32_UINT);
	PassParams->TilesX = TilesX;
	PassParams->TilesY = TilesY;
	PassParams->ViewportSize = FVector2f(ViewportSize);
	PassParams->ViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetViewProjectionMatrix());
	PassParams->VolumeBoundsMin = CachedVolumeTextures.VolumeBoundsMin;
	PassParams->VolumeBoundsMax = CachedVolumeTextures.VolumeBoundsMax;

	const int32 GroupSize = FTileCullingCS::ThreadGroupSize;
	const FIntVector GroupCount(
		FMath::DivideAndRoundUp(TilesX, GroupSize),
		FMath::DivideAndRoundUp(TilesY, GroupSize),
		1);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("TileCulling %dx%d tiles", TilesX, TilesY),
		ComputeShader,
		PassParams,
		GroupCount);
}

void FKawaiiRayMarchingPipeline::ExecuteRayMarching(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FRDGBufferRef TileVisibility,
	FRDGTextureRef& OutFluidColor,
	FRDGTextureRef& OutFluidDepth)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RayMarching.Main");

	const FIntPoint ViewportSize = View.UnscaledViewRect.Size();

	// Create output textures
	FRDGTextureDesc ColorDesc = FRDGTextureDesc::Create2D(
		ViewportSize,
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
		ViewportSize,
		PF_R32_FLOAT,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_RenderTargetable);

	OutFluidColor = GraphBuilder.CreateTexture(ColorDesc, TEXT("FluidRayMarchColor"));
	OutFluidDepth = GraphBuilder.CreateTexture(DepthDesc, TEXT("FluidRayMarchDepth"));

	// Check if using SDF mode
	const bool bUseSDF = RenderParams.bUseSDF && CachedVolumeTextures.HasSDF();

	// Set up shaders
	TShaderMapRef<FRayMarchingVS> VertexShader(GlobalShaderMap);

	if (bUseSDF)
	{
		// SDF Sphere Tracing mode
		TShaderMapRef<FRayMarchingSDFPS> SDFPixelShader(GlobalShaderMap);

		FRayMarchingSDFPS::FParameters* SDFParams = GraphBuilder.AllocParameters<FRayMarchingSDFPS::FParameters>();

		// View uniform buffer
		SDFParams->View = View.ViewUniformBuffer;

		// SDF volume texture
		SDFParams->SDFVolume = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(CachedVolumeTextures.SDFVolume));
		SDFParams->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// Scene textures
		SDFParams->SceneDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepthTexture));
		SDFParams->DepthSampler = TStaticSamplerState<SF_Point>::GetRHI();

		// History textures
		if (bHasHistoryData && HistoryColorRT.IsValid() && HistoryDepthRT.IsValid())
		{
			FRDGTextureRef HistoryColorRDG = GraphBuilder.RegisterExternalTexture(HistoryColorRT, TEXT("HistoryColor"));
			FRDGTextureRef HistoryDepthRDG = GraphBuilder.RegisterExternalTexture(HistoryDepthRT, TEXT("HistoryDepth"));
			SDFParams->HistoryColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryColorRDG));
			SDFParams->HistoryDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryDepthRDG));
		}
		else
		{
			FRDGTextureDesc DummyColorDesc = FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource);
			FRDGTextureDesc DummyDepthDesc = FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource);
			FRDGTextureRef DummyHistoryColor = GraphBuilder.CreateTexture(DummyColorDesc, TEXT("DummyHistoryColor"));
			FRDGTextureRef DummyHistoryDepth = GraphBuilder.CreateTexture(DummyDepthDesc, TEXT("DummyHistoryDepth"));
			SDFParams->HistoryColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DummyHistoryColor));
			SDFParams->HistoryDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DummyHistoryDepth));
		}
		SDFParams->HistorySampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();

		// Tile visibility
		if (TileVisibility)
		{
			SDFParams->TileVisibility = GraphBuilder.CreateSRV(TileVisibility, PF_R32_UINT);
		}
		else
		{
			FRDGBufferDesc DummyTileDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1);
			FRDGBufferRef DummyTileBuffer = GraphBuilder.CreateBuffer(DummyTileDesc, TEXT("DummyTileVisibility"));
			SDFParams->TileVisibility = GraphBuilder.CreateSRV(DummyTileBuffer, PF_R32_UINT);
		}

		//========================================
		// Z-Order Particle Data (for Hybrid Mode)
		// IMPORTANT: Use Pooled buffers and re-register with current GraphBuilder
		// RDG buffers from PrepareRender may be invalid in different GraphBuilder instance
		//========================================
		SDFParams->bEnableHybridMode = RenderParams.bEnableSDFHybridMode ? 1u : 0u;
		SDFParams->HybridThreshold = RenderParams.SDFHybridThreshold;

		// Check if we have valid Pooled buffers (persistent across GraphBuilder instances)
		const bool bHasValidPooledBuffers = CachedZOrderInput.SortedParticlesPooled.IsValid()
			&& CachedZOrderInput.CellStartPooled.IsValid()
			&& CachedZOrderInput.CellEndPooled.IsValid();

		if (RenderParams.bEnableSDFHybridMode && bHasValidPooledBuffers)
		{
			// Re-register pooled buffers with current GraphBuilder
			// This ensures the RDG buffers are valid for this pass
			FRDGBufferRef ParticlesBuffer = GraphBuilder.RegisterExternalBuffer(
				CachedZOrderInput.SortedParticlesPooled, TEXT("HybridMode.Particles"));
			FRDGBufferRef CellStartBuffer = GraphBuilder.RegisterExternalBuffer(
				CachedZOrderInput.CellStartPooled, TEXT("HybridMode.CellStart"));
			FRDGBufferRef CellEndBuffer = GraphBuilder.RegisterExternalBuffer(
				CachedZOrderInput.CellEndPooled, TEXT("HybridMode.CellEnd"));

			SDFParams->Particles = GraphBuilder.CreateSRV(ParticlesBuffer);
			SDFParams->CellStart = GraphBuilder.CreateSRV(CellStartBuffer);
			SDFParams->CellEnd = GraphBuilder.CreateSRV(CellEndBuffer);
			SDFParams->ParticleCount = CachedZOrderInput.ParticleCount;
			SDFParams->ParticleRadius = CachedZOrderInput.ParticleRadius;
			SDFParams->SDFSmoothness = RenderParams.SDFSmoothK;
			SDFParams->CellSize = CachedZOrderInput.CellSize;
			SDFParams->MortonBoundsMin = CachedZOrderInput.MortonBoundsMin;
		}
		else
		{
			// Dummy buffers when Hybrid Mode is disabled or no valid data
			FRDGBufferDesc DummyParticleDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), 1);
			FRDGBufferDesc DummyCellDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
			FRDGBufferRef DummyParticles = GraphBuilder.CreateBuffer(DummyParticleDesc, TEXT("DummyParticles"));
			FRDGBufferRef DummyCellStart = GraphBuilder.CreateBuffer(DummyCellDesc, TEXT("DummyCellStart"));
			FRDGBufferRef DummyCellEnd = GraphBuilder.CreateBuffer(DummyCellDesc, TEXT("DummyCellEnd"));
			SDFParams->Particles = GraphBuilder.CreateSRV(DummyParticles);
			SDFParams->CellStart = GraphBuilder.CreateSRV(DummyCellStart);
			SDFParams->CellEnd = GraphBuilder.CreateSRV(DummyCellEnd);
			SDFParams->ParticleCount = 0;
			// Use cached ParticleRadius if available, otherwise use reasonable default
			SDFParams->ParticleRadius = CachedZOrderInput.ParticleRadius > 0.0f ? CachedZOrderInput.ParticleRadius : 5.0f;
			SDFParams->SDFSmoothness = RenderParams.SDFSmoothK;
			SDFParams->CellSize = CachedZOrderInput.CellSize > 0.0f ? CachedZOrderInput.CellSize : 20.0f;
			SDFParams->MortonBoundsMin = CachedZOrderInput.MortonBoundsMin;
		}

		// Volume parameters
		SDFParams->VolumeResolution = CachedVolumeTextures.VolumeResolution;
		SDFParams->VolumeBoundsMin = CachedVolumeTextures.VolumeBoundsMin;
		SDFParams->VolumeBoundsMax = CachedVolumeTextures.VolumeBoundsMax;

		//========================================
		// Volume Bounds - Always use Simulation Volume (Tight AABB disabled)
		//========================================
		SDFParams->bUseTightAABB = 0u;
		SDFParams->bDebugVisualizeTightAABB = 0u;
		SDFParams->SimulationBoundsMin = CachedZOrderInput.BoundsMin;
		SDFParams->SimulationBoundsMax = CachedZOrderInput.BoundsMax;

		// Dummy AABB buffer (셰이더 파라미터 validation용)
		FRDGBufferDesc DummyAABBDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 6);
		FRDGBufferRef DummyAABBBuffer = GraphBuilder.CreateBuffer(DummyAABBDesc, TEXT("DummyFluidAABB"));
		SDFParams->FluidAABB = GraphBuilder.CreateSRV(DummyAABBBuffer, PF_R32_UINT);

		// Sphere Tracing parameters
		SDFParams->MaxSteps = RenderParams.RayMarchMaxSteps;
		// Calculate VoxelSize from largest axis dimension (not diagonal length)
		// .Size() gives diagonal length which is ~1.73x larger for a cube
		const FVector3f VolumeSize = CachedVolumeTextures.VolumeBoundsMax - CachedVolumeTextures.VolumeBoundsMin;
		const float VoxelSize = VolumeSize.GetMax() / float(CachedVolumeTextures.VolumeResolution);
		SDFParams->SurfaceEpsilon = RenderParams.SDFSurfaceEpsilon * VoxelSize;
		SDFParams->MinStepSize = VoxelSize * 0.5f;
		SDFParams->MaxStepSize = VoxelSize * 10.0f;
		SDFParams->RelaxationFactor = RenderParams.SDFRelaxationFactor;

		// Translucency parameters
		SDFParams->TranslucencyDepth = RenderParams.SDFTranslucencyDepth;
		SDFParams->TranslucencyDensity = RenderParams.SDFTranslucencyDensity;
		SDFParams->SubsurfaceScatterStrength = RenderParams.SDFSubsurfaceScatterStrength;
		SDFParams->SubsurfaceColor = FVector3f(RenderParams.SDFSubsurfaceColor.R, RenderParams.SDFSubsurfaceColor.G, RenderParams.SDFSubsurfaceColor.B);

		// Optimization flags
		SDFParams->bEnableTileCulling = RenderParams.bEnableTileCulling ? 1u : 0u;
		SDFParams->bEnableTemporalReprojection = RenderParams.bEnableTemporalReprojection ? 1u : 0u;
		SDFParams->TemporalBlendFactor = RenderParams.TemporalBlendFactor;

		// Appearance parameters
		SDFParams->FluidColor = RenderParams.FluidColor;
		SDFParams->FresnelStrength = RenderParams.FresnelStrength;
		SDFParams->RefractiveIndex = RenderParams.RefractiveIndex;
		SDFParams->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;
		SDFParams->AbsorptionColorCoefficients = RenderParams.AbsorptionColorCoefficients;
		SDFParams->SpecularStrength = RenderParams.SpecularStrength;
		SDFParams->SpecularRoughness = RenderParams.SpecularRoughness;
		SDFParams->ReflectionStrength = RenderParams.SDFReflectionStrength;

		// View parameters
		SDFParams->ViewportSize = FVector2f(ViewportSize);
		SDFParams->CameraPosition = FVector3f(View.ViewMatrices.GetViewOrigin());
		SDFParams->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
		SDFParams->PrevViewProjectionMatrix = FMatrix44f(PrevViewProjectionMatrix);

		// Get Sun Direction from Scene's Directional Light
		FVector3f SunDirection(0.0f, 0.0f, -1.0f);  // Default: top-down
		FLinearColor SunColor(1.0f, 0.95f, 0.9f, 1.0f);
		if (View.Family && View.Family->Scene)
		{
			const FScene* Scene = View.Family->Scene->GetRenderScene();
			if (Scene && Scene->SimpleDirectionalLight)
			{
				// GetDirection() returns the direction light travels (away from light)
				// For NdotL we need direction TO light, so negate it
				FVector LightDir = Scene->SimpleDirectionalLight->Proxy->GetDirection();
				SunDirection = FVector3f(-LightDir);

				// Get light color
				SunColor = Scene->SimpleDirectionalLight->Proxy->GetColor();
			}
		}
		SDFParams->SunDirection = SunDirection;
		SDFParams->SunColor = SunColor;

		SDFParams->TilesX = FMath::DivideAndRoundUp(ViewportSize.X, TILE_SIZE);
		SDFParams->FrameIndex = View.Family->FrameNumber;

		// Render targets
		SDFParams->RenderTargets[0] = FRenderTargetBinding(OutFluidColor, ERenderTargetLoadAction::EClear);

		// Add graphics pass
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("RayMarchingSDF"),
			SDFParams,
			ERDGPassFlags::Raster,
			[VertexShader, SDFPixelShader, SDFParams, ViewportSize](FRHICommandList& RHICmdList)
			{
				RHICmdList.SetViewport(0, 0, 0.0f, ViewportSize.X, ViewportSize.Y, 1.0f);

				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = SDFPixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleList;

				GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
				SetShaderParameters(RHICmdList, SDFPixelShader, SDFPixelShader.GetPixelShader(), *SDFParams);

				RHICmdList.DrawPrimitive(0, 1, 1);
			});

		return;  // Early return for SDF mode
	}

	// Legacy Density mode
	TShaderMapRef<FRayMarchingMainPS> PixelShader(GlobalShaderMap);

	FRayMarchingMainPS::FParameters* PassParams = GraphBuilder.AllocParameters<FRayMarchingMainPS::FParameters>();

	// View uniform buffer (required for View.ViewToClip access in shader)
	PassParams->View = View.ViewUniformBuffer;

	// Volume textures (always bind valid SRVs)
	if (CachedVolumeTextures.DensityVolume)
	{
		PassParams->DensityVolume = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(CachedVolumeTextures.DensityVolume));
	}
	else
	{
		// Create dummy 1x1x1 3D texture - shouldn't happen if IsValid() checked
		FRDGTextureDesc DummyDensityDesc = FRDGTextureDesc::Create3D(
			FIntVector(1, 1, 1), PF_R32_FLOAT, FClearValueBinding::Black,
			TexCreate_ShaderResource);
		FRDGTextureRef DummyDensityVolume = GraphBuilder.CreateTexture(DummyDensityDesc, TEXT("DummyDensityVolume"));
		PassParams->DensityVolume = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DummyDensityVolume));
	}
	PassParams->DensitySampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	if (CachedVolumeTextures.MinMaxMipmap)
	{
		PassParams->MinMaxMipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(CachedVolumeTextures.MinMaxMipmap));
	}
	else
	{
		// Create dummy 1x1x1 3D texture when min-max mipmap is disabled
		FRDGTextureDesc DummyMinMaxDesc = FRDGTextureDesc::Create3D(
			FIntVector(1, 1, 1), PF_G16R16F, FClearValueBinding::Black,
			TexCreate_ShaderResource);
		FRDGTextureRef DummyMinMaxMipmap = GraphBuilder.CreateTexture(DummyMinMaxDesc, TEXT("DummyMinMaxMipmap"));
		PassParams->MinMaxMipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DummyMinMaxMipmap));
	}
	PassParams->MinMaxSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	if (CachedVolumeTextures.OccupancyMask)
	{
		PassParams->OccupancyMask = GraphBuilder.CreateSRV(CachedVolumeTextures.OccupancyMask, PF_R32_UINT);
	}
	else
	{
		// Create dummy 1-element buffer when occupancy mask is disabled
		FRDGBufferDesc DummyOccupancyDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyOccupancyBuffer = GraphBuilder.CreateBuffer(DummyOccupancyDesc, TEXT("DummyOccupancyMask"));
		PassParams->OccupancyMask = GraphBuilder.CreateSRV(DummyOccupancyBuffer, PF_R32_UINT);
	}

	// Scene textures
	PassParams->SceneDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepthTexture));
	PassParams->DepthSampler = TStaticSamplerState<SF_Point>::GetRHI();

	// History textures (always bind valid SRVs - use dummy when no history)
	if (bHasHistoryData && HistoryColorRT.IsValid() && HistoryDepthRT.IsValid())
	{
		// Register external textures for history
		FRDGTextureRef HistoryColorRDG = GraphBuilder.RegisterExternalTexture(HistoryColorRT, TEXT("HistoryColor"));
		FRDGTextureRef HistoryDepthRDG = GraphBuilder.RegisterExternalTexture(HistoryDepthRT, TEXT("HistoryDepth"));
		PassParams->HistoryColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryColorRDG));
		PassParams->HistoryDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryDepthRDG));
	}
	else
	{
		// Create dummy 1x1 textures when no history available
		FRDGTextureDesc DummyColorDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding::Black,
			TexCreate_ShaderResource);
		FRDGTextureDesc DummyDepthDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, 1), PF_R32_FLOAT, FClearValueBinding::Black,
			TexCreate_ShaderResource);
		FRDGTextureRef DummyHistoryColor = GraphBuilder.CreateTexture(DummyColorDesc, TEXT("DummyHistoryColor"));
		FRDGTextureRef DummyHistoryDepth = GraphBuilder.CreateTexture(DummyDepthDesc, TEXT("DummyHistoryDepth"));
		PassParams->HistoryColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DummyHistoryColor));
		PassParams->HistoryDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DummyHistoryDepth));
	}
	PassParams->HistorySampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();

	// Tile visibility (always bind valid SRV)
	if (TileVisibility)
	{
		PassParams->TileVisibility = GraphBuilder.CreateSRV(TileVisibility, PF_R32_UINT);
	}
	else
	{
		// Create dummy 1-element buffer when tile culling is disabled
		FRDGBufferDesc DummyTileDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyTileBuffer = GraphBuilder.CreateBuffer(DummyTileDesc, TEXT("DummyTileVisibility"));
		PassParams->TileVisibility = GraphBuilder.CreateSRV(DummyTileBuffer, PF_R32_UINT);
	}

	// Volume parameters
	PassParams->VolumeResolution = CachedVolumeTextures.VolumeResolution;
	PassParams->VolumeBoundsMin = CachedVolumeTextures.VolumeBoundsMin;
	PassParams->VolumeBoundsMax = CachedVolumeTextures.VolumeBoundsMax;

	// Ray marching parameters
	PassParams->MaxSteps = RenderParams.RayMarchMaxSteps;
	PassParams->DensityThreshold = RenderParams.DensityThreshold;
	PassParams->AdaptiveStepMultiplier = RenderParams.AdaptiveStepMultiplier;
	PassParams->EarlyTerminationAlpha = RenderParams.EarlyTerminationAlpha;

	// Optimization flags
	PassParams->bEnableOccupancyMask = RenderParams.bEnableOccupancyMask ? 1u : 0u;
	PassParams->bEnableMinMaxMipmap = RenderParams.bEnableMinMaxMipmap ? 1u : 0u;
	PassParams->bEnableTileCulling = RenderParams.bEnableTileCulling ? 1u : 0u;
	PassParams->bEnableTemporalReprojection = RenderParams.bEnableTemporalReprojection ? 1u : 0u;
	PassParams->TemporalBlendFactor = RenderParams.TemporalBlendFactor;

	// Appearance parameters
	PassParams->FluidColor = RenderParams.FluidColor;
	PassParams->FresnelStrength = RenderParams.FresnelStrength;
	PassParams->RefractiveIndex = RenderParams.RefractiveIndex;
	PassParams->AbsorptionCoefficient = RenderParams.AbsorptionCoefficient;
	PassParams->AbsorptionColorCoefficients = RenderParams.AbsorptionColorCoefficients;
	PassParams->SpecularStrength = RenderParams.SpecularStrength;
	PassParams->SpecularRoughness = RenderParams.SpecularRoughness;

	// View parameters
	PassParams->ViewportSize = FVector2f(ViewportSize);
	PassParams->CameraPosition = FVector3f(View.ViewMatrices.GetViewOrigin());
	PassParams->InvViewProjectionMatrix = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
	PassParams->PrevViewProjectionMatrix = FMatrix44f(PrevViewProjectionMatrix);

	// Get Sun Direction from Scene's Directional Light
	FVector3f DensitySunDirection(0.0f, 0.0f, -1.0f);  // Default: top-down
	FLinearColor DensitySunColor(1.0f, 0.95f, 0.9f, 1.0f);
	if (View.Family && View.Family->Scene)
	{
		const FScene* Scene = View.Family->Scene->GetRenderScene();
		if (Scene && Scene->SimpleDirectionalLight)
		{
			FVector LightDir = Scene->SimpleDirectionalLight->Proxy->GetDirection();
			DensitySunDirection = FVector3f(-LightDir);
			DensitySunColor = Scene->SimpleDirectionalLight->Proxy->GetColor();
		}
	}
	PassParams->SunDirection = DensitySunDirection;
	PassParams->SunColor = DensitySunColor;

	PassParams->TilesX = FMath::DivideAndRoundUp(ViewportSize.X, TILE_SIZE);

	// Frame index for temporal jittering (prevents static stripe patterns)
	PassParams->FrameIndex = View.Family->FrameNumber;

	// Render targets
	PassParams->RenderTargets[0] = FRenderTargetBinding(OutFluidColor, ERenderTargetLoadAction::EClear);

	// Add graphics pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("RayMarchingMain"),
		PassParams,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, PassParams, ViewportSize](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(0, 0, 0.0f, ViewportSize.X, ViewportSize.Y, 1.0f);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParams);

			// Draw fullscreen triangle
			RHICmdList.DrawPrimitive(0, 1, 1);
		});
}

void FKawaiiRayMarchingPipeline::ExecuteTemporalBlend(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	FRDGTextureRef CurrentColor,
	FRDGTextureRef CurrentDepth,
	FScreenPassRenderTarget Output)
{
	RDG_EVENT_SCOPE(GraphBuilder, "TemporalBlend");

	// TODO: Implement actual temporal blending with history
	// For now, just use direct composite
	ExecuteDirectComposite(GraphBuilder, View, CurrentColor, nullptr, Output);
}

void FKawaiiRayMarchingPipeline::ExecuteDirectComposite(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef FluidColor,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	RDG_EVENT_SCOPE(GraphBuilder, "DirectComposite");

	if (!FluidColor || !Output.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[RayMarching] DirectComposite - Invalid input: FluidColor=%p, Output=%d"),
			FluidColor, Output.IsValid() ? 1 : 0);
		return;
	}

	const FIntRect ViewRect = Output.ViewRect;

	// Create simple composite shader parameters
	FRayMarchingCompositePS::FParameters* PassParams = GraphBuilder.AllocParameters<FRayMarchingCompositePS::FParameters>();
	PassParams->FluidColorTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(FluidColor));
	PassParams->FluidColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
	PassParams->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);

	// Get shaders
	TShaderMapRef<FRayMarchingVS> VertexShader(GlobalShaderMap);
	TShaderMapRef<FRayMarchingCompositePS> PixelShader(GlobalShaderMap);

	// Alpha blend composite pass
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("CompositeFluid"),
		PassParams,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, PassParams, ViewRect](FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
				ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			// Pre-multiplied alpha blending: Src + Dst * (1 - SrcAlpha)
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParams);

			// Draw fullscreen triangle
			RHICmdList.DrawPrimitive(0, 1, 1);
		});

	static int32 LogCounter = 0;
	if (++LogCounter % 60 == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[RayMarching] DirectComposite - Blend completed"));
	}
}
