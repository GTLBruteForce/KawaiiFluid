// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "GPU/GPUFluidParticle.h"
#include "GPU/GPUFluidSimulatorShaders.h"  // For GPU_MORTON_GRID_AXIS_BITS

//=============================================================================
// Ray Marching Constants
//=============================================================================

// Volume build thread group sizes
#define VOLUME_BUILD_THREAD_GROUP_SIZE 8
#define TILE_CULL_THREAD_GROUP_SIZE 16

// Occupancy bitmask constants
#define OCCUPANCY_RESOLUTION 32
#define OCCUPANCY_UINT_COUNT 1024  // (32 * 32 * 32) / 32

// Tile size for screen-space culling
#define TILE_SIZE 16

//=============================================================================
// Build Density Volume Compute Shader
// Converts Z-Order sorted particles to 3D density volume
//=============================================================================

class FBuildDensityVolumeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildDensityVolumeCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildDensityVolumeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Z-Order sorted particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellEnd)

		// Output: 3D density volume
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, DensityVolume)

		// Parameters
		SHADER_PARAMETER(int32, VolumeResolution)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, Poly6Coeff)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMin)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMax)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = VOLUME_BUILD_THREAD_GROUP_SIZE;

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

		// CRITICAL: Must match Z-Order sorting's MORTON_GRID_AXIS_BITS
		// Otherwise Morton code computation will produce different cell IDs
		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), GPU_MORTON_GRID_AXIS_BITS);
	}
};

//=============================================================================
// Build SDF Volume Compute Shader
// Converts Z-Order sorted particles to 3D Signed Distance Field
//=============================================================================

class FBuildSDFVolumeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildSDFVolumeCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildSDFVolumeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Z-Order sorted particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellEnd)

		// Output: 3D SDF volume
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, SDFVolume)

		// Parameters
		SHADER_PARAMETER(int32, VolumeResolution)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, SmoothK)
		SHADER_PARAMETER(float, SurfaceOffset)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMin)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMax)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)

		// Tight AABB (GPU-only) - 시뮬레이션 bounds로 clamp됨
		SHADER_PARAMETER(FVector3f, SimulationBoundsMin)
		SHADER_PARAMETER(FVector3f, SimulationBoundsMax)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FluidAABB)
		SHADER_PARAMETER(uint32, bUseTightAABB)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = VOLUME_BUILD_THREAD_GROUP_SIZE;

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
		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), GPU_MORTON_GRID_AXIS_BITS);
	}
};

//=============================================================================
// Build Occupancy Mask Compute Shader
// Creates 32³ bit mask for O(1) empty block detection
//=============================================================================

class FBuildOccupancyMaskCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildOccupancyMaskCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildOccupancyMaskCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Density volume
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, DensityVolume)

		// Output: Occupancy bitmask (32³ bits = 1024 uint32)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OccupancyMask)

		// Parameters
		SHADER_PARAMETER(int32, VolumeResolution)
		SHADER_PARAMETER(float, DensityThreshold)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = VOLUME_BUILD_THREAD_GROUP_SIZE;

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
		OutEnvironment.SetDefine(TEXT("OCCUPANCY_RESOLUTION"), OCCUPANCY_RESOLUTION);
	}
};

//=============================================================================
// Build MinMax Mipmap Level 0 Compute Shader
// Downsamples density volume to MinMax mip level 0 (128³)
//=============================================================================

class FBuildMinMaxMipLevel0CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildMinMaxMipLevel0CS);
	SHADER_USE_PARAMETER_STRUCT(FBuildMinMaxMipLevel0CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Density volume (256³)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, DensityVolume)
		SHADER_PARAMETER_SAMPLER(SamplerState, DensitySampler)

		// Output: MinMax mip level 0 (128³)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float2>, MinMaxMipLevel0)

		// Parameters
		SHADER_PARAMETER(int32, InputResolution)
		SHADER_PARAMETER(int32, OutputResolution)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = VOLUME_BUILD_THREAD_GROUP_SIZE;

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
// Build MinMax Mipmap Chain Compute Shader
// Builds subsequent mip levels from previous level
//=============================================================================

class FBuildMinMaxMipChainCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildMinMaxMipChainCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildMinMaxMipChainCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Previous mip level
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float2>, InputMipLevel)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)

		// Output: Current mip level
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float2>, OutputMipLevel)

		// Parameters
		SHADER_PARAMETER(int32, InputResolution)
		SHADER_PARAMETER(int32, OutputResolution)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = VOLUME_BUILD_THREAD_GROUP_SIZE;

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
// Compute Fluid AABB Compute Shader
// Calculates tight bounding box around fluid particles using parallel reduction
//=============================================================================

class FComputeFluidAABBCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FComputeFluidAABBCS);
	SHADER_USE_PARAMETER_STRUCT(FComputeFluidAABBCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)

		// Output: AABB as sortable uint (6 values: min.xyz, max.xyz)
		// Uses uint for atomic operations with float bit manipulation
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FluidAABB)

		// Parameters
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
// Initialize Fluid AABB Compute Shader
// Sets extreme values before parallel reduction
//=============================================================================

class FInitFluidAABBCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FInitFluidAABBCS);
	SHADER_USE_PARAMETER_STRUCT(FInitFluidAABBCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Output: AABB buffer to initialize
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FluidAABB)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

//=============================================================================
// Tile Culling Compute Shader
// Determines which screen tiles intersect with fluid AABB
//=============================================================================

class FTileCullingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FTileCullingCS);
	SHADER_USE_PARAMETER_STRUCT(FTileCullingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Fluid AABB as sortable uint (needs SortableUintToFloat conversion)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FluidAABB)

		// Input: Depth buffer for conservative depth bounds
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SceneDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, DepthSampler)

		// Output: Tile visibility mask (1 bit per tile)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TileVisibility)

		// Output: Indirect dispatch args for visible tiles
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndirectArgs)

		// Parameters
		SHADER_PARAMETER(int32, TilesX)
		SHADER_PARAMETER(int32, TilesY)
		SHADER_PARAMETER(FVector2f, ViewportSize)
		SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMin)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMax)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = TILE_CULL_THREAD_GROUP_SIZE;

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
		OutEnvironment.SetDefine(TEXT("TILE_SIZE"), TILE_SIZE);
	}
};

//=============================================================================
// Ray Marching Main Pixel Shader
// Performs volumetric ray marching through density volume
//=============================================================================

class FRayMarchingMainPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRayMarchingMainPS);
	SHADER_USE_PARAMETER_STRUCT(FRayMarchingMainPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// View uniform buffer (required for View.ViewToClip access in shader)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Volume textures
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, DensityVolume)
		SHADER_PARAMETER_SAMPLER(SamplerState, DensitySampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float2>, MinMaxMipmap)
		SHADER_PARAMETER_SAMPLER(SamplerState, MinMaxSampler)

		// Occupancy mask
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, OccupancyMask)

		// Scene textures
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SceneDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, DepthSampler)

		// History textures (for temporal reprojection)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, HistoryColor)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, HistoryDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, HistorySampler)

		// Tile visibility
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileVisibility)

		// Output
		RENDER_TARGET_BINDING_SLOTS()

		// Volume parameters
		SHADER_PARAMETER(int32, VolumeResolution)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMin)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMax)

		// Ray marching parameters
		SHADER_PARAMETER(int32, MaxSteps)
		SHADER_PARAMETER(float, DensityThreshold)
		SHADER_PARAMETER(float, AdaptiveStepMultiplier)
		SHADER_PARAMETER(float, EarlyTerminationAlpha)

		// Optimization flags
		SHADER_PARAMETER(uint32, bEnableOccupancyMask)
		SHADER_PARAMETER(uint32, bEnableMinMaxMipmap)
		SHADER_PARAMETER(uint32, bEnableTileCulling)
		SHADER_PARAMETER(uint32, bEnableTemporalReprojection)
		SHADER_PARAMETER(float, TemporalBlendFactor)

		// Appearance parameters
		SHADER_PARAMETER(FLinearColor, FluidColor)
		SHADER_PARAMETER(float, FresnelStrength)
		SHADER_PARAMETER(float, RefractiveIndex)
		SHADER_PARAMETER(float, AbsorptionCoefficient)
		SHADER_PARAMETER(FLinearColor, AbsorptionColorCoefficients)
		SHADER_PARAMETER(float, SpecularStrength)
		SHADER_PARAMETER(float, SpecularRoughness)

		// View parameters
		SHADER_PARAMETER(FVector2f, ViewportSize)
		SHADER_PARAMETER(FVector3f, CameraPosition)
		SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
		SHADER_PARAMETER(FMatrix44f, PrevViewProjectionMatrix)
		SHADER_PARAMETER(FVector3f, SunDirection)
		SHADER_PARAMETER(FLinearColor, SunColor)

		// Tile parameters
		SHADER_PARAMETER(int32, TilesX)

		// Frame index for temporal jittering
		SHADER_PARAMETER(uint32, FrameIndex)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

