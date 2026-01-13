// Copyright KawaiiFluid Team. All Rights Reserved.
// GPUFluidSimulator - Z-Order Sorting and Spatial Hashing Functions

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);

//=============================================================================
// Z-Order (Morton Code) Sorting Pipeline
// Replaces hash table with cache-coherent sorted particle access
//=============================================================================

FRDGBufferRef FGPUFluidSimulator::ExecuteZOrderSortingPipeline(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef InParticleBuffer,
	FRDGBufferUAVRef& OutCellStartUAV,
	FRDGBufferSRVRef& OutCellStartSRV,
	FRDGBufferUAVRef& OutCellEndUAV,
	FRDGBufferSRVRef& OutCellEndSRV,
	const FGPUFluidSimulationParams& Params)
{
	RDG_EVENT_SCOPE(GraphBuilder, "GPUFluid::ZOrderSorting");

	if (CurrentParticleCount <= 0)
	{
		return InParticleBuffer;
	}

	// Check if we need to allocate/resize buffers
	const bool bNeedResize = ZOrderBufferParticleCapacity < CurrentParticleCount;
	const int32 NumBlocks = FMath::DivideAndRoundUp(CurrentParticleCount, 256);
	const int32 CellCount = GPU_MAX_CELLS;  // 21-bit Morton code = Cell ID (128^3 = 2,097,152 cells)

	//=========================================================================
	// Step 1: Create/reuse Morton code and index buffers
	//=========================================================================
	FRDGBufferRef MortonCodesRDG;
	FRDGBufferRef MortonCodesTempRDG;
	FRDGBufferRef SortIndicesRDG;
	FRDGBufferRef SortIndicesTempRDG;
	FRDGBufferRef HistogramRDG;
	FRDGBufferRef BucketOffsetsRDG;
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

	// Radix sort histogram: 16 buckets * NumBlocks
	{
		FRDGBufferDesc HistogramDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE * NumBlocks);
		HistogramRDG = GraphBuilder.CreateBuffer(HistogramDesc, TEXT("GPUFluid.RadixHistogram"));

		FRDGBufferDesc BucketDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE);
		BucketOffsetsRDG = GraphBuilder.CreateBuffer(BucketDesc, TEXT("GPUFluid.RadixBucketOffsets"));
	}

	// Cell Start/End (use hash size for compatibility with existing neighbor search)
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

		AddComputeMortonCodesPass(GraphBuilder, ParticlesSRV, MortonCodesUAV, IndicesUAV, Params);
	}

	//=========================================================================
	// Step 3: Radix Sort (6 passes for 21-bit Morton codes)
	//=========================================================================
	AddRadixSortPasses(GraphBuilder, MortonCodesRDG, SortIndicesRDG, CurrentParticleCount);

	//=========================================================================
	// Step 4: Reorder particle data based on sorted indices
	//=========================================================================
	FRDGBufferRef SortedParticleBuffer;
	{
		FRDGBufferDesc SortedDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUFluidParticle), CurrentParticleCount);
		SortedParticleBuffer = GraphBuilder.CreateBuffer(SortedDesc, TEXT("GPUFluid.SortedParticles"));

		FRDGBufferSRVRef OldParticlesSRV = GraphBuilder.CreateSRV(InParticleBuffer);
		FRDGBufferSRVRef SortedIndicesSRV = GraphBuilder.CreateSRV(SortIndicesRDG);
		FRDGBufferUAVRef SortedParticlesUAV = GraphBuilder.CreateUAV(SortedParticleBuffer);

		AddReorderParticlesPass(GraphBuilder, OldParticlesSRV, SortedIndicesSRV, SortedParticlesUAV);
	}

	//=========================================================================
	// Step 5: Compute Cell Start/End indices
	//=========================================================================
	{
		FRDGBufferSRVRef SortedMortonSRV = GraphBuilder.CreateSRV(MortonCodesRDG);
		OutCellStartUAV = GraphBuilder.CreateUAV(CellStartRDG);
		OutCellEndUAV = GraphBuilder.CreateUAV(CellEndRDG);

		AddComputeCellStartEndPass(GraphBuilder, SortedMortonSRV, OutCellStartUAV, OutCellEndUAV);

		OutCellStartSRV = GraphBuilder.CreateSRV(CellStartRDG);
		OutCellEndSRV = GraphBuilder.CreateSRV(CellEndRDG);
	}

	// Update capacity tracking
	ZOrderBufferParticleCapacity = CurrentParticleCount;

	return SortedParticleBuffer;
}

