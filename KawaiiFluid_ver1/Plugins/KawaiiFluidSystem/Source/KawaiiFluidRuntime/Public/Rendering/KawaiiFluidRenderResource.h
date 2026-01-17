// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RHIResources.h"
#include "RenderGraphResources.h"
#include "Core/KawaiiRenderParticle.h"
#include <atomic>

// Forward declaration
class FGPUFluidSimulator;
class FRDGBuilder;

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

	/** 버퍼 최대 용량 반환 */
	int32 GetBufferCapacity() const { return BufferCapacity; }

	/** 버퍼가 유효한지 확인 */
	bool IsValid() const { return ParticleBuffer.IsValid() && ParticleSRV.IsValid(); }

	//========================================
	// GPU 버퍼 접근 (Phase 2: GPU → GPU 렌더링)
	//========================================

	/** GPU Pooled Buffer 반환 (RDG 등록용) - Legacy AoS */
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

	//========================================
	// SoA (Structure of Arrays) 버퍼 접근
	// - 메모리 대역폭 최적화: 32B/particle → 12B/particle (SDF용)
	//========================================

	/** Position 전용 SoA 버퍼 (float3 * N, 12B each) - SDF 핫패스 */
	TRefCountPtr<FRDGPooledBuffer> GetPooledPositionBuffer() const
	{
		return PooledPositionBuffer;
	}

	/** Velocity 전용 SoA 버퍼 (float3 * N, 12B each) - 모션블러용 */
	TRefCountPtr<FRDGPooledBuffer> GetPooledVelocityBuffer() const
	{
		return PooledVelocityBuffer;
	}

	/** SoA 버퍼가 유효한지 확인 */
	bool HasValidSoABuffers() const
	{
		return PooledPositionBuffer.IsValid() && ParticleCount > 0 && bBufferReadyForRendering.load();
	}

	//========================================
	// GPU 시뮬레이터 인터페이스
	//========================================

	/**
	 * GPU 시뮬레이터 참조 설정 (게임 스레드에서 호출)
	 * GPU 모드에서 렌더 스레드가 직접 시뮬레이터 버퍼에 접근할 수 있도록 함
	 * @param InSimulator GPU 시뮬레이터 참조 (nullptr이면 CPU 모드)
	 * @param InParticleCount GPU 파티클 수
	 * @param InParticleRadius 파티클 반경
	 */
	void SetGPUSimulatorReference(FGPUFluidSimulator* InSimulator, int32 InParticleCount, float InParticleRadius);

	/** GPU 시뮬레이터 참조 해제 (CPU 모드로 전환) */
	void ClearGPUSimulatorReference();

	/** 현재 GPU 시뮬레이터 참조 반환 */
	FGPUFluidSimulator* GetGPUSimulator() const { return CachedGPUSimulator; }

	/** GPU 시뮬레이터 모드인지 확인 */
	bool HasGPUSimulator() const { return CachedGPUSimulator != nullptr; }

	/**
	 * 통합 파티클 수 반환 (렌더 스레드에서 호출)
	 * GPU 모드: GPU 시뮬레이터의 파티클 수
	 * CPU 모드: 캐시된 파티클 수
	 */
	int32 GetUnifiedParticleCount() const;

	/**
	 * 통합 파티클 반경 반환
	 */
	float GetUnifiedParticleRadius() const { return CachedParticleRadius; }

	/**
	 * Physics 버퍼 SRV 반환 (렌더 스레드에서 호출)
	 * GPU 모드: GPUSimulator의 PersistentParticleBuffer (FGPUFluidParticle 형식)
	 * CPU 모드: nullptr (Physics 버퍼 없음)
	 * @param GraphBuilder RDG 빌더
	 * @return Physics 버퍼 SRV 또는 nullptr
	 */
	FRDGBufferSRVRef GetPhysicsBufferSRV(FRDGBuilder& GraphBuilder) const;

	/**
	 * Anisotropy Axis 버퍼들 반환 (GPU 모드에서만 유효)
	 * @param GraphBuilder RDG 빌더
	 * @param OutAxis1SRV Axis1 버퍼 SRV
	 * @param OutAxis2SRV Axis2 버퍼 SRV
	 * @param OutAxis3SRV Axis3 버퍼 SRV
	 * @return Anisotropy 버퍼가 유효하면 true
	 */
	bool GetAnisotropyBufferSRVs(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef& OutAxis1SRV,
		FRDGBufferSRVRef& OutAxis2SRV,
		FRDGBufferSRVRef& OutAxis3SRV) const;

	/** Anisotropy가 활성화되어 있는지 확인 */
	bool IsAnisotropyEnabled() const;

	//========================================
	// Bounds 데이터 (SDF 볼륨용)
	//========================================

	/**
	 * GPU Bounds 버퍼 설정 (렌더 스레드에서 호출)
	 * ViewExtension에서 ExtractRenderDataWithBoundsPass 후 설정
	 */
	void SetBoundsBuffer(TRefCountPtr<FRDGPooledBuffer> InBoundsBuffer);

	/** GPU Bounds 버퍼 반환 */
	TRefCountPtr<FRDGPooledBuffer> GetPooledBoundsBuffer() const { return PooledBoundsBuffer; }

	/** Bounds 버퍼가 유효한지 확인 */
	bool HasValidBoundsBuffer() const { return PooledBoundsBuffer.IsValid(); }

	/**
	 * FKawaiiRenderParticle 버퍼 설정 (렌더 스레드에서 호출)
	 * ViewExtension에서 ExtractRenderDataPass 후 설정
	 */
	void SetRenderParticleBuffer(TRefCountPtr<FRDGPooledBuffer> InBuffer);

	/** FKawaiiRenderParticle 버퍼 반환 (SDF iteration용) */
	TRefCountPtr<FRDGPooledBuffer> GetPooledRenderParticleBuffer() const { return PooledRenderParticleBuffer; }

	/** Bounds 버퍼 포인터 반환 (QueueBufferExtraction용) */
	TRefCountPtr<FRDGPooledBuffer>* GetPooledBoundsBufferPtr() { return &PooledBoundsBuffer; }

	/** RenderParticle 버퍼 포인터 반환 (QueueBufferExtraction용) */
	TRefCountPtr<FRDGPooledBuffer>* GetPooledRenderParticleBufferPtr() { return &PooledRenderParticleBuffer; }

	/** Position 버퍼 포인터 반환 (QueueBufferExtraction용) */
	TRefCountPtr<FRDGPooledBuffer>* GetPooledPositionBufferPtr() { return &PooledPositionBuffer; }

	/** Velocity 버퍼 포인터 반환 (QueueBufferExtraction용) */
	TRefCountPtr<FRDGPooledBuffer>* GetPooledVelocityBufferPtr() { return &PooledVelocityBuffer; }

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

	/** Pooled Buffer for RDG registration (Phase 2 GPU→GPU 복사) - Legacy AoS */
	TRefCountPtr<FRDGPooledBuffer> PooledParticleBuffer;

	//========================================
	// SoA (Structure of Arrays) 버퍼
	//========================================

	/** Position 전용 버퍼 (float3 * N, 12B each) - SDF 핫패스 */
	TRefCountPtr<FRDGPooledBuffer> PooledPositionBuffer;

	/** Velocity 전용 버퍼 (float3 * N, 12B each) - 모션블러용 */
	TRefCountPtr<FRDGPooledBuffer> PooledVelocityBuffer;

	/** Bounds 버퍼 (float3 * 2: Min, Max) - SDF 볼륨 범위 */
	TRefCountPtr<FRDGPooledBuffer> PooledBoundsBuffer;

	/** FKawaiiRenderParticle 버퍼 (SDF iteration용) */
	TRefCountPtr<FRDGPooledBuffer> PooledRenderParticleBuffer;

	/** 현재 버퍼에 저장된 파티클 수 */
	int32 ParticleCount;

	/** 버퍼 최대 용량 (재할당 최소화) */
	int32 BufferCapacity;

	/** Buffer is ready for rendering (ExtractRenderDataPass has completed) */
	std::atomic<bool> bBufferReadyForRendering{false};

	//========================================
	// GPU 시뮬레이터 참조
	//========================================

	/** GPU 시뮬레이터 참조 (렌더 스레드에서 직접 버퍼 접근용) */
	std::atomic<FGPUFluidSimulator*> CachedGPUSimulator{nullptr};

	/** 캐시된 파티클 수 */
	std::atomic<int32> CachedGPUParticleCount{0};

	/** 캐시된 파티클 반경 */
	std::atomic<float> CachedParticleRadius{10.0f};

	//========================================
	// 내부 헬퍼
	//========================================

	/** 버퍼 크기 조정 필요 여부 확인 */
	bool NeedsResize(int32 NewCount) const;

	/** 버퍼 재생성 (크기 변경 시) */
	void ResizeBuffer(FRHICommandListBase& RHICmdList, int32 NewCapacity);
};
