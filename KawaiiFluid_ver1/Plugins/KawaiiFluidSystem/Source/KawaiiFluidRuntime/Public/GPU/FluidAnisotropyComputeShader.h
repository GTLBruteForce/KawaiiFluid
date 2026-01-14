// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "Core/FluidAnisotropy.h"
#include "GPU/GPUFluidSimulatorShaders.h"  // For FGridResolutionDim, GridResolutionPermutation

// Forward declaration
struct FGPUFluidParticle;
struct FGPUParticleAttachment;

//=============================================================================
// GPU Compute Parameters for Anisotropy Shader Dispatch
// Contains all buffer references and parameters needed for GPU anisotropy calculation.
//=============================================================================

struct KAWAIIFLUIDRUNTIME_API FAnisotropyComputeParams
{
	// Input buffers
	FRDGBufferSRVRef PhysicsParticlesSRV = nullptr;	// FGPUFluidParticle buffer
	FRDGBufferSRVRef AttachmentsSRV = nullptr;		// FGPUParticleAttachment buffer (for surface normal)

	// TODO(KHJ): Remove legacy hash-based lookup - bUseZOrderSorting is always true
	// Legacy hash-based spatial lookup (random access)
	FRDGBufferSRVRef CellCountsSRV = nullptr;		// Spatial hash cell counts
	FRDGBufferSRVRef ParticleIndicesSRV = nullptr;	// Spatial hash particle indices

	// Morton-sorted spatial lookup (sequential access, cache-friendly)
	// When bUseZOrderSorting=true, PhysicsParticles buffer is already sorted by Morton code
	FRDGBufferSRVRef CellStartSRV = nullptr;			// CellStart[cellID] = first particle index
	FRDGBufferSRVRef CellEndSRV = nullptr;				// CellEnd[cellID] = last particle index

	// Output buffers (float4: direction.xyz + scale.w)
	FRDGBufferUAVRef OutAxis1UAV = nullptr;
	FRDGBufferUAVRef OutAxis2UAV = nullptr;
	FRDGBufferUAVRef OutAxis3UAV = nullptr;

	// Parameters
	int32 ParticleCount = 0;
	EGPUAnisotropyMode Mode = EGPUAnisotropyMode::DensityBased;

	// Velocity-based params
	float VelocityStretchFactor = 0.01f;

	// Common params
	float AnisotropyScale = 1.0f;
	float AnisotropyMin = 0.2f;
	float AnisotropyMax = 2.5f;

	// Density-based params
	float DensityWeight = 0.5f;
	float SmoothingRadius = 10.0f;
	float CellSize = 10.0f;

	// TODO(KHJ): Remove bUseZOrderSorting - always true, legacy path is dead code
	// Morton code params
	bool bUseZOrderSorting = false;		// true = use Morton-sorted CellStart/End, false = legacy hash
	FVector3f MortonBoundsMin = FVector3f::ZeroVector;	// Grid origin for Morton code calculation

	// Grid resolution preset for shader permutation selection
	EGridResolutionPreset GridResolutionPreset = EGridResolutionPreset::Medium;

	// Attached particle anisotropy params
	float AttachedFlattenScale = 0.3f;	// How much to flatten attached particles (0.3 = 30% of original height)
	float AttachedStretchScale = 1.5f;	// How much to stretch perpendicular to normal

	// Isolated particle handling params (Yu & Turk style + extensions)
	int32 MinNeighborsForAnisotropy = 8;	// Below this: isotropic fallback
	bool bFadeIsolatedParticles = true;		// Enable size fade for isolated particles
	float MinIsolatedScale = 0.3f;			// Minimum scale when fully isolated
	bool bStretchIsolatedByVelocity = true;	// Stretch isolated particles by velocity
	bool bFadeSlowIsolated = false;			// Additional fade for slow isolated particles
	float IsolationFadeSpeed = 10.0f;		// Speed threshold for slow fade (cm/s)
};

// Constants (must match FluidSpatialHash.ush and FluidAnisotropyCompute.usf)
#define ANISOTROPY_SPATIAL_HASH_SIZE 65536
#define ANISOTROPY_MAX_PARTICLES_PER_CELL 16

