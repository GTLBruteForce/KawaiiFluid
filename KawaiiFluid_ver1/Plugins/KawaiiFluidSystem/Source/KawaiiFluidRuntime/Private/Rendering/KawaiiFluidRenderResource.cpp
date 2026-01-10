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
#include "GPU/GPUFluidSimulator.h"

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
	// Legacy AoS 버퍼 해제
	ParticleBuffer.SafeRelease();
	ParticleSRV.SafeRelease();
	ParticleUAV.SafeRelease();
	PooledParticleBuffer.SafeRelease();

	// SoA 버퍼 해제
	PooledPositionBuffer.SafeRelease();
	PooledVelocityBuffer.SafeRelease();

	ParticleCount = 0;
	BufferCapacity = 0;
	bBufferReadyForRendering.store(false);
}

void FKawaiiFluidRenderResource::AppendParticlesSnapshot(TArray<FKawaiiRenderParticle>&& InParticles)
{
	// 렌더 스레드 전용 - ENQUEUE_RENDER_COMMAND 내부에서 호출됨
	check(IsInRenderingThread());

	// CPU 모드로 마킹
	bIsInGPUMode.store(false);

	if (InParticles.Num() == 0)
	{
		return;
	}

	// 새 프레임이면 자동 Clear
	uint32 CurrentFrame = GFrameCounterRenderThread;
	if (LastSnapshotFrame != CurrentFrame)
	{
		CachedParticles.Empty();
		LastSnapshotFrame = CurrentFrame;
	}

	// 파티클 추가
	CachedParticles.Append(MoveTemp(InParticles));

	// Radius 업데이트 (첫 파티클 기준)
	if (CachedParticles.Num() > 0)
	{
		CachedParticleRadius.store(CachedParticles[0].Radius);
	}

	// 버퍼 크기 조정 필요 시 즉시 ResizeBuffer 호출 (이미 렌더 스레드)
	const int32 TotalCount = CachedParticles.Num();
	if (NeedsResize(TotalCount))
	{
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		int32 NewCapacity = FMath::Max(TotalCount, BufferCapacity * 2);
		ResizeBuffer(RHICmdList, NewCapacity);
	}
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
	// 기존 버퍼 해제 (Legacy AoS)
	ParticleBuffer.SafeRelease();
	ParticleSRV.SafeRelease();
	ParticleUAV.SafeRelease();
	PooledParticleBuffer.SafeRelease();

	// SoA 버퍼 해제
	PooledPositionBuffer.SafeRelease();
	PooledVelocityBuffer.SafeRelease();

	// Bounds/RenderParticle 버퍼 해제
	PooledBoundsBuffer.SafeRelease();
	PooledRenderParticleBuffer.SafeRelease();

	BufferCapacity = NewCapacity;

	if (NewCapacity == 0)
	{
		return;
	}

	const uint32 ElementSize = sizeof(FKawaiiRenderParticle);

	// Create Pooled Buffer via RDG (Phase 2: single source of truth)
	FRHICommandListImmediate& ImmediateCmdList = static_cast<FRHICommandListImmediate&>(RHICmdList);
	FRDGBuilder GraphBuilder(ImmediateCmdList);

	//========================================
	// Legacy AoS 버퍼 생성 (호환성 유지)
	//========================================
	FRDGBufferDesc RDGBufferDesc = FRDGBufferDesc::CreateStructuredDesc(ElementSize, NewCapacity);
	RDGBufferDesc.Usage |= EBufferUsageFlags::UnorderedAccess;
	FRDGBufferRef RDGBuffer = GraphBuilder.CreateBuffer(RDGBufferDesc, TEXT("RenderParticlesPooled"));

	FRDGBufferUAVRef BufferUAV = GraphBuilder.CreateUAV(RDGBuffer);
	AddClearUAVPass(GraphBuilder, BufferUAV, 0u);
	GraphBuilder.QueueBufferExtraction(RDGBuffer, &PooledParticleBuffer, ERHIAccess::SRVMask);

	//========================================
	// SoA 버퍼 생성 (메모리 대역폭 최적화)
	//========================================

	// Position 버퍼 (float3 = 12 bytes)
	FRDGBufferDesc PositionBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), NewCapacity);
	PositionBufferDesc.Usage |= EBufferUsageFlags::UnorderedAccess;
	FRDGBufferRef PositionRDGBuffer = GraphBuilder.CreateBuffer(PositionBufferDesc, TEXT("RenderPositionsSoA"));

	FRDGBufferUAVRef PositionUAV = GraphBuilder.CreateUAV(PositionRDGBuffer);
	AddClearUAVPass(GraphBuilder, PositionUAV, 0u);
	GraphBuilder.QueueBufferExtraction(PositionRDGBuffer, &PooledPositionBuffer, ERHIAccess::SRVMask);

	// Velocity 버퍼 (float3 = 12 bytes)
	FRDGBufferDesc VelocityBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), NewCapacity);
	VelocityBufferDesc.Usage |= EBufferUsageFlags::UnorderedAccess;
	FRDGBufferRef VelocityRDGBuffer = GraphBuilder.CreateBuffer(VelocityBufferDesc, TEXT("RenderVelocitiesSoA"));

	FRDGBufferUAVRef VelocityUAV = GraphBuilder.CreateUAV(VelocityRDGBuffer);
	AddClearUAVPass(GraphBuilder, VelocityUAV, 0u);
	GraphBuilder.QueueBufferExtraction(VelocityRDGBuffer, &PooledVelocityBuffer, ERHIAccess::SRVMask);

	//========================================
	// Bounds 버퍼 (float3 * 2 = Min, Max)
	//========================================
	FRDGBufferDesc BoundsBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), 2);
	BoundsBufferDesc.Usage |= EBufferUsageFlags::UnorderedAccess;
	FRDGBufferRef BoundsRDGBuffer = GraphBuilder.CreateBuffer(BoundsBufferDesc, TEXT("ParticleBounds"));

	FRDGBufferUAVRef BoundsUAV = GraphBuilder.CreateUAV(BoundsRDGBuffer);
	AddClearUAVPass(GraphBuilder, BoundsUAV, 0u);
	GraphBuilder.QueueBufferExtraction(BoundsRDGBuffer, &PooledBoundsBuffer, ERHIAccess::SRVMask);

	//========================================
	// RenderParticle 버퍼 (FKawaiiRenderParticle)
	//========================================
	FRDGBufferDesc RenderParticleBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FKawaiiRenderParticle), NewCapacity);
	RenderParticleBufferDesc.Usage |= EBufferUsageFlags::UnorderedAccess;
	FRDGBufferRef RenderParticleRDGBuffer = GraphBuilder.CreateBuffer(RenderParticleBufferDesc, TEXT("RenderParticlesSDF"));

	FRDGBufferUAVRef RenderParticleUAV = GraphBuilder.CreateUAV(RenderParticleRDGBuffer);
	AddClearUAVPass(GraphBuilder, RenderParticleUAV, 0u);
	GraphBuilder.QueueBufferExtraction(RenderParticleRDGBuffer, &PooledRenderParticleBuffer, ERHIAccess::SRVMask);

	GraphBuilder.Execute();

	// Get RHI buffer from pooled buffer (Legacy AoS)
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

			// Register our render pooled buffer (destination) - Legacy AoS
			FRDGBufferRef RenderBufferRDG = GraphBuilder.RegisterExternalBuffer(
				RenderResource->PooledParticleBuffer,
				TEXT("RenderParticlesBuffer")
			);
			FRDGBufferUAVRef RenderParticlesUAV = GraphBuilder.CreateUAV(RenderBufferRDG);

			// Use existing AddExtractRenderDataPass (Legacy AoS)
			FGPUFluidSimulatorPassBuilder::AddExtractRenderDataPass(
				GraphBuilder,
				PhysicsParticlesSRV,
				RenderParticlesUAV,
				NewCount,
				Radius
			);

			// SoA 버퍼도 추출 (메모리 대역폭 최적화)
			if (RenderResource->PooledPositionBuffer.IsValid() && RenderResource->PooledVelocityBuffer.IsValid())
			{
				FRDGBufferRef PositionBufferRDG = GraphBuilder.RegisterExternalBuffer(
					RenderResource->PooledPositionBuffer,
					TEXT("RenderPositionsSoA")
				);
				FRDGBufferUAVRef PositionsUAV = GraphBuilder.CreateUAV(PositionBufferRDG);

				FRDGBufferRef VelocityBufferRDG = GraphBuilder.RegisterExternalBuffer(
					RenderResource->PooledVelocityBuffer,
					TEXT("RenderVelocitiesSoA")
				);
				FRDGBufferUAVRef VelocitiesUAV = GraphBuilder.CreateUAV(VelocityBufferRDG);

				FGPUFluidSimulatorPassBuilder::AddExtractRenderDataSoAPass(
					GraphBuilder,
					PhysicsParticlesSRV,
					PositionsUAV,
					VelocitiesUAV,
					NewCount,
					Radius
				);
			}

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

