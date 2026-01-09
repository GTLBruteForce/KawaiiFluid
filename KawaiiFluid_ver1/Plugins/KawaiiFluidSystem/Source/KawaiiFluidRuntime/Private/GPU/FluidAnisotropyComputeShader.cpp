// Copyright KawaiiFluid Team. All Rights Reserved.

#include "GPU/FluidAnisotropyComputeShader.h"
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
	TShaderMapRef<FFluidAnisotropyCS> ComputeShader(GlobalShaderMap);

	FFluidAnisotropyCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFluidAnisotropyCS::FParameters>();

	// Input buffers
	PassParameters->InPhysicsParticles = Params.PhysicsParticlesSRV;
	PassParameters->InAttachments = Params.AttachmentsSRV;
	PassParameters->CellCounts = Params.CellCountsSRV;
	PassParameters->ParticleIndices = Params.ParticleIndicesSRV;

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

	// Attached particle anisotropy params
	PassParameters->AttachedFlattenScale = Params.AttachedFlattenScale;
	PassParameters->AttachedStretchScale = Params.AttachedStretchScale;

	const int32 ThreadGroupSize = FFluidAnisotropyCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(Params.ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FluidAnisotropy(%d particles, mode=%d)",
			Params.ParticleCount, static_cast<int32>(Params.Mode)),
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
