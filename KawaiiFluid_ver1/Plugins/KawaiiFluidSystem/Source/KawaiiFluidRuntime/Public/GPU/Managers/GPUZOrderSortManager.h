// Copyright KawaiiFluid Team. All Rights Reserved.
// FGPUZOrderSortManager - Z-Order Morton Code Sorting System

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIResources.h"
#include "GPU/GPUFluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"  // For EGridResolutionPreset

class FRDGBuilder;

/**
 * FGPUZOrderSortManager
 *
 * Manages GPU-based particle sorting using Z-Order (Morton Code) curve.
 * Provides cache-coherent sorted particle access for efficient neighbor search
 * in SPH simulation.
 *
 * Pipeline:
 * 1. Compute Morton codes from particle positions
 * 2. GPU Radix Sort (sorts Morton codes + particle indices)
 * 3. Reorder particle data based on sorted indices
 * 4. Compute Cell Start/End indices
 *
 * Features:
 * - Configurable Morton code resolution (6/7/8 bits per axis via GridResolutionPreset)
 * - 6-pass 4-bit Radix Sort
 * - Cache-coherent memory access
 * - No hash collisions (unlike traditional spatial hashing)
 */
class KAWAIIFLUIDRUNTIME_API FGPUZOrderSortManager
{
public:
	FGPUZOrderSortManager();
	~FGPUZOrderSortManager();

	//=========================================================================
	// Lifecycle
	//=========================================================================

	void Initialize();
	void Release();
	bool IsReady() const { return bIsInitialized; }

	//=========================================================================
	// Configuration
	//=========================================================================

	/**
	 * Set simulation bounds for Morton code computation
	 * IMPORTANT: Bounds must fit within Morton code capacity!
	 * With 7-bit axes (128 cells per axis) and default CellSize,
	 * max extent is 128 * CellSize per axis.
	 */
	void SetSimulationBounds(const FVector3f& BoundsMin, const FVector3f& BoundsMax)
	{
		SimulationBoundsMin = BoundsMin;
		SimulationBoundsMax = BoundsMax;
	}

	void GetSimulationBounds(FVector3f& OutMin, FVector3f& OutMax) const
	{
		OutMin = SimulationBoundsMin;
		OutMax = SimulationBoundsMax;
	}

	/** Enable/disable Z-Order sorting */
	void SetZOrderSortingEnabled(bool bEnabled) { bUseZOrderSorting = bEnabled; }
	bool IsZOrderSortingEnabled() const { return bUseZOrderSorting; }

	/** Set grid resolution preset for shader permutation selection */
	void SetGridResolutionPreset(EGridResolutionPreset Preset) { GridResolutionPreset = Preset; }
	EGridResolutionPreset GetGridResolutionPreset() const { return GridResolutionPreset; }

	//=========================================================================
	// Z-Order Sorting Pipeline
	//=========================================================================

	/**
	 * Execute the complete Z-Order sorting pipeline
	 * @param GraphBuilder - RDG builder
	 * @param InParticleBuffer - Input particle buffer
	 * @param OutCellStartUAV - Output cell start indices UAV
	 * @param OutCellStartSRV - Output cell start indices SRV
	 * @param OutCellEndUAV - Output cell end indices UAV
	 * @param OutCellEndSRV - Output cell end indices SRV
	 * @param OutCellStartBuffer - Output cell start buffer ref (for persistent extraction)
	 * @param OutCellEndBuffer - Output cell end buffer ref (for persistent extraction)
	 * @param CurrentParticleCount - Number of particles
	 * @param Params - Simulation parameters (for CellSize)
	 * @return Sorted particle buffer
	 */
	FRDGBufferRef ExecuteZOrderSortingPipeline(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef InParticleBuffer,
		FRDGBufferUAVRef& OutCellStartUAV,
		FRDGBufferSRVRef& OutCellStartSRV,
		FRDGBufferUAVRef& OutCellEndUAV,
		FRDGBufferSRVRef& OutCellEndSRV,
		FRDGBufferRef& OutCellStartBuffer,
		FRDGBufferRef& OutCellEndBuffer,
		int32 CurrentParticleCount,
		const FGPUFluidSimulationParams& Params);

private:
	//=========================================================================
	// Internal Passes
	//=========================================================================

	/** Step 1: Compute Morton codes from particle positions */
	void AddComputeMortonCodesPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef ParticlesSRV,
		FRDGBufferUAVRef MortonCodesUAV,
		FRDGBufferUAVRef InParticleIndicesUAV,
		int32 CurrentParticleCount,
		const FGPUFluidSimulationParams& Params);

	/** Step 2: GPU Radix Sort (sorts Morton codes + particle indices) */
	void AddRadixSortPasses(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef& InOutMortonCodes,
		FRDGBufferRef& InOutParticleIndices,
		int32 ParticleCount);

	/** Step 3: Reorder particle data based on sorted indices */
	void AddReorderParticlesPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef OldParticlesSRV,
		FRDGBufferSRVRef SortedIndicesSRV,
		FRDGBufferUAVRef SortedParticlesUAV,
		int32 CurrentParticleCount);

	/** Step 4: Compute Cell Start/End indices from sorted Morton codes */
	void AddComputeCellStartEndPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef SortedMortonCodesSRV,
		FRDGBufferUAVRef CellStartUAV,
		FRDGBufferUAVRef CellEndUAV,
		int32 CurrentParticleCount);

	//=========================================================================
	// State
	//=========================================================================

	bool bIsInitialized = false;

	//=========================================================================
	// Configuration
	//=========================================================================

	bool bUseZOrderSorting = true;

	// Grid resolution preset for shader permutation selection
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;

	// Simulation bounds for Morton code computation
	FVector3f SimulationBoundsMin = FVector3f(-1280.0f, -1280.0f, -1280.0f);
	FVector3f SimulationBoundsMax = FVector3f(1280.0f, 1280.0f, 1280.0f);

	//=========================================================================
	// Buffer Capacity Tracking
	//=========================================================================

	int32 ZOrderBufferParticleCapacity = 0;
};

//=============================================================================
// LEGACY COMPATIBILITY ALIAS
// The old name is kept for backward compatibility during transition.
// New code should use FGPUZOrderSortManager instead.
//=============================================================================
using FGPUSpatialHashManager = FGPUZOrderSortManager;