//========================================
// 통합 인터페이스 구현 (CPU/GPU 일원화)
//========================================

void FKawaiiFluidRenderResource::SetGPUSimulatorReference(
	FGPUFluidSimulator* InSimulator,
	int32 InParticleCount,
	float InParticleRadius)
{
	CachedGPUSimulator.store(InSimulator);
	CachedGPUParticleCount.store(InParticleCount);
	CachedParticleRadius.store(InParticleRadius);

	if (InSimulator)
	{
		bIsInGPUMode.store(true);

		// GPU 모드에서는 CPU 캐시 비우기
		if (CachedParticles.Num() > 0)
		{
			CachedParticles.Empty();
		}

		// 버퍼 크기 조정 필요 시 렌더 스레드에서 ResizeBuffer 호출
		const bool bNeedsResizeNow = NeedsResize(InParticleCount);
		if (bNeedsResizeNow)
		{
			FKawaiiFluidRenderResource* RenderResource = this;
			const int32 NewCount = InParticleCount;

			ENQUEUE_RENDER_COMMAND(ResizeBufferForGPUMode)(
				[RenderResource, NewCount](FRHICommandListImmediate& RHICmdList)
				{
					int32 NewCapacity = FMath::Max(NewCount, RenderResource->BufferCapacity * 2);
					RenderResource->ResizeBuffer(RHICmdList, NewCapacity);
				}
			);
		}
	}
}

