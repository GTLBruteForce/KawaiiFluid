// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RHIResources.h"
#include "Core/KawaiiRenderParticle.h"

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

	//========================================
	// GPU 버퍼 접근 (렌더 스레드)
	//========================================

	/** Structured Buffer의 SRV (Shader Resource View) 반환 */
	FRHIShaderResourceView* GetParticleSRV() const { return ParticleSRV; }

	/** 현재 파티클 수 */
	int32 GetParticleCount() const { return ParticleCount; }

	/** 버퍼가 유효한지 확인 */
	bool IsValid() const { return ParticleBuffer.IsValid() && ParticleSRV.IsValid(); }

private:
	//========================================
	// GPU 리소스
	//========================================

	/** Structured Buffer (GPU 메모리) */
	FBufferRHIRef ParticleBuffer;

	/** Shader Resource View (쉐이더 접근용) */
	FShaderResourceViewRHIRef ParticleSRV;

	/** 현재 버퍼에 저장된 파티클 수 */
	int32 ParticleCount;

	/** 버퍼 최대 용량 (재할당 최소화) */
	int32 BufferCapacity;

	//========================================
	// 내부 헬퍼
	//========================================

	/** 버퍼 크기 조정 필요 여부 확인 */
	bool NeedsResize(int32 NewCount) const;

	/** 버퍼 재생성 (크기 변경 시) */
	void ResizeBuffer(FRHICommandListBase& RHICmdList, int32 NewCapacity);
};
