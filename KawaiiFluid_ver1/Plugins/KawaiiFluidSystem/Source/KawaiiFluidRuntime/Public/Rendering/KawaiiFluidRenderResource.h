// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RHIResources.h"
#include "RenderGraphResources.h"
#include "Core/KawaiiRenderParticle.h"
#include <atomic>

/**
 * 유체 입자 데이터를 GPU 버퍼로 관리하는 렌더 리소스
 * CPU 시뮬레이션 데이터를 GPU에 업로드하여 Niagara/SSFR에서 사용 가능하게 함
 */
class KAWAIIFLUIDRUNTIME_API FKawaiiFluidRenderResource : public FRenderResource
{
public:
	FKawaiiFluidRenderResource();
	virtual ~FKawaiiFluidRenderResource();

	//========================================
	// FRenderResource Interface
	//========================================

	/** GPU 리소스 초기화 (렌더 스레드) */
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;

	/** GPU 리소스 해제 (렌더 스레드) */
	virtual void ReleaseRHI() override;

	//========================================
	// 데이터 업데이트 (게임 스레드에서 호출)
	//========================================

	/**
	 * 파티클 데이터 업데이트 (게임 스레드 -> 렌더 스레드)
	 * @param InParticles 렌더링용 파티클 배열
	 */
	void UpdateParticleData(const TArray<FKawaiiRenderParticle>& InParticles);

	/**
	 * GPU 버퍼에서 직접 복사 (Phase 2: GPU → GPU, no CPU involvement)
	 * @param PhysicsPooledBuffer 물리 시뮬레이터의 Pooled 파티클 버퍼
	 * @param InParticleCount 파티클 수
	 * @param InParticleRadius 파티클 반경
	 */
	void UpdateFromGPUBuffer(TRefCountPtr<FRDGPooledBuffer> PhysicsPooledBuffer, int32 InParticleCount, float InParticleRadius);

	//========================================
	// GPU 버퍼 접근 (렌더 스레드)
	//========================================

	/** Structured Buffer의 SRV (Shader Resource View) 반환 */
	FRHIShaderResourceView* GetParticleSRV() const { return ParticleSRV; }

	/** RHI 버퍼 직접 반환 (미래 통합용) */
	FRHIBuffer* GetParticleBufferRHI() const 
	{ 
		return ParticleBuffer.GetReference(); 
	}

	/** 현재 파티클 수 */
	int32 GetParticleCount() const { return ParticleCount; }

	/** 버퍼가 유효한지 확인 */
	bool IsValid() const { return ParticleBuffer.IsValid() && ParticleSRV.IsValid(); }

	//========================================
	// CPU 측 데이터 캐시 (게임 스레드)
	//========================================

	/** 캐시된 파티클 데이터 반환 (RenderGraph Pass에서 사용) */
	const TArray<FKawaiiRenderParticle>& GetCachedParticles() const
	{
		return CachedParticles;
	}

	//========================================
	// GPU 버퍼 접근 (Phase 2: GPU → GPU 렌더링)
	//========================================

	/** GPU Pooled Buffer 반환 (RDG 등록용) */
	TRefCountPtr<FRDGPooledBuffer> GetPooledParticleBuffer() const
	{
		return PooledParticleBuffer;
	}

	/** GPU 버퍼가 유효한지 확인 (Phase 2 경로용) */
	bool HasValidGPUBuffer() const
	{
		return PooledParticleBuffer.IsValid() && ParticleCount > 0 && bBufferReadyForRendering.load();
	}

	/** Mark buffer as ready for rendering (called after ExtractRenderDataPass completes) */
	void SetBufferReadyForRendering(bool bReady) { bBufferReadyForRendering = bReady; }

	/** Check if GPU mode is active (UpdateFromGPUBuffer was called instead of UpdateParticleData) */
	bool IsInGPUMode() const { return bIsInGPUMode.load(); }

private:
	//========================================
	// GPU 리소스
	//========================================

	/** Structured Buffer (GPU 메모리) */
	FBufferRHIRef ParticleBuffer;

	/** Shader Resource View (쉐이더 읽기용) */
	FShaderResourceViewRHIRef ParticleSRV;

	/** Unordered Access View (쉐이더 쓰기용 - Phase 2 GPU→GPU 복사) */
	FUnorderedAccessViewRHIRef ParticleUAV;

	/** Pooled Buffer for RDG registration (Phase 2 GPU→GPU 복사) */
	TRefCountPtr<FRDGPooledBuffer> PooledParticleBuffer;

	/** 현재 버퍼에 저장된 파티클 수 */
	int32 ParticleCount;

	/** 버퍼 최대 용량 (재할당 최소화) */
	int32 BufferCapacity;

	/** Buffer is ready for rendering (ExtractRenderDataPass has completed) */
	std::atomic<bool> bBufferReadyForRendering{false};

	/** Flag indicating GPU mode is active (UpdateFromGPUBuffer was called) */
	std::atomic<bool> bIsInGPUMode{false};

	//========================================
	// CPU 측 데이터 캐시 (게임 스레드)
	//========================================

	/** 
	 * 파티클 데이터 캐시 (게임 스레드에서 접근)
	 * RenderGraph Pass에서 Position 추출용으로 사용
	 */
	TArray<FKawaiiRenderParticle> CachedParticles;

	//========================================
	// 내부 헬퍼
	//========================================

	/** 버퍼 크기 조정 필요 여부 확인 */
	bool NeedsResize(int32 NewCount) const;

	/** 버퍼 재생성 (크기 변경 시) */
	void ResizeBuffer(FRHICommandListBase& RHICmdList, int32 NewCapacity);
};
