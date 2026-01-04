// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shaders/FluidSpatialHashShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

//=============================================================================
// Shader Implementations
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FClearCellDataCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "ClearCellDataCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildSpatialHashSimpleCS, "/Plugin/KawaiiFluidSystem/Private/FluidSpatialHashBuild.usf", "BuildSpatialHashSimpleCS", SF_Compute);

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
