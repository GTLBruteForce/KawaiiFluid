// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidRenderResource.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ClearQuad.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "ShaderParameterUtils.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/GPUFluidParticle.h"

FKawaiiFluidRenderResource::FKawaiiFluidRenderResource()
	: ParticleCount(0)
	, BufferCapacity(0)
{
}

FKawaiiFluidRenderResource::~FKawaiiFluidRenderResource()
{
	// 렌더 리소스가 제대로 정리되었는지 확인
	check(!IsInitialized() && "RenderResource must be released before destruction!");
}

void FKawaiiFluidRenderResource::InitRHI(FRHICommandListBase& RHICmdList)
{
	// 초기에는 작은 버퍼 생성 (실제 데이터 업데이트 시 리사이즈됨)
	if (BufferCapacity == 0)
	{
		BufferCapacity = 100; // 기본 100개 파티클
	}

	ResizeBuffer(RHICmdList, BufferCapacity);
}

void FKawaiiFluidRenderResource::ReleaseRHI()
{
	ParticleBuffer.SafeRelease();
	ParticleSRV.SafeRelease();
	ParticleUAV.SafeRelease();
	PooledParticleBuffer.SafeRelease();
	ParticleCount = 0;
	BufferCapacity = 0;
	bBufferReadyForRendering.store(false);
}

void FKawaiiFluidRenderResource::UpdateParticleData(const TArray<FKawaiiRenderParticle>& InParticles)
{
	// Mark as CPU mode (not GPU mode)
	bIsInGPUMode.store(false);

	int32 NewCount = InParticles.Num();

	if (NewCount == 0)
	{
		ParticleCount = 0;
		CachedParticles.Empty();  // ✅ 캐시 비우기
		return;
	}

	// ✅ CPU 측 캐시 업데이트 (게임 스레드)
	CachedParticles = InParticles;

	// 데이터 복사 (렌더 스레드로 전달하기 위해)
	TArray<FKawaiiRenderParticle> ParticlesCopy = InParticles;

	// 렌더 스레드로 전송
	FKawaiiFluidRenderResource* RenderResource = this;
	ENQUEUE_RENDER_COMMAND(UpdateFluidParticleBuffer)(
		[RenderResource, ParticlesCopy, NewCount](FRHICommandListImmediate& RHICmdList)
		{
			// 버퍼 크기 조정 필요 시 재생성
			if (RenderResource->NeedsResize(NewCount))
			{
				int32 NewCapacity = FMath::Max(NewCount, RenderResource->BufferCapacity * 2);
				RenderResource->ResizeBuffer(RHICmdList, NewCapacity);
			}

			// GPU 버퍼에 데이터 업로드
			if (RenderResource->ParticleBuffer.IsValid())
			{
				// 상태 전환: SRVMask → CopyDest
				RHICmdList.Transition(FRHITransitionInfo(
					RenderResource->ParticleBuffer,
					ERHIAccess::SRVMask,
					ERHIAccess::CopyDest
				));

				void* BufferData = RHICmdList.LockBuffer(
					RenderResource->ParticleBuffer,
					0,
					NewCount * sizeof(FKawaiiRenderParticle),
					RLM_WriteOnly
				);

				FMemory::Memcpy(BufferData, ParticlesCopy.GetData(), NewCount * sizeof(FKawaiiRenderParticle));

				RHICmdList.UnlockBuffer(RenderResource->ParticleBuffer);

				// 상태 전환: CopyDest → SRVMask
				RHICmdList.Transition(FRHITransitionInfo(
					RenderResource->ParticleBuffer,
					ERHIAccess::CopyDest,
					ERHIAccess::SRVMask
				));
			}

			RenderResource->ParticleCount = NewCount;
		}
	);
}

bool FKawaiiFluidRenderResource::NeedsResize(int32 NewCount) const
{
	// Only resize if buffer is too SMALL (not for shrinking)
	// This prevents flickering from resize during normal operation
	// Buffer shrinking is disabled to improve stability
	const bool bNeedGrow = NewCount > BufferCapacity;

	if (bNeedGrow)
	{
		UE_LOG(LogTemp, Warning, TEXT("RenderResource: Buffer resize needed (Count %d > Capacity %d)"),
			NewCount, BufferCapacity);
	}

	return bNeedGrow;
}

