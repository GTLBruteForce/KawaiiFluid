// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

/**
 * GPU Fluid Particle Structure
 *
 * This structure mirrors the HLSL struct in FluidGPUPhysics.ush
 *
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
	int32 SourceID;               // 4 bytes  - Source Component ID (-1 = invalid)
	uint32 Flags;                 // 4 bytes  - Bitfield flags (see EGPUParticleFlags)
	uint32 NeighborCount;         // 4 bytes  - Number of neighbors (for stats) (total: 64)
	

	FGPUFluidParticle()
		: Position(FVector3f::ZeroVector)
		, Mass(1.0f)
		, PredictedPosition(FVector3f::ZeroVector)
		, Density(0.0f)
		, Velocity(FVector3f::ZeroVector)
		, Lambda(0.0f)
		, ParticleID(0)
		, SourceID(-1)
		, Flags(0)
		, NeighborCount(0)
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
	constexpr uint32 IsAttached = 1 << 0;        // Particle is attached to a surface (skips physics)
	constexpr uint32 IsSurface = 1 << 1;         // Particle is on the fluid surface
	constexpr uint32 JustDetached = 1 << 2;      // Particle just detached this frame
	constexpr uint32 NearGround = 1 << 3;        // Particle is near the ground
	constexpr uint32 HasCollided = 1 << 4;       // Particle collided this frame
	constexpr uint32 IsSleeping = 1 << 5;        // Particle is in sleep state (low velocity)
	constexpr uint32 NearBoundary = 1 << 6;      // Particle is near boundary (for visualization, doesn't skip physics)
}

/**
 * GPU Particle Source Identification
 * SourceID = Component's unique ID (int32), -1 = invalid
 */
namespace EGPUParticleSource
{
	constexpr int32 InvalidSourceID = -1;
	constexpr int32 MaxSourceCount = 64;  // Maximum number of unique sources (components) for GPU counter tracking
}

/** Check if SourceID is valid */
FORCEINLINE bool HasValidSource(int32 SourceID)
{
	return SourceID != EGPUParticleSource::InvalidSourceID;
}

/**
 * GPU Despawn Brush Request
 * Defines a spherical region for particle removal
 * Matches HLSL FDespawnBrushRequest struct (16 bytes)
 */
struct FGPUDespawnBrushRequest
{
	FVector3f Center;    // 12 bytes - World position of brush center
	float RadiusSq;      // 4 bytes  - Squared radius (pre-computed on CPU)

	FGPUDespawnBrushRequest() : Center(FVector3f::ZeroVector), RadiusSq(0.0f) {}
	FGPUDespawnBrushRequest(const FVector3f& InCenter, float InRadius)
		: Center(InCenter), RadiusSq(InRadius * InRadius) {}
};

