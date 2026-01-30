// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// FGPUZOrderSortManager - Z-Order Morton Code Sorting System

#include "GPU/Managers/GPUZOrderSortManager.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/GPUBoundaryAttachment.h"  // For FGPUBoneDeltaAttachment
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUZOrderSort, Log, All);
DEFINE_LOG_CATEGORY(LogGPUZOrderSort);

//=============================================================================
// Constructor / Destructor
//=============================================================================

FGPUZOrderSortManager::FGPUZOrderSortManager()
	: bIsInitialized(false)
	, ZOrderBufferParticleCapacity(0)
{
}

FGPUZOrderSortManager::~FGPUZOrderSortManager()
{
	Release();
}

//=============================================================================
// Lifecycle
//=============================================================================

void FGPUZOrderSortManager::Initialize()
{
	bIsInitialized = true;
	UE_LOG(LogGPUZOrderSort, Log, TEXT("GPUZOrderSortManager initialized"));
}

void FGPUZOrderSortManager::Release()
{
	ZOrderBufferParticleCapacity = 0;
	bIsInitialized = false;
	UE_LOG(LogGPUZOrderSort, Log, TEXT("GPUZOrderSortManager released"));
}

//=============================================================================
// Z-Order (Morton Code) Sorting Pipeline
//=============================================================================

