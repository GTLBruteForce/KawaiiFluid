// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidRenderResource.h"
#include "RenderGraphBuilder.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

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
	ParticleCount = 0;
	BufferCapacity = 0;
}

void FKawaiiFluidRenderResource::UpdateParticleData(const TArray<FKawaiiRenderParticle>& InParticles)
{
	int32 NewCount = InParticles.Num();

	if (NewCount == 0)
	{
		ParticleCount = 0;
		return;
	}

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

				// 1단계: LockBuffer 호출
				// 내부: Staging Buffer 할당 (Upload Heap)
                // 내부: CPU-writable 포인터 반환
				void* BufferData = RHICmdList.LockBuffer(
					RenderResource->ParticleBuffer,
					0,
					NewCount * sizeof(FKawaiiRenderParticle),
					RLM_WriteOnly
				);

				// 2단계: 데이터 쓰기
				FMemory::Memcpy(BufferData, ParticlesCopy.GetData(), NewCount * sizeof(FKawaiiRenderParticle));

				// 3단계: UnlockBuffer 호출
				// 내부: GPU Copy Command 발행
                // 내부: Staging → GPU Buffer로 DMA 전송
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
	// 버퍼가 너무 작거나, 과도하게 큰 경우 리사이즈
	return NewCount > BufferCapacity || (BufferCapacity > 1000 && NewCount < BufferCapacity / 4);
}

void FKawaiiFluidRenderResource::ResizeBuffer(FRHICommandListBase& RHICmdList, int32 NewCapacity)
{
	// 기존 버퍼 해제
	ParticleBuffer.SafeRelease();
	ParticleSRV.SafeRelease();

	BufferCapacity = NewCapacity;

	if (NewCapacity == 0)
	{
		return;
	}

	// Structured Buffer 생성 (최적화된 설정)
	const uint32 ElementSize = sizeof(FKawaiiRenderParticle);
	const uint32 BufferSize = NewCapacity * ElementSize;
	
	const FRHIBufferCreateDesc BufferDesc = 
		FRHIBufferCreateDesc::CreateStructured(
			TEXT("KawaiiFluidParticleBuffer"), 
			BufferSize,      // 전체 바이트 크기
			ElementSize      // 요소당 바이트 크기
		)
		.AddUsage(EBufferUsageFlags::ShaderResource)  // Dynamic 제거
		.SetInitialState(ERHIAccess::SRVMask);        // 초기 상태 명시

	ParticleBuffer = RHICmdList.CreateBuffer(BufferDesc);

	// Shader Resource View 생성
	ParticleSRV = RHICmdList.CreateShaderResourceView(
		ParticleBuffer,
		FRHIViewDesc::CreateBufferSRV()
			.SetTypeFromBuffer(ParticleBuffer)
	);
}