static_assert(sizeof(FGPUDespawnBrushRequest) == 16, "FGPUDespawnBrushRequest must be 16 bytes");

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
	float CohesionStrength;       // Legacy force-based cohesion (DISABLED - set to 0.0f)
	float GlobalDamping;          // Velocity damping per substep (1.0 = no damping)

	// SPH kernel coefficients (precomputed)
	float Poly6Coeff;             // 315 / (64 * PI * h^9)
	float SpikyCoeff;             // -45 / (PI * h^6)
	float Poly6GradCoeff;         // Gradient coefficient
	float SpikyGradCoeff;         // Gradient coefficient for pressure

	// Spatial hash
	float CellSize;               // Hash cell size (typically = SmoothingRadius)
	int32 ParticleCount;          // Number of active particles

	// Uniform particle mass (B plan optimization - same for all particles)
	float ParticleMass;           // Particle mass (kg), from Preset

	// Time
	float DeltaTime;              // Simulation substep delta time
	float DeltaTimeSq;            // DeltaTime squared

	// Bounds collision (OBB support)
	FVector3f BoundsCenter;       // OBB center in world space
	float BoundsRestitution;      // Collision restitution (bounciness)
	FVector3f BoundsExtent;       // OBB half-extents (local space)
	float BoundsFriction;         // Collision friction
	FVector4f BoundsRotation;     // OBB rotation quaternion (x, y, z, w)

	// Legacy AABB bounds (for backward compatibility, used when rotation is identity)
	FVector3f BoundsMin;          // World bounds minimum (AABB mode)
	float Padding1;
	FVector3f BoundsMax;          // World bounds maximum (AABB mode)
	int32 bUseOBB;                // 1 = OBB mode, 0 = AABB mode

	// Iteration
	int32 SubstepIndex;           // Current substep index
	int32 TotalSubsteps;          // Total substeps per frame
	int32 SolverIterations;       // Number of XPBD constraint solver iterations
	float CurrentTime;            // Current game time (seconds)

	// Cohesion via Artificial Pressure (PBF paper Eq.13-14)
	// s_corr = -k * (W(r)/W(Δq))^n
	// Artificial Pressure (Tensile Instability Correction) from Physics|Simulation|Stability
	// NOTE: TensileK is scaled 100x from user-facing ArtificialPressure value
	//       (ArtificialPressure=0.1 in editor → TensileK=10 internally)
	int32 bEnableTensileInstability;  // 1 if ArtificialPressure > 0, 0 otherwise
	float TensileK;               // Scaled strength k (ArtificialPressure * 100)
	int32 TensileN;               // Exponent n (from ArtificialPressureExponent, 1~8)
	float TensileDeltaQ;          // Δq ratio (from ArtificialPressureDeltaQ, 0~0.5)
	float InvW_DeltaQ;            // Precomputed 1/W(Δq, h) for efficiency

	// Stack Pressure (weight transfer from stacked attached particles)
	float StackPressureScale;     // Stack pressure strength (0 = disabled, 1.0 = default)

	// Boundary Interaction (Moving Characters/Objects)
	int32 bEnableRelativeVelocityDamping;   // Enable relative velocity pressure damping
	float RelativeVelocityDampingStrength;  // 0~1, how much to reduce pressure when boundary approaches
	int32 bEnableBoundaryVelocityTransfer;  // Enable boundary velocity transfer (fluid following boundaries)
	float BoundaryVelocityTransferStrength; // 0~1, how much fluid follows boundary velocity
	float BoundaryDetachSpeedThreshold;     // cm/s, relative speed where detachment begins
	float BoundaryMaxDetachSpeed;           // cm/s, relative speed for full detachment
	float BoundaryAdhesionStrength;         // 0~1, adhesion strength for velocity transfer

	// Particle Sleeping (NVIDIA Flex stabilization technique)
	int32 bEnableParticleSleeping;          // Enable particle sleeping for stability
	float SleepVelocityThreshold;           // cm/s, velocity below which particles may sleep
	int32 SleepFrameThreshold;              // Number of consecutive low-velocity frames before sleeping
	float WakeVelocityThreshold;            // cm/s, velocity above which sleeping particles wake up (external force)

	// Boundary Attachment (Strong position constraint to boundary particles)
	int32 bEnableBoundaryAttachment;        // Enable boundary attachment system
	float BoundaryAttachRadius;             // cm, distance threshold for attachment
	float BoundaryDetachDistanceMultiplier; // Detach when distance > AttachRadius * this
	float BoundaryAttachDetachSpeedThreshold; // cm/s, detach when relative speed exceeds this
	float BoundaryAttachCooldown;           // seconds, cooldown after detach before re-attach
	float BoundaryAttachConstraintBlend;    // 0~1, position constraint strength (1 = fully follow boundary)

	// Surface Tension (Position-Based, always enabled)
	int32 bEnablePositionBasedSurfaceTension;   // Always 1 (Position-Based ST)
	float SurfaceTensionStrength;               // 0~1, from Physics|Material SurfaceTension
	float SurfaceTensionActivationRatio;        // 0~1, distance ratio where ST activates
	float SurfaceTensionFalloffRatio;           // 0~1, distance ratio where ST starts fading
	int32 SurfaceTensionSurfaceThreshold;       // Surface particles get stronger ST
	float SurfaceTensionVelocityDamping;        // 0~1, reduces velocity from ST correction (0=full velocity, 1=no velocity)
	float SurfaceTensionTolerance;              // cm, dead zone around activation distance (prevents oscillation)

	// Surface Tension max correction
	float MaxSurfaceTensionCorrectionPerIteration;  // cm, max position correction per iteration

	// Bounds Collision Skip (for Unlimited Size mode)
	int32 bSkipBoundsCollision;                 // 1 = skip volume bounds collision (Unlimited Size mode)

	FGPUFluidSimulationParams()
		: RestDensity(1000.0f)
		, SmoothingRadius(20.0f)
		, Compliance(0.01f)
		, ParticleRadius(5.0f)
		, Gravity(FVector3f(0.0f, 0.0f, -980.0f))
		, ViscosityCoefficient(0.01f)
		, CohesionStrength(0.0f)
		, GlobalDamping(1.0f)
		, Poly6Coeff(0.0f)
		, SpikyCoeff(0.0f)
		, Poly6GradCoeff(0.0f)
		, SpikyGradCoeff(0.0f)
		, CellSize(20.0f)
		, ParticleCount(0)
		, ParticleMass(1.0f)
		, DeltaTime(0.016f)
		, DeltaTimeSq(0.000256f)
		, BoundsCenter(FVector3f::ZeroVector)
		, BoundsRestitution(0.3f)
		, BoundsExtent(FVector3f(1000.0f))
		, BoundsFriction(0.1f)
		, BoundsRotation(FVector4f(0.0f, 0.0f, 0.0f, 1.0f))  // Identity quaternion
		, BoundsMin(FVector3f(-1000.0f))
		, Padding1(0.0f)
		, BoundsMax(FVector3f(1000.0f))
		, bUseOBB(0)
		, SubstepIndex(0)
		, TotalSubsteps(1)
		, SolverIterations(1)
		, CurrentTime(0.0f)
		, bEnableTensileInstability(1)  // Artificial Pressure enabled by default
		, TensileK(10.0f)  // Default: ArtificialPressure(0.1) * ScaleFactor(100) = 10
		, TensileN(4)  // Default ArtificialPressureExponent = 4
		, TensileDeltaQ(0.0f)  // Default ArtificialPressureDeltaQ = 0.0 (Flex style)
		, InvW_DeltaQ(0.0f)
		, StackPressureScale(0.0f)
		, bEnableRelativeVelocityDamping(1)
		, RelativeVelocityDampingStrength(0.6f)
		, bEnableBoundaryVelocityTransfer(1)
		, BoundaryVelocityTransferStrength(0.8f)
		, BoundaryDetachSpeedThreshold(500.0f)
		, BoundaryMaxDetachSpeed(1500.0f)
		, BoundaryAdhesionStrength(0.5f)
		, bEnableParticleSleeping(0)
		, SleepVelocityThreshold(5.0f)
		, SleepFrameThreshold(30)
		, WakeVelocityThreshold(20.0f)
		, bEnableBoundaryAttachment(1)
		, BoundaryAttachRadius(25.0f)
		, BoundaryDetachDistanceMultiplier(3.0f)
		, BoundaryAttachDetachSpeedThreshold(500.0f)
		, BoundaryAttachCooldown(0.2f)
		, BoundaryAttachConstraintBlend(0.8f)
		// Surface Tension (Position-Based, always enabled)
		, bEnablePositionBasedSurfaceTension(1)  // Always enabled
		, SurfaceTensionStrength(0.3f)
		, SurfaceTensionActivationRatio(0.5f)
		, SurfaceTensionFalloffRatio(0.7f)
		, SurfaceTensionSurfaceThreshold(0)  // 0 = applies to all particles
		, SurfaceTensionVelocityDamping(0.7f)
		, SurfaceTensionTolerance(1.0f)
		// Surface Tension max correction
		, MaxSurfaceTensionCorrectionPerIteration(5.0f)
		// Bounds Collision Skip
		, bSkipBoundsCollision(0)  // 0 = use bounds collision, 1 = skip (Unlimited Size mode)
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

		// Tensile Instability: Precompute 1/W(Δq, h) for ratio calculation
		// ratio = W(r)/W(Δq) = (h²-r²)³ / (h²-Δq²)³
		// Poly6Coeff cancels out in the ratio, so we only store 1/(h²-Δq²)³
		// GPU computes: ratio = (h²-r²)³ * InvW_DeltaQ
		// When Δq=0 (NVIDIA Flex style): W(0) = h⁶, gives pure anti-clustering
		if (bEnableTensileInstability && TensileDeltaQ >= 0.0f)
		{
			const float DeltaQ = TensileDeltaQ * h;  // Δq in meters
			const float DeltaQ2 = DeltaQ * DeltaQ;
			const float Diff = h2 - DeltaQ2;
			if (Diff > 0.0f)
			{
				// NOTE: Poly6Coeff is intentionally omitted here because it cancels out
				// in the ratio W(r)/W(Δq). GPU shader also computes W_r without Poly6Coeff.
				const float W_DeltaQ = Diff * Diff * Diff;
				InvW_DeltaQ = (W_DeltaQ > 1e-30f) ? (1.0f / W_DeltaQ) : 0.0f;
			}
			else
			{
				InvW_DeltaQ = 0.0f;
			}
		}
		else
		{
			InvW_DeltaQ = 0.0f;
		}

	}
};