FRDGBufferRef FGPUZOrderSortManager::ExecuteZOrderSortingPipeline(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef InParticleBuffer,
	FRDGBufferUAVRef& OutCellStartUAV,
	FRDGBufferSRVRef& OutCellStartSRV,
	FRDGBufferUAVRef& OutCellEndUAV,
	FRDGBufferSRVRef& OutCellEndSRV,
	FRDGBufferRef& OutCellStartBuffer,
	FRDGBufferRef& OutCellEndBuffer,
	int32 CurrentParticleCount,
	const FGPUFluidSimulationParams& Params,
	FRDGBufferRef InAttachmentBuffer,
	FRDGBufferRef* OutSortedAttachmentBuffer)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid::ZOrderSorting");

	if (CurrentParticleCount <= 0)
	{
		return InParticleBuffer;
	}

	// Get grid parameters from current preset
	const int32 GridAxisBits = GridResolutionPresetHelper::GetAxisBits(GridResolutionPreset);
	const int32 GridSize = GridResolutionPresetHelper::GetGridResolution(GridResolutionPreset);
	const int32 CellCount = GridResolutionPresetHelper::GetMaxCells(GridResolutionPreset);

	const int32 NumBlocks = FMath::DivideAndRoundUp(CurrentParticleCount, 256);

	//=========================================================================
	// Step 1: Create Morton code and index buffers
	//=========================================================================
	FRDGBufferRef MortonCodesRDG;
	FRDGBufferRef MortonCodesTempRDG;
	FRDGBufferRef SortIndicesRDG;
	FRDGBufferRef SortIndicesTempRDG;
	FRDGBufferRef CellStartRDG;
	FRDGBufferRef CellEndRDG;

	// Morton codes and indices
	{
		FRDGBufferDesc MortonDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CurrentParticleCount);
		MortonCodesRDG = GraphBuilder.CreateBuffer(MortonDesc, TEXT("GPUFluid.MortonCodes"));
		MortonCodesTempRDG = GraphBuilder.CreateBuffer(MortonDesc, TEXT("GPUFluid.MortonCodesTemp"));
		SortIndicesRDG = GraphBuilder.CreateBuffer(MortonDesc, TEXT("GPUFluid.SortIndices"));
		SortIndicesTempRDG = GraphBuilder.CreateBuffer(MortonDesc, TEXT("GPUFluid.SortIndicesTemp"));
	}

	// Cell Start/End
	{
		FRDGBufferDesc CellDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CellCount);
		CellStartRDG = GraphBuilder.CreateBuffer(CellDesc, TEXT("GPUFluid.CellStart"));
		CellEndRDG = GraphBuilder.CreateBuffer(CellDesc, TEXT("GPUFluid.CellEnd"));
	}

	//=========================================================================
	// Step 2: Compute Morton codes
	//=========================================================================
	{
		FRDGBufferSRVRef ParticlesSRV = GraphBuilder.CreateSRV(InParticleBuffer);
		FRDGBufferUAVRef MortonCodesUAV = GraphBuilder.CreateUAV(MortonCodesRDG);
		FRDGBufferUAVRef IndicesUAV = GraphBuilder.CreateUAV(SortIndicesRDG);

		AddComputeMortonCodesPass(GraphBuilder, ParticlesSRV, MortonCodesUAV, IndicesUAV, CurrentParticleCount, Params);
	}

	//=========================================================================
	// Step 3: Radix Sort
	//=========================================================================
	AddRadixSortPasses(GraphBuilder, MortonCodesRDG, SortIndicesRDG, CurrentParticleCount);

	//=========================================================================
	// Step 4: Reorder particle data based on sorted indices
	// Also reorder BoneDeltaAttachment buffer if provided (keeps indices synchronized)
	//=========================================================================
	FRDGBufferRef SortedParticleBuffer;
	FRDGBufferRef SortedAttachmentBuffer = nullptr;
	{
		FRDGBufferDesc SortedDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), CurrentParticleCount);
		SortedParticleBuffer = GraphBuilder.CreateBuffer(SortedDesc, TEXT("GPUFluid.SortedParticles"));

		FRDGBufferSRVRef OldParticlesSRV = GraphBuilder.CreateSRV(InParticleBuffer);
		FRDGBufferSRVRef SortedIndicesSRV = GraphBuilder.CreateSRV(SortIndicesRDG);
		FRDGBufferUAVRef SortedParticlesUAV = GraphBuilder.CreateUAV(SortedParticleBuffer);

		// Optional: Create sorted attachment buffer if input is provided
		FRDGBufferSRVRef OldAttachmentsSRV = nullptr;
		FRDGBufferUAVRef SortedAttachmentsUAV = nullptr;
		if (InAttachmentBuffer)
		{
			FRDGBufferDesc AttachmentDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoneDeltaAttachment), CurrentParticleCount);
			SortedAttachmentBuffer = GraphBuilder.CreateBuffer(AttachmentDesc, TEXT("GPUFluid.SortedBoneDeltaAttachments"));
			OldAttachmentsSRV = GraphBuilder.CreateSRV(InAttachmentBuffer);
			SortedAttachmentsUAV = GraphBuilder.CreateUAV(SortedAttachmentBuffer);
		}

		AddReorderParticlesPass(GraphBuilder, OldParticlesSRV, SortedIndicesSRV, SortedParticlesUAV, CurrentParticleCount,
			OldAttachmentsSRV, SortedAttachmentsUAV);

		// Output sorted attachment buffer if requested
		if (OutSortedAttachmentBuffer && SortedAttachmentBuffer)
		{
			*OutSortedAttachmentBuffer = SortedAttachmentBuffer;
		}
	}

	//=========================================================================
	// Step 5: Compute Cell Start/End indices
	//=========================================================================
	{
		FRDGBufferSRVRef SortedMortonSRV = GraphBuilder.CreateSRV(MortonCodesRDG);
		OutCellStartUAV = GraphBuilder.CreateUAV(CellStartRDG);
		OutCellEndUAV = GraphBuilder.CreateUAV(CellEndRDG);

		AddComputeCellStartEndPass(GraphBuilder, SortedMortonSRV, OutCellStartUAV, OutCellEndUAV, CurrentParticleCount);

		OutCellStartSRV = GraphBuilder.CreateSRV(CellStartRDG);
		OutCellEndSRV = GraphBuilder.CreateSRV(CellEndRDG);

		// Output buffer refs for persistent extraction (Ray Marching)
		OutCellStartBuffer = CellStartRDG;
		OutCellEndBuffer = CellEndRDG;
	}

	ZOrderBufferParticleCapacity = CurrentParticleCount;

	return SortedParticleBuffer;
}

