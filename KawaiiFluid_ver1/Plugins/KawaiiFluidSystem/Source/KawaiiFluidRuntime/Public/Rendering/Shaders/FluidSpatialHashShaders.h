// Copyright KawaiiFluid Team. All Rights Reserved.
//
// LEGACY: Traditional Spatial Hash Shaders
// =========================================
// These shaders implement traditional hash-table based spatial partitioning.
// They are kept for backward compatibility (bUseZOrderSorting=false path).
//
// The RECOMMENDED approach is Z-Order (Morton Code) sorting via FGPUZOrderSortManager,
// which provides cache-coherent memory access and no hash collisions.
//
// This file is used when:
// - FGPUZOrderSortManager is not available
// - Legacy fallback mode is explicitly enabled
//
// For new features, prefer using Z-Order sorting defined in GPUZOrderSortManager.h

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphDefinitions.h"

//=============================================================================
// LEGACY: Spatial Hash Constants (must match shader defines)
//=============================================================================

static constexpr uint32 SPATIAL_HASH_SIZE = 65536;          // 2^16 cells
static constexpr uint32 MAX_PARTICLES_PER_CELL = 16;
static constexpr uint32 SPATIAL_HASH_THREAD_GROUP_SIZE = 256;

//=============================================================================
// Clear Cell Data Compute Shader
//=============================================================================

class FClearCellDataCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FClearCellDataCS);
    SHADER_USE_PARAMETER_STRUCT(FClearCellDataCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounts)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
        OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), MAX_PARTICLES_PER_CELL);
    }
};

//=============================================================================
// Simple Version Shaders
//=============================================================================

class FBuildSpatialHashSimpleCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FBuildSpatialHashSimpleCS);
    SHADER_USE_PARAMETER_STRUCT(FBuildSpatialHashSimpleCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, ParticlePositions)
        SHADER_PARAMETER(int32, ParticleCount)
        SHADER_PARAMETER(float, ParticleRadius)
        SHADER_PARAMETER(float, CellSize)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounts)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleIndices)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
        OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), MAX_PARTICLES_PER_CELL);
    }
};

class FSortCellParticlesCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FSortCellParticlesCS);
    SHADER_USE_PARAMETER_STRUCT(FSortCellParticlesCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounts)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleIndices)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
        OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), MAX_PARTICLES_PER_CELL);
    }
};

//=============================================================================
// Multi-pass Version Shaders (Dynamic Array - No particle limit per cell)
//=============================================================================

class FClearCellDataMultipassCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FClearCellDataMultipassCS);
    SHADER_USE_PARAMETER_STRUCT(FClearCellDataMultipassCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUintVector2>, CellData)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounters)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("USE_MULTIPASS_BUILD"), 1);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
    }
};

class FCountParticlesPerCellCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FCountParticlesPerCellCS);
    SHADER_USE_PARAMETER_STRUCT(FCountParticlesPerCellCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, ParticlePositions)
        SHADER_PARAMETER(int32, ParticleCount)
        SHADER_PARAMETER(float, CellSize)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounters)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCellHashes)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("USE_MULTIPASS_BUILD"), 1);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
    }
};

class FPrefixSumCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FPrefixSumCS);
    SHADER_USE_PARAMETER_STRUCT(FPrefixSumCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounters)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, PrefixSumBuffer)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("USE_MULTIPASS_BUILD"), 1);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
    }
};

class FScatterParticlesCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FScatterParticlesCS);
    SHADER_USE_PARAMETER_STRUCT(FScatterParticlesCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCellHashes)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, PrefixSumBuffer)
        SHADER_PARAMETER(int32, ParticleCount)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounters)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleIndices)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("USE_MULTIPASS_BUILD"), 1);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
    }
};

class FFinalizeCellDataCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFinalizeCellDataCS);
    SHADER_USE_PARAMETER_STRUCT(FFinalizeCellDataCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, PrefixSumBuffer)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounters)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUintVector2>, CellData)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("USE_MULTIPASS_BUILD"), 1);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
    }
};

class FSortBucketParticlesCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FSortBucketParticlesCS);
    SHADER_USE_PARAMETER_STRUCT(FSortBucketParticlesCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUintVector2>, CellData)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleIndices)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("USE_MULTIPASS_BUILD"), 1);
        OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), SPATIAL_HASH_SIZE);
    }
};

//=============================================================================
// Spatial Hash GPU Resources (Simple Version)
//=============================================================================

/**
 * GPU resources for spatial hash acceleration structure
 * Simplified: CellCounts stores only count per cell (uint)
 * StartIndex is implicit: Hash * MAX_PARTICLES_PER_CELL
 */
struct FSpatialHashGPUResources
{
    // Cell counts: count per cell (startIndex is implicit)
    FRDGBufferRef CellCountsBuffer = nullptr;
    FRDGBufferSRVRef CellCountsSRV = nullptr;
    FRDGBufferUAVRef CellCountsUAV = nullptr;

