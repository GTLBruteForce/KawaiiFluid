// Copyright 2026 Team_Bruteforce. All Rights Reserved.

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
struct FGPUBoundaryParticle;
struct FGPUBoneDeltaAttachment;
struct FGPUCollisionSphere;
struct FGPUCollisionCapsule;
struct FGPUCollisionBox;

//=============================================================================
// GPU Compute Parameters for Anisotropy Shader Dispatch
// Contains all buffer references and parameters needed for GPU anisotropy calculation.
//=============================================================================

struct KAWAIIFLUIDRUNTIME_API FAnisotropyComputeParams
{
	// Input buffers (SoA - Structure of Arrays)
	FRDGBufferSRVRef PositionsSRV = nullptr;			// float3 positions as Buffer<float> [count*3]
	FRDGBufferSRVRef PackedVelocitiesSRV = nullptr;		// B plan: half3 packed as Buffer<uint2> [count]
	FRDGBufferSRVRef FlagsSRV = nullptr;				// uint flags as Buffer<uint> [count]
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

	// Render offset for surface particles (pull toward neighbors for rendering)
	FRDGBufferUAVRef OutRenderOffsetUAV = nullptr;
	float ParticleRadius = 0.0f;  // passed from simulation

	// Parameters
	int32 ParticleCount = 0;
	EGPUAnisotropyMode Mode = EGPUAnisotropyMode::DensityBased;

	// Velocity-based params
	float VelocityStretchFactor = 0.01f;

	// Common params
	float Strength = 1.0f;
	float MinStretch = 0.2f;
	float MaxStretch = 2.5f;

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

	// Boundary Particles for anisotropy calculation (covariance contribution)
	FRDGBufferSRVRef BoundaryParticlesSRV = nullptr;		// Legacy brute-force (FGPUBoundaryParticle)
	int32 BoundaryParticleCount = 0;
	bool bUseBoundaryAnisotropy = false;

	// Boundary Z-Order sorted buffers
	FRDGBufferSRVRef SortedBoundaryParticlesSRV = nullptr;
	FRDGBufferSRVRef BoundaryCellStartSRV = nullptr;
	FRDGBufferSRVRef BoundaryCellEndSRV = nullptr;
	bool bUseBoundaryZOrder = false;
	float BoundaryWeight = 1.0f;	// Weight for boundary contribution to covariance

	// Hybrid Tiled Z-Order mode (for unlimited simulation range)
	// When true, uses 21-bit Hybrid keys instead of classic Morton codes
	bool bUseHybridTiledZOrder = false;

	// Surface Normal Anisotropy for NEAR_BOUNDARY particles
	FRDGBufferSRVRef BoneDeltaAttachmentsSRV = nullptr;	// FGPUBoneDeltaAttachment for NEAR_BOUNDARY particles
	bool bEnableSurfaceNormalAnisotropy = true;			// Use surface normal for NEAR_BOUNDARY particles

	// Collision Primitives for direct surface normal calculation
	// These are bone colliders (Capsule/Box) already transformed to world space
	FRDGBufferSRVRef CollisionSpheresSRV = nullptr;
	FRDGBufferSRVRef CollisionCapsulesSRV = nullptr;
	FRDGBufferSRVRef CollisionBoxesSRV = nullptr;
	int32 SphereCount = 0;
	int32 CapsuleCount = 0;
	int32 BoxCount = 0;
	float ColliderSearchRadius = 50.0f;  // Search radius for finding closest collider normal

	// Temporal Smoothing parameters
	FRDGBufferSRVRef PrevAxis1SRV = nullptr;
	FRDGBufferSRVRef PrevAxis2SRV = nullptr;
	FRDGBufferSRVRef PrevAxis3SRV = nullptr;
	bool bEnableTemporalSmoothing = true;
	float TemporalSmoothFactor = 0.8f;  // 0.0 = no smoothing, 1.0 = previous frame only
	bool bHasPreviousFrame = false;

	// Volume Preservation (Yu & Turk vs FleX style)
	// true = log-space volume preservation (Scale1*Scale2*Scale3 = 1.0)
	// false = raw eigenvalues (FleX reference style, larger ellipsoids)
	bool bPreserveVolume = true;

	// FleX style render scale (only used when bPreserveVolume = false)
	// Controls overall ellipsoid size in FleX mode
	float FlexRenderScale = 1.0f;
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
		// SoA (Structure of Arrays) Particle Buffers
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, InPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint2>, InPackedVelocities)  // B plan: half3 packed
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InFlags)

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

		// Output: Render offset for surface particles (pull toward neighbors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector3f>, OutRenderOffset)
		SHADER_PARAMETER(float, ParticleRadius)

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
		SHADER_PARAMETER(int32, bUseHybridTiledZOrder)  // 1 = Hybrid Tiled Z-Order (unlimited range), 0 = Classic Morton

		// Attached particle anisotropy params
		SHADER_PARAMETER(float, AttachedFlattenScale)  // How flat (0.3 = 30% height)
		SHADER_PARAMETER(float, AttachedStretchScale)  // Perpendicular stretch

		// Boundary Particles for anisotropy calculation
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, BoundaryParticles)
		SHADER_PARAMETER(int32, BoundaryParticleCount)
		SHADER_PARAMETER(int32, bUseBoundaryAnisotropy)

		// Boundary Z-Order sorted buffers
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoundaryParticle>, SortedBoundaryParticles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BoundaryCellEnd)
		SHADER_PARAMETER(int32, bUseBoundaryZOrder)
		SHADER_PARAMETER(float, BoundaryWeight)

		// Temporal Smoothing
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, PrevAnisotropyAxis1)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, PrevAnisotropyAxis2)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, PrevAnisotropyAxis3)
		SHADER_PARAMETER(int32, bEnableTemporalSmoothing)
		SHADER_PARAMETER(float, TemporalSmoothFactor)
		SHADER_PARAMETER(int32, bHasPreviousFrame)

		// Surface Normal Anisotropy for NEAR_BOUNDARY particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBoneDeltaAttachment>, InBoneDeltaAttachments)
		SHADER_PARAMETER(int32, bEnableSurfaceNormalAnisotropy)

		// Collision Primitives for direct surface normal calculation (already in world space)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionSphere>, CollisionSpheres)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionCapsule>, CollisionCapsules)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollisionBox>, CollisionBoxes)
		SHADER_PARAMETER(int32, SphereCount)
		SHADER_PARAMETER(int32, CapsuleCount)
		SHADER_PARAMETER(int32, BoxCount)
		SHADER_PARAMETER(float, ColliderSearchRadius)

		// Volume Preservation mode
		// 1 = Yu & Turk style (log-space, volume = 1.0)
		// 0 = FleX style (raw eigenvalues, no volume constraint)
		SHADER_PARAMETER(int32, bPreserveVolume)

		// FleX style render scale (only used when bPreserveVolume = 0)
		SHADER_PARAMETER(float, FlexRenderScale)
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
