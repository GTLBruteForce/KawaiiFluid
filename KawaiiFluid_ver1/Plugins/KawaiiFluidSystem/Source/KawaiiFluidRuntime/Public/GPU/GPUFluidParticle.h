// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

/**
 * GPU Fluid Particle Structure
 * 64 bytes, 16-byte aligned for optimal GPU memory access
 *
 * This structure mirrors the HLSL struct in FluidGPUPhysics.ush
 */
struct FGPUFluidParticle
{
	FVector3f Position;           // 12 bytes - Current position
	float Mass;                   // 4 bytes  - Particle mass (total: 16)

	FVector3f PredictedPosition;  // 12 bytes - XPBD predicted position
	float Density;                // 4 bytes  - Current density (total: 32)

	FVector3f Velocity;           // 12 bytes - Current velocity
	float Lambda;                 // 4 bytes  - Lagrange multiplier for density constraint (total: 48)

	int32 ParticleID;             // 4 bytes  - Unique particle ID
	int32 ClusterID;              // 4 bytes  - Cluster ID for slime grouping
	uint32 Flags;                 // 4 bytes  - Bitfield flags (see EGPUParticleFlags)
	float Padding;                // 4 bytes  - Padding for 16-byte alignment (total: 64)

	FGPUFluidParticle()
		: Position(FVector3f::ZeroVector)
		, Mass(1.0f)
		, PredictedPosition(FVector3f::ZeroVector)
		, Density(0.0f)
		, Velocity(FVector3f::ZeroVector)
		, Lambda(0.0f)
		, ParticleID(0)
		, ClusterID(0)
		, Flags(0)
		, Padding(0.0f)
	{
	}
};

// Compile-time size validation
static_assert(sizeof(FGPUFluidParticle) == 64, "FGPUFluidParticle must be 64 bytes");
static_assert(alignof(FGPUFluidParticle) <= 16, "FGPUFluidParticle alignment must not exceed 16 bytes");

/**
 * GPU Particle Flags (stored in FGPUFluidParticle::Flags)
 */
namespace EGPUParticleFlags
{
	constexpr uint32 None = 0;
	constexpr uint32 IsAttached = 1 << 0;        // Particle is attached to a surface
	constexpr uint32 IsSurface = 1 << 1;         // Particle is on the fluid surface
	constexpr uint32 IsCore = 1 << 2;            // Particle is a core particle (slime)
	constexpr uint32 JustDetached = 1 << 3;      // Particle just detached this frame
	constexpr uint32 NearGround = 1 << 4;        // Particle is near the ground
}

/**
 * GPU Fluid Simulation Parameters
 * Passed to compute shaders as constant buffer
 */
struct FGPUFluidSimulationParams
{
	// Physics parameters
	float RestDensity;            // Target rest density (kg/m³)
	float SmoothingRadius;        // SPH smoothing radius (cm)
	float Compliance;             // XPBD compliance (softness)
	float ParticleRadius;         // Particle collision radius (cm)

	// Forces
	FVector3f Gravity;            // Gravity vector (cm/s²)
	float ViscosityCoefficient;   // XSPH viscosity coefficient (0-1)

	// SPH kernel coefficients (precomputed)
	float Poly6Coeff;             // 315 / (64 * PI * h^9)
	float SpikyCoeff;             // -45 / (PI * h^6)
	float Poly6GradCoeff;         // Gradient coefficient
	float SpikyGradCoeff;         // Gradient coefficient for pressure

	// Spatial hash
	float CellSize;               // Hash cell size (typically = SmoothingRadius)
	int32 ParticleCount;          // Number of active particles

	// Time
	float DeltaTime;              // Simulation substep delta time
	float DeltaTimeSq;            // DeltaTime squared

	// Bounds collision
	FVector3f BoundsMin;          // World bounds minimum
	float BoundsRestitution;      // Collision restitution (bounciness)
	FVector3f BoundsMax;          // World bounds maximum
	float BoundsFriction;         // Collision friction

	// Iteration
	int32 SubstepIndex;           // Current substep index
	int32 TotalSubsteps;          // Total substeps per frame
	int32 PressureIterations;     // Number of pressure solve iterations
	int32 Padding;                // Padding for alignment

