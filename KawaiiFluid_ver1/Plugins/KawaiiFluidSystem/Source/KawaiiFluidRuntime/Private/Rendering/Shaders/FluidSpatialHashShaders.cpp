// Copyright KawaiiFluid Team. All Rights Reserved.
// LEGACY: Traditional Spatial Hash Shader Implementations
// See FluidSpatialHashShaders.h for details on legacy vs. Z-Order approach.

#include "Rendering/Shaders/FluidSpatialHashShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

//=============================================================================
// Shader Implementations
//=============================================================================

// Simple Version Shaders
IMPLEMENT_GLOBAL_SHADER(FClearCellDataCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "ClearCellDataCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildSpatialHashSimpleCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "BuildSpatialHashSimpleCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSortCellParticlesCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "SortCellParticlesCS", SF_Compute);

// Multi-pass Version Shaders
IMPLEMENT_GLOBAL_SHADER(FClearCellDataMultipassCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "ClearCellDataMultipassCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCountParticlesPerCellCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "CountParticlesPerCellCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FPrefixSumCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "PrefixSumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FScatterParticlesCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "ScatterParticlesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFinalizeCellDataCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "FinalizeCellDataCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSortBucketParticlesCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "SortBucketParticlesCS", SF_Compute);

//=============================================================================
// FSpatialHashBuilder Implementation
//=============================================================================

FSpatialHashGPUResources FSpatialHashBuilder::CreateResources(
    FRDGBuilder& GraphBuilder,
    float CellSize)
{
    FSpatialHashGPUResources Resources;
    Resources.CellSize = CellSize;

    // Cell counts buffer: uint per cell (count only, startIndex is implicit)
    {
        FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SPATIAL_HASH_SIZE);
        Desc.Usage = EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
        Resources.CellCountsBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("SpatialHash.CellCounts"));
        Resources.CellCountsSRV = GraphBuilder.CreateSRV(Resources.CellCountsBuffer);
        Resources.CellCountsUAV = GraphBuilder.CreateUAV(Resources.CellCountsBuffer);
    }

    // Particle indices buffer: fixed layout (HASH_SIZE * MAX_PER_CELL)
    {
        uint32 TotalSlots = SPATIAL_HASH_SIZE * MAX_PARTICLES_PER_CELL;
        FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TotalSlots);
        Desc.Usage = EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
        Resources.ParticleIndicesBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("SpatialHash.ParticleIndices"));
        Resources.ParticleIndicesSRV = GraphBuilder.CreateSRV(Resources.ParticleIndicesBuffer);
        Resources.ParticleIndicesUAV = GraphBuilder.CreateUAV(Resources.ParticleIndicesBuffer);
    }

    return Resources;
}

void FSpatialHashBuilder::ClearBuffers(
    FRDGBuilder& GraphBuilder,
    FSpatialHashGPUResources& Resources)
{
    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
    if (!ShaderMap)
    {
        UE_LOG(LogTemp, Warning, TEXT("FSpatialHashBuilder::ClearBuffers - ShaderMap is null"));
        return;
    }

    TShaderMapRef<FClearCellDataCS> ComputeShader(ShaderMap);
    if (!ComputeShader.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("FSpatialHashBuilder::ClearBuffers - FClearCellDataCS shader is not valid"));
        return;
    }

    FClearCellDataCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearCellDataCS::FParameters>();
    PassParameters->CellCounts = Resources.CellCountsUAV;

    uint32 NumGroups = FMath::DivideAndRoundUp(SPATIAL_HASH_SIZE, SPATIAL_HASH_THREAD_GROUP_SIZE);

    GraphBuilder.AddPass(
        RDG_EVENT_NAME("SpatialHash::Clear"),
        PassParameters,
        ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
        [ComputeShader, PassParameters, NumGroups](FRHIComputeCommandList& RHICmdList)
        {
            FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, FIntVector(NumGroups, 1, 1));
        });
}