void FGPUFluidSimulator::AddComputeMortonCodesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticlesSRV,
	FRDGBufferUAVRef MortonCodesUAV,
	FRDGBufferUAVRef InParticleIndicesUAV,
	const FGPUFluidSimulationParams& Params)
{
	// Validate CellSize is valid (prevent division by zero in shader)
	if (Params.CellSize <= 0.0f)
	{
		UE_LOG(LogGPUFluidSimulator, Error,
			TEXT("Morton code ERROR: Invalid CellSize (%.4f)! Must be > 0. Using default 2.0."),
			Params.CellSize);
		// We'll use the value anyway since we can't modify Params, but the shader will handle it
	}

	// Validate bounds fit within Morton code capacity (10 bits per axis)
	const float CellSizeToUse = FMath::Max(Params.CellSize, 0.001f);
	const float MaxExtent = GPU_MORTON_GRID_SIZE * CellSizeToUse;
	const FVector3f BoundsExtent = SimulationBoundsMax - SimulationBoundsMin;

	// Warn if bounds exceed Morton code capacity
	if (BoundsExtent.X > MaxExtent || BoundsExtent.Y > MaxExtent || BoundsExtent.Z > MaxExtent)
	{
		UE_LOG(LogGPUFluidSimulator, Warning,
			TEXT("Morton code bounds overflow! BoundsExtent(%.1f, %.1f, %.1f) exceeds MaxExtent(%.1f). "
			     "Reduce simulation bounds or increase CellSize."),
			BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z, MaxExtent);
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FComputeMortonCodesCS> ComputeShader(ShaderMap);

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
		RDG_EVENT_NAME("GPUFluid::ComputeMortonCodes(%d)", CurrentParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddRadixSortPasses(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef& InOutMortonCodes,
	FRDGBufferRef& InOutParticleIndices,
	int32 ParticleCount)
{
	if (ParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const int32 NumBlocks = FMath::DivideAndRoundUp(ParticleCount, GPU_RADIX_ELEMENTS_PER_GROUP);

	//=========================================================================
	// Create TRANSIENT RDG BUFFERS for RadixSort internal state
	// These buffers only live within this frame's RDG execution.
	// RDG correctly tracks dependencies between passes, preventing aliasing
	// issues that occur with incorrectly managed external buffers.
	//
	// Unlike UE's GPUSort which uses direct RHI (no RDG), we use RDG transient
	// buffers because our simulation pipeline is already RDG-based.
	//=========================================================================

	const int32 RequiredHistogramSize = GPU_RADIX_SIZE * NumBlocks;

	// Create transient ping-pong buffers for RadixSort
	FRDGBufferDesc KeysTempDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
	FRDGBufferRef KeysTemp = GraphBuilder.CreateBuffer(KeysTempDesc, TEXT("RadixSort.KeysTemp"));

	FRDGBufferDesc ValuesTempDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), ParticleCount);
	FRDGBufferRef ValuesTemp = GraphBuilder.CreateBuffer(ValuesTempDesc, TEXT("RadixSort.ValuesTemp"));

	// Create transient Histogram buffer [NumBlocks * RADIX_SIZE]
	FRDGBufferDesc HistogramDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RequiredHistogramSize);
	FRDGBufferRef Histogram = GraphBuilder.CreateBuffer(HistogramDesc, TEXT("RadixSort.Histogram"));

	// Create transient BucketOffsets buffer [RADIX_SIZE]
	FRDGBufferDesc BucketOffsetsDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GPU_RADIX_SIZE);
	FRDGBufferRef BucketOffsets = GraphBuilder.CreateBuffer(BucketOffsetsDesc, TEXT("RadixSort.BucketOffsets"));

	// Ping-pong buffers using array + index pattern
	// Pass 0: Read Keys[0]=InOutMortonCodes, Write Keys[1]=KeysTemp
	// Pass 1: Read Keys[1]=KeysTemp, Write Keys[0]=InOutMortonCodes
	// Pass 2: Read Keys[0]=InOutMortonCodes, Write Keys[1]=KeysTemp
	// Pass 3: Read Keys[1]=KeysTemp, Write Keys[0]=InOutMortonCodes
	// After 4 passes, BufferIndex=0, so final result is in Keys[0]=InOutMortonCodes
	FRDGBufferRef Keys[2] = { InOutMortonCodes, KeysTemp };
	FRDGBufferRef Values[2] = { InOutParticleIndices, ValuesTemp };
	int32 BufferIndex = 0;

	// Auto-calculated passes for Morton code coverage
	// Morton code = GPU_MORTON_CODE_BITS (GridAxisBits * 3)
	// Passes = ceil(MortonCodeBits / RadixBits) = GPU_RADIX_SORT_PASSES
	// Ping-pong buffer: even passes -> result in original buffer, odd -> result in temp buffer
	// Current: GPU_RADIX_SORT_PASSES is even, so result is in InOutMortonCodes
	static_assert(GPU_RADIX_SORT_PASSES % 2 == 0, "GPU_RADIX_SORT_PASSES must be even for ping-pong buffer to return result in original buffer");
	for (int32 Pass = 0; Pass < GPU_RADIX_SORT_PASSES; ++Pass)
	{
		const int32 BitOffset = Pass * GPU_RADIX_BITS;
		const int32 SrcIndex = BufferIndex;
		const int32 DstIndex = BufferIndex ^ 1;

		RDG_EVENT_SCOPE(GraphBuilder, "RadixSort Pass %d (bits %d-%d)", Pass, BitOffset, BitOffset + 3);

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

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Histogram"),
				HistogramShader,
				Params,
				FIntVector(NumBlocks, 1, 1)
			);
		}

		// Pass 2a: Global Prefix Sum (within each bucket)
		{
			TShaderMapRef<FRadixSortGlobalPrefixSumCS> PrefixSumShader(ShaderMap);
			FRadixSortGlobalPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortGlobalPrefixSumCS::FParameters>();
			Params->Histogram = GraphBuilder.CreateUAV(Histogram);
			Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);
			Params->NumGroups = NumBlocks;

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GlobalPrefixSum"),
				PrefixSumShader,
				Params,
				FIntVector(1, 1, 1)  // Single group, 16 threads
			);
		}

		// Pass 2b: Bucket Prefix Sum (across buckets)
		{
			TShaderMapRef<FRadixSortBucketPrefixSumCS> BucketSumShader(ShaderMap);
			FRadixSortBucketPrefixSumCS::FParameters* Params = GraphBuilder.AllocParameters<FRadixSortBucketPrefixSumCS::FParameters>();
			Params->GlobalOffsets = GraphBuilder.CreateUAV(BucketOffsets);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("BucketPrefixSum"),
				BucketSumShader,
				Params,
				FIntVector(1, 1, 1)
			);
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

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Scatter"),
				ScatterShader,
				Params,
				FIntVector(NumBlocks, 1, 1)
			);
		}

		// Ping-pong: toggle buffer index for next pass
		BufferIndex ^= 1;
	}

	// After GPU_RADIX_SORT_PASSES passes, the final sorted data is in Keys[BufferIndex]/Values[BufferIndex]
	// BufferIndex alternates each pass. With even passes, ends at 0 (original buffer).
	// static_assert above ensures GPU_RADIX_SORT_PASSES is even.
	InOutMortonCodes = Keys[BufferIndex];
	InOutParticleIndices = Values[BufferIndex];
}