	FGPUFluidSimulationParams()
		: RestDensity(1000.0f)
		, SmoothingRadius(20.0f)
		, Compliance(0.01f)
		, ParticleRadius(5.0f)
		, Gravity(FVector3f(0.0f, 0.0f, -980.0f))
		, ViscosityCoefficient(0.01f)
		, Poly6Coeff(0.0f)
		, SpikyCoeff(0.0f)
		, Poly6GradCoeff(0.0f)
		, SpikyGradCoeff(0.0f)
		, CellSize(20.0f)
		, ParticleCount(0)
		, DeltaTime(0.016f)
		, DeltaTimeSq(0.000256f)
		, BoundsMin(FVector3f(-1000.0f))
		, BoundsRestitution(0.3f)
		, BoundsMax(FVector3f(1000.0f))
		, BoundsFriction(0.1f)
		, SubstepIndex(0)
		, TotalSubsteps(1)
		, PressureIterations(1)
		, Padding(0)
	{
	}

	/** Precompute SPH kernel coefficients based on smoothing radius */
	void PrecomputeKernelCoefficients()
	{
		// IMPORTANT: Convert cm to m for kernel calculations to match CPU physics
		// Unreal uses centimeters, but SPH kernels are designed for meters
		constexpr float CmToMeters = 0.01f;
		const float h = SmoothingRadius * CmToMeters;  // Convert to meters
		const float h2 = h * h;
		const float h3 = h2 * h;
		const float h6 = h3 * h3;
		const float h9 = h6 * h3;

		// Poly6: W(r,h) = 315/(64*PI*h^9) * (h^2 - r^2)^3
		Poly6Coeff = 315.0f / (64.0f * PI * h9);

		// Spiky gradient: ∇W(r,h) = -45/(PI*h^6) * (h-r)^2 * r̂
		SpikyCoeff = -45.0f / (PI * h6);

		// Gradient coefficients
		Poly6GradCoeff = -945.0f / (32.0f * PI * h9);
		SpikyGradCoeff = -45.0f / (PI * h6);

		// Precompute dt²
		DeltaTimeSq = DeltaTime * DeltaTime;
	}
};

/**
 * Distance Field Collision Parameters
 * Used for GPU collision detection against UE5 Global Distance Field
 */
struct FGPUDistanceFieldCollisionParams
{
	// Volume parameters
	FVector3f VolumeCenter;       // Center of the distance field volume
	float MaxDistance;            // Maximum distance stored in the field

	FVector3f VolumeExtent;       // Half-extents of the volume
	float VoxelSize;              // Size of each voxel in world units

	// Collision response
	float Restitution;            // Bounciness (0-1)
	float Friction;               // Friction coefficient (0-1)
	float CollisionThreshold;     // Distance threshold for collision detection
	float ParticleRadius;         // Particle radius for collision

	// Enable flag
	int32 bEnabled;               // Whether to use distance field collision
	int32 Padding1;
	int32 Padding2;
	int32 Padding3;

	FGPUDistanceFieldCollisionParams()
		: VolumeCenter(FVector3f::ZeroVector)
		, MaxDistance(1000.0f)
		, VolumeExtent(FVector3f(5000.0f))
		, VoxelSize(10.0f)
		, Restitution(0.3f)
		, Friction(0.1f)
		, CollisionThreshold(1.0f)
		, ParticleRadius(5.0f)
		, bEnabled(0)
		, Padding1(0)
		, Padding2(0)
		, Padding3(0)
	{
	}
};

//=============================================================================
// GPU Collision Primitives
// Uploaded from FluidCollider system for GPU-based collision detection
//=============================================================================

/** Collision primitive types */
namespace EGPUCollisionPrimitiveType
{
	constexpr uint32 Sphere = 0;
	constexpr uint32 Capsule = 1;
	constexpr uint32 Box = 2;
	constexpr uint32 Convex = 3;
}

/**
 * GPU Sphere Primitive (32 bytes)
 */