bool FSpatialHashBuilder::BuildHash(
    FRDGBuilder& GraphBuilder,
    FRDGBufferSRVRef ParticlePositionsSRV,
    int32 ParticleCount,
    float ParticleRadius,
    FSpatialHashGPUResources& Resources)
{
    // WARNING: Resources must already be created. If this function fails,
    // the caller is responsible for handling orphan buffers.
    // Use CreateAndBuildHash() for safe atomic operation.

    if (!Resources.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("FSpatialHashBuilder::BuildHash - Resources not valid"));
        return false;
    }

    if (ParticleCount <= 0 || !ParticlePositionsSRV)
    {
        UE_LOG(LogTemp, Warning, TEXT("FSpatialHashBuilder::BuildHash - Invalid inputs (ParticleCount: %d)"), ParticleCount);
        return false;
    }

    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
    if (!ShaderMap)
    {
        UE_LOG(LogTemp, Warning, TEXT("FSpatialHashBuilder::BuildHash - ShaderMap is null"));
        return false;
    }

    TShaderMapRef<FBuildSpatialHashSimpleCS> BuildShader(ShaderMap);
    TShaderMapRef<FClearCellDataCS> ClearShader(ShaderMap);

    if (!BuildShader.IsValid() || !ClearShader.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("FSpatialHashBuilder::BuildHash - Shaders not valid"));
        return false;
    }

    RDG_EVENT_SCOPE(GraphBuilder, "SpatialHash::Build (Particles: %d)", ParticleCount);

    // Step 1: Clear buffers (CellCounts only)
    {
        FClearCellDataCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellDataCS::FParameters>();
        ClearParams->CellCounts = Resources.CellCountsUAV;

        uint32 NumGroups = FMath::DivideAndRoundUp(SPATIAL_HASH_SIZE, SPATIAL_HASH_THREAD_GROUP_SIZE);

        GraphBuilder.AddPass(
            RDG_EVENT_NAME("SpatialHash::Clear"),
            ClearParams,
            ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
            [ClearShader, ClearParams, NumGroups](FRHIComputeCommandList& RHICmdList)
            {
                FComputeShaderUtils::Dispatch(RHICmdList, ClearShader, *ClearParams, FIntVector(NumGroups, 1, 1));
            });
    }

    // Step 2: Build hash
    {
        FBuildSpatialHashSimpleCS::FParameters* PassParameters =
            GraphBuilder.AllocParameters<FBuildSpatialHashSimpleCS::FParameters>();

        PassParameters->ParticlePositions = ParticlePositionsSRV;
        PassParameters->ParticleCount = ParticleCount;
        PassParameters->ParticleRadius = ParticleRadius;
        PassParameters->CellSize = Resources.CellSize;
        PassParameters->CellCounts = Resources.CellCountsUAV;
        PassParameters->ParticleIndices = Resources.ParticleIndicesUAV;

        uint32 NumGroups = FMath::DivideAndRoundUp((uint32)ParticleCount, SPATIAL_HASH_THREAD_GROUP_SIZE);

        GraphBuilder.AddPass(
            RDG_EVENT_NAME("SpatialHash::BuildSimple"),
            PassParameters,
            ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
            [BuildShader, PassParameters, NumGroups](FRHIComputeCommandList& RHICmdList)
            {
                FComputeShaderUtils::Dispatch(RHICmdList, BuildShader, *PassParameters, FIntVector(NumGroups, 1, 1));
            });
    }

    return true;
}

