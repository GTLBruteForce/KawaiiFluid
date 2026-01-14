// Copyright KawaiiFluid Team. All Rights Reserved.

#include "GPU/FluidAnisotropyComputeShader.h"
#include "GPU/GPUFluidParticle.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "GlobalShader.h"

//=============================================================================
// Shader Implementation
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FFluidAnisotropyCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidAnisotropyCompute.usf",
	"MainCS", SF_Compute);

//=============================================================================
// Pass Builder Implementation
//=============================================================================

/**
 * @brief Add anisotropy calculation pass to RDG.
 * @param GraphBuilder RDG builder.
 * @param Params Anisotropy compute parameters (buffers and settings).
 */
void FFluidAnisotropyPassBuilder::AddAnisotropyPass(
	FRDGBuilder& GraphBuilder,
	const FAnisotropyComputeParams& Params)
{
	if (Params.ParticleCount <= 0 || !Params.PhysicsParticlesSRV)
	{
		return;
	}

	if (!Params.OutAxis1UAV || !Params.OutAxis2UAV || !Params.OutAxis3UAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Create permutation vector based on grid resolution preset
	FFluidAnisotropyCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(Params.GridResolutionPreset));
	TShaderMapRef<FFluidAnisotropyCS> ComputeShader(GlobalShaderMap, PermutationVector);

	FFluidAnisotropyCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFluidAnisotropyCS::FParameters>();

	// Create dummy buffers for optional inputs that are null
	// RDG requires all shader parameters to have valid buffers
	// Must use QueueBufferUpload to mark buffer as "produced"
	FRDGBufferSRVRef AttachmentsSRV = Params.AttachmentsSRV;
	// TODO(KHJ): Remove legacy CellCounts/ParticleIndices - bUseZOrderSorting is always true
	FRDGBufferSRVRef CellCountsSRV = Params.CellCountsSRV;
	FRDGBufferSRVRef ParticleIndicesSRV = Params.ParticleIndicesSRV;

	// Morton-sorted buffers (ParticleBuffer is already sorted when bUseZOrderSorting=true)
	FRDGBufferSRVRef CellStartSRV = Params.CellStartSRV;
	FRDGBufferSRVRef CellEndSRV = Params.CellEndSRV;

	if (!AttachmentsSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(
			sizeof(FGPUParticleAttachment), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyAttachmentBuffer"));
		FGPUParticleAttachment ZeroData;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(FGPUParticleAttachment));
		AttachmentsSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// TODO: Remove legacy dummy buffer creation - bUseZOrderSorting is always true
	if (!CellCountsSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCellCountBuffer"));
		uint32 ZeroData = 0;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(uint32));
		CellCountsSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	if (!ParticleIndicesSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyParticleIndicesBuffer"));
		uint32 ZeroData = 0;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(uint32));
		ParticleIndicesSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// Create dummy buffers for Morton-sorted inputs if not provided
	if (!CellStartSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCellStartBuffer"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &InvalidIndex, sizeof(uint32));
		CellStartSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	if (!CellEndSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCellEndBuffer"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &InvalidIndex, sizeof(uint32));
		CellEndSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// Input buffers
	PassParameters->InPhysicsParticles = Params.PhysicsParticlesSRV;
	PassParameters->InAttachments = AttachmentsSRV;

	// TODO: Remove legacy hash-based binding - bUseZOrderSorting is always true
	// Legacy hash-based spatial lookup
	PassParameters->CellCounts = CellCountsSRV;
	PassParameters->ParticleIndices = ParticleIndicesSRV;

	// Morton-sorted spatial lookup
	PassParameters->CellStart = CellStartSRV;
	PassParameters->CellEnd = CellEndSRV;

	// Output buffers
	PassParameters->OutAnisotropyAxis1 = Params.OutAxis1UAV;
	PassParameters->OutAnisotropyAxis2 = Params.OutAxis2UAV;
	PassParameters->OutAnisotropyAxis3 = Params.OutAxis3UAV;

	// Parameters
	PassParameters->ParticleCount = static_cast<uint32>(Params.ParticleCount);
	PassParameters->AnisotropyMode = static_cast<uint32>(Params.Mode);
	PassParameters->VelocityStretchFactor = Params.VelocityStretchFactor;
	PassParameters->AnisotropyScale = Params.AnisotropyScale;
	PassParameters->AnisotropyMin = Params.AnisotropyMin;
	PassParameters->AnisotropyMax = Params.AnisotropyMax;
	PassParameters->DensityWeight = Params.DensityWeight;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->CellSize = Params.CellSize;

	// TODO(KHJ): Remove bUseZOrderSorting - always 1, legacy path is dead code
	// Morton code params
	PassParameters->bUseZOrderSorting = Params.bUseZOrderSorting ? 1 : 0;
	PassParameters->MortonBoundsMin = Params.MortonBoundsMin;

	// Attached particle anisotropy params
	PassParameters->AttachedFlattenScale = Params.AttachedFlattenScale;
	PassParameters->AttachedStretchScale = Params.AttachedStretchScale;

	// Isolated particle handling params
	PassParameters->MinNeighborsForAnisotropy = Params.MinNeighborsForAnisotropy;
	PassParameters->bFadeIsolatedParticles = Params.bFadeIsolatedParticles ? 1 : 0;
	PassParameters->MinIsolatedScale = Params.MinIsolatedScale;
	PassParameters->bStretchIsolatedByVelocity = Params.bStretchIsolatedByVelocity ? 1 : 0;
	PassParameters->bFadeSlowIsolated = Params.bFadeSlowIsolated ? 1 : 0;
	PassParameters->IsolationFadeSpeed = Params.IsolationFadeSpeed;

	const int32 ThreadGroupSize = FFluidAnisotropyCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(Params.ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FluidAnisotropy(%d,Preset=%d,mode=%d)",
			Params.ParticleCount, static_cast<int32>(Params.GridResolutionPreset), static_cast<int32>(Params.Mode)),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

/**
 * @brief Create anisotropy output buffers.
 * @param GraphBuilder RDG builder.
 * @param ParticleCount Number of particles.
 * @param OutAxis1 Output axis 1 buffer (direction.xyz + scale.w).
 * @param OutAxis2 Output axis 2 buffer.
 * @param OutAxis3 Output axis 3 buffer.
 */
void FFluidAnisotropyPassBuilder::CreateAnisotropyBuffers(
	FRDGBuilder& GraphBuilder,
	int32 ParticleCount,
	FRDGBufferRef& OutAxis1,
	FRDGBufferRef& OutAxis2,
	FRDGBufferRef& OutAxis3)
{
	if (ParticleCount <= 0)
	{
		OutAxis1 = nullptr;
		OutAxis2 = nullptr;
		OutAxis3 = nullptr;
		return;
	}

	// Each axis is float4 (direction.xyz + scale.w)
	FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(
		sizeof(FVector4f), ParticleCount);

	OutAxis1 = GraphBuilder.CreateBuffer(BufferDesc, TEXT("FluidAnisotropyAxis1"));
	OutAxis2 = GraphBuilder.CreateBuffer(BufferDesc, TEXT("FluidAnisotropyAxis2"));
	OutAxis3 = GraphBuilder.CreateBuffer(BufferDesc, TEXT("FluidAnisotropyAxis3"));
}