    // Particle indices sorted by cell
    FRDGBufferRef ParticleIndicesBuffer = nullptr;
    FRDGBufferSRVRef ParticleIndicesSRV = nullptr;
    FRDGBufferUAVRef ParticleIndicesUAV = nullptr;

    // Configuration
    float CellSize = 0.0f;

    bool IsValid() const
    {
        return CellCountsBuffer != nullptr && ParticleIndicesBuffer != nullptr;
    }
};

//=============================================================================
// Spatial Hash GPU Resources (Multi-pass Version - Dynamic Array)
//=============================================================================

struct FSpatialHashMultipassResources
{
    // CellData: {startIndex, count} per cell
    FRDGBufferRef CellDataBuffer = nullptr;
    FRDGBufferSRVRef CellDataSRV = nullptr;
    FRDGBufferUAVRef CellDataUAV = nullptr;

    // Particle indices (dynamic size = ParticleCount)
    FRDGBufferRef ParticleIndicesBuffer = nullptr;
    FRDGBufferSRVRef ParticleIndicesSRV = nullptr;
    FRDGBufferUAVRef ParticleIndicesUAV = nullptr;

    // Temporary buffers for build
    FRDGBufferRef CellCountersBuffer = nullptr;
    FRDGBufferUAVRef CellCountersUAV = nullptr;

    FRDGBufferRef ParticleCellHashesBuffer = nullptr;
    FRDGBufferSRVRef ParticleCellHashesSRV = nullptr;
    FRDGBufferUAVRef ParticleCellHashesUAV = nullptr;

    FRDGBufferRef PrefixSumBuffer = nullptr;
    FRDGBufferSRVRef PrefixSumSRV = nullptr;
    FRDGBufferUAVRef PrefixSumUAV = nullptr;

    // Configuration
    float CellSize = 0.0f;
    int32 ParticleCount = 0;

    bool IsValid() const
    {
        return CellDataBuffer != nullptr && ParticleIndicesBuffer != nullptr;
    }

    void Reset()
    {
        CellDataBuffer = nullptr;
        CellDataSRV = nullptr;
        CellDataUAV = nullptr;
        ParticleIndicesBuffer = nullptr;
        ParticleIndicesSRV = nullptr;
        ParticleIndicesUAV = nullptr;
        CellCountersBuffer = nullptr;
        CellCountersUAV = nullptr;
        ParticleCellHashesBuffer = nullptr;
        ParticleCellHashesSRV = nullptr;
        ParticleCellHashesUAV = nullptr;
        PrefixSumBuffer = nullptr;
        PrefixSumSRV = nullptr;
        PrefixSumUAV = nullptr;
        CellSize = 0.0f;
        ParticleCount = 0;
    }
};

//=============================================================================
// Spatial Hash Builder
//=============================================================================

/**
 * Utility class for building spatial hash on GPU
 */
class KAWAIIFLUIDRUNTIME_API FSpatialHashBuilder
{
public:
    /**
     * Create GPU buffers for spatial hash
     * WARNING: Only call this if you will IMMEDIATELY add producer passes!
     * Orphan buffers (without producers) cause RDG crashes.
     */
    static FSpatialHashGPUResources CreateResources(
        FRDGBuilder& GraphBuilder,
        float CellSize);

    /**
     * Build spatial hash from particle positions
     * @return true if build succeeded, false if shaders not available
     * WARNING: Resources must already be created, and this function MUST succeed
     * to avoid orphan buffers.
     */
    static bool BuildHash(
        FRDGBuilder& GraphBuilder,
        FRDGBufferSRVRef ParticlePositionsSRV,
        int32 ParticleCount,
        float ParticleRadius,
        FSpatialHashGPUResources& Resources);

    /**
     * RECOMMENDED: Atomically create resources and build hash.
     * This is the safe way to use spatial hash - it validates ALL conditions
     * BEFORE creating any RDG resources, preventing orphan buffer crashes.
     *
     * @param OutResources Output - will be populated only on success
     * @return true if build succeeded, false if conditions not met (no resources created)
     */
    static bool CreateAndBuildHash(
        FRDGBuilder& GraphBuilder,
        FRDGBufferSRVRef ParticlePositionsSRV,
        int32 ParticleCount,
        float ParticleRadius,
        float CellSize,
        FSpatialHashGPUResources& OutResources);

    // Multi-pass version (dynamic array, no particle limit per cell)
    static bool CreateAndBuildHashMultipass(
        FRDGBuilder& GraphBuilder,
        FRDGBufferSRVRef ParticlePositionsSRV,
        int32 ParticleCount,
        float CellSize,
        FSpatialHashMultipassResources& OutResources);

private:
    static void ClearBuffers(
        FRDGBuilder& GraphBuilder,
        FSpatialHashGPUResources& Resources);

    static void SortCellParticles(
        FRDGBuilder& GraphBuilder,
        FSpatialHashGPUResources& Resources);
};
