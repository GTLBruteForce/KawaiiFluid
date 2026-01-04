// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphDefinitions.h"
#include "Core/KawaiiRenderParticle.h"

/**
 * @brief Compute shader parameters for GPU-based bounds calculation
 *
 * Uses parallel reduction to calculate min/max bounds of all particles.
 * Single group of 256 threads with grid-stride loop handles up to ~100k particles.
 *
 * Output: float3[2] buffer where [0] = Min, [1] = Max (already expanded by radius + margin)
 */
BEGIN_SHADER_PARAMETER_STRUCT(FBoundsReductionParameters, )
	//========================================
	// Input: Particle Buffer
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FKawaiiRenderParticle>, RenderParticles)
	SHADER_PARAMETER(uint32, ParticleCount)
	SHADER_PARAMETER(float, ParticleRadius)
	SHADER_PARAMETER(float, BoundsMargin)

	//========================================
	// Output: Min/Max Bounds
	//========================================
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector3f>, OutputBounds)
END_SHADER_PARAMETER_STRUCT()

/**
 * @brief Compute shader for calculating particle bounds via parallel reduction
 *
 * Dispatches a single group of 256 threads.
 * Each thread processes multiple particles using grid-stride loop.
 * Uses shared memory for parallel reduction within the group.
 */
class FBoundsReductionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoundsReductionCS);
	SHADER_USE_PARAMETER_STRUCT(FBoundsReductionCS, FGlobalShader);

	using FParameters = FBoundsReductionParameters;

	static constexpr int32 ThreadGroupSize = 256;

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