/**
 * Heightmap Collision Parameters
 * Used for GPU collision detection against Landscape terrain
 * 64 bytes, 16-byte aligned for GPU
 */
struct FGPUHeightmapCollisionParams
{
	// World space coverage (Row 1: 16 bytes)
	FVector3f WorldMin;           // Heightmap coverage min XYZ
	float HeightScale;            // Height scale factor (usually 1.0)

	// World space coverage (Row 2: 16 bytes)
	FVector3f WorldMax;           // Heightmap coverage max XYZ
	float ParticleRadius;         // Particle radius for collision

	// UV transform (Row 3: 16 bytes)
	FVector2f InvWorldExtent;     // 1/(Max.xy - Min.xy) for UV transform
	float Friction;               // Friction coefficient (0-1)
	float Restitution;            // Bounciness (0-1)

	// Texture info (Row 4: 16 bytes)
	int32 TextureWidth;           // Heightmap texture width
	int32 TextureHeight;          // Heightmap texture height
	float InvTextureWidth;        // 1/TextureWidth for texel offset
	float InvTextureHeight;       // 1/TextureHeight for texel offset

	// Control parameters (Row 5: 16 bytes)
	int32 bEnabled;               // Whether heightmap collision is enabled
	float NormalStrength;         // Normal calculation strength (gradient scale)
	float CollisionOffset;        // Extra offset for collision detection
	int32 Padding;                // Alignment padding

	FGPUHeightmapCollisionParams()
		: WorldMin(FVector3f(-10000.0f, -10000.0f, -10000.0f))
		, HeightScale(1.0f)
		, WorldMax(FVector3f(10000.0f, 10000.0f, 10000.0f))
		, ParticleRadius(5.0f)
		, InvWorldExtent(FVector2f(0.00005f, 0.00005f))  // 1/20000
		, Friction(0.3f)
		, Restitution(0.1f)
		, TextureWidth(1024)
		, TextureHeight(1024)
		, InvTextureWidth(1.0f / 1024.0f)
		, InvTextureHeight(1.0f / 1024.0f)
		, bEnabled(0)
		, NormalStrength(1.0f)
		, CollisionOffset(0.0f)
		, Padding(0)
	{
	}

	/** Update inverse values when world bounds change */
	void UpdateInverseValues()
	{
		FVector3f Extent = WorldMax - WorldMin;
		if (Extent.X > SMALL_NUMBER && Extent.Y > SMALL_NUMBER)
		{
			InvWorldExtent = FVector2f(1.0f / Extent.X, 1.0f / Extent.Y);
		}
		if (TextureWidth > 0 && TextureHeight > 0)
		{
			InvTextureWidth = 1.0f / static_cast<float>(TextureWidth);
			InvTextureHeight = 1.0f / static_cast<float>(TextureHeight);
		}
	}
};
static_assert(sizeof(FGPUHeightmapCollisionParams) == 80, "FGPUHeightmapCollisionParams must be 80 bytes");

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
 * GPU Sphere Primitive (40 bytes)
 */
struct FGPUCollisionSphere
{
	FVector3f Center;     // 12 bytes
	float Radius;         // 4 bytes
	float Friction;       // 4 bytes
	float Restitution;    // 4 bytes
	int32 BoneIndex;      // 4 bytes - Index into bone transform buffer (-1 = no bone)
	int32 FluidTagID;     // 4 bytes - Fluid tag hash for event identification (e.g., "Water", "Lava")
	int32 OwnerID;        // 4 bytes - Unique ID of collider owner (actor/component)
	int32 bHasFluidInteraction; // 4 bytes - 1 if from FluidInteraction component, 0 if from WorldCollision

	FGPUCollisionSphere()
		: Center(FVector3f::ZeroVector)
		, Radius(10.0f)
		, Friction(0.1f)
		, Restitution(0.3f)
		, BoneIndex(-1)
		, FluidTagID(0)
		, OwnerID(0)
		, bHasFluidInteraction(0)
	{
	}
};
static_assert(sizeof(FGPUCollisionSphere) == 40, "FGPUCollisionSphere must be 40 bytes");

/**
 * GPU Capsule Primitive (56 bytes)
 */
struct FGPUCollisionCapsule
{
	FVector3f Start;      // 12 bytes
	float Radius;         // 4 bytes
	FVector3f End;        // 12 bytes
	float Friction;       // 4 bytes
	float Restitution;    // 4 bytes
	int32 BoneIndex;      // 4 bytes - Index into bone transform buffer (-1 = no bone)
	int32 FluidTagID;     // 4 bytes - Fluid tag hash for event identification
	int32 OwnerID;        // 4 bytes - Unique ID of collider owner (actor/component)
	int32 bHasFluidInteraction; // 4 bytes - 1 if from FluidInteraction component, 0 if from WorldCollision
	int32 Padding;        // 4 bytes - Alignment padding

