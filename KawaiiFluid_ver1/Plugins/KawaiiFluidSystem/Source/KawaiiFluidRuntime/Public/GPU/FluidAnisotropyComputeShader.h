// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "Core/FluidAnisotropy.h"

// Forward declaration
struct FGPUFluidParticle;
struct FGPUParticleAttachment;

//=============================================================================
// GPU Compute Parameters for Anisotropy Shader Dispatch
// Contains all buffer references and parameters needed for GPU anisotropy calculation.
//=============================================================================

struct KAWAIIFLUIDRUNTIME_API FAnisotropyComputeParams
{
	// Input buffers
	FRDGBufferSRVRef PhysicsParticlesSRV = nullptr;	// FGPUFluidParticle buffer
	FRDGBufferSRVRef AttachmentsSRV = nullptr;		// FGPUParticleAttachment buffer (for surface normal)
	FRDGBufferSRVRef CellCountsSRV = nullptr;		// Spatial hash cell counts
	FRDGBufferSRVRef ParticleIndicesSRV = nullptr;	// Spatial hash particle indices

	// Output buffers (float4: direction.xyz + scale.w)
	FRDGBufferUAVRef OutAxis1UAV = nullptr;
	FRDGBufferUAVRef OutAxis2UAV = nullptr;
	FRDGBufferUAVRef OutAxis3UAV = nullptr;

	// Parameters
	int32 ParticleCount = 0;
	EGPUAnisotropyMode Mode = EGPUAnisotropyMode::DensityBased;

	// Velocity-based params
	float VelocityStretchFactor = 0.01f;

	// Common params
	float AnisotropyScale = 1.0f;
	float AnisotropyMin = 0.2f;
	float AnisotropyMax = 2.5f;

	// Density-based params
	float DensityWeight = 0.5f;
	float SmoothingRadius = 10.0f;
	float CellSize = 10.0f;

	// Attached particle anisotropy params
	float AttachedFlattenScale = 0.3f;	// How much to flatten attached particles (0.3 = 30% of original height)
	float AttachedStretchScale = 1.5f;	// How much to stretch perpendicular to normal
};

// Constants (must match FluidSpatialHash.ush and FluidAnisotropyCompute.usf)
#define ANISOTROPY_SPATIAL_HASH_SIZE 65536
#define ANISOTROPY_MAX_PARTICLES_PER_CELL 16

//=============================================================================
// Anisotropy Compute Shader
// Calculates ellipsoid orientation and scale for each particle
// Based on NVIDIA FleX and Yu & Turk 2013 paper
//=============================================================================

class FFluidAnisotropyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidAnisotropyCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidAnisotropyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Physics particle buffer (FGPUFluidParticle)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, InPhysicsParticles)

		// Input: Attachment buffer (FGPUParticleAttachment) - for surface normal of attached particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUParticleAttachment>, InAttachments)

		// Spatial hash buffers (for neighbor search in DensityBased mode)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleIndices)

		// Output: Anisotropy SoA buffers (float4 = direction.xyz + scale.w)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutAnisotropyAxis1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutAnisotropyAxis2)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutAnisotropyAxis3)

		// Parameters
		SHADER_PARAMETER(uint32, ParticleCount)
		SHADER_PARAMETER(uint32, AnisotropyMode)  // 0=Velocity, 1=Density, 2=Hybrid
		SHADER_PARAMETER(float, VelocityStretchFactor)
		SHADER_PARAMETER(float, AnisotropyScale)
		SHADER_PARAMETER(float, AnisotropyMin)
		SHADER_PARAMETER(float, AnisotropyMax)
		SHADER_PARAMETER(float, DensityWeight)  // For Hybrid mode
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, CellSize)

		// Attached particle anisotropy params
		SHADER_PARAMETER(float, AttachedFlattenScale)  // How flat (0.3 = 30% height)
		SHADER_PARAMETER(float, AttachedStretchScale)  // Perpendicular stretch
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), ANISOTROPY_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), ANISOTROPY_MAX_PARTICLES_PER_CELL);
	}
};

//=============================================================================
// Anisotropy Pass Builder
// Utility class for adding anisotropy compute passes to RDG
//=============================================================================

class KAWAIIFLUIDRUNTIME_API FFluidAnisotropyPassBuilder
{
public:
	/**
	 * @brief Add anisotropy calculation pass to RDG.
	 * @param GraphBuilder RDG builder.
	 * @param Params Anisotropy compute parameters (buffers and settings).
	 */
	static void AddAnisotropyPass(
		FRDGBuilder& GraphBuilder,
		const FAnisotropyComputeParams& Params);

	/**
	 * @brief Create anisotropy output buffers.
	 * @param GraphBuilder RDG builder.
	 * @param ParticleCount Number of particles.
	 * @param OutAxis1 Output axis 1 buffer (direction.xyz + scale.w).
	 * @param OutAxis2 Output axis 2 buffer.
	 * @param OutAxis3 Output axis 3 buffer.
	 */
	static void CreateAnisotropyBuffers(
		FRDGBuilder& GraphBuilder,
		int32 ParticleCount,
		FRDGBufferRef& OutAxis1,
		FRDGBufferRef& OutAxis2,
		FRDGBufferRef& OutAxis3);
};