void FKawaiiFluidRenderResource::ClearGPUSimulatorReference()
{
	CachedGPUSimulator.store(nullptr);
	CachedGPUParticleCount.store(0);
	bIsInGPUMode.store(false);
}

int32 FKawaiiFluidRenderResource::GetUnifiedParticleCount() const
{
	FGPUFluidSimulator* Simulator = CachedGPUSimulator.load();
	if (Simulator)
	{
		// GPU 모드: 시뮬레이터에서 최신 파티클 수 가져오기
		return Simulator->GetPersistentParticleCount();
	}
	else
	{
		// CPU 모드: 캐시된 파티클 수
		return CachedParticles.Num();
	}
}

FRDGBufferSRVRef FKawaiiFluidRenderResource::GetPhysicsBufferSRV(FRDGBuilder& GraphBuilder) const
{
	FGPUFluidSimulator* Simulator = CachedGPUSimulator.load();
	if (!Simulator)
	{
		return nullptr;
	}

	TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer = Simulator->GetPersistentParticleBuffer();
	if (!PhysicsPooledBuffer.IsValid())
	{
		return nullptr;
	}

	FRDGBufferRef PhysicsBuffer = GraphBuilder.RegisterExternalBuffer(
		PhysicsPooledBuffer,
		TEXT("UnifiedPhysicsParticles")
	);
	return GraphBuilder.CreateSRV(PhysicsBuffer);
}

bool FKawaiiFluidRenderResource::GetAnisotropyBufferSRVs(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef& OutAxis1SRV,
	FRDGBufferSRVRef& OutAxis2SRV,
	FRDGBufferSRVRef& OutAxis3SRV) const
{
	FGPUFluidSimulator* Simulator = CachedGPUSimulator.load();
	if (!Simulator || !Simulator->IsAnisotropyEnabled())
	{
		OutAxis1SRV = nullptr;
		OutAxis2SRV = nullptr;
		OutAxis3SRV = nullptr;
		return false;
	}

	TRefCountPtr<FRDGPooledBuffer> Axis1Pooled = Simulator->GetPersistentAnisotropyAxis1Buffer();
	TRefCountPtr<FRDGPooledBuffer> Axis2Pooled = Simulator->GetPersistentAnisotropyAxis2Buffer();
	TRefCountPtr<FRDGPooledBuffer> Axis3Pooled = Simulator->GetPersistentAnisotropyAxis3Buffer();

	if (!Axis1Pooled.IsValid() || !Axis2Pooled.IsValid() || !Axis3Pooled.IsValid())
	{
		OutAxis1SRV = nullptr;
		OutAxis2SRV = nullptr;
		OutAxis3SRV = nullptr;
		return false;
	}

	FRDGBufferRef Axis1Buffer = GraphBuilder.RegisterExternalBuffer(Axis1Pooled, TEXT("UnifiedAnisotropyAxis1"));
	FRDGBufferRef Axis2Buffer = GraphBuilder.RegisterExternalBuffer(Axis2Pooled, TEXT("UnifiedAnisotropyAxis2"));
	FRDGBufferRef Axis3Buffer = GraphBuilder.RegisterExternalBuffer(Axis3Pooled, TEXT("UnifiedAnisotropyAxis3"));

	OutAxis1SRV = GraphBuilder.CreateSRV(Axis1Buffer);
	OutAxis2SRV = GraphBuilder.CreateSRV(Axis2Buffer);
	OutAxis3SRV = GraphBuilder.CreateSRV(Axis3Buffer);

	return true;
}

bool FKawaiiFluidRenderResource::IsAnisotropyEnabled() const
{
	FGPUFluidSimulator* Simulator = CachedGPUSimulator.load();
	return Simulator && Simulator->IsAnisotropyEnabled();
}

//========================================
// Bounds 및 RenderParticle 버퍼 관리
//========================================

void FKawaiiFluidRenderResource::SetBoundsBuffer(TRefCountPtr<FRDGPooledBuffer> InBoundsBuffer)
{
	PooledBoundsBuffer = InBoundsBuffer;
}

void FKawaiiFluidRenderResource::SetRenderParticleBuffer(TRefCountPtr<FRDGPooledBuffer> InBuffer)
{
	PooledRenderParticleBuffer = InBuffer;
}