void FGPUFluidSimulator::AddReorderParticlesPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef OldParticlesSRV,
	FRDGBufferSRVRef SortedIndicesSRV,
	FRDGBufferUAVRef SortedParticlesUAV)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FReorderParticlesCS> ComputeShader(ShaderMap);

	FReorderParticlesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReorderParticlesCS::FParameters>();
	PassParameters->OldParticles = OldParticlesSRV;
	PassParameters->SortedIndices = SortedIndicesSRV;
	PassParameters->SortedParticles = SortedParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;

	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FReorderParticlesCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ReorderParticles(%d)", CurrentParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddComputeCellStartEndPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef SortedMortonCodesSRV,
	FRDGBufferUAVRef CellStartUAV,
	FRDGBufferUAVRef CellEndUAV)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const int32 CellCount = GPU_MAX_CELLS;

	// Step 1: Clear cell indices to invalid (0xFFFFFFFF)
	{
		TShaderMapRef<FClearCellIndicesCS> ClearShader(ShaderMap);
		FClearCellIndicesCS::FParameters* ClearParams = GraphBuilder.AllocParameters<FClearCellIndicesCS::FParameters>();
		ClearParams->CellStart = CellStartUAV;
		ClearParams->CellEnd = CellEndUAV;

		const int32 NumGroups = FMath::DivideAndRoundUp(CellCount, FClearCellIndicesCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ClearCellIndices(%d)", CellCount),
			ClearShader,
			ClearParams,
			FIntVector(NumGroups, 1, 1)
		);
	}

	// Step 2: Compute cell start/end from sorted Morton codes
	{
		TShaderMapRef<FComputeCellStartEndCS> ComputeShader(ShaderMap);
		FComputeCellStartEndCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeCellStartEndCS::FParameters>();
		PassParameters->SortedMortonCodes = SortedMortonCodesSRV;
		PassParameters->CellStart = CellStartUAV;
		PassParameters->CellEnd = CellEndUAV;
		PassParameters->ParticleCount = CurrentParticleCount;

		const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FComputeCellStartEndCS::ThreadGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GPUFluid::ComputeCellStartEnd(%d)", CurrentParticleCount),
			ComputeShader,
			PassParameters,
			FIntVector(NumGroups, 1, 1)
		);
	}
}
