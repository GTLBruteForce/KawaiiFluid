// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidSceneViewExtension.h"

#include "FluidRendererSubsystem.h"
#include "Rendering/FluidShadowHistoryManager.h"
#include "Rendering/FluidShadowProjection.h"
#include "Rendering/FluidVSMBlur.h"
#include "Rendering/FluidShadowReceiver.h"
#include "Rendering/FluidShadowUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "SceneTexturesConfig.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "PostProcess/PostProcessing.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "EngineUtils.h"

// New Pipeline architecture (ShadingPass removed - Pipeline handles ShadingMode internally)
#include "Rendering/Pipeline/IKawaiiMetaballRenderingPipeline.h"

// Context-based batching
#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"  // For FContextCacheKey
#include "Data/KawaiiFluidPresetDataAsset.h"

// 통합 인터페이스
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Core/KawaiiRenderParticle.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"

static TRefCountPtr<IPooledRenderTarget> GFluidCompositeDebug_KeepAlive;

// ==============================================================================
// Shadow Projection Helper
// ==============================================================================

/**
 * @brief Execute fluid shadow projection pass.
 * @param GraphBuilder RDG builder.
 * @param View Scene view.
 * @param Subsystem Fluid renderer subsystem.
 * @param RenderParams Rendering parameters.
 */
static void ExecuteFluidShadowProjection(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	const FFluidRenderingParameters& RenderParams)
{
	// Debug: Log shadow projection entry
	UE_LOG(LogTemp, Log,
	       TEXT(
		       "FluidShadow: ExecuteFluidShadowProjection called - bEnableShadowCasting=%d, Subsystem=%p"
	       ),
	       RenderParams.bEnableShadowCasting, Subsystem);

	if (!Subsystem || !RenderParams.bEnableShadowCasting)
	{
		UE_LOG(LogTemp, Log,
		       TEXT("FluidShadow: Early exit - Subsystem=%p, bEnableShadowCasting=%d"),
		       Subsystem, RenderParams.bEnableShadowCasting);
		return;
	}

	FFluidShadowHistoryManager* HistoryManager = Subsystem->GetShadowHistoryManager();
	const bool bHasValidHistory = HistoryManager ? HistoryManager->HasValidHistory() : false;
	UE_LOG(LogTemp, Log, TEXT("FluidShadow: HistoryManager=%p, HasValidHistory=%d"),
	       HistoryManager, bHasValidHistory);

	if (!HistoryManager || !bHasValidHistory)
	{
		UE_LOG(LogTemp, Log, TEXT("FluidShadow: No valid history - waiting for next frame"));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "FluidShadowProjection");

	// Get history buffer
	const FFluidShadowHistoryBuffer& HistoryBuffer = HistoryManager->GetPreviousFrameBuffer();

	// Get cached light data from subsystem (updated on game thread in SetupViewFamily)
	if (!Subsystem->HasValidCachedLightData())
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidShadow: No valid cached light data"));
		return;
	}

	FFluidShadowLightParams LightParams;
	LightParams.LightDirection = Subsystem->GetCachedLightDirection();
	LightParams.LightViewProjectionMatrix = Subsystem->GetCachedLightViewProjectionMatrix();
	LightParams.bIsValid = true;

	UE_LOG(LogTemp, Log, TEXT("FluidShadow: LightDir=(%f,%f,%f)"),
	       LightParams.LightDirection.X, LightParams.LightDirection.Y,
	       LightParams.LightDirection.Z);

	// Check if history buffer has valid depth texture
	if (!HistoryBuffer.bIsValid || !HistoryBuffer.DepthTexture.IsValid())
	{
		UE_LOG(LogTemp, Warning,
		       TEXT("FluidShadow: History buffer invalid - bIsValid=%d, DepthTexture=%d"),
		       HistoryBuffer.bIsValid, HistoryBuffer.DepthTexture.IsValid());
		return;
	}

	// Setup projection parameters
	FFluidShadowProjectionParams ProjectionParams;
	ProjectionParams.VSMResolution = FIntPoint(RenderParams.VSMResolution,
	                                           RenderParams.VSMResolution);
	ProjectionParams.LightViewProjectionMatrix = LightParams.LightViewProjectionMatrix;

	UE_LOG(LogTemp, Log, TEXT("FluidShadow: Calling RenderFluidShadowProjection VSMRes=%dx%d"),
	       ProjectionParams.VSMResolution.X, ProjectionParams.VSMResolution.Y);

	// Execute shadow projection
	FFluidShadowProjectionOutput ProjectionOutput;
	RenderFluidShadowProjection(
		GraphBuilder,
		View,
		HistoryBuffer,
		ProjectionParams,
		ProjectionOutput);

	UE_LOG(LogTemp, Log, TEXT("FluidShadow: ProjectionOutput - bIsValid=%d, VSMTexture=%d"),
	       ProjectionOutput.bIsValid, ProjectionOutput.VSMTexture != nullptr);

	if (!ProjectionOutput.bIsValid || !ProjectionOutput.VSMTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidShadow: Projection output invalid"));
		return;
	}

	// Apply VSM blur
	FRDGTextureRef BlurredVSM = nullptr;
	if (RenderParams.VSMBlurIterations > 0 && RenderParams.VSMBlurRadius > 0.0f)
	{
		FFluidVSMBlurParams BlurParams;
		BlurParams.BlurRadius = RenderParams.VSMBlurRadius;
		BlurParams.NumIterations = RenderParams.VSMBlurIterations;

		RenderFluidVSMBlur(
			GraphBuilder,
			ProjectionOutput.VSMTexture,
			BlurParams,
			BlurredVSM);
	}
	else
	{
		BlurredVSM = ProjectionOutput.VSMTexture;
	}

	// Extract VSM to pooled render target for persistence (write buffer)
	if (BlurredVSM)
	{
		GraphBuilder.QueueTextureExtraction(BlurredVSM, Subsystem->GetVSMTextureWritePtr());

		// Store light matrix for shadow receiving (write buffer)
		Subsystem->SetLightVPMatrixWrite(LightParams.LightViewProjectionMatrix);

		UE_LOG(LogTemp, Log, TEXT("FluidShadow: VSM texture queued for extraction"));
	}
}