bool FSpatialHashBuilder::CreateAndBuildHash(
    FRDGBuilder& GraphBuilder,
    FRDGBufferSRVRef ParticlePositionsSRV,
    int32 ParticleCount,
    float ParticleRadius,
    float CellSize,
    FSpatialHashGPUResources& OutResources)
{
    
    OutResources = FSpatialHashGPUResources();

    if (ParticleCount <= 0 || !ParticlePositionsSRV)
    {
        return false;
    }

    if (CellSize <= 0.0f)
    {
        return false;
    }

    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
    if (!ShaderMap)
    {
        return false;
    }

    TShaderMapRef<FBuildSpatialHashSimpleCS> BuildShader(ShaderMap);
    if (!BuildShader.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("CreateAndBuildHash - BuildShader not valid"));
        return false;
    }

    OutResources.CellSize = CellSize;

    // Create CellCounts buffer with initial data (implicit upload producer)
    // Each cell: uint count = 0 (startIndex is implicit: cellIdx * MAX_PER_CELL)
    {
        TArray<uint32> InitialCellCounts;
        InitialCellCounts.SetNumZeroed(SPATIAL_HASH_SIZE);  // All counts start at 0

        OutResources.CellCountsBuffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("SpatialHash.CellCounts"),
            sizeof(uint32),
            SPATIAL_HASH_SIZE,
            InitialCellCounts.GetData(),
            InitialCellCounts.Num() * sizeof(uint32));

        OutResources.CellCountsSRV = GraphBuilder.CreateSRV(OutResources.CellCountsBuffer);
        OutResources.CellCountsUAV = GraphBuilder.CreateUAV(OutResources.CellCountsBuffer);
    }

    // Create ParticleIndices buffer with zero initial data (implicit upload producer)
    {
        uint32 TotalSlots = SPATIAL_HASH_SIZE * MAX_PARTICLES_PER_CELL;
        TArray<uint32> InitialIndices;
        InitialIndices.SetNumZeroed(TotalSlots);

        OutResources.ParticleIndicesBuffer = CreateStructuredBuffer(
            GraphBuilder,
            TEXT("SpatialHash.ParticleIndices"),
            sizeof(uint32),
            TotalSlots,
            InitialIndices.GetData(),
            InitialIndices.Num() * sizeof(uint32));

        OutResources.ParticleIndicesSRV = GraphBuilder.CreateSRV(OutResources.ParticleIndicesBuffer);
        OutResources.ParticleIndicesUAV = GraphBuilder.CreateUAV(OutResources.ParticleIndicesBuffer);
    }

    // Build pass - writes particle indices into the hash grid
    {
        FBuildSpatialHashSimpleCS::FParameters* PassParameters =
            GraphBuilder.AllocParameters<FBuildSpatialHashSimpleCS::FParameters>();

        PassParameters->ParticlePositions = ParticlePositionsSRV;
        PassParameters->ParticleCount = ParticleCount;
        PassParameters->ParticleRadius = ParticleRadius;
        PassParameters->CellSize = CellSize;
        PassParameters->CellCounts = OutResources.CellCountsUAV;
        PassParameters->ParticleIndices = OutResources.ParticleIndicesUAV;

        uint32 NumGroups = FMath::DivideAndRoundUp((uint32)ParticleCount, SPATIAL_HASH_THREAD_GROUP_SIZE);

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("SpatialHash::Build"),
            BuildShader,
            PassParameters,
            FIntVector(NumGroups, 1, 1));
    }

    //UE_LOG(LogTemp, Log, TEXT("CreateAndBuildHash - Success (Particles: %d, CellSize: %.2f)"), ParticleCount, CellSize);

    return true;
}

//=============================================================================
// Multi-pass Spatial Hash Build (Dynamic Array - No particle limit per cell)
//=============================================================================