void FKawaiiFluidRenderResource::ResizeBuffer(FRHICommandListBase& RHICmdList, int32 NewCapacity)
{
	// 기존 버퍼 해제
	ParticleBuffer.SafeRelease();
	ParticleSRV.SafeRelease();
	ParticleUAV.SafeRelease();
	PooledParticleBuffer.SafeRelease();

	BufferCapacity = NewCapacity;

	if (NewCapacity == 0)
	{
		return;
	}

	const uint32 ElementSize = sizeof(FKawaiiRenderParticle);

	// Create Pooled Buffer via RDG (Phase 2: single source of truth)
	FRHICommandListImmediate& ImmediateCmdList = static_cast<FRHICommandListImmediate&>(RHICmdList);
	FRDGBuilder GraphBuilder(ImmediateCmdList);

	FRDGBufferDesc RDGBufferDesc = FRDGBufferDesc::CreateStructuredDesc(ElementSize, NewCapacity);
	RDGBufferDesc.Usage |= EBufferUsageFlags::UnorderedAccess;
	FRDGBufferRef RDGBuffer = GraphBuilder.CreateBuffer(RDGBufferDesc, TEXT("RenderParticlesPooled"));

	// Must have a pass that produces the buffer before extraction
	FRDGBufferUAVRef BufferUAV = GraphBuilder.CreateUAV(RDGBuffer);
	AddClearUAVPass(GraphBuilder, BufferUAV, 0u);

	// Extract to pooled buffer
	GraphBuilder.QueueBufferExtraction(RDGBuffer, &PooledParticleBuffer, ERHIAccess::SRVMask);
	GraphBuilder.Execute();

	// Get RHI buffer from pooled buffer
	if (PooledParticleBuffer.IsValid())
	{
		ParticleBuffer = PooledParticleBuffer->GetRHI();

		// Create Shader Resource View
		ParticleSRV = ImmediateCmdList.CreateShaderResourceView(
			ParticleBuffer,
			FRHIViewDesc::CreateBufferSRV()
				.SetTypeFromBuffer(ParticleBuffer)
		);

		// Create Unordered Access View (Phase 2)
		ParticleUAV = ImmediateCmdList.CreateUnorderedAccessView(
			ParticleBuffer,
			FRHIViewDesc::CreateBufferUAV()
				.SetTypeFromBuffer(ParticleBuffer)
		);
	}
}

void FKawaiiFluidRenderResource::UpdateFromGPUBuffer(
	TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer,
	int32 InParticleCount,
	float InParticleRadius)
{
	if (!PhysicsPooledBuffer.IsValid() || InParticleCount <= 0)
	{
		ParticleCount = 0;
		bBufferReadyForRendering.store(false);
		return;
	}

	// Mark as GPU mode
	bIsInGPUMode.store(true);

	// GPU mode: Clear CPU cache to prevent stale data from being used
	// This fixes the "5 cached particles" issue where old CPU data was being rendered
	if (CachedParticles.Num() > 0)
	{
		CachedParticles.Empty();
	}

	// NOTE: Do NOT set bBufferReadyForRendering = false here every frame!
	// Keep using the previous frame's buffer until the new one is ready.
	// This prevents flickering caused by timing gap between game/render threads.
	// Only invalidate when buffer needs resize (will be recreated).

	FKawaiiFluidRenderResource* RenderResource = this;
	const int32 NewCount = InParticleCount;
	const float Radius = InParticleRadius;

	// Check if resize is needed BEFORE enqueueing
	const bool bNeedsResizeNow = NeedsResize(NewCount);

	// NOTE: Do NOT set bBufferReadyForRendering = false here in game thread!
	// The render thread is 1 frame behind, so the previous frame's buffer is still valid.
	// Setting the flag here causes flickering because render pipeline skips valid data.
	// The flag will be managed entirely in the render thread.

	// Capture pooled buffer reference for render thread
	TRefCountPtr<FRDGPooledBuffer> PhysicsBufferRef = PhysicsPooledBuffer;

	ENQUEUE_RENDER_COMMAND(UpdateFromGPUBuffer)(
		[RenderResource, PhysicsBufferRef, NewCount, Radius, bNeedsResizeNow](FRHICommandListImmediate& RHICmdList)
		{
			// 버퍼 크기 조정 필요 시 재생성
			if (bNeedsResizeNow)
			{
				int32 NewCapacity = FMath::Max(NewCount, RenderResource->BufferCapacity * 2);
				RenderResource->ResizeBuffer(RHICmdList, NewCapacity);
			}

			if (!RenderResource->PooledParticleBuffer.IsValid())
			{
				return;
			}

			// Use RDG for compute shader dispatch (UE5.5 pattern)
			FRDGBuilder GraphBuilder(RHICmdList);

			// Register external physics pooled buffer (source - from GPU simulator)
			FRDGBufferRef PhysicsBufferRDG = GraphBuilder.RegisterExternalBuffer(
				PhysicsBufferRef,
				TEXT("PhysicsParticlesBuffer")
			);
			FRDGBufferSRVRef PhysicsParticlesSRV = GraphBuilder.CreateSRV(PhysicsBufferRDG);

			// Register our render pooled buffer (destination)
			FRDGBufferRef RenderBufferRDG = GraphBuilder.RegisterExternalBuffer(
				RenderResource->PooledParticleBuffer,
				TEXT("RenderParticlesBuffer")
			);
			FRDGBufferUAVRef RenderParticlesUAV = GraphBuilder.CreateUAV(RenderBufferRDG);

			// Use existing AddExtractRenderDataPass
			FGPUFluidSimulatorPassBuilder::AddExtractRenderDataPass(
				GraphBuilder,
				PhysicsParticlesSRV,
				RenderParticlesUAV,
				NewCount,
				Radius
			);

			GraphBuilder.Execute();

			RenderResource->ParticleCount = NewCount;

			// Mark buffer as ready for rendering AFTER extract pass completes
			RenderResource->bBufferReadyForRendering.store(true);

			// Log for debugging
			static int32 FrameCounter = 0;
			if (++FrameCounter % 60 == 0)
			{
				UE_LOG(LogTemp, Log, TEXT("RenderResource: GPU→GPU copy via RDG (%d particles, buffer ready)"), NewCount);
			}
		}
	);
}