// ==============================================================================
// Shadow Receiver Helper
// ==============================================================================

/**
 * @brief Apply fluid shadows to the scene using the cached VSM.
 * @param GraphBuilder RDG builder.
 * @param View Scene view.
 * @param Subsystem Fluid renderer subsystem (for per-world VSM access).
 * @param RenderParams Rendering parameters.
 * @param SceneColorTexture Input scene color.
 * @param SceneDepthTexture Scene depth texture.
 * @param Output Output render target.
 */
static void ApplyFluidShadowReceiver(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	UFluidRendererSubsystem* Subsystem,
	const FFluidRenderingParameters& RenderParams,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef SceneDepthTexture,
	FScreenPassRenderTarget& Output)
{
	if (!Subsystem || !RenderParams.bEnableShadowCasting)
	{
		return;
	}

	TRefCountPtr<IPooledRenderTarget> VSMTextureRead = Subsystem->GetVSMTextureRead();
	UE_LOG(LogTemp, Log,
	       TEXT(
		       "FluidShadow: ApplyFluidShadowReceiver called - VSMTexture_Read=%d, ShadowCasting=%d"
	       ),
	       VSMTextureRead.IsValid(), RenderParams.bEnableShadowCasting);

	// Check if we have valid VSM from previous frame (read buffer)
	// Need to check both TRefCountPtr validity AND internal RHI resource
	if (!VSMTextureRead.IsValid() || !VSMTextureRead->GetRHI())
	{
		UE_LOG(LogTemp, Log,
		       TEXT(
			       "FluidShadow: ApplyFluidShadowReceiver early exit - waiting for VSM from previous frame"
		       ));
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "FluidShadowReceiver");

	// Import cached VSM texture into RDG (from read buffer - previous frame)
	FRDGTextureRef VSMTexture = GraphBuilder.RegisterExternalTexture(
		VSMTextureRead,
		TEXT("FluidVSMTexture"));

	if (!VSMTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidShadow: Failed to register external VSM texture"));
		return;
	}

	// Setup receiver parameters
	FFluidShadowReceiverParams ReceiverParams;
	ReceiverParams.ShadowIntensity = RenderParams.ShadowIntensity;
	ReceiverParams.ShadowBias = 0.001f;
	ReceiverParams.MinVariance = 0.00001f;
	ReceiverParams.LightBleedReduction = 0.2f;
	ReceiverParams.bDebugVisualization = false;

	// Apply shadow receiver pass (use read buffer's light matrix)
	RenderFluidShadowReceiver(
		GraphBuilder,
		View,
		SceneColorTexture,
		SceneDepthTexture,
		VSMTexture,
		Subsystem->GetLightVPMatrixRead(),
		ReceiverParams,
		Output);

	UE_LOG(LogTemp, Verbose, TEXT("FluidShadow: Shadow receiver applied"));
}

// ==============================================================================
// Class Implementation
// ==============================================================================

FFluidSceneViewExtension::FFluidSceneViewExtension(const FAutoRegister& AutoRegister,
                                                   UFluidRendererSubsystem* InSubsystem)
	: FSceneViewExtensionBase(AutoRegister), Subsystem(InSubsystem)
{
}