void FGPUZOrderSortManager::AddComputeMortonCodesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticlesSRV,
	FRDGBufferUAVRef MortonCodesUAV,
	FRDGBufferUAVRef InParticleIndicesUAV,
	int32 CurrentParticleCount,
	const FGPUFluidSimulationParams& Params)
{
	// Validate CellSize
	if (Params.CellSize <= 0.0f)
	{
		UE_LOG(LogGPUZOrderSort, Error, TEXT("Morton code ERROR: Invalid CellSize (%.4f)!"), Params.CellSize);
	}

	// Log bounds info for debugging - disabled for performance
	// static int32 LogFrameCounter = 0;
	// if (++LogFrameCounter % 60 == 0)
	// {
	// 	FVector3f GridMin = FVector3f(
	// 		FMath::Floor(SimulationBoundsMin.X / Params.CellSize),
	// 		FMath::Floor(SimulationBoundsMin.Y / Params.CellSize),
	// 		FMath::Floor(SimulationBoundsMin.Z / Params.CellSize));
	// 	UE_LOG(LogGPUZOrderSort, Log, TEXT("[ZOrder] ComputeMortonCodes - Bounds(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f), CellSize=%.2f, GridMin=(%.0f,%.0f,%.0f)"),
	// 		SimulationBoundsMin.X, SimulationBoundsMin.Y, SimulationBoundsMin.Z,
	// 		SimulationBoundsMax.X, SimulationBoundsMax.Y, SimulationBoundsMax.Z,
	// 		Params.CellSize,
	// 		GridMin.X, GridMin.Y, GridMin.Z);
	// }

	// Get grid parameters from current preset
	const int32 GridSize = GridResolutionPresetHelper::GetGridResolution(GridResolutionPreset);

	// Validate bounds fit within Morton code capacity
	const float CellSizeToUse = FMath::Max(Params.CellSize, 0.001f);
	const float MaxExtent = static_cast<float>(GridSize) * CellSizeToUse;
	const FVector3f BoundsExtent = SimulationBoundsMax - SimulationBoundsMin;

	if (BoundsExtent.X > MaxExtent || BoundsExtent.Y > MaxExtent || BoundsExtent.Z > MaxExtent)
	{
		UE_LOG(LogGPUZOrderSort, Warning,
			TEXT("Morton code bounds overflow! BoundsExtent(%.1f, %.1f, %.1f) exceeds MaxExtent(%.1f). Preset=%d"),
			BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z, MaxExtent, static_cast<int32>(GridResolutionPreset));
	}

	// Get shader with correct permutation
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	FComputeMortonCodesCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));
	TShaderMapRef<FComputeMortonCodesCS> ComputeShader(ShaderMap, PermutationVector);

	FComputeMortonCodesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeMortonCodesCS::FParameters>();
	PassParameters->Particles = ParticlesSRV;
	PassParameters->MortonCodes = MortonCodesUAV;
	PassParameters->ParticleIndices = InParticleIndicesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->BoundsMin = SimulationBoundsMin;
	PassParameters->BoundsExtent = SimulationBoundsMax - SimulationBoundsMin;
	PassParameters->CellSize = Params.CellSize;

	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FComputeMortonCodesCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ComputeMortonCodes(%d,Preset=%d)", CurrentParticleCount, static_cast<int32>(GridResolutionPreset)),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUZOrderSortManager::AddRadixSortPasses(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef& InOutMortonCodes,
	FRDGBufferRef& InOutParticleIndices,
	int32 ParticleCount)
{
	if (ParticleCount <= 0)
	{
		return;
	}

	// Calculate radix sort passes based on Morton code bits from preset
	// Morton code bits = GridAxisBits × 3 (X, Y, Z interleaved)
	// Sort passes = ceil(MortonCodeBits / RadixBits)
	// Round up to even number for ping-pong buffer correctness
	const int32 GridAxisBits = GridResolutionPresetHelper::GetAxisBits(GridResolutionPreset);
	const int32 MortonCodeBits = GridAxisBits * 3;
	int32 RadixSortPasses = (MortonCodeBits + GPU_RADIX_BITS - 1) / GPU_RADIX_BITS;
	// Round up to even for ping-pong buffer to end in original buffer
	if (RadixSortPasses % 2 != 0)
	{
		RadixSortPasses++;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const int32 NumBlocks = FMath::DivideAndRoundUp(ParticleCount, GPU_RADIX_ELEMENTS_PER_GROUP);
	const int32 RequiredHistogramSize = GPU_RADIX_SIZE * NumBlocks;

	// Create transient ping-pong buffers
	FRDGBufferDesc KeysTempDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
	FRDGBufferRef KeysTemp = GraphBuilder.CreateBuffer(KeysTempDesc, TEXT("RadixSort.KeysTemp"));

	FRDGBufferDesc ValuesTempDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
	FRDGBufferRef ValuesTemp = GraphBuilder.CreateBuffer(ValuesTempDesc, TEXT("RadixSort.ValuesTemp"));

	FRDGBufferDesc HistogramDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RequiredHistogramSize);
	FRDGBufferRef Histogram = GraphBuilder.CreateBuffer(HistogramDesc, TEXT("RadixSort.Histogram"));

	FRDGBufferDesc BucketOffsetsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE);
	FRDGBufferRef BucketOffsets = GraphBuilder.CreateBuffer(BucketOffsetsDesc, TEXT("RadixSort.BucketOffsets"));

	// Ping-pong buffers
	FRDGBufferRef Keys[2] = { InOutMortonCodes, KeysTemp };
	FRDGBufferRef Values[2] = { InOutParticleIndices, ValuesTemp };
	int32 BufferIndex = 0;

	for (int32 Pass = 0; Pass < RadixSortPasses; ++Pass)
	{
		const int32 BitOffset = Pass * GPU_RADIX_BITS;
		const int32 SrcIndex = BufferIndex;
		const int32 DstIndex = BufferIndex ^ 1;

		RDG_EVENT_SCOPE(GraphBuilder, "RadixSort Pass %d (bits %d-%d)", Pass, BitOffset, BitOffset + 7);

		// Pass 1: Histogram
		{
			TShaderMapRef<FRadixSortHistogramCS> HistogramShader(ShaderMap);
			FRadixSortHistogramCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortHistogramCS::FParameters>();
			Params->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
			Params->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
			Params->Histogram = GraphBuilder.CreateUAV(Histogram);
			Params->ElementCount = ParticleCount;
			Params->BitOffset = BitOffset;
			Params->NumGroups = NumBlocks;

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Histogram"), HistogramShader, Params, FIntVector(NumBlocks, 1, 1));
		}

		// Pass 2a: Global Prefix Sum
		{
			TShaderMapRef<FRadixSortGlobalPrefixSumCS> PrefixSumShader(ShaderMap);
			FRadixSortGlobalPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortGlobalPrefixSumCS::FParameters>();
			Params->Histogram = GraphBuilder.CreateUAV(Histogram);
			Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);
			Params->NumGroups = NumBlocks;

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GlobalPrefixSum"), PrefixSumShader, Params, FIntVector(1, 1, 1));
		}

		// Pass 2b: Bucket Prefix Sum
		{
			TShaderMapRef<FRadixSortBucketPrefixSumCS> BucketSumShader(ShaderMap);
			FRadixSortBucketPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortBucketPrefixSumCS::FParameters>();
			Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BucketPrefixSum"), BucketSumShader, Params, FIntVector(1, 1, 1));
		}

		// Pass 3: Scatter
		{
			TShaderMapRef<FRadixSortScatterCS> ScatterShader(ShaderMap);
			FRadixSortScatterCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortScatterCS::FParameters>();
			Params->KeysIn = GraphBuilder.CreateSRV(Keys[SrcIndex]);
			Params->ValuesIn = GraphBuilder.CreateSRV(Values[SrcIndex]);
			Params->KeysOut = GraphBuilder.CreateUAV(Keys[DstIndex]);
			Params->ValuesOut = GraphBuilder.CreateUAV(Values[DstIndex]);
			Params->HistogramSRV = GraphBuilder.CreateSRV(Histogram);
			Params->GlobalOffsetsSRV = GraphBuilder.CreateSRV(BucketOffsets);
			Params->ElementCount = ParticleCount;
			Params->BitOffset = BitOffset;

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Scatter"), ScatterShader, Params, FIntVector(NumBlocks, 1, 1));
		}

		BufferIndex ^= 1;
	}

	InOutMortonCodes = Keys[BufferIndex];
	InOutParticleIndices = Values[BufferIndex];
}