	FGPUCollisionCapsule()
		: Start(FVector3f::ZeroVector)
		, Radius(10.0f)
		, End(FVector3f(0.0f, 0.0f, 100.0f))
		, Friction(0.1f)
		, Restitution(0.3f)
		, BoneIndex(-1)
		, FluidTagID(0)
		, OwnerID(0)
		, bHasFluidInteraction(0)
		, Padding(0)
	{
	}
};
static_assert(sizeof(FGPUCollisionCapsule) == 56, "FGPUCollisionCapsule must be 56 bytes");

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
	int32 BoneIndex;      // 4 bytes - Index into bone transform buffer (-1 = no bone)
	int32 FluidTagID;     // 4 bytes - Fluid tag hash for event identification
	int32 OwnerID;        // 4 bytes - Unique ID of collider owner (actor/component)
	int32 bHasFluidInteraction; // 4 bytes - 1 if from FluidInteraction component, 0 if from WorldCollision

	FGPUCollisionBox()
		: Center(FVector3f::ZeroVector)
		, Friction(0.1f)
		, Extent(FVector3f(50.0f))
		, Restitution(0.3f)
		, Rotation(FVector4f(0.0f, 0.0f, 0.0f, 1.0f))
		, BoneIndex(-1)
		, FluidTagID(0)
		, OwnerID(0)
		, bHasFluidInteraction(0)
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
 * GPU Convex Primitive Header (48 bytes)
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
	int32 BoneIndex;      // 4 bytes - Index into bone transform buffer (-1 = no bone)
	int32 FluidTagID;     // 4 bytes - Fluid tag hash for event identification
	int32 OwnerID;        // 4 bytes - Unique ID of collider owner (actor/component)
	int32 bHasFluidInteraction; // 4 bytes - 1 if from FluidInteraction component, 0 if from WorldCollision

	FGPUCollisionConvex()
		: Center(FVector3f::ZeroVector)
		, BoundingRadius(100.0f)
		, PlaneStartIndex(0)
		, PlaneCount(0)
		, Friction(0.1f)
		, Restitution(0.3f)
		, BoneIndex(-1)
		, FluidTagID(0)
		, OwnerID(0)
		, bHasFluidInteraction(0)
	{
	}
};
static_assert(sizeof(FGPUCollisionConvex) == 48, "FGPUCollisionConvex must be 48 bytes");

/**
 * GPU Bone Transform (64 bytes)
 * Stores bone world transform for GPU-based attachment tracking
 */
struct FGPUBoneTransform
{
	FVector3f Position;       // 12 bytes - Bone world position
	float Scale;              // 4 bytes - Uniform scale (for simplicity)
	FVector4f Rotation;       // 16 bytes - Quaternion (x, y, z, w)
	FVector3f PreviousPos;    // 12 bytes - Previous frame position (for velocity)
	float Padding1;           // 4 bytes
	FVector4f PreviousRot;    // 16 bytes - Previous frame rotation

	FGPUBoneTransform()
		: Position(FVector3f::ZeroVector)
		, Scale(1.0f)
		, Rotation(FVector4f(0.0f, 0.0f, 0.0f, 1.0f))
		, PreviousPos(FVector3f::ZeroVector)
		, Padding1(0.0f)
		, PreviousRot(FVector4f(0.0f, 0.0f, 0.0f, 1.0f))
	{
	}

	void SetFromTransform(const FTransform& InTransform)
	{
		Position = FVector3f(InTransform.GetLocation());
		Scale = InTransform.GetScale3D().GetMax();
		FQuat Q = InTransform.GetRotation();
		Rotation = FVector4f(Q.X, Q.Y, Q.Z, Q.W);
	}

	void UpdatePrevious()
	{
		PreviousPos = Position;
		PreviousRot = Rotation;
	}
};
static_assert(sizeof(FGPUBoneTransform) == 64, "FGPUBoneTransform must be 64 bytes");

/**
 * GPU Particle Attachment Data (48 bytes)
 * Stores attachment info for particles attached to bone colliders
 * Managed as a separate buffer indexed by particle index
 */
struct FGPUParticleAttachment
{
	int32 PrimitiveType;      // 4 bytes - 0=Sphere, 1=Capsule, 2=Box, 3=Convex, -1=None
	int32 PrimitiveIndex;     // 4 bytes - Index in primitive buffer
	int32 BoneIndex;          // 4 bytes - Index in bone transform buffer
	float AdhesionStrength;   // 4 bytes - Current adhesion strength (can decay)
	FVector3f LocalOffset;    // 12 bytes - Position in bone-local space
	float AttachmentTime;     // 4 bytes - Time when attached
	FVector3f RelativeVelocity;  // 12 bytes - Velocity relative to bone (for anisotropy)
	float Padding;            // 4 bytes - Padding for 16-byte alignment

	FGPUParticleAttachment()
		: PrimitiveType(-1)
		, PrimitiveIndex(-1)
		, BoneIndex(-1)
		, AdhesionStrength(0.0f)
		, LocalOffset(FVector3f::ZeroVector)
		, AttachmentTime(0.0f)
		, RelativeVelocity(FVector3f::ZeroVector)
		, Padding(0.0f)
	{
	}

	bool IsAttached() const { return PrimitiveType >= 0 && BoneIndex >= 0; }

	void Clear()
	{
		PrimitiveType = -1;
		PrimitiveIndex = -1;
		BoneIndex = -1;
		AdhesionStrength = 0.0f;
		LocalOffset = FVector3f::ZeroVector;
		AttachmentTime = 0.0f;
		RelativeVelocity = FVector3f::ZeroVector;
		Padding = 0.0f;
	}
};
static_assert(sizeof(FGPUParticleAttachment) == 48, "FGPUParticleAttachment must be 48 bytes");

/**
 * GPU Adhesion Parameters
 * Passed to adhesion compute shader
 */
struct FGPUAdhesionParams
{
	float AdhesionStrength;       // 4 bytes - Base adhesion strength
	float AdhesionRadius;         // 4 bytes - Max distance for adhesion
	float DetachAccelThreshold;   // 4 bytes - Acceleration threshold for detachment
	float DetachDistanceThreshold;// 4 bytes - Distance threshold for detachment
	float ColliderContactOffset;  // 4 bytes - Extra contact offset applied to distance checks
	float BoneVelocityScale;      // 4 bytes - How much of bone velocity to inherit (0-1)
	float SlidingFriction;        // 4 bytes - Friction when sliding on surface
	float CurrentTime;            // 4 bytes - Current game time
	int32 bEnableAdhesion;        // 4 bytes - Enable flag
	float GravitySlidingScale;    // 4 bytes - How much gravity affects sliding (0-1)
	FVector3f Gravity;            // 12 bytes - World gravity vector
	int32 Padding;                // 4 bytes

	FGPUAdhesionParams()
		: AdhesionStrength(1.0f)
		, AdhesionRadius(10.0f)
		, DetachAccelThreshold(1000.0f)
		, DetachDistanceThreshold(15.0f)
		, SlidingFriction(0.1f)
		, CurrentTime(0.0f)
		, bEnableAdhesion(0)
		, GravitySlidingScale(1.0f)
		, Gravity(0.0f, 0.0f, -980.0f)  // Default UE gravity (cm/s^2)
		, Padding(0)
	{
	}
};
static_assert(sizeof(FGPUAdhesionParams) == 56, "FGPUAdhesionParams must be 56 bytes");