struct FGPUCollisionSphere
{
	FVector3f Center;     // 12 bytes
	float Radius;         // 4 bytes
	float Friction;       // 4 bytes
	float Restitution;    // 4 bytes
	float Padding1;       // 4 bytes
	float Padding2;       // 4 bytes

	FGPUCollisionSphere()
		: Center(FVector3f::ZeroVector)
		, Radius(10.0f)
		, Friction(0.1f)
		, Restitution(0.3f)
		, Padding1(0.0f)
		, Padding2(0.0f)
	{
	}
};
static_assert(sizeof(FGPUCollisionSphere) == 32, "FGPUCollisionSphere must be 32 bytes");

/**
 * GPU Capsule Primitive (48 bytes)
 */
struct FGPUCollisionCapsule
{
	FVector3f Start;      // 12 bytes
	float Radius;         // 4 bytes
	FVector3f End;        // 12 bytes
	float Friction;       // 4 bytes
	float Restitution;    // 4 bytes
	float Padding1;       // 4 bytes
	float Padding2;       // 4 bytes
	float Padding3;       // 4 bytes

	FGPUCollisionCapsule()
		: Start(FVector3f::ZeroVector)
		, Radius(10.0f)
		, End(FVector3f(0.0f, 0.0f, 100.0f))
		, Friction(0.1f)
		, Restitution(0.3f)
		, Padding1(0.0f)
		, Padding2(0.0f)
		, Padding3(0.0f)
	{
	}
};
static_assert(sizeof(FGPUCollisionCapsule) == 48, "FGPUCollisionCapsule must be 48 bytes");

/**
 * GPU Box Primitive (64 bytes)
 */
struct FGPUCollisionBox
{
	FVector3f Center;     // 12 bytes
	float Friction;       // 4 bytes
	FVector3f Extent;     // 12 bytes (half extents)
	float Restitution;    // 4 bytes
	FVector4f Rotation;   // 16 bytes (quaternion: x, y, z, w)
	FVector3f Padding;    // 12 bytes
	float Padding2;       // 4 bytes

	FGPUCollisionBox()
		: Center(FVector3f::ZeroVector)
		, Friction(0.1f)
		, Extent(FVector3f(50.0f))
		, Restitution(0.3f)
		, Rotation(FVector4f(0.0f, 0.0f, 0.0f, 1.0f))
		, Padding(FVector3f::ZeroVector)
		, Padding2(0.0f)
	{
	}
};
static_assert(sizeof(FGPUCollisionBox) == 64, "FGPUCollisionBox must be 64 bytes");

/**
 * GPU Convex Plane (16 bytes)
 * Convex hull is represented as intersection of half-spaces (planes)
 */
struct FGPUConvexPlane
{
	FVector3f Normal;     // 12 bytes (unit normal pointing outward)
	float Distance;       // 4 bytes (signed distance from origin)

	FGPUConvexPlane()
		: Normal(FVector3f(0.0f, 0.0f, 1.0f))
		, Distance(0.0f)
	{
	}
};
static_assert(sizeof(FGPUConvexPlane) == 16, "FGPUConvexPlane must be 16 bytes");

/**
 * GPU Convex Primitive Header (32 bytes)
 * References a range of planes in the plane buffer
 */
struct FGPUCollisionConvex
{
	FVector3f Center;     // 12 bytes (approximate center for bounds check)
	float BoundingRadius; // 4 bytes (bounding sphere radius)
	int32 PlaneStartIndex;// 4 bytes (start index in plane buffer)
	int32 PlaneCount;     // 4 bytes (number of planes)
	float Friction;       // 4 bytes
	float Restitution;    // 4 bytes

	FGPUCollisionConvex()
		: Center(FVector3f::ZeroVector)
		, BoundingRadius(100.0f)
		, PlaneStartIndex(0)
		, PlaneCount(0)
		, Friction(0.1f)
		, Restitution(0.3f)
	{
	}
};
static_assert(sizeof(FGPUCollisionConvex) == 32, "FGPUCollisionConvex must be 32 bytes");

/**
 * GPU Collision Primitives Collection
 * All collision primitives for GPU upload
 */
