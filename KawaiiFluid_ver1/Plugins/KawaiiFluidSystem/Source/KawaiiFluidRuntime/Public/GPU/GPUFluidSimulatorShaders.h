// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "GPU/GPUFluidParticle.h"
#include "Core/KawaiiFluidSimulationTypes.h"  // For EGridResolutionPreset

// Spatial hash constants (must match FluidSpatialHash.ush)
#define GPU_SPATIAL_HASH_SIZE 65536
#define GPU_MAX_PARTICLES_PER_CELL 16

// Neighbor caching constants
// 64 neighbors is sufficient for typical SPH simulations (avg ~30-40 neighbors)
#define GPU_MAX_NEIGHBORS_PER_PARTICLE 64

//=============================================================================
// Grid Resolution Permutation
// Enables different grid sizes (64³, 128³, 256³) via shader permutation
//=============================================================================

/**
 * Grid Resolution Permutation Dimension
 *
 * Permutation Values:
 *   0 = Small  (6 bits, 64³ cells,    262,144 max cells)
 *   1 = Medium (7 bits, 128³ cells, 2,097,152 max cells)
 *   2 = Large  (8 bits, 256³ cells, 16,777,216 max cells)
 */
class FGridResolutionDim : SHADER_PERMUTATION_RANGE_INT("GRID_RESOLUTION_PRESET", 0, 3);

/** Helper to get Morton grid constants from permutation ID */
namespace GridResolutionPermutation
{
	/** Get axis bits for a permutation index */
	inline int32 GetAxisBits(int32 PermutationId)
	{
		switch (PermutationId)
		{
		case 0: return 6;  // Small
		case 1: return 7;  // Medium
		case 2: return 8;  // Large
		default: return 7;
		}
	}

	/** Get grid resolution for a permutation index */
	inline int32 GetGridResolution(int32 PermutationId)
	{
		return 1 << GetAxisBits(PermutationId);
	}

	/** Get max cells for a permutation index */
	inline int32 GetMaxCells(int32 PermutationId)
	{
		const int32 Res = GetGridResolution(PermutationId);
		return Res * Res * Res;
	}

	/** Get radix sort passes needed for Morton code bits */
	inline int32 GetRadixSortPasses(int32 PermutationId)
	{
		const int32 MortonCodeBits = GetAxisBits(PermutationId) * 3;
		return (MortonCodeBits + 3) / 4;  // ceil(bits / 4)
	}

	/** Convert EGridResolutionPreset to permutation index */
	inline int32 FromPreset(EGridResolutionPreset Preset)
	{
		return static_cast<int32>(Preset);
	}
}

//=============================================================================
// Predict Positions Compute Shader
// Pass 1: Apply forces and predict positions
//=============================================================================

class FPredictPositionsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPredictPositionsCS);
	SHADER_USE_PARAMETER_STRUCT(FPredictPositionsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, DeltaTime)
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER(FVector3f, ExternalForce)
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
// [DEPRECATED] Compute Density Compute Shader
// Pass 3: Calculate density and lambda using spatial hash
//
// NOTE: This shader is deprecated and will be removed in a future version.
// Use FSolveDensityPressureCS instead, which combines density and pressure
// calculation into a single neighbor traversal for better performance.
//=============================================================================

class FComputeDensityCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FComputeDensityCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeDensityCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Particle buffer (read-write)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)

		// Spatial hash buffers (read-only)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleIndices)

		// Simulation parameters
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, RestDensity)
		SHADER_PARAMETER(float, Poly6Coeff)
		SHADER_PARAMETER(float, SpikyCoeff)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, Compliance)
		SHADER_PARAMETER(float, DeltaTimeSq)
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
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);
	}
};

//=============================================================================
// [DEPRECATED] Solve Pressure Compute Shader
// Pass 4: Apply position corrections based on density constraints
//
// NOTE: This shader is deprecated and will be removed in a future version.
// Use FSolveDensityPressureCS instead, which combines density and pressure
// calculation into a single neighbor traversal for better performance.
//=============================================================================

class FSolvePressureCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSolvePressureCS);
	SHADER_USE_PARAMETER_STRUCT(FSolvePressureCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleIndices)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, RestDensity)
		SHADER_PARAMETER(float, SpikyCoeff)
		SHADER_PARAMETER(float, Poly6Coeff)
		SHADER_PARAMETER(float, CellSize)
		// Tensile Instability (PBF Eq.13-14: s_corr = -k * (W(r)/W(Δq))^n)
		SHADER_PARAMETER(int32, bEnableTensileInstability)
		SHADER_PARAMETER(float, TensileK)
		SHADER_PARAMETER(int32, TensileN)
		SHADER_PARAMETER(float, InvW_DeltaQ)
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
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);
	}
};

//=============================================================================
// Combined Density + Pressure Compute Shader (OPTIMIZED)
// Pass 3+4 Combined: Single neighbor traversal for both density and pressure
//
// Optimizations:
// 1. Pass Integration: Reduces neighbor search from 2x to 1x per iteration
// 2. rsqrt: Uses fast inverse square root
// 3. Loop Unroll: 27-cell explicit unrolling
// 4. Neighbor Caching: First iteration builds neighbor list, subsequent iterations reuse
//=============================================================================

class FSolveDensityPressureCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSolveDensityPressureCS);
	SHADER_USE_PARAMETER_STRUCT(FSolveDensityPressureCS, FGlobalShader);

	// Permutation domain for grid resolution (Z-Order neighbor search)
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		// Hash table mode (legacy)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleIndices)
		// Z-Order sorted mode (new)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellEnd)
		SHADER_PARAMETER(int32, bUseZOrderSorting)
		// Morton bounds for Z-Order cell ID calculation
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)
		SHADER_PARAMETER(FVector3f, MortonBoundsExtent)
		// Neighbor caching buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, NeighborList)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, NeighborCounts)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, RestDensity)
		SHADER_PARAMETER(float, Poly6Coeff)
		SHADER_PARAMETER(float, SpikyCoeff)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, Compliance)
		SHADER_PARAMETER(float, DeltaTimeSq)
		// Tensile Instability (PBF Eq.13-14)
		SHADER_PARAMETER(int32, bEnableTensileInstability)
		SHADER_PARAMETER(float, TensileK)
		SHADER_PARAMETER(int32, TensileN)
		SHADER_PARAMETER(float, InvW_DeltaQ)
		// Iteration control for neighbor caching
		SHADER_PARAMETER(int32, IterationIndex)
		// Boundary Particles for density contribution (Akinci 2012)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER(int32, bUseBoundaryDensity)
		// Z-Order sorted boundary particles (Akinci 2012 + Z-Order optimization)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, SortedBoundaryParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellEnd)
		SHADER_PARAMETER(int32, bUseBoundaryZOrder)
		// Relative Velocity Pressure Damping (prevents fluid flying away from fast boundaries)
		SHADER_PARAMETER(int32, bEnableRelativeVelocityDamping)
		SHADER_PARAMETER(float, RelativeVelocityDampingStrength)
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
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);
		OutEnvironment.SetDefine(TEXT("MAX_NEIGHBORS_PER_PARTICLE"), GPU_MAX_NEIGHBORS_PER_PARTICLE);

		// Get grid resolution from permutation for Z-Order neighbor search
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 GridSize = GridResolutionPermutation::GetGridResolution(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MORTON_GRID_SIZE"), GridSize);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

//=============================================================================
// Apply Viscosity Compute Shader (OPTIMIZED: Uses cached neighbor list)
// Pass 5: Apply XSPH viscosity
//=============================================================================

class FApplyViscosityCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FApplyViscosityCS);
	SHADER_USE_PARAMETER_STRUCT(FApplyViscosityCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleIndices)
		// Neighbor caching buffers (reuse from DensityPressure pass)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborList)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborCounts)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, ViscosityCoefficient)
		SHADER_PARAMETER(float, Poly6Coeff)
		SHADER_PARAMETER(float, ViscLaplacianCoeff)  // 45 / (PI * h^6) for Laplacian viscosity
		SHADER_PARAMETER(float, DeltaTime)           // Substep delta time for Laplacian viscosity
		SHADER_PARAMETER(float, CellSize)
		// Flag to use cached neighbors (1 = use cache, 0 = use hash)
		SHADER_PARAMETER(int32, bUseNeighborCache)
		// Boundary Particles for viscosity contribution
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER(int32, bUseBoundaryViscosity)
		SHADER_PARAMETER(float, AdhesionForceStrength)     // Akinci 2013 adhesion force (0~50)
		SHADER_PARAMETER(float, AdhesionVelocityStrength)  // Velocity transfer strength (0~5)
		SHADER_PARAMETER(float, AdhesionRadius)            // Boundary adhesion influence radius (cm)
		// Z-Order sorted boundary particles (same pattern as FSolveDensityPressureCS)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, SortedBoundaryParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellEnd)
		SHADER_PARAMETER(int32, bUseBoundaryZOrder)
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)  // Required for GetMortonCellIDFromCellCoord
		// Improved Boundary Velocity Transfer
		SHADER_PARAMETER(float, BoundaryVelocityTransferStrength)
		SHADER_PARAMETER(float, BoundaryDetachSpeedThreshold)
		SHADER_PARAMETER(float, BoundaryMaxDetachSpeed)
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
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);
		OutEnvironment.SetDefine(TEXT("MAX_NEIGHBORS_PER_PARTICLE"), GPU_MAX_NEIGHBORS_PER_PARTICLE);
	}
};

//=============================================================================
// Particle Sleeping Compute Shader
// NVIDIA Flex stabilization technique: sleep low-velocity particles
// Reduces micro-jitter and improves performance
//=============================================================================

class FParticleSleepingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FParticleSleepingCS);
	SHADER_USE_PARAMETER_STRUCT(FParticleSleepingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SleepCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborList)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborCounts)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, SleepVelocityThreshold)
		SHADER_PARAMETER(int32, SleepFrameThreshold)
		SHADER_PARAMETER(float, WakeVelocityThreshold)
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
		OutEnvironment.SetDefine(TEXT("MAX_NEIGHBORS_PER_PARTICLE"), GPU_MAX_NEIGHBORS_PER_PARTICLE);
	}
};

//=============================================================================
// Combined Viscosity and Cohesion Compute Shader
// Merges ApplyViscosity and ApplyCohesion into single pass for performance
//=============================================================================

class FApplyViscosityAndCohesionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FApplyViscosityAndCohesionCS);
	SHADER_USE_PARAMETER_STRUCT(FApplyViscosityAndCohesionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleIndices)
		// Neighbor caching buffers
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborList)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborCounts)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, DeltaTime)
		SHADER_PARAMETER(int32, bUseNeighborCache)
		// Viscosity parameters
		SHADER_PARAMETER(float, ViscosityCoefficient)
		SHADER_PARAMETER(float, Poly6Coeff)
		SHADER_PARAMETER(float, ViscLaplacianCoeff)
		// Cohesion parameters
		SHADER_PARAMETER(float, CohesionStrength)
		SHADER_PARAMETER(float, RestDensity)
		SHADER_PARAMETER(float, MaxSurfaceTensionForce)
		// Boundary particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER(int32, bUseBoundaryViscosity)
		SHADER_PARAMETER(float, AdhesionStrength)
		SHADER_PARAMETER(float, AdhesionRadius)
		// Z-Order sorted boundary particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, SortedBoundaryParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellEnd)
		SHADER_PARAMETER(int32, bUseBoundaryZOrder)
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)
		// Boundary velocity transfer
		SHADER_PARAMETER(float, BoundaryVelocityTransferStrength)
		SHADER_PARAMETER(float, BoundaryDetachSpeedThreshold)
		SHADER_PARAMETER(float, BoundaryMaxDetachSpeed)
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
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);
		OutEnvironment.SetDefine(TEXT("MAX_NEIGHBORS_PER_PARTICLE"), GPU_MAX_NEIGHBORS_PER_PARTICLE);
	}
};

//=============================================================================
// Bounds Collision Compute Shader
// Pass 6: Apply AABB/OBB bounds collision
//=============================================================================

class FBoundsCollisionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoundsCollisionCS);
	SHADER_USE_PARAMETER_STRUCT(FBoundsCollisionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, ParticleRadius)
		// OBB parameters
		SHADER_PARAMETER(FVector3f, BoundsCenter)
		SHADER_PARAMETER(FVector3f, BoundsExtent)
		SHADER_PARAMETER(FVector4f, BoundsRotation)  // Quaternion (x, y, z, w)
		SHADER_PARAMETER(int32, bUseOBB)
		// Legacy AABB parameters
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsMax)
		// Collision response
		SHADER_PARAMETER(float, Restitution)
		SHADER_PARAMETER(float, Friction)
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
// Distance Field Collision Compute Shader
// Pass 6.5: Apply collision with UE5 Global Distance Field (static mesh collision)
//=============================================================================

class FDistanceFieldCollisionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDistanceFieldCollisionCS);
	SHADER_USE_PARAMETER_STRUCT(FDistanceFieldCollisionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Particle buffer
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, ParticleRadius)

		// Distance Field Volume Parameters
		SHADER_PARAMETER(FVector3f, GDFVolumeCenter)
		SHADER_PARAMETER(FVector3f, GDFVolumeExtent)
		SHADER_PARAMETER(FVector3f, GDFVoxelSize)
		SHADER_PARAMETER(float, GDFMaxDistance)

		// Collision Response Parameters
		SHADER_PARAMETER(float, DFCollisionRestitution)
		SHADER_PARAMETER(float, DFCollisionFriction)
		SHADER_PARAMETER(float, DFCollisionThreshold)

		// Global Distance Field Texture (from scene)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, GlobalDistanceFieldTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, GlobalDistanceFieldSampler)
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
		OutEnvironment.SetDefine(TEXT("USE_GLOBAL_DISTANCE_FIELD"), 1);
	}
};

//=============================================================================
// Primitive Collision Compute Shader
// Pass 6.5: Apply collision with explicit primitives (spheres, capsules, boxes, convexes)
// Also records collision feedback for particle -> player interaction (when enabled)
//=============================================================================

class FPrimitiveCollisionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPrimitiveCollisionCS);
	SHADER_USE_PARAMETER_STRUCT(FPrimitiveCollisionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Particle buffer
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, CollisionThreshold)

		// Collision primitives
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionSphere>, CollisionSpheres)
		SHADER_PARAMETER(int32, SphereCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionCapsule>, CollisionCapsules)
		SHADER_PARAMETER(int32, CapsuleCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionBox>, CollisionBoxes)
		SHADER_PARAMETER(int32, BoxCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionConvex>, CollisionConvexes)
		SHADER_PARAMETER(int32, ConvexCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUConvexPlane>, ConvexPlanes)

		// Bone transforms (for impact offset computation)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoneTransform>, BoneTransforms)
		SHADER_PARAMETER(int32, BoneCount)

		// Collision Feedback (for particle -> player interaction)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUCollisionFeedback>, CollisionFeedback)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CollisionCounter)
		SHADER_PARAMETER(int32, MaxCollisionFeedback)
		SHADER_PARAMETER(int32, bEnableCollisionFeedback)

		// Collider Contact Counts (간단한 충돌 카운트용)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ColliderContactCounts)
		SHADER_PARAMETER(int32, MaxColliderCount)
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
// Finalize Positions Compute Shader
// Pass 7: Finalize positions and update velocities
//=============================================================================

class FFinalizePositionsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFinalizePositionsCS);
	SHADER_USE_PARAMETER_STRUCT(FFinalizePositionsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, DeltaTime)
		SHADER_PARAMETER(float, MaxVelocity)      // Safety clamp (high value, e.g., 50000 cm/s)
		SHADER_PARAMETER(float, GlobalDamping)    // Velocity damping per substep (1.0 = no damping)
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
// Extract Render Data Compute Shader
// Phase 2: Extract render data from physics buffer to render buffer (GPU → GPU)
//=============================================================================

class FExtractRenderDataCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FExtractRenderDataCS);
	SHADER_USE_PARAMETER_STRUCT(FExtractRenderDataCS, FGlobalShader);

	// Render particle structure (must match FKawaiiRenderParticle - 32 bytes)
	struct FRenderParticle
	{
		FVector3f Position;
		FVector3f Velocity;
		float Radius;
		float Padding;
	};

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, PhysicsParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FRenderParticle>, RenderParticles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, ParticleRadius)
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
// Extract Render Data SoA Compute Shader
// Phase 2: Extract to SoA buffers for memory bandwidth optimization
// - Position buffer: 12B per particle (SDF hot path)
// - Velocity buffer: 12B per particle (motion blur)
// Total: 24B vs 32B (AoS) = 25% reduction, SDF uses only Position = 62% reduction
//=============================================================================

class FExtractRenderDataSoACS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FExtractRenderDataSoACS);
	SHADER_USE_PARAMETER_STRUCT(FExtractRenderDataSoACS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, PhysicsParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector3f>, RenderPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector3f>, RenderVelocities)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, DeltaTime)  // For 1-frame delay compensation: RenderPos = Pos + Vel * DeltaTime
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
// Extract Render Data With Bounds Compute Shader (Optimized - Merged Pass)
// Combines ExtractRenderData + CalculateBounds into single pass
// Eliminates separate bounds calculation and reduces GPU dispatch overhead
//=============================================================================

class FExtractRenderDataWithBoundsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FExtractRenderDataWithBoundsCS);
	SHADER_USE_PARAMETER_STRUCT(FExtractRenderDataWithBoundsCS, FGlobalShader);

	// Render particle structure (must match FKawaiiRenderParticle - 32 bytes)
	struct FRenderParticle
	{
		FVector3f Position;
		FVector3f Velocity;
		float Radius;
		float Padding;
	};

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, PhysicsParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FRenderParticle>, RenderParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector3f>, OutputBounds)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, BoundsMargin)
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
// Copy Particles Compute Shader
// Utility: Copy particles from source buffer to destination buffer
// Used for preserving existing GPU simulation results when appending new particles
//=============================================================================

class FCopyParticlesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCopyParticlesCS);
	SHADER_USE_PARAMETER_STRUCT(FCopyParticlesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, SourceParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, DestParticles)
		SHADER_PARAMETER(int32, SourceOffset)
		SHADER_PARAMETER(int32, DestOffset)
		SHADER_PARAMETER(int32, CopyCount)
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
// Spawn Particles Compute Shader
// GPU-based particle creation from spawn requests (eliminates CPU→GPU race condition)
//=============================================================================

class FSpawnParticlesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSpawnParticlesCS);
	SHADER_USE_PARAMETER_STRUCT(FSpawnParticlesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Spawn requests from CPU
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUSpawnRequest>, SpawnRequests)

		// Output: Particle buffer to write new particles into
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)

		// Atomic counter for particle count (RWStructuredBuffer<uint>)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCounter)

		// Per-source particle count (atomic counters indexed by SourceID)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SourceCounters)

		// Spawn parameters
		SHADER_PARAMETER(int32, SpawnRequestCount)
		SHADER_PARAMETER(int32, MaxParticleCount)
		SHADER_PARAMETER(int32, NextParticleID)
		SHADER_PARAMETER(int32, MaxSourceCount)
		SHADER_PARAMETER(float, DefaultRadius)
		SHADER_PARAMETER(float, DefaultMass)
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
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), ThreadGroupSize);
	}
};


//=============================================================================
// ID-Based Despawn Particles Compute Shader
// Marks particles for removal by matching ParticleID against sorted ID list
// Uses binary search for O(log n) lookup per particle
//=============================================================================
class FMarkDespawnByIDCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FMarkDespawnByIDCS);
	SHADER_USE_PARAMETER_STRUCT(FMarkDespawnByIDCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Sorted array of particle IDs to despawn
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, DespawnIDs)

		// Input: Particle buffer to check
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)

		// Output: Alive mask (1 = alive, 0 = dead)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutAliveMask)

		// Per-source particle count (decrement when particle is marked for removal)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SourceCounters)

		// Parameters
		SHADER_PARAMETER(int32, DespawnIDCount)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(int32, MaxSourceCount)
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

class FPrefixSumBlockCS_RDG : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPrefixSumBlockCS_RDG);
	SHADER_USE_PARAMETER_STRUCT(FPrefixSumBlockCS_RDG, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, MarkedFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, PrefixSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BlockSums)
		SHADER_PARAMETER(int32, ElementCount)
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

/**
 * Pass 2b: Scan Block Sums
 * Scans the block sums for multi-block prefix sum
 */
class FScanBlockSumsCS_RDG : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FScanBlockSumsCS_RDG);
	SHADER_USE_PARAMETER_STRUCT(FScanBlockSumsCS_RDG, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BlockSums)
		SHADER_PARAMETER(int32, BlockCount)
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

/**
 * Pass 2c: Add Block Offsets
 * Adds block offsets to get final prefix sums
 */
class FAddBlockOffsetsCS_RDG : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAddBlockOffsetsCS_RDG);
	SHADER_USE_PARAMETER_STRUCT(FAddBlockOffsetsCS_RDG, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, PrefixSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BlockSums)
		SHADER_PARAMETER(int32, ElementCount)
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

/**
 * Pass 3: Compact
 * Compacts marked particles into contiguous array
 */
class FCompactParticlesCS_RDG : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompactParticlesCS_RDG);
	SHADER_USE_PARAMETER_STRUCT(FCompactParticlesCS_RDG, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, MarkedFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PrefixSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, CompactedParticles)
		SHADER_PARAMETER(int32, ParticleCount)
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

/**
 * Pass 4: Write Total Count
 * Writes the total number of compacted particles
 */
class FWriteTotalCountCS_RDG : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FWriteTotalCountCS_RDG);
	SHADER_USE_PARAMETER_STRUCT(FWriteTotalCountCS_RDG, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PrefixSums)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, MarkedFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutTotalCount)
		SHADER_PARAMETER(int32, ParticleCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
//=============================================================================
// Extract Positions Compute Shader
// Utility: Extract positions from particle buffer for spatial hash
//=============================================================================

class FExtractPositionsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FExtractPositionsCS);
	SHADER_USE_PARAMETER_STRUCT(FExtractPositionsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Positions)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(int32, bUsePredictedPosition)
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
// GPU Fluid Simulator Pass Builder
// Utility class for adding compute passes to RDG
//=============================================================================

class KAWAIIFLUIDRUNTIME_API FGPUFluidSimulatorPassBuilder
{
public:
	/** Add primitive collision pass (explicit primitives from FluidCollider) */
	static void AddPrimitiveCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef SpheresSRV,
		FRDGBufferSRVRef CapsulesSRV,
		FRDGBufferSRVRef BoxesSRV,
		FRDGBufferSRVRef ConvexesSRV,
		FRDGBufferSRVRef ConvexPlanesSRV,
		int32 SphereCount,
		int32 CapsuleCount,
		int32 BoxCount,
		int32 ConvexCount,
		int32 ParticleCount,
		float ParticleRadius,
		float CollisionThreshold);

	/** Add extract render data pass (Phase 2: GPU physics → GPU render) */
	static void AddExtractRenderDataPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef PhysicsParticlesSRV,
		FRDGBufferUAVRef RenderParticlesUAV,
		int32 ParticleCount,
		float ParticleRadius);

	/** Add merged extract render data + bounds calculation pass (Optimized) */
	static void AddExtractRenderDataWithBoundsPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef PhysicsParticlesSRV,
		FRDGBufferUAVRef RenderParticlesUAV,
		FRDGBufferUAVRef BoundsBufferUAV,
		int32 ParticleCount,
		float ParticleRadius,
		float BoundsMargin);

	/** Add extract render data SoA pass (Memory bandwidth optimized) */
	static void AddExtractRenderDataSoAPass(
	   FRDGBuilder& GraphBuilder,
	   FRDGBufferSRVRef PhysicsParticlesSRV,
	   FRDGBufferUAVRef RenderPositionsUAV,
	   FRDGBufferUAVRef RenderVelocitiesUAV,
	   int32 ParticleCount,
	   float ParticleRadius,
	   float DeltaTime);  // For 1-frame delay compensation
};

//=============================================================================
// GPU Adhesion Compute Shaders
// Bone-based attachment tracking without CPU readback
//=============================================================================

/**
 * Adhesion Compute Shader
 * Checks particles near primitives and creates attachments
 */
class FAdhesionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAdhesionCS);
	SHADER_USE_PARAMETER_STRUCT(FAdhesionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Particle and attachment buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUParticleAttachment>, Attachments)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, ParticleRadius)

		// Bone transforms
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoneTransform>, BoneTransforms)
		SHADER_PARAMETER(int32, BoneCount)

		// Collision primitives
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionSphere>, CollisionSpheres)
		SHADER_PARAMETER(int32, SphereCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionCapsule>, CollisionCapsules)
		SHADER_PARAMETER(int32, CapsuleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionBox>, CollisionBoxes)
		SHADER_PARAMETER(int32, BoxCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionConvex>, CollisionConvexes)
		SHADER_PARAMETER(int32, ConvexCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUConvexPlane>, ConvexPlanes)

		// Adhesion parameters
		SHADER_PARAMETER(float, AdhesionStrength)
		SHADER_PARAMETER(float, AdhesionRadius)
		SHADER_PARAMETER(float, DetachAccelThreshold)
		SHADER_PARAMETER(float, DetachDistanceThreshold)
		SHADER_PARAMETER(float, ColliderContactOffset)
		SHADER_PARAMETER(float, BoneVelocityScale)
		SHADER_PARAMETER(float, SlidingFriction)
		SHADER_PARAMETER(float, CurrentTime)
		SHADER_PARAMETER(float, DeltaTime)
		SHADER_PARAMETER(int32, bEnableAdhesion)
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

/**
 * Update Attached Positions Compute Shader
 * Moves attached particles with bone transforms
 */
class FUpdateAttachedPositionsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUpdateAttachedPositionsCS);
	SHADER_USE_PARAMETER_STRUCT(FUpdateAttachedPositionsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUParticleAttachment>, Attachments)
		SHADER_PARAMETER(int32, ParticleCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoneTransform>, BoneTransforms)
		SHADER_PARAMETER(int32, BoneCount)

		// Primitives for surface distance check
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionSphere>, CollisionSpheres)
		SHADER_PARAMETER(int32, SphereCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionCapsule>, CollisionCapsules)
		SHADER_PARAMETER(int32, CapsuleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionBox>, CollisionBoxes)
		SHADER_PARAMETER(int32, BoxCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionConvex>, CollisionConvexes)
		SHADER_PARAMETER(int32, ConvexCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUConvexPlane>, ConvexPlanes)

		SHADER_PARAMETER(float, DetachAccelThreshold)
		SHADER_PARAMETER(float, DetachDistanceThreshold)
		SHADER_PARAMETER(float, ColliderContactOffset)
		SHADER_PARAMETER(float, BoneVelocityScale)
		SHADER_PARAMETER(float, SlidingFriction)
		SHADER_PARAMETER(float, DeltaTime)

		// Gravity sliding parameters
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER(float, GravitySlidingScale)
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

/**
 * Clear Detached Flag Compute Shader
 * Clears the JUST_DETACHED flag at end of frame
 */
class FClearDetachedFlagCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClearDetachedFlagCS);
	SHADER_USE_PARAMETER_STRUCT(FClearDetachedFlagCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
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
// Stack Pressure Compute Shader
// Applies weight transfer from stacked attached particles for realistic dripping
// Particles higher up transfer their weight to particles below
//=============================================================================

//=============================================================================
// Boundary Adhesion Compute Shaders (Flex-style Adhesion)
// Applies adhesion forces between fluid particles and boundary particles
// Based on Akinci 2012 "Versatile Rigid-Fluid Coupling"
// Optimized with Spatial Hash for O(N) complexity
//=============================================================================

// Clear boundary spatial hash
class FClearBoundaryHashCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClearBoundaryHashCS);
	SHADER_USE_PARAMETER_STRUCT(FClearBoundaryHashCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBoundaryCellCounts)
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

// Build boundary spatial hash
class FBuildBoundaryHashCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildBoundaryHashCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildBoundaryHashCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER(float, BoundaryCellSize)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBoundaryCellCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBoundaryParticleIndices)
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

// Boundary Attachment (Flex-style - check and attach)
class FBoundaryAttachCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoundaryAttachCS);
	SHADER_USE_PARAMETER_STRUCT(FBoundaryAttachCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUBoundaryAttachment>, BoundaryAttachments)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryParticleIndices)
		SHADER_PARAMETER(float, BoundaryCellSize)
		SHADER_PARAMETER(float, AdhesionStrength)
		SHADER_PARAMETER(float, AdhesionRadius)
		SHADER_PARAMETER(float, CurrentTime)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 256;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{ return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{ FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), ThreadGroupSize); }
};

// Update Boundary Attached (Flex-style - position constraint)
class FUpdateBoundaryAttachedCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUpdateBoundaryAttachedCS);
	SHADER_USE_PARAMETER_STRUCT(FUpdateBoundaryAttachedCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryAttachment>, BoundaryAttachments)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 256;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{ return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{ FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), ThreadGroupSize); }
};

// Boundary Detach (Flex-style - detachment check)
class FBoundaryDetachCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoundaryDetachCS);
	SHADER_USE_PARAMETER_STRUCT(FBoundaryDetachCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUBoundaryAttachment>, BoundaryAttachments)
		SHADER_PARAMETER(float, AdhesionRadius)
		SHADER_PARAMETER(float, DetachThreshold)
		SHADER_PARAMETER(float, DeltaTime)
		SHADER_PARAMETER(float, CurrentTime)
		SHADER_PARAMETER(FVector3f, Gravity)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 256;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{ return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{ FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), ThreadGroupSize); }
};