FFluidSceneViewExtension::~FFluidSceneViewExtension()
{
}

/**
 * @brief Called on game thread to setup view family before rendering.
 * Used to cache light direction for render thread access.
 * @param InViewFamily The view family being set up.
 */
void FFluidSceneViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	// World filtering: Only process ViewFamily from our World
	if (InViewFamily.Scene)
	{
		UWorld* ViewWorld = InViewFamily.Scene->GetWorld();
		if (ViewWorld != SubsystemPtr->GetWorld())
		{
			return; // Skip ViewFamily from other World
		}
	}

	// Update cached light direction on game thread (safe to use TActorIterator here)
	SubsystemPtr->UpdateCachedLightDirection();
}

/**
 * @brief Called at the beginning of each frame's view family rendering.
 * @param InViewFamily The view family being rendered.
 */
void FFluidSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	// World filtering: Only process ViewFamily from our World
	// This prevents multiple extensions from competing over the same resources
	if (InViewFamily.Scene)
	{
		UWorld* ViewWorld = InViewFamily.Scene->GetWorld();
		if (ViewWorld != SubsystemPtr->GetWorld())
		{
			return; // Skip ViewFamily from other World
		}
	}

	// Swap VSM buffers through Subsystem (per-world isolation)
	SubsystemPtr->SwapVSMBuffers();

	// Swap history buffers at the start of each frame
	if (FFluidShadowHistoryManager* HistoryManager = SubsystemPtr->GetShadowHistoryManager())
	{
		HistoryManager->BeginFrame();
	}
	// Note: Per-frame deduplication is handled by Preset-based TMap batching
}