void FGPUZOrderSortManager::AddReorderParticlesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef OldParticlesSRV,
	FRDGBufferSRVRef SortedIndicesSRV,
	FRDGBufferUAVRef SortedParticlesUAV,
	int32 CurrentParticleCount,
	FRDGBufferSRVRef OldAttachmentsSRV,
	FRDGBufferUAVRef SortedAttachmentsUAV)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FReorderParticlesCS> ComputeShader(ShaderMap);

	// Determine if we should reorder attachments
	const bool bReorderAttachments = (OldAttachmentsSRV != nullptr && SortedAttachmentsUAV != nullptr);

	// Create dummy buffers if attachments are not provided (shader requires non-null parameters)
	FRDGBufferSRVRef AttachmentsSRV = OldAttachmentsSRV;
	FRDGBufferUAVRef AttachmentsUAV = SortedAttachmentsUAV;
	if (!bReorderAttachments)
	{
		// Create minimal dummy buffer (1 element) to satisfy shader parameter requirements
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoneDeltaAttachment), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("GPUFluid.DummyAttachment"));
		// Clear dummy buffer to satisfy RDG validation (buffer must be written before read)
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyBuffer), 0);
		AttachmentsSRV = GraphBuilder.CreateSRV(DummyBuffer);
		AttachmentsUAV = GraphBuilder.CreateUAV(DummyBuffer);
	}

	FReorderParticlesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReorderParticlesCS::FParameters>();
	PassParameters->OldParticles = OldParticlesSRV;
	PassParameters->SortedIndices = SortedIndicesSRV;
	PassParameters->SortedParticles = SortedParticlesUAV;
	// BoneDeltaAttachment reordering (dummy buffer used if not actually reordering)
	PassParameters->OldBoneDeltaAttachments = AttachmentsSRV;
	PassParameters->SortedBoneDeltaAttachments = AttachmentsUAV;
	PassParameters->bReorderAttachments = bReorderAttachments ? 1 : 0;
	PassParameters->ParticleCount = CurrentParticleCount;

	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FReorderParticlesCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ReorderParticles(%d)%s", CurrentParticleCount,
			bReorderAttachments ? TEXT("+Attachments") : TEXT("")),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUZOrderSortManager::AddComputeCellStartEndPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef SortedMortonCodesSRV,
	FRDGBufferUAVRef CellStartUAV,
	FRDGBufferUAVRef CellEndUAV,
	int32 CurrentParticleCount)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Get cell count from preset
	const int32 CellCount = GridResolutionPresetHelper::GetMaxCells(GridResolutionPreset);

	// Create permutation vector for current preset
	FClearCellIndicesCS::FPermutationDomain ClearPermutation;
	ClearPermutation.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));

	FComputeCellStartEndCS::FPermutationDomain CellStartEndPermutation;
	CellStartEndPermutation.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridResolutionPreset));

	// Step 1: Clear cell indices
	{
		TShaderMapRef<FClearCellIndicesCS> ClearShader(ShaderMap, ClearPermutation);
		FClearCellIndicesCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellIndicesCS::FParameters>();
		ClearParams->CellStart = CellStartUAV;
		ClearParams->CellEnd = CellEndUAV;

		const int32 NumGroups = FMath::DivideAndRoundUp(CellCount, FClearCellIndicesCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ClearCellIndices(%d,Preset=%d)", CellCount, static_cast<int32>(GridResolutionPreset)),
			ClearShader,
			ClearParams,
			FIntVector(NumGroups, 1, 1)
		);
	}

	// Step 2: Compute cell start/end
	{
		TShaderMapRef<FComputeCellStartEndCS> ComputeShader(ShaderMap, CellStartEndPermutation);
		FComputeCellStartEndCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeCellStartEndCS::FParameters>();
		PassParameters->SortedMortonCodes = SortedMortonCodesSRV;
		PassParameters->CellStart = CellStartUAV;
		PassParameters->CellEnd = CellEndUAV;
		PassParameters->ParticleCount = CurrentParticleCount;

		const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FComputeCellStartEndCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ComputeCellStartEnd(%d,Preset=%d)", CurrentParticleCount, static_cast<int32>(GridResolutionPreset)),
			ComputeShader,
			PassParameters,
			FIntVector(NumGroups, 1, 1)
		);
	}
}