//=============================================================================
// GPU Collision Feedback (for Particle -> Player Interaction)
// Records collision data for CPU readback
//=============================================================================

/**
 * GPU Collision Feedback Structure (64 bytes)
 * Records collision information for CPU-side force calculation and event triggering
 * Written by GPU during collision pass, read back to CPU asynchronously (2-3 frame delay)
 *
 * Used for drag-based force calculation:
 *   F_drag = 0.5 * rho * C_d * A * |v_rel| * v_rel
 *   where v_rel = ParticleVelocity - BodyVelocity
 *
 * BoneIndex enables per-bone force aggregation for additive animation / spring reactions
 */
struct FGPUCollisionFeedback
{
	// Row 1 (16 bytes)
	int32 ParticleIndex;          // 4 bytes - Index of the colliding particle
	int32 ColliderIndex;          // 4 bytes - Index of the collider (in combined primitive list)
	int32 ColliderType;           // 4 bytes - 0=Sphere, 1=Capsule, 2=Box, 3=Convex
	float Density;                // 4 bytes - Particle density at collision time

	// Row 2 (16 bytes)
	FVector3f ImpactNormal;       // 12 bytes - Collision surface normal
	float Penetration;            // 4 bytes - Penetration depth (cm)

	// Row 3 (16 bytes)
	FVector3f ParticleVelocity;   // 12 bytes - Particle velocity at collision (for drag calculation)
	int32 ColliderOwnerID;        // 4 bytes - Unique ID of collider owner (for filtering by actor)

	// Row 4 (16 bytes) - NEW: Particle source identification
	int32 ParticleSourceID;       // 4 bytes - Particle SourceID (ComponentIndex)
	int32 ParticleActorID;        // 4 bytes - Particle source actor ID (optional)
	int32 BoneIndex;              // 4 bytes - Bone index for per-bone force calculation (-1 = no bone)
	int32 Padding1;               // 4 bytes - Alignment padding

	// Row 5 (16 bytes) - Bone-local impact offset (for precise effect placement)
	FVector3f ImpactOffset;       // 12 bytes - Impact position in bone-local space
	int32 Padding2;               // 4 bytes - Alignment padding

	// Row 6 (16 bytes) - Particle world position (for buoyancy center calculation)
	FVector3f ParticlePosition;   // 12 bytes - Particle world position at collision time
	int32 Padding3;               // 4 bytes - Alignment padding

	FGPUCollisionFeedback()
		: ParticleIndex(0)
		, ColliderIndex(0)
		, ColliderType(0)
		, Density(0.0f)
		, ImpactNormal(FVector3f::UpVector)
		, Penetration(0.0f)
		, ParticleVelocity(FVector3f::ZeroVector)
		, ColliderOwnerID(0)
		, ParticleSourceID(EGPUParticleSource::InvalidSourceID)
		, ParticleActorID(0)
		, BoneIndex(-1)
		, Padding1(0)
		, ImpactOffset(FVector3f::ZeroVector)
		, Padding2(0)
		, ParticlePosition(FVector3f::ZeroVector)
		, Padding3(0)
	{
	}

	/** Check if particle source is valid */
	bool HasValidParticleSource() const { return ::HasValidSource(ParticleSourceID); }

