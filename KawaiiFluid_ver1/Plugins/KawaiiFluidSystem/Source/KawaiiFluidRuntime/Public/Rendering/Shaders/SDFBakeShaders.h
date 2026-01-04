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
	// Particle Data (Input) - Uses FKawaiiRenderParticle for GPU buffer compatibility
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FKawaiiRenderParticle>, RenderParticles)
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
END_SHADER_PARAMETER_STRUCT()

/**
 * @brief Compute shader for baking SDF volume
 *
 * Dispatches threads for each voxel in the 3D volume.
 * Each thread calculates the metaball SDF at its voxel position
 * by iterating through all particles once.
 *
 * Thread group size: 8x8x8 = 512 threads per group
 * For 64^3 volume: 8x8x8 dispatch = 512 groups = 262,144 total threads
 */
class FSDFBakeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSDFBakeCS);
	SHADER_USE_PARAMETER_STRUCT(FSDFBakeCS, FGlobalShader);

	using FParameters = FSDFBakeParameters;

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