void FFluidSceneViewExtension::PreRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneViewFamily& InViewFamily)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering)
	{
		return;
	}

	// World filtering
	if (InViewFamily.Scene)
	{
		UWorld* ViewWorld = InViewFamily.Scene->GetWorld();
		if (ViewWorld != SubsystemPtr->GetWorld())
		{
			return;
		}
	}

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluid_PrepareRenderResources");

	// 중복 처리 방지를 위해 처리된 RenderResource 추적
	TSet<FKawaiiFluidRenderResource*> ProcessedResources;

	const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
		if (!MetaballRenderer || !MetaballRenderer->IsRenderingActive())
		{
			continue;
		}

		FKawaiiFluidRenderResource* RenderResource = MetaballRenderer->GetFluidRenderResource();
		if (!RenderResource || !RenderResource->IsValid())
		{
			continue;
		}

		// 이미 처리된 RenderResource는 스킵
		if (ProcessedResources.Contains(RenderResource))
		{
			continue;
		}
		ProcessedResources.Add(RenderResource);

		// GPU/CPU 모드 통합 처리
		FGPUFluidSimulator* GPUSimulator = RenderResource->GetGPUSimulator();
		const float ParticleRadius = RenderResource->GetUnifiedParticleRadius();

		if (GPUSimulator)
		{
			//========================================
			// GPU 모드: GPUSimulator에서 버퍼 추출
			//========================================
			TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer = GPUSimulator->GetPersistentParticleBuffer();
			const int32 ParticleCount = GPUSimulator->GetPersistentParticleCount();

			if (!PhysicsPooledBuffer.IsValid() || ParticleCount <= 0)
			{
				continue;
			}

			RDG_EVENT_SCOPE(GraphBuilder, "ExtractToRenderResource_GPU");

			// Physics 버퍼 등록
			FRDGBufferRef PhysicsBuffer = GraphBuilder.RegisterExternalBuffer(
				PhysicsPooledBuffer,
				TEXT("PhysicsParticles_Extract")
			);
			FRDGBufferSRVRef PhysicsBufferSRV = GraphBuilder.CreateSRV(PhysicsBuffer);

			// Pooled 버퍼 가져오기
			TRefCountPtr<FRDGPooledBuffer> PositionPooledBuffer = RenderResource->GetPooledPositionBuffer();
			TRefCountPtr<FRDGPooledBuffer> VelocityPooledBuffer = RenderResource->GetPooledVelocityBuffer();
			TRefCountPtr<FRDGPooledBuffer> RenderParticlePooled = RenderResource->GetPooledRenderParticleBuffer();
			TRefCountPtr<FRDGPooledBuffer> BoundsPooled = RenderResource->GetPooledBoundsBuffer();

			// RenderParticle + Bounds 버퍼 추출 (SDF용)
			float BoundsMargin = ParticleRadius * 2.0f + 5.0f;

			if (RenderParticlePooled.IsValid() && BoundsPooled.IsValid())
			{
				FRDGBufferRef RenderParticleBuffer = GraphBuilder.RegisterExternalBuffer(
					RenderParticlePooled, TEXT("RenderParticles_Extract"));
				FRDGBufferUAVRef RenderParticleUAV = GraphBuilder.CreateUAV(RenderParticleBuffer);

				FRDGBufferRef BoundsBuffer = GraphBuilder.RegisterExternalBuffer(
					BoundsPooled, TEXT("ParticleBounds_Extract"));
				FRDGBufferUAVRef BoundsBufferUAV = GraphBuilder.CreateUAV(BoundsBuffer);

				FGPUFluidSimulatorPassBuilder::AddExtractRenderDataWithBoundsPass(
					GraphBuilder,
					PhysicsBufferSRV,
					RenderParticleUAV,
					BoundsBufferUAV,
					ParticleCount,
					ParticleRadius,
					BoundsMargin
				);
			}

			// SoA 버퍼 추출 (Position/Velocity)
			if (PositionPooledBuffer.IsValid() && VelocityPooledBuffer.IsValid())
			{
				FRDGBufferRef PositionBuffer = GraphBuilder.RegisterExternalBuffer(
					PositionPooledBuffer, TEXT("RenderPositions_Extract"));
				FRDGBufferUAVRef PositionUAV = GraphBuilder.CreateUAV(PositionBuffer);

				FRDGBufferRef VelocityBuffer = GraphBuilder.RegisterExternalBuffer(
					VelocityPooledBuffer, TEXT("RenderVelocities_Extract"));
				FRDGBufferUAVRef VelocityUAV = GraphBuilder.CreateUAV(VelocityBuffer);

				FGPUFluidSimulatorPassBuilder::AddExtractRenderDataSoAPass(
					GraphBuilder,
					PhysicsBufferSRV,
					PositionUAV,
					VelocityUAV,
					ParticleCount,
					ParticleRadius
				);
			}

			// 버퍼 준비 완료
			RenderResource->SetBufferReadyForRendering(true);
		}
		else
		{
			//========================================
			// CPU 모드: CachedParticles에서 GPU 버퍼로 업로드
			// 버퍼가 없으면 여기서 생성
			//========================================
			const TArray<FKawaiiRenderParticle>& CachedParticles = RenderResource->GetCachedParticles();
			const int32 ParticleCount = CachedParticles.Num();

			if (ParticleCount <= 0)
			{
				continue;
			}

			RDG_EVENT_SCOPE(GraphBuilder, "UploadToRenderResource_CPU");

			// Position/Velocity 데이터 추출
			TArray<FVector3f> Positions;
			TArray<FVector3f> Velocities;
			Positions.SetNumUninitialized(ParticleCount);
			Velocities.SetNumUninitialized(ParticleCount);
			for (int32 i = 0; i < ParticleCount; ++i)
			{
				Positions[i] = CachedParticles[i].Position;
				Velocities[i] = CachedParticles[i].Velocity;
			}

			// Bounds 계산 (CPU에서)
			FVector3f BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
			FVector3f BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			const float BoundsMargin = ParticleRadius * 2.0f + 5.0f;
			for (int32 i = 0; i < ParticleCount; ++i)
			{
				const FVector3f& Pos = CachedParticles[i].Position;
				BoundsMin = FVector3f::Min(BoundsMin, Pos);
				BoundsMax = FVector3f::Max(BoundsMax, Pos);
			}
			BoundsMin -= FVector3f(BoundsMargin);
			BoundsMax += FVector3f(BoundsMargin);
			FVector3f BoundsData[2] = { BoundsMin, BoundsMax };

			// Pooled 버퍼 가져오기
			TRefCountPtr<FRDGPooledBuffer> PositionPooledBuffer = RenderResource->GetPooledPositionBuffer();
			TRefCountPtr<FRDGPooledBuffer> VelocityPooledBuffer = RenderResource->GetPooledVelocityBuffer();
			TRefCountPtr<FRDGPooledBuffer> RenderParticlePooled = RenderResource->GetPooledRenderParticleBuffer();
			TRefCountPtr<FRDGPooledBuffer> BoundsPooled = RenderResource->GetPooledBoundsBuffer();

			//========================================
			// Position 버퍼: 기존 사용 또는 새로 생성
			//========================================
			{
				FRDGBufferRef PositionBuffer;
				if (PositionPooledBuffer.IsValid())
				{
					PositionBuffer = GraphBuilder.RegisterExternalBuffer(
						PositionPooledBuffer, TEXT("RenderPositions_Upload"));
				}
				else
				{
					FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), ParticleCount);
					Desc.Usage |= EBufferUsageFlags::UnorderedAccess;
					PositionBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("RenderPositions_New"));
					GraphBuilder.QueueBufferExtraction(PositionBuffer, RenderResource->GetPooledPositionBufferPtr());
				}
				GraphBuilder.QueueBufferUpload(PositionBuffer, Positions.GetData(), ParticleCount * sizeof(FVector3f));
			}

			//========================================
			// Velocity 버퍼: 기존 사용 또는 새로 생성
			//========================================
			{
				FRDGBufferRef VelocityBuffer;
				if (VelocityPooledBuffer.IsValid())
				{
					VelocityBuffer = GraphBuilder.RegisterExternalBuffer(
						VelocityPooledBuffer, TEXT("RenderVelocities_Upload"));
				}
				else
				{
					FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), ParticleCount);
					Desc.Usage |= EBufferUsageFlags::UnorderedAccess;
					VelocityBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("RenderVelocities_New"));
					GraphBuilder.QueueBufferExtraction(VelocityBuffer, RenderResource->GetPooledVelocityBufferPtr());
				}
				GraphBuilder.QueueBufferUpload(VelocityBuffer, Velocities.GetData(), ParticleCount * sizeof(FVector3f));
			}

			//========================================
			// RenderParticle 버퍼: 기존 사용 또는 새로 생성
			//========================================
			{
				FRDGBufferRef RenderParticleBuffer;
				if (RenderParticlePooled.IsValid())
				{
					RenderParticleBuffer = GraphBuilder.RegisterExternalBuffer(
						RenderParticlePooled, TEXT("RenderParticles_Upload"));
				}
				else
				{
					FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FKawaiiRenderParticle), ParticleCount);
					Desc.Usage |= EBufferUsageFlags::UnorderedAccess;
					RenderParticleBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("RenderParticles_New"));
					GraphBuilder.QueueBufferExtraction(RenderParticleBuffer, RenderResource->GetPooledRenderParticleBufferPtr());
				}
				GraphBuilder.QueueBufferUpload(RenderParticleBuffer, CachedParticles.GetData(), ParticleCount * sizeof(FKawaiiRenderParticle));
			}

			//========================================
			// Bounds 버퍼: 기존 사용 또는 새로 생성
			//========================================
			{
				FRDGBufferRef BoundsBuffer;
				if (BoundsPooled.IsValid())
				{
					BoundsBuffer = GraphBuilder.RegisterExternalBuffer(
						BoundsPooled, TEXT("ParticleBounds_Upload"));
				}
				else
				{
					FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), 2);
					Desc.Usage |= EBufferUsageFlags::UnorderedAccess;
					BoundsBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("ParticleBounds_New"));
					GraphBuilder.QueueBufferExtraction(BoundsBuffer, RenderResource->GetPooledBoundsBufferPtr());
				}
				GraphBuilder.QueueBufferUpload(BoundsBuffer, BoundsData, sizeof(BoundsData));
			}

			// 버퍼 준비 완료
			RenderResource->SetBufferReadyForRendering(true);
		}
	}
}

