// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "GPU/GPUFluidParticle.h"

// Spatial hash constants (must match FluidSpatialHash.ush)
#define GPU_SPATIAL_HASH_SIZE 65536
#define GPU_MAX_PARTICLES_PER_CELL 16

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
// Compute Density Compute Shader
// Pass 3: Calculate density and lambda using spatial hash
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
// Solve Pressure Compute Shader
// Pass 4: Apply position corrections based on density constraints
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
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);
	}
};

//=============================================================================
// Apply Viscosity Compute Shader
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
		SHADER_PARAMETER(int32, ParticleCount)
		SHADER_PARAMETER(float, SmoothingRadius)
		SHADER_PARAMETER(float, ViscosityCoefficient)
		SHADER_PARAMETER(float, Poly6Coeff)
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
		OutEnvironment.SetDefine(TEXT("SPATIAL_HASH_SIZE"), GPU_SPATIAL_HASH_SIZE);
		OutEnvironment.SetDefine(TEXT("MAX_PARTICLES_PER_CELL"), GPU_MAX_PARTICLES_PER_CELL);
	}
};

//=============================================================================
// Bounds Collision Compute Shader
// Pass 6: Apply AABB bounds collision
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
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsMax)
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
		SHADER_PARAMETER(float, VelocityDamping)
		SHADER_PARAMETER(float, MaxVelocity)
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

		// Spawn parameters
		SHADER_PARAMETER(int32, SpawnRequestCount)
		SHADER_PARAMETER(int32, MaxParticleCount)
		SHADER_PARAMETER(int32, NextParticleID)
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
	/** Add predict positions pass */
	static void AddPredictPositionsPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		int32 ParticleCount,
		float DeltaTime,
		const FVector3f& Gravity,
		const FVector3f& ExternalForce);

	/** Add compute density pass */
	static void AddComputeDensityPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add solve pressure pass */
	static void AddSolvePressurePass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add apply viscosity pass */
	static void AddApplyViscosityPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGBufferSRVRef CellCountsSRV,
		FRDGBufferSRVRef ParticleIndicesSRV,
		const FGPUFluidSimulationParams& Params);

	/** Add bounds collision pass */
	static void AddBoundsCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		const FGPUFluidSimulationParams& Params);

	/** Add distance field collision pass (UE5 Global Distance Field) */
	static void AddDistanceFieldCollisionPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		FRDGTextureSRVRef GlobalDistanceFieldSRV,
		const FGPUDistanceFieldCollisionParams& DFParams,
		int32 ParticleCount);

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

	/** Add finalize positions pass */
	static void AddFinalizePositionsPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferUAVRef ParticlesUAV,
		int32 ParticleCount,
		float DeltaTime,
		float VelocityDamping = 0.99f,
		float MaxVelocity = 1000.0f);

	/** Add extract positions pass (for spatial hash) */
	static void AddExtractPositionsPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef ParticlesSRV,
		FRDGBufferUAVRef PositionsUAV,
		int32 ParticleCount,
		bool bUsePredictedPosition);

	/** Add extract render data pass (Phase 2: GPU physics → GPU render) */
	static void AddExtractRenderDataPass(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef PhysicsParticlesSRV,
		FRDGBufferUAVRef RenderParticlesUAV,
		int32 ParticleCount,
		float ParticleRadius);
};