// Boundary adhesion (Force-based, Akinci 2013)
// Supports both Z-Order mode and Legacy Spatial Hash mode
class FBoundaryAdhesionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoundaryAdhesionCS);
	SHADER_USE_PARAMETER_STRUCT(FBoundaryAdhesionCS, FGlobalShader);

	// Permutation domain for grid resolution (Z-Order neighbor search)
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER(int32, ParticleCount)
		// Legacy boundary particles (unsorted)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		// Legacy Spatial Hash (fallback mode)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryParticleIndices)
		SHADER_PARAMETER(float, BoundaryCellSize)
		// Z-Order sorted boundary particles (new mode)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, SortedBoundaryParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellEnd)
		SHADER_PARAMETER(int32, bUseBoundaryZOrder)
		// Z-Order bounds (must match fluid simulation)
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)
		SHADER_PARAMETER(float, CellSize)
		// Adhesion parameters
		SHADER_PARAMETER(float, AdhesionForceStrength)
		SHADER_PARAMETER(float, AdhesionRadius)
		SHADER_PARAMETER(float, CohesionStrength)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER(float, DeltaTime)
		SHADER_PARAMETER(float, RestDensity)
		SHADER_PARAMETER(float, Poly6Coeff)
		// Boundary Owner AABB for particle-level early-out
		// Skip adhesion calculation for particles far from boundary AABB
		SHADER_PARAMETER(FVector3f, BoundaryAABBMin)
		SHADER_PARAMETER(FVector3f, BoundaryAABBMax)
		SHADER_PARAMETER(int32, bUseBoundaryAABBCulling)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 256;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{ return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREAD_GROUP_SIZE"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);

		// Get grid resolution from permutation for Z-Order neighbor search
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 GridSize = GridResolutionPermutation::GetGridResolution(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MORTON_GRID_SIZE"), GridSize);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

//=============================================================================
// GPU Boundary Skinning Compute Shader
// Transforms bone-local boundary particles to world space using bone transforms
// Runs once per frame for each FluidInteractionComponent
//=============================================================================

class FBoundarySkinningCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoundarySkinningCS);
	SHADER_USE_PARAMETER_STRUCT(FBoundarySkinningCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Local boundary particles (persistent, uploaded once)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticleLocal>, LocalBoundaryParticles)
		// Output: World-space boundary particles (updated each frame)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUBoundaryParticle>, WorldBoundaryParticles)
		// Previous frame positions for velocity calculation
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, PreviousWorldBoundaryParticles)
		// Bone transforms (uploaded each frame)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4x4>, BoneTransforms)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER(int32, BoneCount)
		SHADER_PARAMETER(int32, OwnerID)
		SHADER_PARAMETER(int32, bHasPreviousFrame)
		// Fallback transform for static meshes (BoneIndex == -1)
		SHADER_PARAMETER(FMatrix44f, ComponentTransform)
		// Delta time for velocity calculation
		SHADER_PARAMETER(float, DeltaTime)
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

class FStackPressureCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FStackPressureCS);
	SHADER_USE_PARAMETER_STRUCT(FStackPressureCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Particle and attachment buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUParticleAttachment>, Attachments)

		// Spatial hash for neighbor search
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ParticleIndices)

		// Collision primitives (for surface normal calculation)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionSphere>, CollisionSpheres)
		SHADER_PARAMETER(int32, SphereCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionCapsule>, CollisionCapsules)
		SHADER_PARAMETER(int32, CapsuleCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionBox>, CollisionBoxes)
		SHADER_PARAMETER(int32, BoxCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionConvex>, CollisionConvexes)
		SHADER_PARAMETER(int32, ConvexCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUConvexPlane>, ConvexPlanes)

		// Parameters
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, StackPressureScale)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER(float, DeltaTime)
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
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);
	}
};

//=============================================================================
// Z-Order (Morton Code) Sorting Shaders
// GPU-based spatial sorting for cache-coherent neighbor access
// Pipeline: Morton Code → Radix Sort → Reorder → CellStart/End
//=============================================================================

//=============================================================================
// Z-Order Sorting Configuration
//
// Key Design: Only TWO parameters drive everything:
//   1. GridAxisBits (GPU_MORTON_GRID_AXIS_BITS) - bits per axis
//   2. SmoothingRadius (from Preset) - determines Cell Size
//
// All other values are auto-calculated:
//   - GridResolution = 2^GridAxisBits (e.g., 2^7 = 128)
//   - MortonCodeBits = GridAxisBits × 3 (e.g., 7 × 3 = 21 bits)
//   - MaxCells = GridResolution^3 (e.g., 128^3 = 2,097,152)
//   - CellSize = SmoothingRadius (1:1 ratio, optimal for SPH neighbor search)
//   - SimulationBounds = GridResolution × CellSize (e.g., 128 × 20 = 2560cm)
//
// Example with GridAxisBits=7, SmoothingRadius=20cm:
//   - GridResolution = 128
//   - MortonCodeBits = 21-bit
//   - MaxCells = 2,097,152
//   - CellSize = 20cm
//   - SimulationBounds = ±1280cm (25.6m total)
//=============================================================================

// Morton grid constants (must match FluidMortonCode.usf and FluidCellStartEnd.usf)
// 7 bits per axis → 21-bit Morton code → 2M cells max
#define GPU_MORTON_GRID_AXIS_BITS 7
#define GPU_MORTON_GRID_SIZE (1 << GPU_MORTON_GRID_AXIS_BITS)  // 128 (2^7)

//=============================================================================
// Cell Start/End Index Shaders
// Build grid lookup structure from sorted Morton codes
//=============================================================================

// MAX_CELLS = GridResolution^3 (must match FluidCellStartEnd.usf)
// 128^3 = 2,097,152 cells (21-bit Morton code = Cell ID)
#define GPU_MAX_CELLS (GPU_MORTON_GRID_SIZE * GPU_MORTON_GRID_SIZE * GPU_MORTON_GRID_SIZE)  // 2,097,152

/**
 * Compute Morton Codes Compute Shader
 * Converts 3D particle positions to 1D Morton codes for spatial sorting
 * IMPORTANT: Uses PredictedPosition to match Solver's neighbor search
 *
 * Supports GridResolutionPreset permutation (Small/Medium/Large)
 */
class FComputeMortonCodesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FComputeMortonCodesCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeMortonCodesCS, FGlobalShader);

	// Permutation domain for grid resolution
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Full particle structure to access PredictedPosition (must match Solver)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MortonCodes)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleIndices)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsExtent)
		SHADER_PARAMETER(float, CellSize)
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

		// Get grid resolution from permutation
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 GridSize = GridResolutionPermutation::GetGridResolution(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MORTON_GRID_SIZE"), GridSize);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

//=============================================================================
// GPU Radix Sort Shaders
// 4-bit radix (16 buckets), auto-calculated passes for Morton code coverage
// Algorithm: Histogram → Global Prefix Sum → Scatter
//=============================================================================

#define GPU_RADIX_BITS 4
#define GPU_RADIX_SIZE 16  // 2^4 = 16 buckets
#define GPU_RADIX_THREAD_GROUP_SIZE 256
#define GPU_RADIX_ELEMENTS_PER_THREAD 4
#define GPU_RADIX_ELEMENTS_PER_GROUP (GPU_RADIX_THREAD_GROUP_SIZE * GPU_RADIX_ELEMENTS_PER_THREAD)  // 1024

// Auto-calculated: Morton code bits and required sort passes
// Morton code bits = GridAxisBits × 3 (X, Y, Z interleaved)
// Sort passes = ceil(MortonCodeBits / RadixBits)
#define GPU_MORTON_CODE_BITS (GPU_MORTON_GRID_AXIS_BITS * 3)  // 7 × 3 = 21 bits
#define GPU_RADIX_SORT_PASSES ((GPU_MORTON_CODE_BITS + GPU_RADIX_BITS - 1) / GPU_RADIX_BITS)  // ceil(21/4) = 6 passes

/**
 * Radix Sort Histogram Compute Shader
 * Pass 1: Count occurrences of each digit value per block
 */
class FRadixSortHistogramCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadixSortHistogramCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortHistogramCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, KeysIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ValuesIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Histogram)
		SHADER_PARAMETER(int32, ElementCount)
		SHADER_PARAMETER(int32, BitOffset)
		SHADER_PARAMETER(int32, NumGroups)
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
		OutEnvironment.SetDefine(TEXT("RADIX_BITS"), GPU_RADIX_BITS);
		OutEnvironment.SetDefine(TEXT("RADIX_SIZE"), GPU_RADIX_SIZE);
	}
};

/**
 * Radix Sort Global Prefix Sum Compute Shader
 * Pass 2a: Compute prefix sums across all blocks for each bucket
 */
class FRadixSortGlobalPrefixSumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadixSortGlobalPrefixSumCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortGlobalPrefixSumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Histogram)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, GlobalOffsets)
		SHADER_PARAMETER(int32, NumGroups)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 16;  // One thread per bucket

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
		OutEnvironment.SetDefine(TEXT("RADIX_BITS"), GPU_RADIX_BITS);
		OutEnvironment.SetDefine(TEXT("RADIX_SIZE"), GPU_RADIX_SIZE);
	}
};

/**
 * Radix Sort Bucket Prefix Sum Compute Shader
 * Pass 2b: Compute prefix sum across buckets (for global offsets)
 */
class FRadixSortBucketPrefixSumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadixSortBucketPrefixSumCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortBucketPrefixSumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, GlobalOffsets)
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
		OutEnvironment.SetDefine(TEXT("RADIX_BITS"), GPU_RADIX_BITS);
		OutEnvironment.SetDefine(TEXT("RADIX_SIZE"), GPU_RADIX_SIZE);
	}
};

/**
 * Radix Sort Scatter Compute Shader
 * Pass 3: Scatter elements to their sorted positions
 */
class FRadixSortScatterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadixSortScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortScatterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, KeysIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ValuesIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, KeysOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ValuesOut)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HistogramSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GlobalOffsetsSRV)
		SHADER_PARAMETER(int32, ElementCount)
		SHADER_PARAMETER(int32, BitOffset)
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
		OutEnvironment.SetDefine(TEXT("RADIX_BITS"), GPU_RADIX_BITS);
		OutEnvironment.SetDefine(TEXT("RADIX_SIZE"), GPU_RADIX_SIZE);
	}
};

/**
 * Radix Sort Small Array Compute Shader
 * Optimized single-pass sort for small arrays (<1024 elements)
 */
class FRadixSortSmallCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadixSortSmallCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortSmallCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, KeysIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, KeysOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ValuesIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ValuesOut)
		SHADER_PARAMETER(int32, ElementCount)
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
// ParticleID Radix Sort Shaders
// Sort particles by ParticleID for CPU readback optimization.
// Reads ParticleID directly from Particles buffer (first pass only).
//=============================================================================

/**
 * ParticleID Histogram Compute Shader
 * Reads ParticleID directly from Particles buffer instead of KeysIn
 */
class FRadixSortHistogramParticleIDCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadixSortHistogramParticleIDCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortHistogramParticleIDCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Histogram)
		SHADER_PARAMETER(int32, ElementCount)
		SHADER_PARAMETER(int32, BitOffset)
		SHADER_PARAMETER(int32, NumGroups)
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
		OutEnvironment.SetDefine(TEXT("RADIX_BITS"), GPU_RADIX_BITS);
		OutEnvironment.SetDefine(TEXT("RADIX_SIZE"), GPU_RADIX_SIZE);
		OutEnvironment.SetDefine(TEXT("ELEMENTS_PER_THREAD"), GPU_RADIX_ELEMENTS_PER_THREAD);
	}
};

/**
 * ParticleID Scatter Compute Shader
 * Reads ParticleID directly from Particles buffer, outputs to Keys/Values
 */
class FRadixSortScatterParticleIDCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadixSortScatterParticleIDCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortScatterParticleIDCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, KeysOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ValuesOut)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, HistogramSRV)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GlobalOffsetsSRV)
		SHADER_PARAMETER(int32, ElementCount)
		SHADER_PARAMETER(int32, BitOffset)
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
		OutEnvironment.SetDefine(TEXT("RADIX_BITS"), GPU_RADIX_BITS);
		OutEnvironment.SetDefine(TEXT("RADIX_SIZE"), GPU_RADIX_SIZE);
		OutEnvironment.SetDefine(TEXT("ELEMENTS_PER_THREAD"), GPU_RADIX_ELEMENTS_PER_THREAD);
	}
};

//=============================================================================
// Particle Reordering Shaders
// Physically reorder particle data based on sorted indices
//=============================================================================

/**
 * Reorder Particles Compute Shader
 * Physically reorders particle data for cache-coherent access
 */
class FReorderParticlesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReorderParticlesCS);
	SHADER_USE_PARAMETER_STRUCT(FReorderParticlesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, OldParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, SortedParticles)
		SHADER_PARAMETER(int32, ParticleCount)
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

