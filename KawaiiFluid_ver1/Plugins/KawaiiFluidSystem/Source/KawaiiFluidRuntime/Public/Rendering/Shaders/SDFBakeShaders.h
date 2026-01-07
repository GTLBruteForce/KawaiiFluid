// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphDefinitions.h"
#include "Core/KawaiiRenderParticle.h"

/**
 * @brief Compute shader parameters for SDF volume baking
 *
 * Bakes metaball SDF from particle positions into a 3D volume texture.
 * This allows O(1) SDF lookup during ray marching instead of O(N) particle iteration.
 *
 * Now uses FKawaiiRenderParticle for GPU simulation compatibility (Phase 2).
 */
BEGIN_SHADER_PARAMETER_STRUCT(FSDFBakeParameters, )
	//========================================
	// Particle Data (Input) - SoA or AoS based on permutation
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, RenderPositions)  // SoA: 12B per particle
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FKawaiiRenderParticle>, RenderParticles)  // AoS: 32B per particle (legacy)
	SHADER_PARAMETER(int32, ParticleCount)
	SHADER_PARAMETER(float, ParticleRadius)
	SHADER_PARAMETER(float, SDFSmoothness)

	//========================================
	// Volume Parameters
	//========================================
	SHADER_PARAMETER(FVector3f, VolumeMin)
	SHADER_PARAMETER(FVector3f, VolumeMax)
	SHADER_PARAMETER(FIntVector, VolumeResolution)

	//========================================
	// Output SDF Volume (UAV)
	//========================================
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, SDFVolume)

	//========================================
	// Spatial Hash Parameters (Optimization)
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, CellData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SpatialHashParticleIndices)
	SHADER_PARAMETER(float, SpatialHashCellSize)
END_SHADER_PARAMETER_STRUCT()

/**
 * @brief Shader permutation for SoA buffer layout
 */
class FSDFBakeUseSoADim : SHADER_PERMUTATION_BOOL("USE_SOA_BUFFERS");

/**
 * @brief Shader permutation for Spatial Hash optimization
 */
class FSDFBakeUseSpatialHashDim : SHADER_PERMUTATION_BOOL("USE_SPATIAL_HASH");

/**
 * @brief Compute shader for baking SDF volume
 *
 * Dispatches threads for each voxel in the 3D volume.
 * Each thread calculates the metaball SDF at its voxel position
 * by iterating through all particles once.
 *
 * Thread group size: 8x8x8 = 512 threads per group
 * For 64^3 volume: 8x8x8 dispatch = 512 groups = 262,144 total threads
 *
 * Permutations:
 * - USE_SOA_BUFFERS=0: AoS (32B per particle, legacy)
 * - USE_SOA_BUFFERS=1: SoA (12B per particle, 62% bandwidth reduction)
 * - USE_SPATIAL_HASH=0: O(N) iteration (default)
 * - USE_SPATIAL_HASH=1: O(k) neighbor search (optimized)
 */
class FSDFBakeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSDFBakeCS);
	SHADER_USE_PARAMETER_STRUCT(FSDFBakeCS, FGlobalShader);

	using FParameters = FSDFBakeParameters;
	using FPermutationDomain = TShaderPermutationDomain<FSDFBakeUseSoADim, FSDFBakeUseSpatialHashDim>;

	static constexpr int32 ThreadGroupSize = 8;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), ThreadGroupSize);
	}
};

//=============================================================================
// SDF Bake with GPU Bounds - Reads bounds from buffer (same-frame, no readback)
//=============================================================================

/**
 * @brief Shader parameters for SDF baking with GPU-calculated bounds
 *
 * Reads bounds from a structured buffer instead of uniform parameters.
 * This eliminates 1-frame latency from CPU readback.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FSDFBakeWithGPUBoundsParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, RenderPositions)  // SoA: 12B per particle
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FKawaiiRenderParticle>, RenderParticles)  // AoS: 32B per particle (legacy)
	SHADER_PARAMETER(int32, ParticleCount)
	SHADER_PARAMETER(float, ParticleRadius)
	SHADER_PARAMETER(float, SDFSmoothness)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, BoundsBuffer)
	SHADER_PARAMETER(FIntVector, VolumeResolution)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, SDFVolume)

	//========================================
	// Spatial Hash Parameters (Optimization)
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, CellData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SpatialHashParticleIndices)
	SHADER_PARAMETER(float, SpatialHashCellSize)
END_SHADER_PARAMETER_STRUCT()

/**
 * @brief Compute shader for SDF baking that reads bounds from GPU buffer
 *
 * Same as FSDFBakeCS but reads VolumeMin/VolumeMax from a structured buffer
 * instead of uniform parameters. This allows using bounds calculated in the
 * same frame without CPU readback.
 *
 * Permutations:
 * - USE_SOA_BUFFERS=0: AoS (32B per particle, legacy)
 * - USE_SOA_BUFFERS=1: SoA (12B per particle, 62% bandwidth reduction)
 * - USE_SPATIAL_HASH=0: O(N) iteration (default)
 * - USE_SPATIAL_HASH=1: O(k) neighbor search (optimized)
 */
class FSDFBakeWithGPUBoundsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSDFBakeWithGPUBoundsCS);
	SHADER_USE_PARAMETER_STRUCT(FSDFBakeWithGPUBoundsCS, FGlobalShader);

	using FParameters = FSDFBakeWithGPUBoundsParameters;
	using FPermutationDomain = TShaderPermutationDomain<FSDFBakeUseSoADim, FSDFBakeUseSpatialHashDim>;

	static constexpr int32 ThreadGroupSize = 8;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), ThreadGroupSize);
	}
};