bool FSpatialHashBuilder::CreateAndBuildHashMultipass(
    FRDGBuilder& GraphBuilder,
    FRDGBufferSRVRef ParticlePositionsSRV,
    int32 ParticleCount,
    float CellSize,
    FSpatialHashMultipassResources& OutResources)
{
    OutResources.Reset();

    if (ParticleCount <= 0 || !ParticlePositionsSRV)
    {
        return false;
    }

    if (CellSize <= 0.0f)
    {
        return false;
    }

    FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
    if (!ShaderMap)
    {
        return false;
    }

    // Get all shaders
    TShaderMapRef<FClearCellDataMultipassCS> ClearShader(ShaderMap);
    TShaderMapRef<FCountParticlesPerCellCS> CountShader(ShaderMap);
    TShaderMapRef<FPrefixSumCS> PrefixSumShader(ShaderMap);
    TShaderMapRef<FScatterParticlesCS> ScatterShader(ShaderMap);
    TShaderMapRef<FFinalizeCellDataCS> FinalizeShader(ShaderMap);
    TShaderMapRef<FSortBucketParticlesCS> SortShader(ShaderMap);

    if (!ClearShader.IsValid() || !CountShader.IsValid() || !PrefixSumShader.IsValid() ||
        !ScatterShader.IsValid() || !FinalizeShader.IsValid() || !SortShader.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("CreateAndBuildHashMultipass - One or more shaders not valid"));
        return false;
    }

    OutResources.CellSize = CellSize;
    OutResources.ParticleCount = ParticleCount;

    RDG_EVENT_SCOPE(GraphBuilder, "SpatialHash::BuildMultipass (Particles: %d)", ParticleCount);

    //=========================================================================
    // Create Buffers
    //=========================================================================

    // CellData: {startIndex, count} per cell
    {
        FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), SPATIAL_HASH_SIZE);
        Desc.Usage |= EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
        OutResources.CellDataBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("SpatialHash.CellData"));
        OutResources.CellDataSRV = GraphBuilder.CreateSRV(OutResources.CellDataBuffer);
        OutResources.CellDataUAV = GraphBuilder.CreateUAV(OutResources.CellDataBuffer);
    }

    // ParticleIndices: Dynamic size = ParticleCount
    {
        FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
        Desc.Usage |= EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
        OutResources.ParticleIndicesBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("SpatialHash.ParticleIndices"));
        OutResources.ParticleIndicesSRV = GraphBuilder.CreateSRV(OutResources.ParticleIndicesBuffer);
        OutResources.ParticleIndicesUAV = GraphBuilder.CreateUAV(OutResources.ParticleIndicesBuffer);
    }

    // CellCounters: Atomic counter per cell (temporary)
    {
        FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SPATIAL_HASH_SIZE);
        Desc.Usage |= EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
        OutResources.CellCountersBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("SpatialHash.CellCounters"));
        OutResources.CellCountersUAV = GraphBuilder.CreateUAV(OutResources.CellCountersBuffer);
    }

    // ParticleCellHashes: Hash for each particle
    {
        FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
        Desc.Usage |= EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
        OutResources.ParticleCellHashesBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("SpatialHash.ParticleCellHashes"));
        OutResources.ParticleCellHashesSRV = GraphBuilder.CreateSRV(OutResources.ParticleCellHashesBuffer);
        OutResources.ParticleCellHashesUAV = GraphBuilder.CreateUAV(OutResources.ParticleCellHashesBuffer);
    }

    // PrefixSumBuffer: Prefix sum workspace
    {
        FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SPATIAL_HASH_SIZE);
        Desc.Usage |= EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource;
        OutResources.PrefixSumBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("SpatialHash.PrefixSum"));
        OutResources.PrefixSumSRV = GraphBuilder.CreateSRV(OutResources.PrefixSumBuffer);
        OutResources.PrefixSumUAV = GraphBuilder.CreateUAV(OutResources.PrefixSumBuffer);
    }

    uint32 NumCellGroups = FMath::DivideAndRoundUp(SPATIAL_HASH_SIZE, SPATIAL_HASH_THREAD_GROUP_SIZE);
    uint32 NumParticleGroups = FMath::DivideAndRoundUp((uint32)ParticleCount, SPATIAL_HASH_THREAD_GROUP_SIZE);

    //=========================================================================
    // Pass 1: Clear CellData and CellCounters
    //=========================================================================
    {
        FClearCellDataMultipassCS::FParameters* ClearParams =
            GraphBuilder.AllocParameters<FClearCellDataMultipassCS::FParameters>();
        ClearParams->CellData = OutResources.CellDataUAV;
        ClearParams->CellCounters = OutResources.CellCountersUAV;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("SpatialHash::Clear"),
            ClearShader,
            ClearParams,
            FIntVector(NumCellGroups, 1, 1));
    }

    //=========================================================================
    // Pass 2: Count particles per cell
    //=========================================================================
    {
        FCountParticlesPerCellCS::FParameters* CountParams =
            GraphBuilder.AllocParameters<FCountParticlesPerCellCS::FParameters>();
        CountParams->ParticlePositions = ParticlePositionsSRV;
        CountParams->ParticleCount = ParticleCount;
        CountParams->CellSize = CellSize;
        CountParams->CellCounters = OutResources.CellCountersUAV;
        CountParams->ParticleCellHashes = OutResources.ParticleCellHashesUAV;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("SpatialHash::CountParticles"),
            CountShader,
            CountParams,
            FIntVector(NumParticleGroups, 1, 1));
    }

    //=========================================================================
    // Pass 3: Prefix sum (single-threaded for correctness)
    //=========================================================================
    {
        FPrefixSumCS::FParameters* PrefixParams =
            GraphBuilder.AllocParameters<FPrefixSumCS::FParameters>();
        PrefixParams->CellCounters = OutResources.CellCountersUAV;
        PrefixParams->PrefixSumBuffer = OutResources.PrefixSumUAV;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("SpatialHash::PrefixSum"),
            PrefixSumShader,
            PrefixParams,
            FIntVector(1, 1, 1));
    }

    //=========================================================================
    // Pass 3.5: Clear CellCounters before Scatter (reuse as local index)
    //=========================================================================
    AddClearUAVPass(GraphBuilder, OutResources.CellCountersUAV, 0u);

    //=========================================================================
    // Pass 4: Scatter particles into cells
    //=========================================================================
    {
        FScatterParticlesCS::FParameters* ScatterParams =
            GraphBuilder.AllocParameters<FScatterParticlesCS::FParameters>();
        ScatterParams->ParticleCellHashes = OutResources.ParticleCellHashesUAV;
        ScatterParams->PrefixSumBuffer = OutResources.PrefixSumUAV;
        ScatterParams->ParticleCount = ParticleCount;
        ScatterParams->CellCounters = OutResources.CellCountersUAV;
        ScatterParams->ParticleIndices = OutResources.ParticleIndicesUAV;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("SpatialHash::Scatter"),
            ScatterShader,
            ScatterParams,
            FIntVector(NumParticleGroups, 1, 1));
    }

    //=========================================================================
    // Pass 5: Finalize cell data (set start indices)
    //=========================================================================
    {
        FFinalizeCellDataCS::FParameters* FinalizeParams =
            GraphBuilder.AllocParameters<FFinalizeCellDataCS::FParameters>();
        FinalizeParams->PrefixSumBuffer = OutResources.PrefixSumUAV;
        FinalizeParams->CellCounters = OutResources.CellCountersUAV;
        FinalizeParams->CellData = OutResources.CellDataUAV;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("SpatialHash::Finalize"),
            FinalizeShader,
            FinalizeParams,
            FIntVector(NumCellGroups, 1, 1));
    }

    //=========================================================================
    // Pass 6: Sort particles within each bucket (deterministic order)
    //=========================================================================
    {
        FSortBucketParticlesCS::FParameters* SortParams =
            GraphBuilder.AllocParameters<FSortBucketParticlesCS::FParameters>();
        SortParams->CellData = OutResources.CellDataUAV;
        SortParams->ParticleIndices = OutResources.ParticleIndicesUAV;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("SpatialHash::Sort"),
            SortShader,
            SortParams,
            FIntVector(NumCellGroups, 1, 1));
    }

    return true;
}