bool FFluidSceneViewExtension::IsViewFromOurWorld(const FSceneView& InView) const
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return false;
	}

	UWorld* OurWorld = SubsystemPtr->GetWorld();
	if (!OurWorld)
	{
		return false;
	}

	// Get World from View's Family Scene
	if (InView.Family && InView.Family->Scene)
	{
		UWorld* ViewWorld = InView.Family->Scene->GetWorld();
		return ViewWorld == OurWorld;
	}

	return false;
}

void FFluidSceneViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// Only render for views from our World
	if (!IsViewFromOurWorld(InView))
	{
		return;
	}

	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluid_PostBasePass");

	// Collect GBuffer/Translucent renderers only
	// Batching by (Preset + GPUMode) - allows GPU/CPU mixing with same Preset
	// PostProcess mode is handled entirely in SubscribeToPostProcessingPass
	// - GBuffer: writes to GBuffer
	// - Translucent: writes to GBuffer + Stencil marking
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> GBufferBatches;
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> TranslucentBatches;

	const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
		if (MetaballRenderer && MetaballRenderer->IsRenderingActive())
		{
			// Get preset for batching
			UKawaiiFluidPresetDataAsset* Preset = MetaballRenderer->GetPreset();
			if (!Preset)
			{
				continue;
			}

			// Determine GPU mode from RenderResource (Single Source of Truth)
			FKawaiiFluidRenderResource* RR = MetaballRenderer->GetFluidRenderResource();
			bool bUseGPU = (RR && RR->HasGPUSimulator());
			FContextCacheKey BatchKey(Preset, bUseGPU);

			// Get rendering params from preset
			const FFluidRenderingParameters& Params = Preset->RenderingParameters;

			// Route based on ShadingMode (PostProcess is handled in SubscribeToPostProcessingPass)
			switch (Params.ShadingMode)
			{
			case EMetaballShadingMode::GBuffer:
			case EMetaballShadingMode::Opaque:
				GBufferBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
				break;
			case EMetaballShadingMode::Translucent:
				TranslucentBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
				break;
			case EMetaballShadingMode::PostProcess:
				// Handled in SubscribeToPostProcessingPass
				break;
			}
		}
	}

	if (GBufferBatches.Num() == 0 && TranslucentBatches.Num() == 0)
	{
		return;
	}

	UE_LOG(LogTemp, Log,
	       TEXT(
		       "KawaiiFluid: PostRenderBasePassDeferred - Processing %d GBuffer, %d Translucent batches"
	       ),
	       GBufferBatches.Num(), TranslucentBatches.Num());

	// Get SceneDepth from RenderTargets
	FRDGTextureRef SceneDepthTexture = RenderTargets.DepthStencil.GetTexture();

	// Process GBuffer batches using new Pipeline architecture
	// Batched by (Preset + GPUMode) - same context renders only once
	for (auto& Batch : GBufferBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_GBuffer");

		// Get Pipeline from first renderer (all renderers in batch share same preset)
		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PostBasePass - handles GBuffer write internally based on ShadingMode
			Pipeline->ExecutePostBasePass(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture);

			UE_LOG(LogTemp, Log,
			       TEXT("KawaiiFluid: GBuffer Pipeline rendered %d renderers at PostBasePass timing"
			       ), Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for GBuffer batch"));
		}
	}

	// Process Translucent batches - ExecutePostBasePass for GBuffer write with Stencil marking
	// This writes to GBuffer with Stencil=0x01 marking for TransparencyComposite
	// Batched by (Preset + GPUMode) - same context renders only once
	for (auto& Batch : TranslucentBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_Translucent_GBufferWrite");

		// Get Pipeline from first renderer (all renderers in batch share same preset)
		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PostBasePass - handles GBuffer write with Stencil marking internally
			// Transparency pass runs later in PrePostProcessPass_RenderThread via ExecutePrePostProcess
			Pipeline->ExecutePostBasePass(
				GraphBuilder,
				InView,
				BatchParams,
				Renderers,
				SceneDepthTexture);

			UE_LOG(LogTemp, Log,
			       TEXT("KawaiiFluid: Translucent GBuffer write - %d renderers (Stencil marked)"),
			       Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for Translucent batch"));
		}
	}
}

void FFluidSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	// Custom mode: Tonemap pass (ScreenSpace pipeline)
	// Note: Translucent mode is handled in PrePostProcessPass_RenderThread
	if (Pass == EPostProcessingPass::Tonemap)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateLambda(
			[this](FRDGBuilder& GraphBuilder, const FSceneView& View,
			       const FPostProcessMaterialInputs& InInputs)
			{
				// Only render for views from our World
				if (!IsViewFromOurWorld(View))
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();

				// 유효성 검사
				bool bHasAnyModules = SubsystemPtr && SubsystemPtr->GetAllRenderingModules().Num() >
					0;

				if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering || !
					bHasAnyModules)
				{
					return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
				}

				// ============================================
				// All fluid rendering and shadow processing is now in PrePostProcessPass_RenderThread (before TSR)
				// This callback is kept for potential future use but does nothing
				// ============================================
				return InInputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
			}
		));
	}
}

void FFluidSceneViewExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessingInputs& Inputs)
{
	// Only render for views from our World
	if (!IsViewFromOurWorld(View))
	{
		return;
	}

	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr || !SubsystemPtr->RenderingParameters.bEnableRendering)
	{
		return;
	}

	// Collect all renderers for PrePostProcess (before TSR)
	// Batching by (Preset + GPUMode) - allows GPU/CPU mixing with same Preset
	// - Translucent: GBuffer write already done, transparency compositing here
	// - ScreenSpace: Full pipeline (depth/normal/thickness generation + shading)
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> TranslucentBatches;
	TMap<FContextCacheKey, TArray<UKawaiiFluidMetaballRenderer*>> ScreenSpaceBatches;
	const FFluidRenderingParameters* ShadowRenderParams = nullptr;

	const TArray<UKawaiiFluidRenderingModule*>& Modules = SubsystemPtr->GetAllRenderingModules();
	for (UKawaiiFluidRenderingModule* Module : Modules)
	{
		if (!Module) continue;

		UKawaiiFluidMetaballRenderer* MetaballRenderer = Module->GetMetaballRenderer();
		if (MetaballRenderer && MetaballRenderer->IsRenderingActive())
		{
			// Get preset for batching
			UKawaiiFluidPresetDataAsset* Preset = MetaballRenderer->GetPreset();
			if (!Preset)
			{
				continue;
			}

			// Determine GPU mode from RenderResource (Single Source of Truth)
			FKawaiiFluidRenderResource* RR = MetaballRenderer->GetFluidRenderResource();
			bool bUseGPU = (RR && RR->HasGPUSimulator());
			FContextCacheKey BatchKey(Preset, bUseGPU);

			// Get rendering params from preset
			const FFluidRenderingParameters& Params = Preset->RenderingParameters;

			// Collect shadow params from first renderer with shadow casting enabled
			if (!ShadowRenderParams && Params.bEnableShadowCasting)
			{
				ShadowRenderParams = &Params;
			}

			if (Params.ShadingMode == EMetaballShadingMode::Translucent)
			{
				TranslucentBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
			}
			else if (Params.ShadingMode == EMetaballShadingMode::GBuffer)
			{
				// GBuffer shading is handled in PostRenderBasePassDeferred
				continue;
			}
			else if (Params.PipelineType == EMetaballPipelineType::ScreenSpace)
			{
				ScreenSpaceBatches.FindOrAdd(BatchKey).Add(MetaballRenderer);
			}
		}
	}

	// Early return if nothing to render and no shadows
	if (TranslucentBatches.Num() == 0 && ScreenSpaceBatches.Num() == 0 && !ShadowRenderParams)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "KawaiiFluid_TransparencyPass_PrePostProcess");

	// Get textures from Inputs - at this point everything is at internal resolution
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	FIntRect ViewRect = ViewInfo.ViewRect;

	// Get SceneColor and SceneDepth from SceneTextures
	if (!Inputs.SceneTextures)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid PrePostProcess: SceneTextures not available"));
		return;
	}

	FRDGTextureRef SceneColorTexture = (*Inputs.SceneTextures)->SceneColorTexture;
	FRDGTextureRef SceneDepthTexture = (*Inputs.SceneTextures)->SceneDepthTexture;

	if (!SceneColorTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid PrePostProcess: SceneColor not available"));
		return;
	}

	// Get GBuffer textures
	const FSceneTextures& SceneTexturesRef = ViewInfo.GetSceneTextures();
	FRDGTextureRef GBufferATexture = SceneTexturesRef.GBufferA;
	FRDGTextureRef GBufferDTexture = SceneTexturesRef.GBufferD;

	if (!GBufferATexture || !GBufferDTexture || !SceneDepthTexture)
	{
		UE_LOG(LogTemp, Warning,
		       TEXT("KawaiiFluid PrePostProcess: Missing GBuffer or Depth textures"));
		return;
	}

	// Debug log - all should be at internal resolution now
	//UE_LOG(LogTemp, Warning, TEXT("=== PrePostProcess TransparencyPass ==="));
	//UE_LOG(LogTemp, Warning, TEXT("ViewRect: Min(%d,%d) Size(%d,%d)"),
	//       ViewRect.Min.X, ViewRect.Min.Y, ViewRect.Width(), ViewRect.Height());
	//UE_LOG(LogTemp, Warning, TEXT("SceneColor Size: (%d,%d)"),
	//       SceneColorTexture->Desc.Extent.X, SceneColorTexture->Desc.Extent.Y);
	//UE_LOG(LogTemp, Warning, TEXT("GBufferA Size: (%d,%d)"),
	//       GBufferATexture->Desc.Extent.X, GBufferATexture->Desc.Extent.Y);

	// Create output render target from SceneColor
	FScreenPassRenderTarget Output(
		FScreenPassTexture(SceneColorTexture, ViewRect),
		ERenderTargetLoadAction::ELoad);

	// Create copy of SceneColor for reading (can't read and write same texture)
	FRDGTextureDesc LitSceneColorDesc = SceneColorTexture->Desc;
	LitSceneColorDesc.Flags |= TexCreate_ShaderResource;
	FRDGTextureRef LitSceneColorCopy = GraphBuilder.CreateTexture(
		LitSceneColorDesc,
		TEXT("LitSceneColorCopy_PrePostProcess"));

	// Copy SceneColor
	AddCopyTexturePass(GraphBuilder, SceneColorTexture, LitSceneColorCopy);

	// ============================================
	// Shadow Processing (before fluid rendering)
	// ============================================
	if (ShadowRenderParams)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "FluidShadowProcessing");

		// 1. Shadow Projection - generates VSM texture from history buffer
		{
			RDG_EVENT_SCOPE(GraphBuilder, "ShadowProjection");
			ExecuteFluidShadowProjection(
				GraphBuilder,
				View,
				SubsystemPtr,
				*ShadowRenderParams);
		}

		// 2. Shadow Receiver - applies shadows to scene
		{
			RDG_EVENT_SCOPE(GraphBuilder, "ShadowReceiver");
			// Create a copy for shadow receiver input (can't read and write same texture)
			FRDGTextureDesc ShadowInputDesc = SceneColorTexture->Desc;
			ShadowInputDesc.Flags &= ~(TexCreate_Presentable | TexCreate_DepthStencilTargetable |
				TexCreate_ResolveTargetable);
			ShadowInputDesc.Flags |= (TexCreate_RenderTargetable | TexCreate_ShaderResource);
			FRDGTextureRef ShadowInputCopy = GraphBuilder.CreateTexture(
				ShadowInputDesc,
				TEXT("FluidShadowReceiverInput_PrePostProcess"));
			AddCopyTexturePass(GraphBuilder, SceneColorTexture, ShadowInputCopy);

			ApplyFluidShadowReceiver(
				GraphBuilder,
				View,
				SubsystemPtr,
				*ShadowRenderParams,
				ShadowInputCopy,
				SceneDepthTexture,
				Output);
		}

		// 3. Update scene color copy
		{
			RDG_EVENT_SCOPE(GraphBuilder, "UpdateSceneColorCopy");
			AddCopyTexturePass(GraphBuilder, SceneColorTexture, LitSceneColorCopy);
		}
	}

	// Apply TransparencyPass for each Translucent batch using Pipeline
	// Batched by (Preset + GPUMode) - same context renders only once
	for (auto& Batch : TranslucentBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_Translucent(%d renderers)", Renderers.Num());

		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// Execute PrePostProcess with GBuffer textures for transparency compositing
			Pipeline->ExecutePrePostProcess(
				GraphBuilder,
				View,
				BatchParams,
				Renderers,
				SceneDepthTexture, // Has Stencil=0x01 marking from GBuffer write
				LitSceneColorCopy, // Lit scene color (after Lumen/VSM)
				Output,
				GBufferATexture, // Normals for refraction direction
				GBufferDTexture); // Thickness for Beer's Law absorption
		}
	}

	// ============================================
	// ScreenSpace Pipeline Rendering (before TSR)
	// Batched by (Preset + GPUMode) - same context renders only once
	// ============================================
	for (auto& Batch : ScreenSpaceBatches)
	{
		const FContextCacheKey& CacheKey = Batch.Key;
		UKawaiiFluidPresetDataAsset* Preset = CacheKey.Preset;
		const TArray<UKawaiiFluidMetaballRenderer*>& Renderers = Batch.Value;

		// Get rendering params directly from preset
		const FFluidRenderingParameters& BatchParams = Preset->RenderingParameters;

		RDG_EVENT_SCOPE(GraphBuilder, "FluidBatch_ScreenSpace(%d)", Renderers.Num());

		if (Renderers.Num() > 0 && Renderers[0]->GetPipeline())
		{
			TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline = Renderers[0]->GetPipeline();

			// 1. PrepareRender - generate and cache intermediate textures
			{
				RDG_EVENT_SCOPE(GraphBuilder, "PrepareRender");
				Pipeline->PrepareRender(
					GraphBuilder,
					View,
					BatchParams,
					Renderers,
					SceneDepthTexture);
			}

			// 2. ExecuteRender - apply shading
			{
				RDG_EVENT_SCOPE(GraphBuilder, "ExecuteRender");
				Pipeline->ExecuteRender(
					GraphBuilder,
					View,
					BatchParams,
					Renderers,
					SceneDepthTexture,
					LitSceneColorCopy,
					Output);
			}

			// 3. Store smoothed depth to history for shadow projection
			{
				RDG_EVENT_SCOPE(GraphBuilder, "StoreDepthHistory");
				if (FFluidShadowHistoryManager* HistoryManager = SubsystemPtr->
					GetShadowHistoryManager())
				{
					if (const FMetaballIntermediateTextures* IntermediateTextures = Pipeline->
						GetCachedIntermediateTextures())
					{
						if (IntermediateTextures->SmoothedDepthTexture)
						{
							HistoryManager->StoreCurrentFrame(
								GraphBuilder,
								IntermediateTextures->SmoothedDepthTexture,
								View);
						}
						else
						{
							UE_LOG(LogTemp, Warning,
							       TEXT("KawaiiFluid: SmoothedDepthTexture is null!"));
						}
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: IntermediateTextures is null!"));
					}
				}
			}

			UE_LOG(LogTemp, Verbose,
			       TEXT("KawaiiFluid: ScreenSpace Pipeline rendered %d renderers"),
			       Renderers.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("KawaiiFluid: No Pipeline found for ScreenSpace batch"));
		}
	}

	// UE_LOG(LogTemp, Log,
	//        TEXT("KawaiiFluid: PrePostProcess rendered - Translucent:%d ScreenSpace:%d"),
	//        TranslucentBatches.Num(), ScreenSpaceBatches.Num());
}