//=============================================================================
// SDF Ray Marching (Sphere Tracing) Pixel Shader
// Uses Signed Distance Field for efficient rendering with translucency
//=============================================================================

class FRayMarchingSDFPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRayMarchingSDFPS);
	SHADER_USE_PARAMETER_STRUCT(FRayMarchingSDFPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// View uniform buffer
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// SDF volume texture
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFVolume)
		SHADER_PARAMETER_SAMPLER(SamplerState, SDFSampler)

		// Scene textures
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SceneDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, DepthSampler)

		// History textures (for temporal reprojection)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, HistoryColor)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, HistoryDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, HistorySampler)

		// Tile visibility
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileVisibility)

		//========================================
		// Z-Order Particle Data (for Hybrid Mode)
		//========================================
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellEnd)
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, SDFSmoothness)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)
		SHADER_PARAMETER(uint32, bEnableHybridMode)
		SHADER_PARAMETER(float, HybridThreshold)

		// Output
		RENDER_TARGET_BINDING_SLOTS()

		// Volume parameters
		SHADER_PARAMETER(int32, VolumeResolution)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMin)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMax)

		// Tight AABB (GPU-only) - 시뮬레이션 bounds로 clamp됨
		SHADER_PARAMETER(FVector3f, SimulationBoundsMin)
		SHADER_PARAMETER(FVector3f, SimulationBoundsMax)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FluidAABB)
		SHADER_PARAMETER(uint32, bUseTightAABB)
		SHADER_PARAMETER(uint32, bDebugVisualizeTightAABB)

		// Sphere Tracing parameters
		SHADER_PARAMETER(int32, MaxSteps)
		SHADER_PARAMETER(float, SurfaceEpsilon)
		SHADER_PARAMETER(float, MinStepSize)
		SHADER_PARAMETER(float, MaxStepSize)
		SHADER_PARAMETER(float, RelaxationFactor)

		// Translucency parameters
		SHADER_PARAMETER(float, TranslucencyDepth)
		SHADER_PARAMETER(float, TranslucencyDensity)
		SHADER_PARAMETER(float, SubsurfaceScatterStrength)
		SHADER_PARAMETER(FVector3f, SubsurfaceColor)

		// Optimization flags
		SHADER_PARAMETER(uint32, bEnableTileCulling)
		SHADER_PARAMETER(uint32, bEnableTemporalReprojection)
		SHADER_PARAMETER(float, TemporalBlendFactor)

		// Appearance parameters
		SHADER_PARAMETER(FLinearColor, FluidColor)
		SHADER_PARAMETER(float, FresnelStrength)
		SHADER_PARAMETER(float, RefractiveIndex)
		SHADER_PARAMETER(float, AbsorptionCoefficient)
		SHADER_PARAMETER(FLinearColor, AbsorptionColorCoefficients)
		SHADER_PARAMETER(float, SpecularStrength)
		SHADER_PARAMETER(float, SpecularRoughness)
		SHADER_PARAMETER(float, ReflectionStrength)

		// View parameters
		SHADER_PARAMETER(FVector2f, ViewportSize)
		SHADER_PARAMETER(FVector3f, CameraPosition)
		SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
		SHADER_PARAMETER(FMatrix44f, PrevViewProjectionMatrix)
		SHADER_PARAMETER(FVector3f, SunDirection)
		SHADER_PARAMETER(FLinearColor, SunColor)

		// Tile parameters
		SHADER_PARAMETER(int32, TilesX)

		// Frame index for temporal jittering
		SHADER_PARAMETER(uint32, FrameIndex)
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
		// CRITICAL: Must match Z-Order sorting's MORTON_GRID_AXIS_BITS
		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), GPU_MORTON_GRID_AXIS_BITS);
	}
};