struct FGPUCollisionPrimitives
{
	TArray<FGPUCollisionSphere> Spheres;
	TArray<FGPUCollisionCapsule> Capsules;
	TArray<FGPUCollisionBox> Boxes;
	TArray<FGPUCollisionConvex> Convexes;
	TArray<FGPUConvexPlane> ConvexPlanes;

	void Reset()
	{
		Spheres.Reset();
		Capsules.Reset();
		Boxes.Reset();
		Convexes.Reset();
		ConvexPlanes.Reset();
	}

	bool IsEmpty() const
	{
		return Spheres.Num() == 0 && Capsules.Num() == 0 &&
		       Boxes.Num() == 0 && Convexes.Num() == 0;
	}

	int32 GetTotalPrimitiveCount() const
	{
		return Spheres.Num() + Capsules.Num() + Boxes.Num() + Convexes.Num();
	}
};

//=============================================================================
// GPU Particle Spawn System
// CPU sends spawn requests, GPU creates particles via atomic counter
//=============================================================================

/**
 * GPU Spawn Request (32 bytes)
 * CPU sends position/velocity, GPU creates particles atomically
 * This eliminates race conditions between game thread and render thread
 */
struct FGPUSpawnRequest
{
	FVector3f Position;       // 12 bytes - Spawn position
	float Radius;             // 4 bytes  - Initial particle radius (or 0 for default)
	FVector3f Velocity;       // 12 bytes - Initial velocity
	float Mass;               // 4 bytes  - Particle mass (total: 32)

	FGPUSpawnRequest()
		: Position(FVector3f::ZeroVector)
		, Radius(0.0f)
		, Velocity(FVector3f::ZeroVector)
		, Mass(1.0f)
	{
	}

	FGPUSpawnRequest(const FVector3f& InPosition, const FVector3f& InVelocity, float InMass = 1.0f)
		: Position(InPosition)
		, Radius(0.0f)
		, Velocity(InVelocity)
		, Mass(InMass)
	{
	}
};
static_assert(sizeof(FGPUSpawnRequest) == 32, "FGPUSpawnRequest must be 32 bytes");

/**
 * GPU Spawn Parameters
 * Constant buffer for spawn compute shader
 */
struct FGPUSpawnParams
{
	int32 SpawnRequestCount;      // Number of spawn requests this frame
	int32 MaxParticleCount;       // Maximum particle capacity
	int32 CurrentParticleCount;   // Current particle count before spawning
	int32 NextParticleID;         // Starting ID for new particles

	float DefaultRadius;          // Default particle radius if request.Radius == 0
	float DefaultMass;            // Default mass if request.Mass == 0
	int32 Padding1;
	int32 Padding2;

	FGPUSpawnParams()
		: SpawnRequestCount(0)
		, MaxParticleCount(0)
		, CurrentParticleCount(0)
		, NextParticleID(0)
		, DefaultRadius(5.0f)
		, DefaultMass(1.0f)
		, Padding1(0)
		, Padding2(0)
	{
	}
};

/**
 * GPU Resources for fluid simulation
 * Manages RDG buffers for a single simulation frame
 */
struct FGPUFluidSimulationResources
{
	// Particle buffer (read-write)
	FRDGBufferRef ParticleBuffer;
	FRDGBufferSRVRef ParticleSRV;
	FRDGBufferUAVRef ParticleUAV;

	// Position-only buffer for spatial hash (extracted from particles)
	FRDGBufferRef PositionBuffer;
	FRDGBufferSRVRef PositionSRV;

	// Temporary buffers for multi-pass algorithms
	FRDGBufferRef TempBuffer;
	FRDGBufferUAVRef TempUAV;

	int32 ParticleCount;
	float CellSize;

	FGPUFluidSimulationResources()
		: ParticleBuffer(nullptr)
		, ParticleSRV(nullptr)
		, ParticleUAV(nullptr)
		, PositionBuffer(nullptr)
		, PositionSRV(nullptr)
		, TempBuffer(nullptr)
		, TempUAV(nullptr)
		, ParticleCount(0)
		, CellSize(20.0f)
	{
	}

	bool IsValid() const
	{
		return ParticleBuffer != nullptr && ParticleCount > 0;
	}
};