	/** Get source Component ID */
	int32 GetSourceComponentID() const { return ParticleSourceID; }
};
static_assert(sizeof(FGPUCollisionFeedback) == 96, "FGPUCollisionFeedback must be 96 bytes for GPU alignment");

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
	TArray<FGPUBoneTransform> BoneTransforms;  // Bone transforms for attachment

	void Reset()
	{
		Spheres.Reset();
		Capsules.Reset();
		Boxes.Reset();
		Convexes.Reset();
		ConvexPlanes.Reset();
		BoneTransforms.Reset();
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
 * GPU Spawn Request (48 bytes)
 * CPU sends position/velocity, GPU creates particles atomically
 * This eliminates race conditions between game thread and render thread
 *
 * Memory Layout:
 *   [0-15]   Position, Radius
 *   [16-31]  Velocity, Mass
 *   [32-47]  SourceID, Reserved (NEW)
 */
struct FGPUSpawnRequest
{
	// Row 1 (16 bytes)
	FVector3f Position;       // 12 bytes - Spawn position
	float Radius;             // 4 bytes  - Initial particle radius (or 0 for default)

	// Row 2 (16 bytes)
	FVector3f Velocity;       // 12 bytes - Initial velocity
	float Mass;               // 4 bytes  - Particle mass

	// Row 3 (16 bytes) - NEW: Source identification
	int32 SourceID;           // 4 bytes  - Source identification (PresetIndex | ComponentIndex << 16)
	int32 ActorID;            // 4 bytes  - Source actor ID (optional)
	int32 Reserved1;          // 4 bytes
	int32 Reserved2;          // 4 bytes

	FGPUSpawnRequest()
		: Position(FVector3f::ZeroVector)
		, Radius(0.0f)
		, Velocity(FVector3f::ZeroVector)
		, Mass(1.0f)
		, SourceID(EGPUParticleSource::InvalidSourceID)
		, ActorID(0)
		, Reserved1(0)
		, Reserved2(0)
	{
	}

	FGPUSpawnRequest(const FVector3f& InPosition, const FVector3f& InVelocity, float InMass = 1.0f)
		: Position(InPosition)
		, Radius(0.0f)
		, Velocity(InVelocity)
		, Mass(InMass)
		, SourceID(EGPUParticleSource::InvalidSourceID)
		, ActorID(0)
		, Reserved1(0)
		, Reserved2(0)
	{
	}

	FGPUSpawnRequest(const FVector3f& InPosition, const FVector3f& InVelocity, int32 InSourceID, float InMass = 1.0f)
		: Position(InPosition)
		, Radius(0.0f)
		, Velocity(InVelocity)
		, Mass(InMass)
		, SourceID(InSourceID)
		, ActorID(0)
		, Reserved1(0)
		, Reserved2(0)
	{
	}
};
static_assert(sizeof(FGPUSpawnRequest) == 48, "FGPUSpawnRequest must be 48 bytes");

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

//=============================================================================
// GPU Boundary Particles (Flex-style Adhesion)
// Surface-sampled particles for adhesion interaction
//=============================================================================

/**
 * GPU Boundary Owner Transform
 * World transform for a FluidInteractionComponent (boundary owner)
 * Used to convert between local and world space for attachment
 * 128 bytes (two 4x4 matrices), 16-byte aligned for GPU
 */
struct FGPUBoundaryOwnerTransform
{
	FMatrix44f WorldMatrix;          // 64 bytes - Local to World transform
	FMatrix44f InverseWorldMatrix;   // 64 bytes - World to Local transform

	FGPUBoundaryOwnerTransform()
		: WorldMatrix(FMatrix44f::Identity)
		, InverseWorldMatrix(FMatrix44f::Identity)
	{
	}

	FGPUBoundaryOwnerTransform(const FMatrix44f& InWorldMatrix)
		: WorldMatrix(InWorldMatrix)
		, InverseWorldMatrix(InWorldMatrix.Inverse())
	{
	}
};
static_assert(sizeof(FGPUBoundaryOwnerTransform) == 128, "FGPUBoundaryOwnerTransform must be 128 bytes");

/**
 * GPU Boundary Attachment Data
 * Per-particle attachment state for strong position constraints to boundary particles
 * Uses Bone's local space for stable attachment (follows bone animation)
 * 64 bytes, 4-byte aligned (must match HLSL struct in FluidBoundaryAttachment.usf)
 *
 * Memory Layout (64 bytes):
 *   OwnerID         (4 bytes)  - int32
 *   BoneIndex       (4 bytes)  - int32 (skeleton bone index, -1 = use component transform)
 *   AdhesionStrength(4 bytes)  - float
 *   AttachmentTime  (4 bytes)  - float
 *   SurfaceDistance (4 bytes)  - float
 *   LocalDistance   (4 bytes)  - float
 *   Padding1        (8 bytes)  - float2
 *   LocalPosition   (12 bytes) - float3
 *   Padding2        (4 bytes)  - float
 *   LocalNormal     (12 bytes) - float3
 *   Padding3        (4 bytes)  - float
 */
struct FGPUBoundaryAttachment
{
	int32 OwnerID;              // 4 bytes - FluidInteractionComponent ID (-1 = not attached)
	int32 BoneIndex;            // 4 bytes - Bone index for local space transform (-1 = use component transform)
	float AdhesionStrength;     // 4 bytes - Current adhesion strength
	float AttachmentTime;       // 4 bytes - Time when attached/detached (for cooldown)
	float SurfaceDistance;      // 4 bytes - Distance from surface at attach time
	float LocalDistance;        // 4 bytes - Distance from surface in local space
	float Padding1[2];          // 8 bytes - Alignment padding
	FVector3f LocalPosition;    // 12 bytes - Position in Bone's local space
	float Padding2;             // 4 bytes - Alignment
	FVector3f LocalNormal;      // 12 bytes - Normal in Bone's local space
	float Padding3;             // 4 bytes - Alignment

	FGPUBoundaryAttachment()
		: OwnerID(-1)
		, BoneIndex(-1)
		, AdhesionStrength(0.0f)
		, AttachmentTime(0.0f)
		, SurfaceDistance(0.0f)
		, LocalDistance(0.0f)
		, Padding1{0.0f, 0.0f}
		, LocalPosition(FVector3f::ZeroVector)
		, Padding2(0.0f)
		, LocalNormal(FVector3f::ZeroVector)
		, Padding3(0.0f)
	{
	}

	bool IsAttached() const { return OwnerID >= 0; }

	void Clear()
	{
		OwnerID = -1;
		BoneIndex = -1;
		AdhesionStrength = 0.0f;
		AttachmentTime = 0.0f;
		SurfaceDistance = 0.0f;
		LocalDistance = 0.0f;
		Padding1[0] = 0.0f;
		Padding1[1] = 0.0f;
		LocalPosition = FVector3f::ZeroVector;
		Padding2 = 0.0f;
		LocalNormal = FVector3f::ZeroVector;
		Padding3 = 0.0f;
	}
};
static_assert(sizeof(FGPUBoundaryAttachment) == 64, "FGPUBoundaryAttachment must be 64 bytes");

/**
 * GPU Boundary Attachment Parameters
 * Passed to boundary attachment update compute shader
 * Controls when particles attach/detach from boundary particles
 * 48 bytes, 16-byte aligned
 */
struct FGPUBoundaryAttachmentParams
{
	float AttachRadius;              // 4 bytes - Distance threshold for attachment (cm)
	float DetachDistanceMultiplier;  // 4 bytes - Detach when distance > AttachRadius * this
	float DetachSpeedThreshold;      // 4 bytes - Detach when relative speed > this (cm/s)
	float AttachCooldown;            // 4 bytes - Cooldown time after detach (seconds)
	float ConstraintBlend;           // 4 bytes - Position constraint strength (0~1)
	float CurrentTime;               // 4 bytes - Current simulation time (seconds)
	float DeltaTime;                 // 4 bytes - Frame delta time (seconds)
	int32 bEnabled;                  // 4 bytes - Enable attachment system
	int32 FluidParticleCount;        // 4 bytes - Number of fluid particles
	int32 BoundaryParticleCount;     // 4 bytes - Number of boundary particles
	float SmoothingRadius;           // 4 bytes - SPH smoothing radius (for Z-Order search)
	float CellSize;                  // 4 bytes - Spatial hash cell size

	FGPUBoundaryAttachmentParams()
		: AttachRadius(5.0f)
		, DetachDistanceMultiplier(3.0f)
		, DetachSpeedThreshold(500.0f)
		, AttachCooldown(0.2f)
		, ConstraintBlend(0.8f)
		, CurrentTime(0.0f)
		, DeltaTime(0.016f)
		, bEnabled(1)
		, FluidParticleCount(0)
		, BoundaryParticleCount(0)
		, SmoothingRadius(20.0f)
		, CellSize(20.0f)
	{
	}
};
static_assert(sizeof(FGPUBoundaryAttachmentParams) == 48, "FGPUBoundaryAttachmentParams must be 48 bytes");

/**
 * GPU Boundary Owner AABB
 * Axis-Aligned Bounding Box for boundary owner (e.g., character mesh)
 * Used for early-out optimization in boundary adhesion pass
 * 32 bytes, 16-byte aligned
 */
struct FGPUBoundaryOwnerAABB
{
	FVector3f Min;            // 12 bytes - AABB minimum corner
	float Padding1;           // 4 bytes  - Alignment padding
	FVector3f Max;            // 12 bytes - AABB maximum corner
	float Padding2;           // 4 bytes  - Alignment padding

	FGPUBoundaryOwnerAABB()
		: Min(FVector3f(FLT_MAX))
		, Padding1(0.0f)
		, Max(FVector3f(-FLT_MAX))
		, Padding2(0.0f)
	{
	}

	FGPUBoundaryOwnerAABB(const FVector3f& InMin, const FVector3f& InMax)
		: Min(InMin)
		, Padding1(0.0f)
		, Max(InMax)
		, Padding2(0.0f)
	{
	}

	/** Check if this AABB is valid (Min <= Max) */
	bool IsValid() const
	{
		return Min.X <= Max.X && Min.Y <= Max.Y && Min.Z <= Max.Z;
	}

	/** Expand AABB by given radius */
	FGPUBoundaryOwnerAABB ExpandBy(float Radius) const
	{
		return FGPUBoundaryOwnerAABB(
			Min - FVector3f(Radius),
			Max + FVector3f(Radius)
		);
	}

	/** Check if this AABB intersects with another AABB */
	bool Intersects(const FGPUBoundaryOwnerAABB& Other) const
	{
		return (Min.X <= Other.Max.X && Max.X >= Other.Min.X) &&
			   (Min.Y <= Other.Max.Y && Max.Y >= Other.Min.Y) &&
			   (Min.Z <= Other.Max.Z && Max.Z >= Other.Min.Z);
	}

	/** Check if this AABB contains a point */
	bool Contains(const FVector3f& Point) const
	{
		return Point.X >= Min.X && Point.X <= Max.X &&
			   Point.Y >= Min.Y && Point.Y <= Max.Y &&
			   Point.Z >= Min.Z && Point.Z <= Max.Z;
	}

	/** Get squared distance from a point to this AABB (0 if inside) */
	float DistanceSquaredToPoint(const FVector3f& Point) const
	{
		float DistSq = 0.0f;

		// X axis
		if (Point.X < Min.X)
			DistSq += FMath::Square(Min.X - Point.X);
		else if (Point.X > Max.X)
			DistSq += FMath::Square(Point.X - Max.X);

		// Y axis
		if (Point.Y < Min.Y)
			DistSq += FMath::Square(Min.Y - Point.Y);
		else if (Point.Y > Max.Y)
			DistSq += FMath::Square(Point.Y - Max.Y);

		// Z axis
		if (Point.Z < Min.Z)
			DistSq += FMath::Square(Min.Z - Point.Z);
		else if (Point.Z > Max.Z)
			DistSq += FMath::Square(Point.Z - Max.Z);

		return DistSq;
	}

	/** Get distance from a point to this AABB (0 if inside) */
	float DistanceToPoint(const FVector3f& Point) const
	{
		return FMath::Sqrt(DistanceSquaredToPoint(Point));
	}
};
static_assert(sizeof(FGPUBoundaryOwnerAABB) == 32, "FGPUBoundaryOwnerAABB must be 32 bytes");

/**
 * GPU Boundary Particle (World-space boundary particle)
 * Must match memory layout with GPU shader (FluidBoundarySkinning.usf)
 *
 * Memory Layout (64 bytes):
 *   Position      (12 bytes) - float3
 *   Psi           (4 bytes)  - float
 *   Normal        (12 bytes) - float3
 *   OwnerID       (4 bytes)  - int32
 *   Velocity      (12 bytes) - float3
 *   FrictionCoeff (4 bytes)  - float
 *   BoneIndex     (4 bytes)  - int32 (skeleton bone index, -1 = static mesh)
 *   Padding       (12 bytes) - float3 (alignment)
 */
struct FGPUBoundaryParticle
{
	FVector3f Position;      // World position
	float Psi;               // Boundary psi value (density contribution)
	FVector3f Normal;        // Surface normal
	int32 OwnerID;           // FluidInteractionComponent ID
	FVector3f Velocity;      // Boundary particle velocity
	float FrictionCoeff;     // Friction coefficient
	int32 BoneIndex;         // Skeleton bone index (-1 for static mesh)
	int32 OriginalIndex;     // Original index before Z-Order sorting (for stable attachment)
	float Padding1;          // GPU alignment padding
	float Padding2;          // GPU alignment padding

	FGPUBoundaryParticle()
		: Position(FVector3f::ZeroVector)
		, Psi(1.0f)
		, Normal(FVector3f::UpVector)
		, OwnerID(-1)
		, Velocity(FVector3f::ZeroVector)
		, FrictionCoeff(0.6f)
		, BoneIndex(-1)
		, OriginalIndex(-1)
		, Padding1(0.0f)
		, Padding2(0.0f)
	{
	}

	FGPUBoundaryParticle(const FVector3f& InPosition, const FVector3f& InNormal, int32 InOwnerID,
						 float InPsi = 1.0f, const FVector3f& InVelocity = FVector3f::ZeroVector,
						 float InFrictionCoeff = 0.6f, int32 InBoneIndex = -1, int32 InOriginalIndex = -1)
		: Position(InPosition)
		, Psi(InPsi)
		, Normal(InNormal)
		, OwnerID(InOwnerID)
		, Velocity(InVelocity)
		, FrictionCoeff(InFrictionCoeff)
		, BoneIndex(InBoneIndex)
		, OriginalIndex(InOriginalIndex)
		, Padding1(0.0f)
		, Padding2(0.0f)
	{
	}
};
static_assert(sizeof(FGPUBoundaryParticle) == 64, "FGPUBoundaryParticle must be 64 bytes for GPU alignment");

/**
 * GPU Boundary Particles Collection
 * All boundary particles for GPU upload from multiple FluidInteractionComponents
 */
struct FGPUBoundaryParticles
{
	TArray<FGPUBoundaryParticle> Particles;

	void Reset()
	{
		Particles.Reset();
	}

	bool IsEmpty() const
	{
		return Particles.Num() == 0;
	}

	int32 GetCount() const
	{
		return Particles.Num();
	}

	void Add(const FVector3f& Position, const FVector3f& Normal, int32 OwnerID, float Psi = 1.0f, float FrictionCoeff = 0.6f)
	{
		Particles.Emplace(Position, Normal, OwnerID, Psi, FVector3f::ZeroVector, FrictionCoeff);
	}
};

/**
 * GPU Boundary Particle Local (Bone-local boundary particle)
 * Boundary particle in skeletal mesh bone space - for GPU skinning
 * Must match memory layout with shader FluidBoundarySkinning.usf FGPUBoundaryParticleLocal
 *
 * Memory Layout (48 bytes):
 *   LocalPosition (12 bytes) - float3
 *   Psi           (4 bytes)  - float
 *   LocalNormal   (12 bytes) - float3
 *   FrictionCoeff (4 bytes)  - float
 *   BoneIndex     (4 bytes)  - int32
 *   Padding       (12 bytes) - float3 (alignment)
 */
struct FGPUBoundaryParticleLocal
{
	FVector3f LocalPosition;    // Bone-local position
	float Psi;                  // Volume contribution (density contribution)
	FVector3f LocalNormal;      // Bone-local surface normal
	float FrictionCoeff;        // Coulomb friction coefficient (0~2)
	int32 BoneIndex;            // Skeleton bone index (-1 for static mesh)
	FVector3f Padding;          // GPU alignment padding

	FGPUBoundaryParticleLocal()
		: LocalPosition(FVector3f::ZeroVector)
		, Psi(1.0f)
		, LocalNormal(FVector3f::UpVector)
		, FrictionCoeff(0.6f)
		, BoneIndex(-1)
		, Padding(FVector3f::ZeroVector)
	{
	}

	// Constructor matching the call: (LocalPosition, BoneIndex, LocalNormal, Psi, Friction)
	FGPUBoundaryParticleLocal(const FVector3f& InLocalPosition, int32 InBoneIndex, 
							  const FVector3f& InLocalNormal, float InPsi = 1.0f, float InFrictionCoeff = 0.6f)
		: LocalPosition(InLocalPosition)
		, Psi(InPsi)
		, LocalNormal(InLocalNormal)
		, FrictionCoeff(InFrictionCoeff)
		, BoneIndex(InBoneIndex)
		, Padding(FVector3f::ZeroVector)
	{
	}
};
static_assert(sizeof(FGPUBoundaryParticleLocal) == 48, "FGPUBoundaryParticleLocal must be 48 bytes for GPU alignment");

/**
 * GPU Boundary Adhesion Parameters
 * Parameter set used in boundary adhesion pass
 */
struct FGPUBoundaryAdhesionParams
{
	int32 bEnabled = 0;                        // Enable adhesion (0 or 1)
	float AdhesionForceStrength = 1.0f;        // Adhesion force strength
	float AdhesionVelocityStrength = 0.5f;     // Adhesion velocity transfer strength
	float AdhesionRadius = 50.0f;              // Maximum adhesion distance (cm)
	float CohesionStrength = 1.0f;             // Cohesion force strength
	float SmoothingRadius = 25.0f;             // SPH smoothing radius (cm)
	int32 BoundaryParticleCount = 0;           // Number of boundary particles
	int32 FluidParticleCount = 0;              // Number of fluid particles
	float DeltaTime = 0.016f;                  // Delta time

	FGPUBoundaryAdhesionParams() = default;
};

//=============================================================================
// Compact Particle Stats (for optimized GPU→CPU Readback)
// Reduces readback size from 64 bytes to 32 bytes per particle (50% reduction)
// Used in EndFrame when detailed GPU stats are not needed
//=============================================================================

/**
 * Compact Particle Stats Structure (32 bytes)
 * Contains only essential fields for despawn tracking and basic rendering
 * Must match HLSL struct in FluidStatsCompact.usf
 *
 * Memory Layout (32 bytes):
 *   Position      (12 bytes) - float3
 *   ParticleID    (4 bytes)  - int32
 *   SourceID      (4 bytes)  - int32
 *   NeighborCount (4 bytes)  - uint32
 *   Flags         (4 bytes)  - uint32 (for debug visualization)
 *   Padding       (4 bytes)  - for 16-byte alignment
 */
struct FCompactParticleStats
{
	FVector3f Position;       // 12 bytes - Current position
	int32 ParticleID;         // 4 bytes  - Unique particle ID (total: 16)
	int32 SourceID;           // 4 bytes  - Source Component ID
	uint32 NeighborCount;     // 4 bytes  - Neighbor count for surface detection
	uint32 Flags;             // 4 bytes  - Particle flags (EGPUParticleFlags)
	float Padding;            // 4 bytes  - Alignment padding (total: 32)

	FCompactParticleStats()
		: Position(FVector3f::ZeroVector)
		, ParticleID(0)
		, SourceID(-1)
		, NeighborCount(0)
		, Flags(0)
		, Padding(0.0f)
	{
	}
};
static_assert(sizeof(FCompactParticleStats) == 32, "FCompactParticleStats must be 32 bytes");

/**
 * Extended Compact Particle Stats Structure (48 bytes)
 * Includes Velocity for ISM rendering and Shadow system
 * Must match HLSL struct in FluidStatsCompact.usf
 *
 * Memory Layout (48 bytes):
 *   Position      (12 bytes) - float3
 *   ParticleID    (4 bytes)  - int32 (total: 16)
 *   Velocity      (12 bytes) - float3
 *   SourceID      (4 bytes)  - int32 (total: 32)
 *   NeighborCount (4 bytes)  - uint32
 *   Padding       (12 bytes) - for 16-byte alignment (total: 48)
 */
struct FCompactParticleStatsEx
{
	FVector3f Position;       // 12 bytes - Current position
	int32 ParticleID;         // 4 bytes  - Unique particle ID (total: 16)
	FVector3f Velocity;       // 12 bytes - Current velocity
	int32 SourceID;           // 4 bytes  - Source Component ID (total: 32)
	uint32 NeighborCount;     // 4 bytes  - Neighbor count
	FVector3f Padding;        // 12 bytes - Alignment padding (total: 48)

	FCompactParticleStatsEx()
		: Position(FVector3f::ZeroVector)
		, ParticleID(0)
		, Velocity(FVector3f::ZeroVector)
		, SourceID(-1)
		, NeighborCount(0)
		, Padding(FVector3f::ZeroVector)
	{
	}
};
static_assert(sizeof(FCompactParticleStatsEx) == 48, "FCompactParticleStatsEx must be 48 bytes");