//=============================================================================
// Ray Marching Vertex Shader (Full screen quad)
//=============================================================================

class FRayMarchingVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRayMarchingVS);
	SHADER_USE_PARAMETER_STRUCT(FRayMarchingVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

//=============================================================================
// Ray Marching Composite Pixel Shader
// Simple pass-through for alpha blending fluid onto scene
//=============================================================================

class FRayMarchingCompositePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRayMarchingCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FRayMarchingCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, FluidColorTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, FluidColorSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

//=============================================================================
// Temporal Blend Pixel Shader
// Blends current frame with history for temporal stability
//=============================================================================

class FTemporalBlendPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FTemporalBlendPS);
	SHADER_USE_PARAMETER_STRUCT(FTemporalBlendPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Current frame
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, CurrentColor)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, CurrentDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, CurrentSampler)

		// History
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, HistoryColor)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, HistoryDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, HistorySampler)

		// Motion vectors
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, MotionVectors)
		SHADER_PARAMETER_SAMPLER(SamplerState, MotionSampler)

		// Output
		RENDER_TARGET_BINDING_SLOTS()

		// Parameters
		SHADER_PARAMETER(float, TemporalBlendFactor)
		SHADER_PARAMETER(float, DepthRejectionThreshold)
		SHADER_PARAMETER(FVector2f, ViewportSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

//=============================================================================
// Generate Motion Vectors Compute Shader
// Generates per-voxel motion vectors for temporal reprojection
//=============================================================================

class FGenerateMotionVectorsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGenerateMotionVectorsCS);
	SHADER_USE_PARAMETER_STRUCT(FGenerateMotionVectorsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Current and previous frame velocity volumes
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float3>, VelocityVolume)
		SHADER_PARAMETER_SAMPLER(SamplerState, VelocitySampler)

		// Output: 2D motion vectors
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, MotionVectors)

		// Parameters
		SHADER_PARAMETER(FVector2f, ViewportSize)
		SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
		SHADER_PARAMETER(FMatrix44f, PrevViewProjectionMatrix)
		SHADER_PARAMETER(float, DeltaTime)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = TILE_CULL_THREAD_GROUP_SIZE;

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
// Clear Volume Compute Shader
// Clears a 3D texture to zero
//=============================================================================

class FClearVolumeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClearVolumeCS);
	SHADER_USE_PARAMETER_STRUCT(FClearVolumeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, Volume)
		SHADER_PARAMETER(int32, VolumeResolution)
		SHADER_PARAMETER(float, ClearValue)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = VOLUME_BUILD_THREAD_GROUP_SIZE;

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
// Mark Voxel Occupancy Compute Shader (Sparse Voxel Pass 1)
// Marks voxels that are within particle influence range
//=============================================================================

class FMarkVoxelOccupancyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FMarkVoxelOccupancyCS);
	SHADER_USE_PARAMETER_STRUCT(FMarkVoxelOccupancyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)

		// Output: Active voxel bitmask (VolumeResolution³ / 32 uint32s)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ActiveVoxelMask)

		// Parameters
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(int32, VolumeResolution)
		SHADER_PARAMETER(float, SearchRadius)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMin)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMax)
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
// Build SDF Volume Sparse Compute Shader (Sparse Voxel Pass 2)
// Computes SDF only for active voxels, skips inactive ones
//=============================================================================

class FBuildSDFVolumeSparseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildSDFVolumeSparseCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildSDFVolumeSparseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Z-Order sorted particles
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUFluidParticle>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellEnd)

		// Input: Active voxel bitmask
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ActiveVoxelMask)

		// Output: 3D SDF volume
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, SDFVolume)

		// Parameters
		SHADER_PARAMETER(int32, VolumeResolution)
		SHADER_PARAMETER(float, ParticleRadius)
		SHADER_PARAMETER(float, SmoothK)
		SHADER_PARAMETER(float, SurfaceOffset)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMin)
		SHADER_PARAMETER(FVector3f, VolumeBoundsMax)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(FVector3f, MortonBoundsMin)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = VOLUME_BUILD_THREAD_GROUP_SIZE;

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
		OutEnvironment.SetDefine(TEXT("MORTON_GRID_AXIS_BITS"), GPU_MORTON_GRID_AXIS_BITS);
	}
};
