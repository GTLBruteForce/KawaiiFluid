// Copyright 2026 Team_Bruteforce. All Rights Reserved.
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

	/** Set grid resolution preset for shader permutation selection (Classic mode only) */
	void SetGridResolutionPreset(EGridResolutionPreset Preset) { GridResolutionPreset = Preset; }
	EGridResolutionPreset GetGridResolutionPreset() const { return GridResolutionPreset; }

	/**
	 * Get the effective grid resolution preset for shader permutation.
	 * In Hybrid Tiled Z-Order mode, always returns Medium (21-bit keys = 2^21 cells).
	 * In Classic mode, returns the configured GridResolutionPreset.
	 * 
	 * CRITICAL: Always use this instead of GetGridResolutionPreset() for shader permutation selection!
	 */
	EGridResolutionPreset GetEffectiveGridResolutionPreset() const
	{
		return bUseHybridTiledZOrder ? EGridResolutionPreset::Medium : GridResolutionPreset;
	}

	/**
	 * Enable/disable Hybrid Tiled Z-Order mode
	 * When enabled:
	 *   - Uses 21-bit sort keys (TileHash 3 bits + LocalMorton 18 bits)
	 *   - Simulation range is UNLIMITED (no bounds clipping)
	 *   - Same radix sort passes as classic mode (key matches MAX_CELLS)
	 *   - 8 tile hash buckets, collisions filtered by distance check
	 * When disabled:
	 *   - Uses 21-bit Morton codes (classic mode)
	 *   - Simulation range limited to bounds (±1280cm default)
	 */
	void SetHybridTiledZOrderEnabled(bool bEnabled) { bUseHybridTiledZOrder = bEnabled; }
	bool IsHybridTiledZOrderEnabled() const { return bUseHybridTiledZOrder; }

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
	 * @param AllocParticleCount - Buffer allocation size (MaxParticleCount to avoid reallocation hitches)
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
		const FGPUFluidSimulationParams& Params,
		int32 AllocParticleCount = 0,
		// Optional: BoneDeltaAttachment buffer to reorder along with particles
		FRDGBufferRef InAttachmentBuffer = nullptr,
		FRDGBufferRef* OutSortedAttachmentBuffer = nullptr);

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
		int32 ParticleCount,
		int32 AllocParticleCount = 0);

	/** Step 3: Reorder particle data based on sorted indices */
	void AddReorderParticlesPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef OldParticlesSRV,
		FRDGBufferSRVRef SortedIndicesSRV,
		FRDGBufferUAVRef SortedParticlesUAV,
		int32 CurrentParticleCount,
		// Optional: BoneDeltaAttachment reordering
		FRDGBufferSRVRef OldAttachmentsSRV = nullptr,
		FRDGBufferUAVRef SortedAttachmentsUAV = nullptr);

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

	// Hybrid Tiled Z-Order mode for unlimited simulation range
	// When enabled, uses 21-bit keys (TileHash 3-bit + LocalMorton 18-bit)
	// Default: true (recommended for character-attached fluids)
	bool bUseHybridTiledZOrder = true;

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
