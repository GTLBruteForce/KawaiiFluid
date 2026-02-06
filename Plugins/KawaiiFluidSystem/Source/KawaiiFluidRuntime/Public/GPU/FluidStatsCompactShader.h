// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "GPU/GPUFluidParticle.h"

//=============================================================================
// Stats Compact Compute Shader - Basic Mode (32 bytes output)
// Extracts Position, ParticleID, SourceID, NeighborCount from full particle buffer
// Used when ISM rendering and Shadow system are disabled
//=============================================================================

class FCompactStatsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompactStatsCS);
	SHADER_USE_PARAMETER_STRUCT(FCompactStatsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, InParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FCompactParticleStats>, OutCompactStats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleCountBuffer)
	END_SHADER_PARAMETER_STRUCT()

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

//=============================================================================
// Stats Compact Compute Shader - Extended Mode (48 bytes output)
// Extracts Position, ParticleID, Velocity, SourceID, NeighborCount
// Used when ISM rendering or Shadow system is enabled
//=============================================================================

class FCompactStatsExCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompactStatsExCS);
	SHADER_USE_PARAMETER_STRUCT(FCompactStatsExCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, InParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FCompactParticleStatsEx>, OutCompactStatsEx)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleCountBuffer)
	END_SHADER_PARAMETER_STRUCT()

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