//=============================================================================
// Anisotropy Compute Shader
// Calculates ellipsoid orientation and scale for each particle
// Based on NVIDIA FleX and Yu & Turk 2013 paper
//=============================================================================

class FFluidAnisotropyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFluidAnisotropyCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidAnisotropyCS, FGlobalShader);

	// Permutation domain for grid resolution (must match physics solver)
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Physics particle buffer (FGPUFluidParticle)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, InPhysicsParticles)

		// Input: Attachment buffer (FGPUParticleAttachment) - for surface normal of attached particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUParticleAttachment>, InAttachments)

		// TODO(KHJ): Remove legacy hash-based lookup - bUseZOrderSorting is always true
		// Legacy hash-based spatial lookup (random access)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleIndices)

		// Morton-sorted spatial lookup (sequential access, cache-friendly)
		// When bUseZOrderSorting=1, InPhysicsParticles is already sorted by Morton code
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellEnd)

		// Output: Anisotropy SoA buffers (float4 = direction.xyz + scale.w)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutAnisotropyAxis1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutAnisotropyAxis2)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutAnisotropyAxis3)

		// Parameters
		SHADER_PARAMETER(uint32, ParticleCount)
		SHADER_PARAMETER(uint32, AnisotropyMode)  // 0=Velocity, 1=Density, 2=Hybrid
		SHADER_PARAMETER(float, VelocityStretchFactor)
		SHADER_PARAMETER(float, AnisotropyScale)
		SHADER_PARAMETER(float, AnisotropyMin)
		SHADER_PARAMETER(float, AnisotropyMax)
		SHADER_PARAMETER(float, DensityWeight)  // For Hybrid mode
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, CellSize)
		// TODO(KHJ): Remove bUseZOrderSorting - always 1, legacy path is dead code
		SHADER_PARAMETER(int32, bUseZOrderSorting)  // 1 = Morton-sorted, 0 = legacy hash
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)  // Grid origin for Morton code

		// Attached particle anisotropy params
		SHADER_PARAMETER(float, AttachedFlattenScale)  // How flat (0.3 = 30% height)
		SHADER_PARAMETER(float, AttachedStretchScale)  // Perpendicular stretch

		// Isolated particle handling params
		SHADER_PARAMETER(int32, MinNeighborsForAnisotropy)  // Threshold for isolation
		SHADER_PARAMETER(int32, bFadeIsolatedParticles)     // Enable size fade
		SHADER_PARAMETER(float, MinIsolatedScale)           // Min scale when isolated
		SHADER_PARAMETER(int32, bStretchIsolatedByVelocity) // Velocity stretch for isolated
		SHADER_PARAMETER(int32, bFadeSlowIsolated)          // Slow particle fade
		SHADER_PARAMETER(float, IsolationFadeSpeed)         // Speed threshold
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), ANISOTROPY_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), ANISOTROPY_MAX_PARTICLES_PER_CELL);

		// Get grid resolution from permutation for Morton code calculation
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

//=============================================================================
// Anisotropy Pass Builder
// Utility class for adding anisotropy compute passes to RDG
//=============================================================================

class KAWAIIFLUIDRUNTIME_API FFluidAnisotropyPassBuilder
{
public:
	/**
	 * @brief Add anisotropy calculation pass to RDG.
	 * @param GraphBuilder RDG builder.
	 * @param Params Anisotropy compute parameters (buffers and settings).
	 */
	static void AddAnisotropyPass(
		FRDGBuilder& GraphBuilder,
		const FAnisotropyComputeParams& Params);

	/**
	 * @brief Create anisotropy output buffers.
	 * @param GraphBuilder RDG builder.
	 * @param ParticleCount Number of particles.
	 * @param OutAxis1 Output axis 1 buffer (direction.xyz + scale.w).
	 * @param OutAxis2 Output axis 2 buffer.
	 * @param OutAxis3 Output axis 3 buffer.
	 */
	static void CreateAnisotropyBuffers(
		FRDGBuilder& GraphBuilder,
		int32 ParticleCount,
		FRDGBufferRef& OutAxis1,
		FRDGBufferRef& OutAxis2,
		FRDGBufferRef& OutAxis3);
};