/**
 * Build Reverse Mapping Compute Shader
 * Builds mapping from old indices to new sorted positions
 */
class FBuildReverseMappingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildReverseMappingCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildReverseMappingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OldToNewMapping)
		SHADER_PARAMETER(int32, ParticleCount)
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

/**
 * Clear Cell Indices Compute Shader
 * Initializes CellStart/End arrays to invalid values
 */
class FClearCellIndicesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClearCellIndicesCS);
	SHADER_USE_PARAMETER_STRUCT(FClearCellIndicesCS, FGlobalShader);

	// Permutation domain for grid resolution
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellEnd)
	END_SHADER_PARAMETER_STRUCT()

	// Increased to 512 to avoid exceeding GPU dispatch limit (65535) with Large preset
	// Large preset: 256³ = 16,777,216 cells / 512 = 32,768 groups (under limit)
	static constexpr int32 ThreadGroupSize = 512;

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

		// Get grid resolution from permutation
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

/**
 * Compute Cell Start/End Compute Shader
 * Finds where each cell's particles begin and end in sorted array
 */
class FComputeCellStartEndCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FComputeCellStartEndCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeCellStartEndCS, FGlobalShader);

	// Permutation domain for grid resolution
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedMortonCodes)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellEnd)
		SHADER_PARAMETER(int32, ParticleCount)
	END_SHADER_PARAMETER_STRUCT()

	// Increased to 512 to match FluidCellStartEnd.usf
	static constexpr int32 ThreadGroupSize = 512;

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

		// Get grid resolution from permutation
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

//=============================================================================
// Boundary Particle Z-Order Sorting Shaders
// Enables O(K) neighbor search for boundary particles instead of O(M) traversal
// Pipeline: Morton Code → Radix Sort → Reorder → BoundaryCellStart/End
//=============================================================================

/**
 * Compute Boundary Morton Codes Compute Shader
 * Converts boundary particle positions to Morton codes for Z-Order sorting
 */
class FComputeBoundaryMortonCodesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FComputeBoundaryMortonCodesCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeBoundaryMortonCodesCS, FGlobalShader);

	// Permutation domain for grid resolution
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticlesIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BoundaryMortonCodes)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BoundaryParticleIndices)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(float, CellSize)
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

		// Get grid resolution from permutation
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 GridSize = GridResolutionPermutation::GetGridResolution(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MORTON_GRID_SIZE"), GridSize);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

/**
 * Clear Boundary Cell Indices Compute Shader
 * Initializes BoundaryCellStart/End arrays to INVALID_INDEX
 */
class FClearBoundaryCellIndicesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClearBoundaryCellIndicesCS);
	SHADER_USE_PARAMETER_STRUCT(FClearBoundaryCellIndicesCS, FGlobalShader);

	// Permutation domain for grid resolution
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BoundaryCellStart)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BoundaryCellEnd)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 512;

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

		// Get grid resolution from permutation
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

/**
 * Reorder Boundary Particles Compute Shader
 * Physically reorders boundary particles based on sorted Morton code indices
 */
class FReorderBoundaryParticlesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReorderBoundaryParticlesCS);
	SHADER_USE_PARAMETER_STRUCT(FReorderBoundaryParticlesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, OldBoundaryParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedBoundaryIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUBoundaryParticle>, SortedBoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
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

/**
 * Compute Boundary Cell Start/End Compute Shader
 * Determines the range [start, end] of boundary particles in each cell
 * Must be called AFTER boundary particles are sorted by Morton code
 */
class FComputeBoundaryCellStartEndCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FComputeBoundaryCellStartEndCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeBoundaryCellStartEndCS, FGlobalShader);

	// Permutation domain for grid resolution
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SortedBoundaryMortonCodes)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BoundaryCellStart)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BoundaryCellEnd)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 512;

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

		// Get grid resolution from permutation
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};

//=============================================================================
// Boundary Attachment Update Compute Shader
// Manages strong position constraints between fluid and boundary particles
// Handles: Attach, Detach, Cooldown, and Position Constraint
//=============================================================================

class FBoundaryAttachmentUpdateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoundaryAttachmentUpdateCS);
	SHADER_USE_PARAMETER_STRUCT(FBoundaryAttachmentUpdateCS, FGlobalShader);

	// Permutation for grid resolution (used in Z-Order search)
	using FPermutationDomain = TShaderPermutationDomain<FGridResolutionDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Fluid particles (read-write) - now includes embedded attachment data (96 bytes)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FGPUFluidParticle>, Particles)
		// Boundary particles (Z-Order sorted, read-only)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, SortedBoundaryParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellEnd)
		// Owner transforms (fallback for component-level transform)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryOwnerTransform>, OwnerTransforms)
		SHADER_PARAMETER(int32, MaxOwnerID)
		// Bone transforms (for skeletal mesh attachment - follows bone animation)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FMatrix44f>, BoneTransforms)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FMatrix44f>, InverseBoneTransforms)
		SHADER_PARAMETER(int32, BoneCount)
		// Debug counter (atomic increment for attached particle count)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, AttachmentCounter)
		// Particle counts
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		// Attachment parameters
		SHADER_PARAMETER(float, AttachRadius)
		SHADER_PARAMETER(float, DetachDistanceMultiplier)
		SHADER_PARAMETER(float, DetachSpeedThreshold)
		SHADER_PARAMETER(float, AttachCooldown)
		SHADER_PARAMETER(float, ConstraintBlend)
		SHADER_PARAMETER(float, AdhesionStrength)  // For drift-based detach threshold
		SHADER_PARAMETER(float, CurrentTime)
		SHADER_PARAMETER(float, DeltaTime)
		// Z-Order parameters
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)
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

		// Get grid resolution from permutation
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const int32 GridPreset = PermutationVector.Get<FGridResolutionDim>();
		const int32 AxisBits = GridResolutionPermutation::GetAxisBits(GridPreset);
		const int32 MaxCells = GridResolutionPermutation::GetMaxCells(GridPreset);

		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), AxisBits);
		OutEnvironment.SetDefine(TEXT("MAX_CELLS"), MaxCells);
	}
};
