// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Core/KawaiiFluidSimulationContext.h"
#include "Core/SpatialHash.h"
#include "Core/KawaiiFluidSimulationStats.h"
#include "Components/KawaiiFluidVolumeComponent.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Physics/DensityConstraint.h"
#include "Physics/ViscositySolver.h"
#include "Physics/AdhesionSolver.h"
#include "Physics/StackPressureSolver.h"
#include "Collision/FluidCollider.h"
#include "Collision/MeshFluidCollider.h"
#include "Components/KawaiiFluidInteractionComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Async/Async.h"
#include "RenderingThread.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidParticle.h"
#include "Rendering/KawaiiFluidRenderResource.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#include "Landscape/LandscapeHeightmapExtractor.h"
#include "LandscapeProxy.h"

// Profiling
DECLARE_STATS_GROUP(TEXT("KawaiiFluidContext"), STATGROUP_KawaiiFluidContext, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Context Simulate"), STAT_ContextSimulate, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context PredictPositions"), STAT_ContextPredictPositions, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context UpdateNeighbors"), STAT_ContextUpdateNeighbors, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context SolveDensity"), STAT_ContextSolveDensity, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context HandleCollisions"), STAT_ContextHandleCollisions, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context WorldCollision"), STAT_ContextWorldCollision, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context FinalizePositions"), STAT_ContextFinalizePositions, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context ApplyViscosity"), STAT_ContextApplyViscosity, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context ApplyAdhesion"), STAT_ContextApplyAdhesion, STATGROUP_KawaiiFluidContext);
DECLARE_CYCLE_STAT(TEXT("Context ApplyCohesion"), STAT_ContextApplyCohesion, STATGROUP_KawaiiFluidContext);

//========================================
// Auto-Scaling for SmoothingRadius Independence
// SPH stability depends on h (smoothing radius). When h changes, several parameters
// need to scale accordingly to maintain consistent behavior.
//========================================
namespace SPHScaling
{
	// Reference radius where the preset's parameters are tuned (cm)
	constexpr float ReferenceRadius = 20.0f;

	/**
	 * Calculate scaled Compliance for a given SmoothingRadius
	 * Kernel gradients scale as 1/h^5, so sumGradC2 scales as 1/h^10
	 * @param BaseCompliance - The Compliance value tuned for ReferenceRadius
	 * @param SmoothingRadius - Current smoothing radius (cm)
	 * @param Exponent - Scaling exponent (0 = no scaling, 4-6 typical, 10 theoretical)
	 * @return Scaled Compliance value
	 */
	FORCEINLINE float GetScaledCompliance(float BaseCompliance, float SmoothingRadius, float Exponent)
	{
		if (Exponent <= 0.0f)
		{
			return BaseCompliance;  // No scaling
		}
		const float Ratio = ReferenceRadius / FMath::Max(SmoothingRadius, 1.0f);
		return BaseCompliance * FMath::Pow(Ratio, Exponent);
	}
}

namespace
{
	constexpr float GPUWorldCollisionMargin = 1.0f;
	constexpr float GPUWorldBoundsTolerance = 0.1f;

	// ISMC per-instance collision limits
	constexpr int32 MaxISMCInstancesForCollision = 256;

	bool AreBoundsEqual(const FBox& A, const FBox& B)
	{
		if (!A.IsValid || !B.IsValid)
		{
			return false;
		}

		return A.Min.Equals(B.Min, GPUWorldBoundsTolerance) && A.Max.Equals(B.Max, GPUWorldBoundsTolerance);
	}

	void AppendConvexToGPUPrimitives(
		const FKConvexElem& ConvexElem,
		const FTransform& ComponentTransform,
		float Friction,
		float Restitution,
		int32 OwnerID,
		FGPUCollisionPrimitives& OutPrimitives)
	{
		const TArray<FVector>& VertexData = ConvexElem.VertexData;
		if (VertexData.Num() < 4)
		{
			return;
		}

		TArray<FVector> WorldVerts;
		WorldVerts.Reserve(VertexData.Num());
		FVector CenterSum = FVector::ZeroVector;

		for (const FVector& Vertex : VertexData)
		{
			const FVector WorldVertex = ComponentTransform.TransformPosition(Vertex);
			WorldVerts.Add(WorldVertex);
			CenterSum += WorldVertex;
		}

		const FVector Center = CenterSum / static_cast<float>(WorldVerts.Num());

		float MaxDistSq = 0.0f;
		for (const FVector& Vertex : WorldVerts)
		{
			MaxDistSq = FMath::Max(MaxDistSq, FVector::DistSquared(Vertex, Center));
		}

		TArray<FGPUConvexPlane> Planes;
		const TArray<int32>& IndexData = ConvexElem.IndexData;
		if (IndexData.Num() >= 3)
		{
			TSet<uint32> PlaneHashes;

			for (int32 i = 0; i + 2 < IndexData.Num(); i += 3)
			{
				const int32 I0 = IndexData[i];
				const int32 I1 = IndexData[i + 1];
				const int32 I2 = IndexData[i + 2];

				if (!WorldVerts.IsValidIndex(I0) || !WorldVerts.IsValidIndex(I1) || !WorldVerts.IsValidIndex(I2))
				{
					continue;
				}

				const FVector V0 = WorldVerts[I0];
				const FVector V1 = WorldVerts[I1];
				const FVector V2 = WorldVerts[I2];

				FVector Normal = FVector::CrossProduct(V1 - V0, V2 - V0);
				const float NormalLen = Normal.Size();
				if (NormalLen <= KINDA_SMALL_NUMBER)
				{
					continue;
				}

				Normal /= NormalLen;
				if (FVector::DotProduct(Normal, Center - V0) > 0.0f)
				{
					Normal = -Normal;
				}

				const int32 Nx = FMath::RoundToInt(Normal.X * 1000.0f);
				const int32 Ny = FMath::RoundToInt(Normal.Y * 1000.0f);
				const int32 Nz = FMath::RoundToInt(Normal.Z * 1000.0f);
				const uint32 Hash = HashCombine(HashCombine(GetTypeHash(Nx), GetTypeHash(Ny)), GetTypeHash(Nz));
				if (PlaneHashes.Contains(Hash))
				{
					continue;
				}
				PlaneHashes.Add(Hash);

				FGPUConvexPlane Plane;
				Plane.Normal = FVector3f(Normal);
				Plane.Distance = FVector::DotProduct(V0, Normal);
				Planes.Add(Plane);
			}
		}

		// If IndexData is missing or planes are insufficient, fetch directly from ChaosConvex
		if (Planes.Num() < 4)
		{
			TArray<FPlane> ChaosPlanes;
			ConvexElem.GetPlanes(ChaosPlanes);

			if (ChaosPlanes.Num() >= 4)
			{
				Planes.Reset();  // Remove partial planes generated from IndexData

				for (const FPlane& ChaosPlane : ChaosPlanes)
				{
					// ChaosPlane is in local space, so transform to world space
					const FVector LocalNormal = FVector(ChaosPlane.X, ChaosPlane.Y, ChaosPlane.Z);
					const FVector WorldNormal = ComponentTransform.TransformVectorNoScale(LocalNormal);

					// Transform a point on the plane to world space
					const FVector LocalPoint = LocalNormal * ChaosPlane.W;
					const FVector WorldPoint = ComponentTransform.TransformPosition(LocalPoint);
					
					FGPUConvexPlane GPUPlane;
					GPUPlane.Normal = FVector3f(WorldNormal);
					GPUPlane.Distance = FVector::DotProduct(WorldPoint, WorldNormal);
					Planes.Add(GPUPlane);
				}
				
				static int32 FallbackLogCounter = 0;
				if (++FallbackLogCounter % 60 == 1)
				{
					UE_LOG(LogTemp, Log, TEXT("[Convex] Fallback to ChaosConvex: Verts=%d, IndexData=%d, ChaosPlanes=%d"),
						VertexData.Num(), IndexData.Num(), ChaosPlanes.Num());
				}
			}
		}

		if (Planes.Num() < 4)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Convex] SKIP: Verts=%d, Indices=%d, Planes=%d (need >= 4, no ChaosConvex fallback)"),
				VertexData.Num(), IndexData.Num(), Planes.Num());
			return;
		}

		// Convex debug log
		static int32 ConvexLogCounter = 0;
		if (++ConvexLogCounter % 300 == 1)
		{
			UE_LOG(LogTemp, Log, TEXT("[Convex] Created: Center=(%.1f, %.1f, %.1f), BoundingRadius=%.1f, Planes=%d"),
				Center.X, Center.Y, Center.Z, FMath::Sqrt(MaxDistSq) + GPUWorldCollisionMargin, Planes.Num());
			for (int32 i = 0; i < FMath::Min(Planes.Num(), 6); ++i)
			{
				const FGPUConvexPlane& P = Planes[i];
				UE_LOG(LogTemp, Log, TEXT("  Plane[%d]: Normal=(%.3f, %.3f, %.3f), Dist=%.1f"),
					i, P.Normal.X, P.Normal.Y, P.Normal.Z, P.Distance);
			}
		}

		FGPUCollisionConvex Convex;
		Convex.Center = FVector3f(Center);
		Convex.BoundingRadius = FMath::Sqrt(MaxDistSq) + GPUWorldCollisionMargin;
		Convex.PlaneStartIndex = OutPrimitives.ConvexPlanes.Num();
		Convex.PlaneCount = Planes.Num();

		// PlaneStartIndex debug log
		if (ConvexLogCounter % 300 == 1)
		{
			UE_LOG(LogTemp, Log, TEXT("[Convex] PlaneStartIndex=%d, PlaneCount=%d, TotalPlanesBeforeAppend=%d"),
				Convex.PlaneStartIndex, Convex.PlaneCount, OutPrimitives.ConvexPlanes.Num());
		}
		Convex.Friction = Friction;
		Convex.Restitution = Restitution;
		Convex.BoneIndex = -1;
		Convex.OwnerID = OwnerID;
		Convex.bHasFluidInteraction = 0;  // WorldCollision (no FluidInteraction)

		OutPrimitives.Convexes.Add(Convex);
		OutPrimitives.ConvexPlanes.Append(Planes);
	}

	void AppendAggGeomToGPUPrimitives(
		const FKAggregateGeom& AggGeom,
		const FTransform& ComponentTransform,
		float Friction,
		float Restitution,
		int32 OwnerID,
		FGPUCollisionPrimitives& OutPrimitives)
	{
		const FVector3d Scale = ComponentTransform.GetScale3D();

		for (const FKSphereElem& SphereElem : AggGeom.SphereElems)
		{
			const FTransform SphereWorldTransform = SphereElem.GetTransform() * ComponentTransform;
			FGPUCollisionSphere Sphere;
			Sphere.Center = FVector3f(SphereWorldTransform.GetLocation());
			Sphere.Radius = SphereElem.Radius * ComponentTransform.GetScale3D().GetMax() + GPUWorldCollisionMargin;
			Sphere.Friction = Friction;
			Sphere.Restitution = Restitution;
			Sphere.BoneIndex = -1;
			Sphere.OwnerID = OwnerID;
			Sphere.bHasFluidInteraction = 0;  // WorldCollision (no FluidInteraction)
			OutPrimitives.Spheres.Add(Sphere);
		}

		for (const FKSphylElem& SphylElem : AggGeom.SphylElems)
		{
			const FTransform CapsuleWorldTransform = SphylElem.GetTransform() * ComponentTransform;
			const FVector CapsuleCenter = CapsuleWorldTransform.GetLocation();
			const float ScaledRadius = SphylElem.Radius * FMath::Max(Scale.X, Scale.Y) + GPUWorldCollisionMargin;
			const float ScaledLength = SphylElem.Length * Scale.Z;
			const FVector CapsuleUp = CapsuleWorldTransform.GetRotation().GetUpVector();
			const float HalfLength = ScaledLength * 0.5f;

			FGPUCollisionCapsule Capsule;
			Capsule.Start = FVector3f(CapsuleCenter - CapsuleUp * HalfLength);
			Capsule.End = FVector3f(CapsuleCenter + CapsuleUp * HalfLength);
			Capsule.Radius = ScaledRadius;
			Capsule.Friction = Friction;
			Capsule.Restitution = Restitution;
			Capsule.BoneIndex = -1;
			Capsule.OwnerID = OwnerID;
			Capsule.bHasFluidInteraction = 0;  // WorldCollision (no FluidInteraction)
			OutPrimitives.Capsules.Add(Capsule);
		}

		for (const FKBoxElem& BoxElem : AggGeom.BoxElems)
		{
			const FTransform BoxWorldTransform = BoxElem.GetTransform() * ComponentTransform;
			FGPUCollisionBox Box;
			Box.Center = FVector3f(BoxWorldTransform.GetLocation());
			Box.Extent = FVector3f(
				BoxElem.X * 0.5f * Scale.X + GPUWorldCollisionMargin,
				BoxElem.Y * 0.5f * Scale.Y + GPUWorldCollisionMargin,
				BoxElem.Z * 0.5f * Scale.Z + GPUWorldCollisionMargin
			);
			const FQuat BoxRotation = BoxWorldTransform.GetRotation();
			Box.Rotation = FVector4f(
				static_cast<float>(BoxRotation.X),
				static_cast<float>(BoxRotation.Y),
				static_cast<float>(BoxRotation.Z),
				static_cast<float>(BoxRotation.W)
			);
			Box.Friction = Friction;
			Box.Restitution = Restitution;
			Box.BoneIndex = -1;
			Box.OwnerID = OwnerID;
			Box.bHasFluidInteraction = 0;  // WorldCollision (no FluidInteraction)
			OutPrimitives.Boxes.Add(Box);
		}

		for (const FKConvexElem& ConvexElem : AggGeom.ConvexElems)
		{
			AppendConvexToGPUPrimitives(ConvexElem, ComponentTransform, Friction, Restitution, OwnerID, OutPrimitives);
		}
	}
}

UKawaiiFluidSimulationContext::UKawaiiFluidSimulationContext()
{
}

UKawaiiFluidSimulationContext::~UKawaiiFluidSimulationContext()
{
	ReleaseRenderResource();
	ReleaseGPUSimulator();
}

void UKawaiiFluidSimulationContext::InitializeSolvers(const UKawaiiFluidPresetDataAsset* Preset)
{
	if (!Preset)
	{
		return;
	}

	DensityConstraint = MakeShared<FDensityConstraint>(
		Preset->Density,
		Preset->SmoothingRadius,
		SPHScaling::GetScaledCompliance(Preset->Compressibility, Preset->SmoothingRadius, Preset->ComplianceExponent)
	);
	ViscositySolver = MakeShared<FViscositySolver>();
	AdhesionSolver = MakeShared<FAdhesionSolver>();
	StackPressureSolver = MakeShared<FStackPressureSolver>();

	bSolversInitialized = true;
}

void UKawaiiFluidSimulationContext::EnsureSolversInitialized(const UKawaiiFluidPresetDataAsset* Preset)
{
	if (!bSolversInitialized && Preset)
	{
		InitializeSolvers(Preset);
	}
}

//=============================================================================
// GPU Simulation Methods
//=============================================================================

void UKawaiiFluidSimulationContext::InitializeGPUSimulator(int32 MaxParticleCount)
{
	if (GPUSimulator.IsValid())
	{
		// Already initialized - resize if needed
		if (GPUSimulator->GetMaxParticleCount() < MaxParticleCount)
		{
			GPUSimulator->Release();
			GPUSimulator->Initialize(MaxParticleCount);
		}
		return;
	}

	GPUSimulator = MakeShared<FGPUFluidSimulator>();
	GPUSimulator->Initialize(MaxParticleCount);

	UE_LOG(LogTemp, Log, TEXT("GPU Fluid Simulator initialized with capacity: %d"), MaxParticleCount);
}

void UKawaiiFluidSimulationContext::ReleaseGPUSimulator()
{
	if (GPUSimulator.IsValid())
	{
		GPUSimulator->Release();
		GPUSimulator.Reset();
	}
}

//=============================================================================
// Render Resource Methods (for batch rendering)
//=============================================================================

void UKawaiiFluidSimulationContext::InitializeRenderResource()
{
	if (RenderResource.IsValid())
	{
		return; // Already initialized
	}

	RenderResource = MakeShared<FKawaiiFluidRenderResource>();

	// Initialize on render thread
	ENQUEUE_RENDER_COMMAND(InitContextRenderResource)(
		[RenderResourcePtr = RenderResource.Get()](FRHICommandListImmediate& RHICmdList)
		{
			RenderResourcePtr->InitResource(RHICmdList);
		}
	);

	UE_LOG(LogTemp, Log, TEXT("SimulationContext: RenderResource initialized"));
}

void UKawaiiFluidSimulationContext::ReleaseRenderResource()
{
	if (RenderResource.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(ReleaseContextRenderResource)(
			[RenderResource = MoveTemp(RenderResource)](FRHICommandListImmediate& RHICmdList) mutable
			{
				if (RenderResource.IsValid())
				{
					RenderResource->ReleaseResource();
					RenderResource.Reset();
				}
			}
		);
	}
}

bool UKawaiiFluidSimulationContext::IsGPUSimulatorReady() const
{
	return GPUSimulator.IsValid() && GPUSimulator->IsReady();
}

FGPUFluidSimulationParams UKawaiiFluidSimulationContext::BuildGPUSimParams(
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	float SubstepDT) const
{
	FGPUFluidSimulationParams GPUParams;

	// Physics parameters from preset
	GPUParams.RestDensity = Preset->Density;
	GPUParams.SmoothingRadius = Preset->SmoothingRadius;
	// Scale Compliance for SmoothingRadius-independent stability
	GPUParams.Compliance = SPHScaling::GetScaledCompliance(
		Preset->Compressibility, Preset->SmoothingRadius, Preset->ComplianceExponent);
	GPUParams.ParticleRadius = Preset->ParticleRadius;
	GPUParams.ParticleMass = Preset->ParticleMass;  // Uniform particle mass (B plan optimization)
	GPUParams.ViscosityCoefficient = Preset->Viscosity;
	// Cohesion (Akinci 2013 force-based) - set in Surface Tension section below
	GPUParams.GlobalDamping = Preset->GetDamping();

	// Stack Pressure (weight transfer from stacked attached particles)
	GPUParams.StackPressureScale = Preset->bEnableStackPressure ? Preset->StackPressureScale : 0.0f;

	// Boundary Interaction (Moving Characters/Objects)
	GPUParams.bEnableRelativeVelocityDamping = Preset->bEnableRelativeVelocityDamping ? 1 : 0;
	GPUParams.RelativeVelocityDampingStrength = Preset->RelativeVelocityDampingStrength;
	GPUParams.BoundaryVelocityTransferStrength = Preset->BoundaryVelocityTransferStrength;
	GPUParams.BoundaryDetachSpeedThreshold = Preset->BoundaryDetachSpeedThreshold;
	GPUParams.BoundaryMaxDetachSpeed = Preset->BoundaryMaxDetachSpeed;
	GPUParams.BoundaryAttachRadius = Preset->AdhesionRadius;  // Use AdhesionRadius for BoneDeltaAttachment
	GPUParams.bEnableBoundaryAttachment = Preset->bEnableBoundaryAttachment ? 1 : 0;

	// Particle Sleeping (NVIDIA Flex stabilization)
	GPUParams.bEnableParticleSleeping = Preset->bEnableParticleSleeping ? 1 : 0;
	GPUParams.SleepVelocityThreshold = Preset->SleepVelocityThreshold;
	GPUParams.SleepFrameThreshold = Preset->SleepFrameThreshold;
	GPUParams.WakeVelocityThreshold = Preset->WakeVelocityThreshold;
	

	// Gravity from preset
	GPUParams.Gravity = FVector3f(Preset->Gravity);

	// Time
	GPUParams.DeltaTime = SubstepDT;
	GPUParams.CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	// Spatial hash cell size - must match the Volume's CellSize for Morton code consistency
	// Priority: TargetVolumeComponent's CellSize > Fallback to SmoothingRadius
	if (UKawaiiFluidVolumeComponent* Volume = TargetVolumeComponent.Get())
	{
		GPUParams.CellSize = Volume->CellSize;
	}
	else
	{
		GPUParams.CellSize = Preset->SmoothingRadius;
	}

	// Bounds collision (use world bounds from params if containment is enabled)
	if (Params.WorldBounds.IsValid)
	{
		// Containment enabled - check if OBB or AABB mode
		const bool bHasRotation = !Params.BoundsRotation.Equals(FQuat::Identity, 0.0001f);

		if (bHasRotation)
		{
			// OBB mode - use Center, Extent, Rotation
			GPUParams.bUseOBB = 1;
			GPUParams.BoundsCenter = FVector3f(Params.BoundsCenter);
			GPUParams.BoundsExtent = FVector3f(Params.BoundsExtent);
			GPUParams.BoundsRotation = FVector4f(
				Params.BoundsRotation.X,
				Params.BoundsRotation.Y,
				Params.BoundsRotation.Z,
				Params.BoundsRotation.W
			);

			// Also set AABB for fallback (compute AABB from OBB)
			GPUParams.BoundsMin = FVector3f(Params.WorldBounds.Min);
			GPUParams.BoundsMax = FVector3f(Params.WorldBounds.Max);
		}
		else
		{
			// AABB mode - use Min/Max
			GPUParams.bUseOBB = 0;
			GPUParams.BoundsMin = FVector3f(Params.WorldBounds.Min);
			GPUParams.BoundsMax = FVector3f(Params.WorldBounds.Max);
			GPUParams.BoundsCenter = FVector3f(Params.BoundsCenter);
			GPUParams.BoundsExtent = FVector3f(Params.BoundsExtent);
			GPUParams.BoundsRotation = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);  // Identity
		}

		GPUParams.BoundsRestitution = Params.BoundsRestitution;
		GPUParams.BoundsFriction = Params.BoundsFriction;

		// Skip bounds collision when Unlimited Size mode is enabled
		GPUParams.bSkipBoundsCollision = Params.bSkipBoundsCollision ? 1 : 0;
	}
	else
	{
		// Default large bounds (effectively no bounds collision)
		GPUParams.bUseOBB = 0;
		GPUParams.BoundsMin = FVector3f(-1000000.0f, -1000000.0f, -1000000.0f);
		GPUParams.BoundsMax = FVector3f(1000000.0f, 1000000.0f, 1000000.0f);
		GPUParams.BoundsCenter = FVector3f::ZeroVector;
		GPUParams.BoundsExtent = FVector3f(1000000.0f);
		GPUParams.BoundsRotation = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);  // Identity
		GPUParams.BoundsRestitution = Preset->Bounciness;
		GPUParams.BoundsFriction = Preset->Friction;
		GPUParams.bSkipBoundsCollision = 0;  // Use bounds collision by default
	}

	// Solver iterations (typically 1-4 for density constraint)
	GPUParams.SolverIterations = Preset->SolverIterations;

	// Cohesion via Artificial Pressure (PBF Eq.13-14)
	// Cohesion controls anti-clumping behavior that creates connected fluid streams
	// Must be set before PrecomputeKernelCoefficients() to compute InvW_DeltaQ
	//
	// SCALE FACTOR: User-facing Cohesion (0~1) is scaled by 100x internally
	// This is because scorr = -k * ratio^n needs to compete with lambda values
	// Artificial Pressure (Tensile Instability Correction)
	// Uses artificial pressure term from PBF (Eq.13-14) to prevent particle clumping
	// Effective k values:
	//   ArtificialPressure=0.05 → k=5   (subtle anti-clumping)
	//   ArtificialPressure=0.1  → k=10  (water-like, recommended)
	//   ArtificialPressure=0.2  → k=20  (strong anti-clumping)
	constexpr float ArtificialPressureScaleFactor = 100.0f;
	GPUParams.bEnableTensileInstability = (Preset->ArtificialPressure > 0.0f) ? 1 : 0;
	GPUParams.TensileK = Preset->ArtificialPressure * ArtificialPressureScaleFactor;
	GPUParams.TensileN = Preset->ArtificialPressureExponent;
	GPUParams.TensileDeltaQ = Preset->ArtificialPressureDeltaQ;

	// Surface Tension (Position-Based, always enabled)
	GPUParams.bEnablePositionBasedSurfaceTension = 1;  // Always enabled
	GPUParams.SurfaceTensionStrength = Preset->SurfaceTension;  // From Physics|Material (0~1)
	GPUParams.SurfaceTensionActivationRatio = Preset->SurfaceTensionActivationRatio;
	GPUParams.SurfaceTensionFalloffRatio = Preset->SurfaceTensionFalloffRatio;
	GPUParams.SurfaceTensionSurfaceThreshold = Preset->SurfaceTensionSurfaceThreshold;
	GPUParams.SurfaceTensionVelocityDamping = Preset->SurfaceTensionVelocityDamping;
	GPUParams.SurfaceTensionTolerance = Preset->SurfaceTensionTolerance;

	// Fluid Cohesion (Akinci 2013 force-based)
	// Uses C(r) spline kernel with K_ij particle deficiency correction
	// Applied in PredictPositions pass (Phase 2) via FluidForceAccumulation.ush
	GPUParams.CohesionStrength = Preset->Cohesion;  // From Physics|Material (0~50)

	// Surface Tension / Cohesion max correction
	GPUParams.MaxSurfaceTensionCorrectionPerIteration = Preset->MaxSurfaceTensionCorrectionPerIteration;

	// Precompute kernel coefficients (including InvW_DeltaQ for tensile instability)
	GPUParams.PrecomputeKernelCoefficients();

	return GPUParams;
}

TArray<int32> UKawaiiFluidSimulationContext::ExtractAttachedParticleIndices(const TArray<FFluidParticle>& Particles) const
{
	TArray<int32> AttachedIndices;
	AttachedIndices.Reserve(Particles.Num() / 10);  // Estimate ~10% attached

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		if (Particles[i].bIsAttached)
		{
			AttachedIndices.Add(i);
		}
	}

	return AttachedIndices;
}

void UKawaiiFluidSimulationContext::HandleAttachedParticlesCPU(
	TArray<FFluidParticle>& Particles,
	const TArray<int32>& AttachedIndices,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	float SubstepDT)
{
	if (AttachedIndices.Num() == 0)
	{
		return;
	}

	// Update attached particle positions (bone tracking)
	// This is already done in the main Simulate loop before substeps

	// Apply adhesion for attached particles
	if (AdhesionSolver.IsValid() && Preset->Adhesion > 0.0f)
	{
		// Only apply to attached particles
		for (int32 Idx : AttachedIndices)
		{
			FFluidParticle& Particle = Particles[Idx];

			// Apply sliding gravity (tangent component)
			const FVector& Normal = Particle.AttachedSurfaceNormal;
			float NormalComponent = FVector::DotProduct(Preset->Gravity, Normal);
			FVector TangentGravity = Preset->Gravity - NormalComponent * Normal;
			Particle.Velocity += TangentGravity * SubstepDT;

			// Apply velocity damping for attached particles (they move slower)
			Particle.Velocity *= 0.95f;

			// Update predicted position
			Particle.PredictedPosition = Particle.Position + Particle.Velocity * SubstepDT;
		}
	}

	// CPU handles world collision for attached particles
	// (Per-polygon collision will be added in Phase 2)
}

void UKawaiiFluidSimulationContext::SimulateGPU(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float DeltaTime,
	float& AccumulatedTime)
{
	
	
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_SimulateGPU);

	if (!Preset)
	{
		return;
	}

	// Ensure GPU simulator is ready
	if (!IsGPUSimulatorReady())
	{
		const int32 MaxParticles = TargetVolumeComponent.IsValid() ? TargetVolumeComponent->MaxParticleCount : 200000;
		InitializeGPUSimulator(MaxParticles);
		if (!IsGPUSimulatorReady())
		{
			UE_LOG(LogTemp, Warning, TEXT("GPU Simulator not ready"));
			return;
		}
	}

	// Set default spawn parameters from Preset (for fallback when spawn request values are 0)
	GPUSimulator->SetDefaultSpawnRadius(Preset->ParticleRadius);
	GPUSimulator->SetDefaultSpawnMass(Preset->ParticleMass);

	// Set external force from simulation params (wind, player force, etc.)
	GPUSimulator->SetExternalForce(FVector3f(Params.ExternalForce));

	// Set primitive collision threshold from Preset
	GPUSimulator->SetPrimitiveCollisionThreshold(Preset->CollisionThreshold);

	// Set simulation bounds for Z-Order sorting (Morton code)
	// Priority: TargetVolumeComponent bounds > Preset bounds + SimulationOrigin
	FVector3f WorldBoundsMin, WorldBoundsMax;
	EGridResolutionPreset GridPreset = Params.GridResolutionPreset;

	if (UKawaiiFluidVolumeComponent* Volume = TargetVolumeComponent.Get())
	{
		// Use Volume's world-space bounds and GridResolutionPreset
		WorldBoundsMin = FVector3f(Volume->GetWorldBoundsMin());
		WorldBoundsMax = FVector3f(Volume->GetWorldBoundsMax());
		GridPreset = Volume->GetGridResolutionPreset();
	}
	else
	{
		// Fallback: Calculate bounds from GridResolutionPreset and SmoothingRadius
		const int32 GridResolution = GridResolutionPresetHelper::GetGridResolution(GridPreset);
		const float BoundsExtent = static_cast<float>(GridResolution) * Preset->SmoothingRadius;
		const float HalfExtent = BoundsExtent * 0.5f;
		WorldBoundsMin = FVector3f(-HalfExtent) + FVector3f(Params.SimulationOrigin);
		WorldBoundsMax = FVector3f(HalfExtent) + FVector3f(Params.SimulationOrigin);
	}

	GPUSimulator->SetSimulationBounds(WorldBoundsMin, WorldBoundsMax);
	GPUSimulator->SetGridResolutionPreset(GridPreset);

	// Hybrid Tiled Z-Order mode for unlimited simulation range
	// When enabled, particles can exist anywhere in the world (no bounds clipping)
	bool bUseHybridTiledZOrder = false;
	bool bUseUnlimitedSize = false;
	if (UKawaiiFluidVolumeComponent* Volume = TargetVolumeComponent.Get())
	{
		bUseHybridTiledZOrder = Volume->bUseHybridTiledZOrder;
		bUseUnlimitedSize = Volume->bUseUnlimitedSize;
	}
	GPUSimulator->SetHybridTiledZOrderEnabled(bUseHybridTiledZOrder);

	// GPU World Collision Query Bounds
	// In Unlimited Size mode: Use particle bounds (readback) + character bounds (InteractionComponents)
	// In Normal mode: Use simulation volume bounds + particle radius padding
	FBox GPUWorldQueryBounds{FVector(WorldBoundsMin), FVector(WorldBoundsMax)};
	GPUWorldQueryBounds = GPUWorldQueryBounds.ExpandBy(Preset->ParticleRadius);

	if (bUseUnlimitedSize)
	{
		// Enable particle bounds readback for next frame (if not already enabled)
		if (!GPUSimulator->IsParticleBoundsReadbackEnabled())
		{
			GPUSimulator->SetParticleBoundsReadbackEnabled(true);
		}

		// Option D: Character-centered bounds (immediate, no latency)
		FBox CharacterBounds(EForceInit::ForceInit);
		constexpr float CharacterInteractionRadius = 300.0f;  // 3m radius around each character
		
		for (const TObjectPtr<UKawaiiFluidInteractionComponent>& Interaction : Params.InteractionComponents)
		{
			if (Interaction)
			{
				if (AActor* Owner = Interaction->GetOwner())
				{
					FBox ActorBounds = Owner->GetComponentsBoundingBox();
					if (ActorBounds.IsValid)
					{
						ActorBounds = ActorBounds.ExpandBy(CharacterInteractionRadius);
						CharacterBounds += ActorBounds;
					}
				}
			}
		}

		// Option A: Particle bounds from GPU readback (2-3 frame latency)
		FBox ParticleBounds(EForceInit::ForceInit);
		if (GPUSimulator->HasReadyParticleBounds())
		{
			ParticleBounds = GPUSimulator->GetCachedParticleBounds();
			
			// Velocity-based margin for latency compensation
			// Assume max particle velocity ~500 cm/s, 3 frame latency, ~60 FPS
			constexpr float MaxVelocity = 500.0f;
			constexpr float LatencyFrames = 3.0f;
			constexpr float FrameTime = 1.0f / 60.0f;
			const float LatencyMargin = MaxVelocity * LatencyFrames * FrameTime;
			ParticleBounds = ParticleBounds.ExpandBy(LatencyMargin);
		}

		// Combine: Character bounds + Particle bounds
		FBox CombinedBounds(EForceInit::ForceInit);
		if (CharacterBounds.IsValid)
		{
			CombinedBounds += CharacterBounds;
		}
		if (ParticleBounds.IsValid)
		{
			CombinedBounds += ParticleBounds;
		}

		// Fallback: If neither available, use default volume bounds
		if (CombinedBounds.IsValid)
		{
			// Add particle radius margin for collision detection
			GPUWorldQueryBounds = CombinedBounds.ExpandBy(Preset->ParticleRadius);
		}
		// else: Keep original volume-based bounds as fallback
	}

	// =====================================================
	// GPU-Only Mode: No CPU Particles array dependency
	// - Spawning: Handled directly by SpawnParticle() → GPUSimulator->AddSpawnRequest()
	// - Physics: GPU handles everything
	// - Rendering: GPU buffer is source of truth
	// =====================================================

	// Check if we have any particles (either existing GPU particles or pending spawns)
	const int32 CurrentGPUCount = GPUSimulator->GetParticleCount();
	const int32 PendingSpawnCount = GPUSimulator->GetPendingSpawnCount();

	static int32 SimGPULogCounter = 0;
	if (++SimGPULogCounter % 60 == 1)
	{
		UE_LOG(LogTemp, Log, TEXT("SimulateGPU: GPUCount=%d, PendingSpawn=%d, GPUSimulator=%p"),
			CurrentGPUCount, PendingSpawnCount, GPUSimulator.Get());
	}

	// Build GPU simulation parameters
	const float SubstepDT = Preset->SubstepDeltaTime;
	FGPUFluidSimulationParams GPUParams = BuildGPUSimParams(Preset, Params, SubstepDT);

	// ParticleCount will be updated by GPU after spawn processing
	// Use current GPU count + pending spawns as estimate
	GPUParams.ParticleCount = CurrentGPUCount + PendingSpawnCount;

	// =====================================================
	// Collision Primitives Upload (optimized - upload when dirty)
	// TODO: Add dirty flag check for further optimization
	// =====================================================

	// Phase 2: Skip readback to avoid CPU-GPU sync stall
	// Particles array on CPU may be outdated, but GPU has correct data
	// Renderer should use GPU buffer directly via DataProvider::IsGPUSimulationActive()

	// TODO: Only readback occasionally if CPU needs to know positions
	// (e.g., for spawning near existing particles)
	/*
	if (GPUSimulator->GetParticleCount() > 0)
	{
		GPUSimulator->DownloadParticles(Particles);
	}
	*/

	// Cache collider shapes once per frame (required for IsCacheValid() to return true)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_CacheColliderShapes);
		CacheColliderShapes(Params.Colliders);
	}


	// Collect and upload collision primitives to GPU (with bone tracking for adhesion)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_CollectCollisionPrimitives);
		FGPUCollisionPrimitives CollisionPrimitives;
		// Use persistent bone data for velocity calculation across frames
		CollisionPrimitives.BoneTransforms = MoveTemp(PersistentBoneTransforms);
		const float DefaultFriction = Preset->Friction;
		const float DefaultRestitution = Preset->Bounciness;

		// Check if adhesion is enabled (use bone-aware export path)
		const bool bUseGPUAdhesion = (Preset->Adhesion > 0.0f || Preset->AdhesionVelocityStrength > 0.0f);

		// Collect FluidCollider owner actors to exclude from World Collision query (avoid duplicate collision)
		TSet<const AActor*> FluidColliderOwners;
		for (const UFluidCollider* Collider : Params.Colliders)
		{
			if (Collider && Collider->GetOwner())
			{
				FluidColliderOwners.Add(Collider->GetOwner());
			}
		}

		for (UFluidCollider* Collider : Params.Colliders)
		{
			if (!Collider || !Collider->IsColliderEnabled())
			{
				continue;
			}

			AActor* ColliderOwner = Collider->GetOwner();

			// Check if this is a MeshFluidCollider (has ExportToGPUPrimitives)
			UMeshFluidCollider* MeshCollider = Cast<UMeshFluidCollider>(Collider);
			if (MeshCollider)
			{
				// Cache collider shape if needed
				MeshCollider->CacheCollisionShapes();

				if (MeshCollider->IsCacheValid())
				{
					// Get OwnerID for collision feedback filtering
					// Use the owner actor's UniqueID to identify which actor owns these primitives
					int32 OwnerID = ColliderOwner ? ColliderOwner->GetUniqueID() : 0;

					// Use bone-aware export for GPU adhesion
					if (bUseGPUAdhesion)
					{
						MeshCollider->ExportToGPUPrimitivesWithBones(
							CollisionPrimitives.Spheres,
							CollisionPrimitives.Capsules,
							CollisionPrimitives.Boxes,
							CollisionPrimitives.Convexes,
							CollisionPrimitives.ConvexPlanes,
							CollisionPrimitives.BoneTransforms,
							PersistentBoneNameToIndex,  // Use persistent mapping
							DefaultFriction,
							DefaultRestitution,
							OwnerID
						);
					}
					else
					{
						// Legacy path without bone tracking
						MeshCollider->ExportToGPUPrimitives(
							CollisionPrimitives.Spheres,
							CollisionPrimitives.Capsules,
							CollisionPrimitives.Boxes,
							CollisionPrimitives.Convexes,
							CollisionPrimitives.ConvexPlanes,
							DefaultFriction,
							DefaultRestitution,
							OwnerID
						);
					}
				}
			}
		}

		// Auto-collect Simple Collision from StaticMeshes within Simulation Volume
		// FluidColliderOwners are excluded to avoid duplicate collision processing
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_WorldCollision);
			AppendGPUWorldCollisionPrimitives(
				CollisionPrimitives,
				Params,
				GPUWorldQueryBounds,
				DefaultFriction,
				DefaultRestitution,
				FluidColliderOwners
			);
		}

		// Upload to GPU only if we have primitives
		if (!CollisionPrimitives.IsEmpty())
		{
			// 1. Upload collision primitives to GPU
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_Upload_Primitives);
				GPUSimulator->UploadCollisionPrimitives(CollisionPrimitives);
			}

			// 2. Generate static boundary particles from collision primitives (Akinci 2012)
			// Uses per-primitive caching: only generates particles for NEW primitives
			// GPU upload is skipped if no changes detected (caching optimization)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_Upload_StaticBoundary);
				const bool bHasStaticBoundary = GPUSimulator->HasStaticBoundaryParticles();
				const bool bIsStaticBoundaryEnabledOnGPU = GPUSimulator->IsGPUStaticBoundaryEnabled();

				if (Params.bEnableStaticBoundaryParticles)
				{
					// Invalidate cache if dirty flag is set (World changed)
					if (bStaticBoundaryParticlesDirty)
					{
						GPUSimulator->InvalidateStaticBoundaryCache();
						bStaticBoundaryParticlesDirty = false;
					}

					// Always call GenerateStaticBoundaryParticles - it handles caching internally:
					// - Reuses cached particles for known primitives (no CPU work)
					// - Only generates particles for new primitives
					// - Skips GPU upload if active primitive set unchanged
					GPUSimulator->SetStaticBoundaryParticleSpacing(Params.StaticBoundaryParticleSpacing);
					GPUSimulator->GenerateStaticBoundaryParticles(Preset->SmoothingRadius, Preset->Density);
				}
				else
				{
					// Clear if we have particles or GPU flag is still enabled
					if (bHasStaticBoundary || bIsStaticBoundaryEnabledOnGPU)
					{
						GPUSimulator->ClearStaticBoundaryParticles();
						UE_LOG(LogTemp, Log, TEXT("Static boundary cleared: bHasStaticBoundary=%d, bIsGPUEnabled=%d"),
							bHasStaticBoundary, bIsStaticBoundaryEnabledOnGPU);
					}
					bStaticBoundaryParticlesDirty = false;
				}
			}

			// 3. Set adhesion parameters if enabled
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_Upload_Adhesion);
				if (bUseGPUAdhesion && CollisionPrimitives.BoneTransforms.Num() > 0)
				{
					FGPUAdhesionParams AdhesionParams;
					AdhesionParams.bEnableAdhesion = 0;
					AdhesionParams.AdhesionStrength = Preset->Adhesion;
					AdhesionParams.AdhesionRadius = Preset->AdhesionRadius;
					AdhesionParams.ColliderContactOffset = Preset->AdhesionContactOffset;
					AdhesionParams.BoneVelocityScale = Preset->AdhesionBoneVelocityScale;
					AdhesionParams.SlidingFriction = DefaultFriction;
					AdhesionParams.CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
					AdhesionParams.Gravity = FVector3f(Params.ExternalForce);
					AdhesionParams.GravitySlidingScale = 1.0f;

					GPUSimulator->SetAdhesionParams(AdhesionParams);
				}
				else
				{
					FGPUAdhesionParams AdhesionParams;
					AdhesionParams.bEnableAdhesion = 0;
					GPUSimulator->SetAdhesionParams(AdhesionParams);
				}
			}
		}

		// Save bone transforms back to persistent storage for next frame
		PersistentBoneTransforms = MoveTemp(CollisionPrimitives.BoneTransforms);
	}

	// =====================================================
	// Landscape Heightmap Collision
	// Extract heightmap from Landscape actors and upload to GPU
	// =====================================================
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_LandscapeHeightmap);
		UpdateLandscapeHeightmapCollision(Params, Preset);
	}

	// =====================================================
	// GPU Boundary Skinning (Flex-style Adhesion)
	// Upload local particles once, bone transforms each frame
	// GPU transforms local → world (much faster than CPU)
	// =====================================================
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_BoundarySkinning);
		int32 TotalBoundaryParticles = 0;

		for (UKawaiiFluidInteractionComponent* Interaction : Params.InteractionComponents)
		{
			if (!Interaction) continue;

			const int32 OwnerID = Interaction->GetBoundaryOwnerID();

			// Check if this interaction has active boundary particles (enabled AND initialized)
			if (Interaction->HasLocalBoundaryParticles())
			{
				// Upload local particles only once (first time or after regeneration)
				if (!GPUSimulator->IsGPUBoundarySkinningEnabled() ||
				    GPUSimulator->GetTotalLocalBoundaryParticleCount() == 0)
				{
					// Calculate Psi from Preset and Interaction spacing (Akinci 2012)
					// Psi = RestDensity * EffectiveVolume * ScalingFactor
					// For surface sampling: EffectiveVolume = Spacing² × (Spacing/2)
					// Convert cm to m for proper unit consistency
					const float Spacing_m = Interaction->BoundaryParticleSpacing * 0.01f;  // cm → m
					const float RestDensity = Preset->Density;  // kg/m³
					const float ParticleRadius_m = Spacing_m * 0.5f;
					const float SurfaceArea_m = Spacing_m * Spacing_m;
					const float EffectiveVolume_m = SurfaceArea_m * ParticleRadius_m;
					const float Psi = RestDensity * EffectiveVolume_m * 0.3f;
					const float Friction = Preset->Friction;

					TArray<FGPUBoundaryParticleLocal> LocalParticles;
					Interaction->CollectLocalBoundaryParticles(LocalParticles, Psi, Friction);

					if (LocalParticles.Num() > 0)
					{
						GPUSimulator->UploadLocalBoundaryParticles(OwnerID, LocalParticles);

						// Register skeletal mesh reference for late bone transform refresh
						// Bone transforms will be read in BeginRenderViewFamily for frame sync
						if (USkeletalMeshComponent* SkelMesh = Interaction->GetOwnerSkeletalMesh())
						{
							GPUSimulator->RegisterSkeletalMeshForBoundary(OwnerID, SkelMesh);
						}
					}
				}

				// NOTE: Bone transforms are NO LONGER uploaded here!
				// They are refreshed in FluidSceneViewExtension::BeginRenderViewFamily
				// right before render thread starts, ensuring sync with skeletal mesh rendering

				// Update boundary owner AABB for early-out optimization
				// Get bounds from SkeletalMeshComponent if available
				if (USkeletalMeshComponent* SkelMesh = Interaction->GetOwnerSkeletalMesh())
				{
					FBoxSphereBounds MeshBounds = SkelMesh->Bounds;
					FGPUBoundaryOwnerAABB OwnerAABB(
						FVector3f(MeshBounds.Origin - MeshBounds.BoxExtent),
						FVector3f(MeshBounds.Origin + MeshBounds.BoxExtent)
					);
					GPUSimulator->UpdateBoundaryOwnerAABB(OwnerID, OwnerAABB);
				}
			}
			else if (Interaction->HasInitializedBoundaryParticles())
			{
				// Interaction has initialized particles but bEnableBoundaryParticles is false
				// Remove skinning data from GPU to prevent unnecessary processing
				GPUSimulator->RemoveBoundarySkinningData(OwnerID);
			}
		}

		TotalBoundaryParticles = GPUSimulator->GetTotalLocalBoundaryParticleCount();

		// Set boundary adhesion parameters
		if (TotalBoundaryParticles > 0)
		{
			FGPUBoundaryAdhesionParams BoundaryAdhesionParams;
			BoundaryAdhesionParams.bEnabled = 1;
			BoundaryAdhesionParams.AdhesionForceStrength = Preset->Adhesion;
			BoundaryAdhesionParams.AdhesionVelocityStrength = Preset->AdhesionVelocityStrength;
			BoundaryAdhesionParams.AdhesionRadius = Preset->AdhesionRadius;
			BoundaryAdhesionParams.CohesionStrength = Preset->SurfaceTension;
			BoundaryAdhesionParams.SmoothingRadius = Preset->SmoothingRadius;
			BoundaryAdhesionParams.BoundaryParticleCount = TotalBoundaryParticles;
			BoundaryAdhesionParams.FluidParticleCount = GPUSimulator->GetParticleCount();
			BoundaryAdhesionParams.DeltaTime = Preset->SubstepDeltaTime;

			GPUSimulator->SetBoundaryAdhesionParams(BoundaryAdhesionParams);
		}
		else
		{
			FGPUBoundaryAdhesionParams BoundaryAdhesionParams;
			BoundaryAdhesionParams.bEnabled = 0;
			GPUSimulator->SetBoundaryAdhesionParams(BoundaryAdhesionParams);
		}
	}

	// =====================================================
	// Run GPU simulation with Accumulator method
	// Simulate with fixed dt substeps for frame-rate independence
	// =====================================================
	int32 SubstepCount = 0;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SimGPU_Substeps);
		const int32 MaxSubstepsPerFrame = Preset->MaxSubsteps;
		const float MaxAllowedTime = Preset->SubstepDeltaTime * MaxSubstepsPerFrame;
		AccumulatedTime += FMath::Min(DeltaTime, MaxAllowedTime);

		int32 TotalSubsteps = FMath::Min(
			FMath::FloorToInt(AccumulatedTime / Preset->SubstepDeltaTime),
			MaxSubstepsPerFrame
		);

		// Frame lifecycle: BeginFrame (spawn/despawn, readback process)
		GPUSimulator->BeginFrame();

		for (; SubstepCount < TotalSubsteps; ++SubstepCount)
		{
			GPUParams.SubstepIndex = SubstepCount;
			GPUParams.TotalSubsteps = TotalSubsteps;

			GPUSimulator->SimulateSubstep(GPUParams);

			AccumulatedTime -= Preset->SubstepDeltaTime;
		}

		// Frame lifecycle: EndFrame (readback enqueue)
		GPUSimulator->EndFrame();
	}

	//========================================
	// GPU Statistics Collection
	// For GPU comparison, collect basic stats without particle readback
	//========================================
	CollectGPUSimulationStats(Preset, GPUParams.ParticleCount, SubstepCount);
}

void UKawaiiFluidSimulationContext::Simulate(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float DeltaTime,
	float& AccumulatedTime)
{
	SCOPE_CYCLE_COUNTER(STAT_ContextSimulate);
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_Simulate);

	if (!Preset)
	{
		return;
	}

	// Always use GPU simulation
	SimulateGPU(Particles, Preset, Params, SpatialHash, DeltaTime, AccumulatedTime);
}

void UKawaiiFluidSimulationContext::SimulateSubstep(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float SubstepDT)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_SimulateSubstep);

	// 1. Predict positions
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextPredictPositions);
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_PredictPositions);
		PredictPositions(Particles, Preset, Params.ExternalForce, SubstepDT);
	}

	// 2. Update neighbors
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextUpdateNeighbors);
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_UpdateNeighbors);
		UpdateNeighbors(Particles, SpatialHash, Preset->SmoothingRadius);
	}

	// 3. Solve density constraints
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextSolveDensity);
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiFluidContext_SolveDensity);

		SolveDensityConstraints(Particles, Preset, SubstepDT);
	}

	// 4. Handle collisions
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextHandleCollisions);
		HandleCollisions(Particles, Params.Colliders, SubstepDT);
	}

	// 5. World collision
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextWorldCollision);
		if (Params.bUseWorldCollision && Params.World)
		{
			HandleWorldCollision(Particles, Params, SpatialHash, Params.ParticleRadius, SubstepDT, Preset->Friction, Preset->Bounciness);
		}
	}

	// 6. Finalize positions
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextFinalizePositions);
		FinalizePositions(Particles, SubstepDT);
	}

	// 7. Apply viscosity
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextApplyViscosity);
		ApplyViscosity(Particles, Preset);
	}

	// 8. Apply adhesion
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextApplyAdhesion);
		ApplyAdhesion(Particles, Preset, Params.Colliders);
	}

	// 9. Apply cohesion (surface tension between particles)
	{
		SCOPE_CYCLE_COUNTER(STAT_ContextApplyCohesion);
		ApplyCohesion(Particles, Preset);
	}

	// 10. Apply stack pressure (weight transfer from stacked attached particles)
	if (Preset->bEnableStackPressure && StackPressureSolver.IsValid())
	{
		float SearchRadius = Preset->StackPressureRadius > 0.0f
			? Preset->StackPressureRadius
			: Preset->SmoothingRadius;

		StackPressureSolver->Apply(
			Particles,
			Preset->Gravity,
			Preset->StackPressureScale,
			SearchRadius,
			SubstepDT
		);
	}
}

void UKawaiiFluidSimulationContext::RunInitializationSimulation(
	const UKawaiiFluidPresetDataAsset* Preset,
	const FKawaiiFluidSimulationParams& Params,
	int32 ParticleCount)
{
	if (!Preset)
	{
		UE_LOG(LogTemp, Warning, TEXT("RunInitializationSimulation: No Preset provided"));
		return;
	}

	if (!IsGPUSimulatorReady())
	{
		UE_LOG(LogTemp, Warning, TEXT("RunInitializationSimulation: GPU Simulator not ready"));
		return;
	}

	// Convert to GPU parameters (use full params with colliders/interactions)
	const float SubstepDT = Preset->SubstepDeltaTime;
	FGPUFluidSimulationParams GPUParams = BuildGPUSimParams(Preset, Params, SubstepDT);
	GPUParams.ParticleCount = ParticleCount;
	GPUParams.SubstepIndex = 0;
	GPUParams.TotalSubsteps = 1;  // Single substep (triggers anisotropy: 0 == 1-1)

	// Temporarily enable anisotropy for initialization (restore after simulation)
	FFluidAnisotropyParams TempAnisotropyParams;
	TempAnisotropyParams.bEnabled = true;
	TempAnisotropyParams.Mode = EFluidAnisotropyMode::DensityBased;
	TempAnisotropyParams.Strength = 1.0f;
	TempAnisotropyParams.VelocityStretchFactor = 0.0f;  // No velocity stretch for initialization
	TempAnisotropyParams.MinStretch = 0.5f;
	TempAnisotropyParams.MaxStretch = 2.0f;
	TempAnisotropyParams.DensityWeight = 1.0f;
	TempAnisotropyParams.UpdateInterval = 1;

	FFluidAnisotropyParams PreviousParams = GPUSimulator->GetAnisotropyParams();
	GPUSimulator->SetAnisotropyParams(TempAnisotropyParams);

	// Run one simulation step to stabilize particles
	GPUSimulator->RunInitializationSimulation(GPUParams);

	// Restore previous anisotropy parameters
	GPUSimulator->SetAnisotropyParams(PreviousParams);

	UE_LOG(LogTemp, Log, TEXT("RunInitializationSimulation: Completed for %d particles"), ParticleCount);
}

void UKawaiiFluidSimulationContext::PredictPositions(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const FVector& ExternalForce,
	float DeltaTime)
{
	const FVector TotalForce = Preset->Gravity + ExternalForce;

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		FFluidParticle& Particle = Particles[i];

		FVector AppliedForce = TotalForce;

		// Attached particles: apply only tangent gravity (sliding effect)
		if (Particle.bIsAttached)
		{
			const FVector& Normal = Particle.AttachedSurfaceNormal;
			float NormalComponent = FVector::DotProduct(Preset->Gravity, Normal);
			FVector TangentGravity = Preset->Gravity - NormalComponent * Normal;
			AppliedForce = TangentGravity + ExternalForce;
		}

		Particle.Velocity += AppliedForce * DeltaTime;
		Particle.PredictedPosition = Particle.Position + Particle.Velocity * DeltaTime;
	});
}

void UKawaiiFluidSimulationContext::UpdateNeighbors(
	TArray<FFluidParticle>& Particles,
	FSpatialHash& SpatialHash,
	float SmoothingRadius)
{
	// Rebuild spatial hash (sequential - hashmap write)
	TArray<FVector> Positions;
	Positions.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Positions.Add(Particle.PredictedPosition);
	}

	SpatialHash.BuildFromPositions(Positions);

	// Cache neighbors for each particle (parallel - read only)
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		SpatialHash.GetNeighbors(
			Particles[i].PredictedPosition,
			SmoothingRadius,
			Particles[i].NeighborIndices
		);
	});
}

void UKawaiiFluidSimulationContext::SolveDensityConstraints(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	float DeltaTime)
{
	if (!DensityConstraint.IsValid())
	{
		return;
	}

	// XPBD: Initialize Lambda (reset to 0 at start of each timestep)
	ParallelFor(Particles.Num(), [&](int32 i)
	{
		Particles[i].Lambda = 0.0f;
	});

	// Artificial Pressure (PBF Eq.13-14) for Tensile Instability Correction
	// ArtificialPressure > 0 enables anti-clumping effect
	// SCALE FACTOR: Same as GPU path (100x) for consistent behavior
	constexpr float ArtificialPressureScaleFactorCPU = 100.0f;
	const bool bUseArtificialPressure = (Preset->ArtificialPressure > 0.0f);
	const float ScaledArtificialPressureK = Preset->ArtificialPressure * ArtificialPressureScaleFactorCPU;

	// Scale Compliance based on SmoothingRadius for stability across different particle sizes
	const float ScaledCompliance = SPHScaling::GetScaledCompliance(
		Preset->Compressibility, Preset->SmoothingRadius, Preset->ComplianceExponent);

	// XPBD iterative solver (viscous fluid: 2-3 iterations, water: 4-6 iterations)
	const int32 SolverIterations = Preset->SolverIterations;
	for (int32 Iter = 0; Iter < SolverIterations; ++Iter)
	{
		if (bUseArtificialPressure)
		{
			// Solver with Artificial Pressure (Tensile Instability correction)
			FTensileInstabilityParams TensileParams;
			TensileParams.bEnabled = true;
			TensileParams.K = ScaledArtificialPressureK;
			TensileParams.N = Preset->ArtificialPressureExponent;
			TensileParams.DeltaQ = Preset->ArtificialPressureDeltaQ;

			DensityConstraint->SolveWithTensileCorrection(
				Particles,
				Preset->SmoothingRadius,
				Preset->Density,
				ScaledCompliance,
				DeltaTime,
				TensileParams
			);
		}
		else
		{
			// Default solver
			DensityConstraint->Solve(
				Particles,
				Preset->SmoothingRadius,
				Preset->Density,
				ScaledCompliance,
				DeltaTime
			);
		}
	}
}

void UKawaiiFluidSimulationContext::CacheColliderShapes(const TArray<TObjectPtr<UFluidCollider>>& Colliders)
{
	for (UFluidCollider* Collider : Colliders)
	{
		if (Collider && Collider->IsColliderEnabled())
		{
			Collider->CacheCollisionShapes();
		}
	}
}

void UKawaiiFluidSimulationContext::AppendGPUWorldCollisionPrimitives(
	FGPUCollisionPrimitives& OutPrimitives,
	const FKawaiiFluidSimulationParams& Params,
	const FBox& QueryBounds,
	float DefaultFriction,
	float DefaultRestitution,
	const TSet<const AActor*>& FluidColliderOwners)
{
	// Diagnostic log (every 60 frames)
	static int32 DiagLogCounter = 0;
	const bool bShouldLog = (++DiagLogCounter % 60 == 1);

	if (!Params.bUseWorldCollision)
	{
		if (bShouldLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[WorldCollision] SKIP: bUseWorldCollision=false"));
		}
		bGPUWorldCollisionCacheDirty = true;
		return;
	}

	if (!Params.World)
	{
		if (bShouldLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[WorldCollision] SKIP: World is nullptr"));
		}
		bGPUWorldCollisionCacheDirty = true;
		return;
	}

	if (!QueryBounds.IsValid)
	{
		if (bShouldLog)
		{
			UE_LOG(LogTemp, Warning, TEXT("[WorldCollision] SKIP: QueryBounds is invalid"));
		}
		return;
	}

	const bool bWorldChanged = CachedGPUWorldCollisionWorld.Get() != Params.World;
	const bool bBoundsChanged = !AreBoundsEqual(CachedGPUWorldCollisionBounds, QueryBounds);

	if (bGPUWorldCollisionCacheDirty || bWorldChanged || bBoundsChanged)
	{
		CachedGPUWorldCollisionPrimitives.Reset();
		CachedGPUWorldCollisionBounds = QueryBounds;
		CachedGPUWorldCollisionWorld = Params.World;
		bGPUWorldCollisionCacheDirty = false;

		// Static boundary particle cache invalidation policy:
		// - World changed: Full cache invalidation required (primitives may have moved/changed)
		// - Bounds expanded: NO invalidation needed - existing cached particles remain valid
		//   (StaticBoundaryManager uses per-primitive caching, so only NEW primitives
		//    discovered in the expanded region will trigger generation)
		// This optimization prevents 1+ second stalls every frame in Unlimited mode
		if (bWorldChanged)
		{
			bStaticBoundaryParticlesDirty = true;
			bLandscapeHeightmapDirty = true;
		}
		// Note: bBoundsChanged alone no longer triggers bStaticBoundaryParticlesDirty
		// StaticBoundaryManager handles caching internally

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(KawaiiFluidGPUWorldCollision), false);
		QueryParams.bTraceComplex = false;
		QueryParams.bReturnPhysicalMaterial = false;
		if (Params.IgnoreActor.IsValid())
		{
			QueryParams.AddIgnoredActor(Params.IgnoreActor.Get());
		}

		TArray<FOverlapResult> Overlaps;
		const FVector QueryCenter = QueryBounds.GetCenter();
		const FVector QueryExtent = QueryBounds.GetExtent();

		// Query both WorldStatic and WorldDynamic
		FCollisionObjectQueryParams ObjectQueryParams;
		ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldStatic);
		ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldDynamic);
		
		Params.World->OverlapMultiByObjectType(
			Overlaps,
			QueryCenter,
			FQuat::Identity,
			ObjectQueryParams,
			FCollisionShape::MakeBox(QueryExtent),
			QueryParams
		);

		TSet<const UPrimitiveComponent*> UniqueComponents;
		int32 TotalOverlaps = Overlaps.Num();
		int32 ValidStaticMeshCount = 0;

		for (const FOverlapResult& Overlap : Overlaps)
		{
			const UPrimitiveComponent* PrimComp = Overlap.Component.Get();
			if (!PrimComp || UniqueComponents.Contains(PrimComp))
			{
				continue;
			}
			UniqueComponents.Add(PrimComp);

			if (PrimComp->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
			{
				continue;
			}

			// Allow both WorldStatic and WorldDynamic as World Collision targets
			const ECollisionChannel ObjectType = PrimComp->GetCollisionObjectType();
			if (ObjectType != ECC_WorldStatic && ObjectType != ECC_WorldDynamic)
			{
				continue;
			}

			const AActor* Owner = PrimComp->GetOwner();

			// Skip if this actor has FluidCollider (already processed, avoid duplicate collision)
			if (Owner && FluidColliderOwners.Contains(Owner))
			{
				continue;
			}

			// ISMC: Process each instance with its own world transform
			const UInstancedStaticMeshComponent* ISMComp = Cast<UInstancedStaticMeshComponent>(PrimComp);
			if (ISMComp)
			{
				const UStaticMesh* StaticMesh = ISMComp->GetStaticMesh();
				const UBodySetup* BodySetup = StaticMesh ? StaticMesh->GetBodySetup() : nullptr;
				if (!BodySetup)
				{
					continue;
				}

				const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
				if (AggGeom.SphereElems.Num() == 0 && AggGeom.SphylElems.Num() == 0 &&
					AggGeom.BoxElems.Num() == 0 && AggGeom.ConvexElems.Num() == 0)
				{
					continue;
				}

				const int32 InstanceCount = ISMComp->GetInstanceCount();
				if (InstanceCount == 0)
				{
					continue;
				}

				// Limit instance count to prevent GPU buffer overflow
				const int32 EffectiveInstanceCount = FMath::Min(InstanceCount, MaxISMCInstancesForCollision);
				if (InstanceCount > MaxISMCInstancesForCollision)
				{
					static int32 ISMCWarningLogCounter = 0;
					if (++ISMCWarningLogCounter % 300 == 1)
					{
						UE_LOG(LogTemp, Warning,
							TEXT("[WorldCollision] ISMC '%s' has %d instances, limiting to %d for collision"),
							*ISMComp->GetName(), InstanceCount, MaxISMCInstancesForCollision);
					}
				}

				ValidStaticMeshCount++;
				const int32 OwnerID = Owner ? Owner->GetUniqueID() : 0;

				// Create collision primitives for each instance
				for (int32 InstanceIndex = 0; InstanceIndex < EffectiveInstanceCount; ++InstanceIndex)
				{
					FTransform InstanceWorldTransform;
					if (!ISMComp->GetInstanceTransform(InstanceIndex, InstanceWorldTransform, true))
					{
						continue;
					}

					AppendAggGeomToGPUPrimitives(
						AggGeom,
						InstanceWorldTransform,
						DefaultFriction,
						DefaultRestitution,
						OwnerID,
						CachedGPUWorldCollisionPrimitives
					);
				}

				// Debug logging (throttled)
				if (bShouldLog)
				{
					UE_LOG(LogTemp, Log, TEXT("  [WorldCollision] ISMC: %s, Instances: %d (effective: %d)"),
						*ISMComp->GetName(), InstanceCount, EffectiveInstanceCount);
				}

				continue;  // Skip regular StaticMeshComponent handling below
			}

			// Regular StaticMeshComponent handling
			const UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(PrimComp);
			if (!StaticMeshComp)
			{
				continue;
			}

			const UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh();
			const UBodySetup* BodySetup = StaticMesh ? StaticMesh->GetBodySetup() : nullptr;
			if (!BodySetup)
			{
				continue;
			}

			const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
			if (AggGeom.SphereElems.Num() == 0 && AggGeom.SphylElems.Num() == 0 &&
				AggGeom.BoxElems.Num() == 0 && AggGeom.ConvexElems.Num() == 0)
			{
				continue;
			}

			ValidStaticMeshCount++;

			// Individual StaticMesh debug log
			if (bShouldLog)
			{
				const FVector MeshLocation = StaticMeshComp->GetComponentLocation();
				const FVector MeshScale = StaticMeshComp->GetComponentScale();
				UE_LOG(LogTemp, Log, TEXT("  [WorldCollision] Mesh: %s, Owner: %s"),
					*StaticMesh->GetName(),
					Owner ? *Owner->GetName() : TEXT("None"));
				UE_LOG(LogTemp, Log, TEXT("    Location: (%.1f, %.1f, %.1f), Scale: (%.2f, %.2f, %.2f)"),
					MeshLocation.X, MeshLocation.Y, MeshLocation.Z,
					MeshScale.X, MeshScale.Y, MeshScale.Z);
				UE_LOG(LogTemp, Log, TEXT("    Collision: Spheres=%d, Capsules=%d, Boxes=%d, Convexes=%d"),
					AggGeom.SphereElems.Num(), AggGeom.SphylElems.Num(),
					AggGeom.BoxElems.Num(), AggGeom.ConvexElems.Num());

				// Detailed primitive information
				for (int32 i = 0; i < AggGeom.SphereElems.Num(); ++i)
				{
					const FKSphereElem& Sphere = AggGeom.SphereElems[i];
					UE_LOG(LogTemp, Log, TEXT("      Sphere[%d]: Center=(%.1f, %.1f, %.1f), Radius=%.1f"),
						i, Sphere.Center.X, Sphere.Center.Y, Sphere.Center.Z, Sphere.Radius);
				}
				for (int32 i = 0; i < AggGeom.BoxElems.Num(); ++i)
				{
					const FKBoxElem& Box = AggGeom.BoxElems[i];
					UE_LOG(LogTemp, Log, TEXT("      Box[%d]: Center=(%.1f, %.1f, %.1f), Size=(%.1f, %.1f, %.1f)"),
						i, Box.Center.X, Box.Center.Y, Box.Center.Z, Box.X, Box.Y, Box.Z);
				}
				for (int32 i = 0; i < AggGeom.ConvexElems.Num(); ++i)
				{
					const FKConvexElem& Convex = AggGeom.ConvexElems[i];
					UE_LOG(LogTemp, Log, TEXT("      Convex[%d]: Vertices=%d, Indices=%d"),
						i, Convex.VertexData.Num(), Convex.IndexData.Num());
					if (Convex.VertexData.Num() > 0)
					{
						// Calculate bounding box
						FBox ConvexBounds(ForceInit);
						for (const FVector& V : Convex.VertexData)
						{
							ConvexBounds += V;
						}
						UE_LOG(LogTemp, Log, TEXT("        LocalBounds: Min=(%.1f, %.1f, %.1f), Max=(%.1f, %.1f, %.1f)"),
							ConvexBounds.Min.X, ConvexBounds.Min.Y, ConvexBounds.Min.Z,
							ConvexBounds.Max.X, ConvexBounds.Max.Y, ConvexBounds.Max.Z);
					}
				}
			}

			const int32 OwnerID = Owner ? Owner->GetUniqueID() : 0;
			AppendAggGeomToGPUPrimitives(
				AggGeom,
				StaticMeshComp->GetComponentTransform(),
				DefaultFriction,
				DefaultRestitution,
				OwnerID,
				CachedGPUWorldCollisionPrimitives
			);
		}

		// Log output (only on cache refresh, every 60 frames)
		static int32 WorldCollisionLogCounter = 0;
		if (++WorldCollisionLogCounter % 60 == 1)
		{
			UE_LOG(LogTemp, Log, TEXT("========== GPU World Collision Cache Updated =========="));
			UE_LOG(LogTemp, Log, TEXT("  Query Bounds: Center=(%.1f, %.1f, %.1f) Extent=(%.1f, %.1f, %.1f)"),
				QueryCenter.X, QueryCenter.Y, QueryCenter.Z,
				QueryExtent.X, QueryExtent.Y, QueryExtent.Z);
			UE_LOG(LogTemp, Log, TEXT("  Overlaps Found: %d (Unique Components: %d)"),
				TotalOverlaps, UniqueComponents.Num());
			UE_LOG(LogTemp, Log, TEXT("  Valid StaticMeshes with Simple Collision: %d"), ValidStaticMeshCount);
			UE_LOG(LogTemp, Log, TEXT("  Cached Primitives: Spheres=%d, Capsules=%d, Boxes=%d, Convexes=%d"),
				CachedGPUWorldCollisionPrimitives.Spheres.Num(),
				CachedGPUWorldCollisionPrimitives.Capsules.Num(),
				CachedGPUWorldCollisionPrimitives.Boxes.Num(),
				CachedGPUWorldCollisionPrimitives.Convexes.Num());
			UE_LOG(LogTemp, Log, TEXT("========================================================"));
		}
	}

	if (CachedGPUWorldCollisionPrimitives.IsEmpty())
	{
		return;
	}

	const int32 PlaneOffset = OutPrimitives.ConvexPlanes.Num();
	OutPrimitives.Spheres.Append(CachedGPUWorldCollisionPrimitives.Spheres);
	OutPrimitives.Capsules.Append(CachedGPUWorldCollisionPrimitives.Capsules);
	OutPrimitives.Boxes.Append(CachedGPUWorldCollisionPrimitives.Boxes);
	OutPrimitives.ConvexPlanes.Append(CachedGPUWorldCollisionPrimitives.ConvexPlanes);

	for (const FGPUCollisionConvex& CachedConvex : CachedGPUWorldCollisionPrimitives.Convexes)
	{
		FGPUCollisionConvex Convex = CachedConvex;
		Convex.PlaneStartIndex += PlaneOffset;
		OutPrimitives.Convexes.Add(Convex);
	}
}

void UKawaiiFluidSimulationContext::HandleCollisions(
	TArray<FFluidParticle>& Particles,
	const TArray<TObjectPtr<UFluidCollider>>& Colliders,
	float SubstepDT)
{
	for (UFluidCollider* Collider : Colliders)
	{
		if (Collider && Collider->IsColliderEnabled())
		{
			Collider->ResolveCollisions(Particles, SubstepDT);
		}
	}
}

void UKawaiiFluidSimulationContext::HandleWorldCollision(
	TArray<FFluidParticle>& Particles,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float ParticleRadius,
	float SubstepDT,
	float Friction,
	float Restitution)
{
	// Dispatch to appropriate method based on WorldCollisionMethod
	switch (Params.WorldCollisionMethod)
	{
	case EWorldCollisionMethod::SDF:
		HandleWorldCollision_SDF(Particles, Params, SpatialHash, ParticleRadius, SubstepDT, Friction, Restitution);
		break;

	case EWorldCollisionMethod::Sweep:
	default:
		HandleWorldCollision_Sweep(Particles, Params, SpatialHash, ParticleRadius, SubstepDT, Friction, Restitution);
		break;
	}
}

//========================================
// Legacy Sweep-based World Collision
//========================================

void UKawaiiFluidSimulationContext::HandleWorldCollision_Sweep(
	TArray<FFluidParticle>& Particles,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float ParticleRadius,
	float SubstepDT,
	float Friction,
	float Restitution)
{
	UWorld* World = Params.World;
	if (!World || Particles.Num() == 0)
	{
		return;
	}

	const float CellSize = SpatialHash.GetCellSize();
	const auto& Grid = SpatialHash.GetGrid();

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;
	if (Params.IgnoreActor.IsValid())
	{
		QueryParams.AddIgnoredActor(Params.IgnoreActor.Get());
	}

	// Cell-based broad-phase
	struct FCellQueryData
	{
		FVector CellCenter;
		FVector CellExtent;
		TArray<int32> ParticleIndices;
	};

	TArray<FCellQueryData> CellQueries;
	CellQueries.Reserve(Grid.Num());

	for (const auto& Pair : Grid)
	{
		FCellQueryData CellData;
		CellData.CellCenter = FVector(Pair.Key) * CellSize + FVector(CellSize * 0.5f);
		CellData.CellExtent = FVector(CellSize * 0.5f);
		CellData.ParticleIndices = Pair.Value;
		CellQueries.Add(MoveTemp(CellData));
	}

	// Cell overlap check - parallel
	TArray<uint8> CellCollisionResults;
	CellCollisionResults.SetNumZeroed(CellQueries.Num());

	ParallelFor(CellQueries.Num(), [&](int32 CellIdx)
	{
		const FCellQueryData& CellData = CellQueries[CellIdx];
		if (World->OverlapBlockingTestByChannel(
			CellData.CellCenter, FQuat::Identity, ECC_WorldStatic,
			FCollisionShape::MakeBox(CellData.CellExtent), QueryParams))
		{
			CellCollisionResults[CellIdx] = 1;
		}
	});

	// Collect collision candidates
	TArray<int32> CollisionParticleIndices;
	CollisionParticleIndices.Reserve(Particles.Num());

	for (int32 CellIdx = 0; CellIdx < CellQueries.Num(); ++CellIdx)
	{
		if (CellCollisionResults[CellIdx])
		{
			CollisionParticleIndices.Append(CellQueries[CellIdx].ParticleIndices);
		}
	}

	if (CollisionParticleIndices.Num() == 0)
	{
		return;
	}

	// Physics scene read lock
	FPhysScene* PhysScene = World->GetPhysicsScene();
	if (!PhysScene)
	{
		return;
	}

	FPhysicsCommand::ExecuteRead(PhysScene, [&]()
	{
		ParallelFor(CollisionParticleIndices.Num(), [&](int32 j)
		{
			const int32 i = CollisionParticleIndices[j];
			FFluidParticle& Particle = Particles[i];

			FCollisionQueryParams LocalParams;
			LocalParams.bTraceComplex = false;
			LocalParams.bReturnPhysicalMaterial = false;
			if (Params.IgnoreActor.IsValid())
			{
				LocalParams.AddIgnoredActor(Params.IgnoreActor.Get());
			}

			// Sweep from Position to PredictedPosition
			FHitResult HitResult;
			bool bHit = World->SweepSingleByChannel(
				HitResult,
				Particle.Position,
				Particle.PredictedPosition,
				FQuat::Identity,
				ECC_WorldStatic,
				FCollisionShape::MakeSphere(ParticleRadius),
				LocalParams
			);

			if (bHit && HitResult.bBlockingHit)
			{
				FVector CollisionPos = HitResult.Location + HitResult.ImpactNormal * 0.01f;
				const FVector& Normal = HitResult.ImpactNormal;

				// Only modify PredictedPosition
				Particle.PredictedPosition = CollisionPos;

				// Calculate desired velocity after collision response
				// Initialize to zero - particle stops on surface by default
				FVector DesiredVelocity = FVector::ZeroVector;
				float VelDotNormal = FVector::DotProduct(Particle.Velocity, Normal);

				// Minimum velocity threshold for applying restitution bounce
				// Prevents "popcorn" oscillation for particles resting on surfaces
				const float MinBounceVelocity = 50.0f;  // cm/s

				if (VelDotNormal < 0.0f)
				{
					// Particle moving INTO surface - apply collision response
					FVector VelNormal = Normal * VelDotNormal;
					FVector VelTangent = Particle.Velocity - VelNormal;

					if (VelDotNormal < -MinBounceVelocity)
					{
						// Significant impact - apply full collision response
						// Normal: reflect with Restitution (0 = stick, 1 = perfect bounce)
						// Tangent: dampen with Friction (0 = slide, 1 = stop)
						DesiredVelocity = VelTangent * (1.0f - Friction) - VelNormal * Restitution;
					}
					else
					{
						// Low velocity contact (resting on surface) - no bounce, just slide
						DesiredVelocity = VelTangent * (1.0f - Friction);
					}
				}
				// else: VelDotNormal >= 0 means particle moving AWAY from surface
				// DesiredVelocity stays zero - particle stops on surface (same as OLD behavior)

				// Back-calculate Position so FinalizePositions derives DesiredVelocity
				// FinalizePositions: Velocity = (PredictedPosition - Position) / dt
				// Therefore: Position = PredictedPosition - DesiredVelocity * dt
				Particle.Position = Particle.PredictedPosition - DesiredVelocity * SubstepDT;

				// Add to collision event buffer (processed later in ProcessCollisionFeedback)
				if (Params.bEnableCollisionEvents && Params.CPUCollisionFeedbackBufferPtr && Params.CPUCollisionFeedbackLockPtr)
				{
					const float Speed = Particle.Velocity.Size();
					if (Speed >= Params.MinVelocityForEvent)
					{
						FKawaiiFluidCollisionEvent Event;
						Event.ParticleIndex = Particle.ParticleID;
						Event.SourceID = Particle.SourceID;
						Event.ColliderOwnerID = HitResult.GetActor() ? HitResult.GetActor()->GetUniqueID() : -1;
						Event.BoneIndex = -1;  // CPU path doesn't have bone info
						Event.HitActor = HitResult.GetActor();
						Event.HitLocation = HitResult.Location;
						Event.HitNormal = HitResult.ImpactNormal;
						Event.HitSpeed = Speed;
						// HitInteractionComponent is looked up in ProcessCollisionFeedback

						// Add to buffer (thread-safe)
						FScopeLock Lock(Params.CPUCollisionFeedbackLockPtr);
						Params.CPUCollisionFeedbackBufferPtr->Add(Event);
					}
				}

				// Detach from character if hitting different surface
				if (Particle.bIsAttached)
				{
					AActor* HitActor = HitResult.GetActor();
					if (HitActor != Particle.AttachedActor.Get())
					{
						Particle.bIsAttached = false;
						Particle.AttachedActor.Reset();
						Particle.AttachedBoneName = NAME_None;
						Particle.AttachedLocalOffset = FVector::ZeroVector;
						Particle.AttachedSurfaceNormal = FVector::UpVector;
					}
				}
			}
			else if (Particle.bIsAttached)
			{
				// Floor detection for attached particles
				const float FloorCheckDistance = 3.0f;
				FHitResult FloorHit;
				bool bNearFloor = World->LineTraceSingleByChannel(
					FloorHit,
					Particle.Position,
					Particle.Position - FVector(0, 0, FloorCheckDistance),
					ECC_WorldStatic,
					LocalParams
				);

				if (bNearFloor && FloorHit.GetActor() != Particle.AttachedActor.Get())
				{
					Particle.bIsAttached = false;
					Particle.AttachedActor.Reset();
					Particle.AttachedBoneName = NAME_None;
					Particle.AttachedLocalOffset = FVector::ZeroVector;
					Particle.AttachedSurfaceNormal = FVector::UpVector;
				}
			}
		});
	});

	// Floor detachment check
	const float FloorDetachDistance = 5.0f;
	const float FloorNearDistance = 20.0f;

	for (FFluidParticle& Particle : Particles)
	{
		if (!Particle.bIsAttached)
		{
			Particle.bNearGround = false;
			continue;
		}

		FCollisionQueryParams FloorQueryParams;
		FloorQueryParams.bTraceComplex = false;
		if (Params.IgnoreActor.IsValid())
		{
			FloorQueryParams.AddIgnoredActor(Params.IgnoreActor.Get());
		}
		if (Particle.AttachedActor.IsValid())
		{
			FloorQueryParams.AddIgnoredActor(Particle.AttachedActor.Get());
		}

		FHitResult FloorHit;
		bool bNearFloor = World->LineTraceSingleByChannel(
			FloorHit,
			Particle.Position,
			Particle.Position - FVector(0, 0, FloorNearDistance),
			ECC_WorldStatic,
			FloorQueryParams
		);

		Particle.bNearGround = bNearFloor;

		if (bNearFloor && FloorHit.Distance <= FloorDetachDistance)
		{
			Particle.bIsAttached = false;
			Particle.AttachedActor.Reset();
			Particle.AttachedBoneName = NAME_None;
			Particle.AttachedLocalOffset = FVector::ZeroVector;
			Particle.AttachedSurfaceNormal = FVector::UpVector;
			Particle.bJustDetached = true;
		}
	}
}

//========================================
// SDF-based World Collision (Overlap + ClosestPoint)
//========================================

void UKawaiiFluidSimulationContext::HandleWorldCollision_SDF(
	TArray<FFluidParticle>& Particles,
	const FKawaiiFluidSimulationParams& Params,
	FSpatialHash& SpatialHash,
	float ParticleRadius,
	float SubstepDT,
	float Friction,
	float Restitution)
{
	UWorld* World = Params.World;
	if (!World || Particles.Num() == 0)
	{
		return;
	}

	const float CellSize = SpatialHash.GetCellSize();
	const auto& Grid = SpatialHash.GetGrid();

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;
	if (Params.IgnoreActor.IsValid())
	{
		QueryParams.AddIgnoredActor(Params.IgnoreActor.Get());
	}

	// Cell-based broad-phase (same as Sweep method)
	struct FCellQueryData
	{
		FVector CellCenter;
		FVector CellExtent;
		TArray<int32> ParticleIndices;
	};

	TArray<FCellQueryData> CellQueries;
	CellQueries.Reserve(Grid.Num());

	for (const auto& Pair : Grid)
	{
		FCellQueryData CellData;
		CellData.CellCenter = FVector(Pair.Key) * CellSize + FVector(CellSize * 0.5f);
		CellData.CellExtent = FVector(CellSize * 0.5f);
		CellData.ParticleIndices = Pair.Value;
		CellQueries.Add(MoveTemp(CellData));
	}

	// Cell overlap check - parallel
	TArray<uint8> CellCollisionResults;
	CellCollisionResults.SetNumZeroed(CellQueries.Num());

	ParallelFor(CellQueries.Num(), [&](int32 CellIdx)
	{
		const FCellQueryData& CellData = CellQueries[CellIdx];
		if (World->OverlapBlockingTestByChannel(
			CellData.CellCenter, FQuat::Identity, ECC_WorldStatic,
			FCollisionShape::MakeBox(CellData.CellExtent), QueryParams))
		{
			CellCollisionResults[CellIdx] = 1;
		}
	});

	// Collect collision candidates
	TArray<int32> CollisionParticleIndices;
	CollisionParticleIndices.Reserve(Particles.Num());

	for (int32 CellIdx = 0; CellIdx < CellQueries.Num(); ++CellIdx)
	{
		if (CellCollisionResults[CellIdx])
		{
			CollisionParticleIndices.Append(CellQueries[CellIdx].ParticleIndices);
		}
	}

	if (CollisionParticleIndices.Num() == 0)
	{
		return;
	}

	// Collision margin (particle radius + safety margin)
	const float CollisionMargin = ParticleRadius * 1.1f;

	// Physics scene read lock
	FPhysScene* PhysScene = World->GetPhysicsScene();
	if (!PhysScene)
	{
		return;
	}

	FPhysicsCommand::ExecuteRead(PhysScene, [&]()
	{
		ParallelFor(CollisionParticleIndices.Num(), [&](int32 j)
		{
			const int32 i = CollisionParticleIndices[j];
			FFluidParticle& Particle = Particles[i];

			FCollisionQueryParams LocalParams;
			LocalParams.bTraceComplex = false;
			LocalParams.bReturnPhysicalMaterial = false;
			if (Params.IgnoreActor.IsValid())
			{
				LocalParams.AddIgnoredActor(Params.IgnoreActor.Get());
			}

			// SDF approach: Overlap + ClosestPoint
			TArray<FOverlapResult> Overlaps;
			bool bOverlapped = World->OverlapMultiByChannel(
				Overlaps,
				Particle.PredictedPosition,
				FQuat::Identity,
				ECC_WorldStatic,
				FCollisionShape::MakeSphere(CollisionMargin),
				LocalParams
			);

			if (!bOverlapped || Overlaps.Num() == 0)
			{
				return;
			}

			// Find closest collision among all overlapping primitives
			float MinSignedDistance = MAX_FLT;
			FVector BestNormal = FVector::UpVector;
			FVector BestClosestPoint = Particle.PredictedPosition;
			AActor* HitActor = nullptr;

			for (const FOverlapResult& Overlap : Overlaps)
			{
				UPrimitiveComponent* Comp = Overlap.GetComponent();
				if (!Comp)
				{
					continue;
				}

				FVector ClosestPoint;
				float DistToSurface = Comp->GetClosestPointOnCollision(
					Particle.PredictedPosition, ClosestPoint);

				if (DistToSurface < 0.0f)
				{
					// GetClosestPointOnCollision returns -1 on failure
					continue;
				}

				// Calculate signed distance and normal
				FVector ToParticle = Particle.PredictedPosition - ClosestPoint;
				float Dist = ToParticle.Size();
				FVector Normal;

				if (Dist > KINDA_SMALL_NUMBER)
				{
					Normal = ToParticle / Dist;
				}
				else
				{
					// Particle is exactly on or inside surface
					// Use velocity direction to determine push direction
					Normal = -Particle.Velocity.GetSafeNormal();
					if (Normal.IsNearlyZero())
					{
						Normal = FVector::UpVector;
					}
					Dist = 0.0f;
				}

				// Convert to signed distance (negative if inside)
				// Since we're overlapping, if Dist is very small, we're likely inside
				float SignedDist = Dist;
				if (Dist < CollisionMargin * 0.5f)
				{
					// Treat as being inside or very close to surface
					SignedDist = Dist - CollisionMargin;
				}

				if (SignedDist < MinSignedDistance)
				{
					MinSignedDistance = SignedDist;
					BestNormal = Normal;
					BestClosestPoint = ClosestPoint;
					HitActor = Overlap.GetActor();
				}
			}

			// Apply collision response if within margin
			if (MinSignedDistance < CollisionMargin)
			{
				// Push particle to surface + margin
				float Penetration = CollisionMargin - MinSignedDistance;
				FVector CollisionPos = Particle.PredictedPosition + BestNormal * Penetration;

				// Only modify PredictedPosition
				Particle.PredictedPosition = CollisionPos;

				// Calculate desired velocity after collision response
				// Initialize to zero - particle stops on surface by default
				FVector DesiredVelocity = FVector::ZeroVector;
				float VelDotNormal = FVector::DotProduct(Particle.Velocity, BestNormal);

				// Minimum velocity threshold for applying restitution bounce
				// Prevents "popcorn" oscillation for particles resting on surfaces
				const float MinBounceVelocity = 50.0f;  // cm/s

				if (VelDotNormal < 0.0f)
				{
					// Particle moving INTO surface - apply collision response
					FVector VelNormal = BestNormal * VelDotNormal;
					FVector VelTangent = Particle.Velocity - VelNormal;

					if (VelDotNormal < -MinBounceVelocity)
					{
						// Significant impact - apply full collision response
						// Normal: reflect with Restitution (0 = stick, 1 = perfect bounce)
						// Tangent: dampen with Friction (0 = slide, 1 = stop)
						DesiredVelocity = VelTangent * (1.0f - Friction) - VelNormal * Restitution;
					}
					else
					{
						// Low velocity contact (resting on surface) - no bounce, just slide
						DesiredVelocity = VelTangent * (1.0f - Friction);
					}
				}
				// else: VelDotNormal >= 0 means particle moving AWAY from surface
				// DesiredVelocity stays zero - particle stops on surface (same as OLD behavior)

				// Back-calculate Position so FinalizePositions derives DesiredVelocity
				Particle.Position = Particle.PredictedPosition - DesiredVelocity * SubstepDT;

				// Add to collision event buffer (processed later in ProcessCollisionFeedback)
				if (Params.bEnableCollisionEvents && Params.CPUCollisionFeedbackBufferPtr && Params.CPUCollisionFeedbackLockPtr)
				{
					const float Speed = Particle.Velocity.Size();
					if (Speed >= Params.MinVelocityForEvent)
					{
						FKawaiiFluidCollisionEvent Event;
						Event.ParticleIndex = Particle.ParticleID;
						Event.SourceID = Particle.SourceID;
						Event.ColliderOwnerID = HitActor ? HitActor->GetUniqueID() : -1;
						Event.BoneIndex = -1;  // CPU path doesn't have bone info
						Event.HitActor = HitActor;
						Event.HitLocation = BestClosestPoint;
						Event.HitNormal = BestNormal;
						Event.HitSpeed = Speed;
						// HitInteractionComponent is looked up in ProcessCollisionFeedback

						// Add to buffer (thread-safe)
						FScopeLock Lock(Params.CPUCollisionFeedbackLockPtr);
						Params.CPUCollisionFeedbackBufferPtr->Add(Event);
					}
				}

				// Detach from character if hitting different surface
				if (Particle.bIsAttached && HitActor != Particle.AttachedActor.Get())
				{
					Particle.bIsAttached = false;
					Particle.AttachedActor.Reset();
					Particle.AttachedBoneName = NAME_None;
					Particle.AttachedLocalOffset = FVector::ZeroVector;
					Particle.AttachedSurfaceNormal = FVector::UpVector;
				}
			}
		});
	});

	// Floor detachment check (same as Sweep method)
	const float FloorDetachDistance = 5.0f;
	const float FloorNearDistance = 20.0f;

	for (FFluidParticle& Particle : Particles)
	{
		if (!Particle.bIsAttached)
		{
			Particle.bNearGround = false;
			continue;
		}

		FCollisionQueryParams FloorQueryParams;
		FloorQueryParams.bTraceComplex = false;
		if (Params.IgnoreActor.IsValid())
		{
			FloorQueryParams.AddIgnoredActor(Params.IgnoreActor.Get());
		}
		if (Particle.AttachedActor.IsValid())
		{
			FloorQueryParams.AddIgnoredActor(Particle.AttachedActor.Get());
		}

		FHitResult FloorHit;
		bool bNearFloor = World->LineTraceSingleByChannel(
			FloorHit,
			Particle.Position,
			Particle.Position - FVector(0, 0, FloorNearDistance),
			ECC_WorldStatic,
			FloorQueryParams
		);

		Particle.bNearGround = bNearFloor;

		if (bNearFloor && FloorHit.Distance <= FloorDetachDistance)
		{
			Particle.bIsAttached = false;
			Particle.AttachedActor.Reset();
			Particle.AttachedBoneName = NAME_None;
			Particle.AttachedLocalOffset = FVector::ZeroVector;
			Particle.AttachedSurfaceNormal = FVector::UpVector;
			Particle.bJustDetached = true;
		}
	}
}

void UKawaiiFluidSimulationContext::FinalizePositions(
	TArray<FFluidParticle>& Particles,
	float DeltaTime)
{
	const float InvDeltaTime = 1.0f / DeltaTime;

	ParallelFor(Particles.Num(), [&](int32 i)
	{
		FFluidParticle& Particle = Particles[i];
		Particle.Velocity = (Particle.PredictedPosition - Particle.Position) * InvDeltaTime;
		Particle.Position = Particle.PredictedPosition;
	});
}

void UKawaiiFluidSimulationContext::ApplyViscosity(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset)
{
if (ViscositySolver.IsValid() && Preset->Viscosity > 0.0f)
	{
		ViscositySolver->ApplyXSPH(Particles, Preset->Viscosity, Preset->SmoothingRadius);
	}
}

void UKawaiiFluidSimulationContext::ApplyAdhesion(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	const TArray<TObjectPtr<UFluidCollider>>& Colliders)
{
	// DEPRECATED: CPU adhesion solver is no longer used
	// All adhesion is now handled by GPU boundary particle system (FluidApplyViscosity.usf)
}

void UKawaiiFluidSimulationContext::ApplyCohesion(
	TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset)
{
if (AdhesionSolver.IsValid() && Preset->SurfaceTension > 0.0f)
	{
		AdhesionSolver->ApplyCohesion(
			Particles,
			Preset->SurfaceTension,
			Preset->SmoothingRadius
		);
	}
}

void UKawaiiFluidSimulationContext::UpdateAttachedParticlePositions(
	TArray<FFluidParticle>& Particles,
	const TArray<TObjectPtr<UKawaiiFluidInteractionComponent>>& InteractionComponents)
{
	if (InteractionComponents.Num() == 0 || Particles.Num() == 0)
	{
		return;
	}

	// Group particles by owner (O(P) single traversal)
	TMap<AActor*, TArray<int32>> OwnerToParticleIndices;

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		const FFluidParticle& Particle = Particles[i];
		if (Particle.bIsAttached && Particle.AttachedActor.IsValid() && Particle.AttachedBoneName != NAME_None)
		{
			OwnerToParticleIndices.FindOrAdd(Particle.AttachedActor.Get()).Add(i);
		}
	}

	// Process per InteractionComponent
	for (UKawaiiFluidInteractionComponent* Interaction : InteractionComponents)
	{
		if (!Interaction)
		{
			continue;
		}

		AActor* InteractionOwner = Interaction->GetOwner();
		if (!InteractionOwner)
		{
			continue;
		}

		TArray<int32>* ParticleIndicesPtr = OwnerToParticleIndices.Find(InteractionOwner);
		if (!ParticleIndicesPtr || ParticleIndicesPtr->Num() == 0)
		{
			continue;
		}

		USkeletalMeshComponent* SkelMesh = InteractionOwner->FindComponentByClass<USkeletalMeshComponent>();
		if (!SkelMesh)
		{
			continue;
		}

		// Group by bone (optimization: minimize GetBoneTransform calls)
		TMap<FName, TArray<int32>> BoneToParticleIndices;
		for (int32 ParticleIdx : *ParticleIndicesPtr)
		{
			const FFluidParticle& Particle = Particles[ParticleIdx];
			BoneToParticleIndices.FindOrAdd(Particle.AttachedBoneName).Add(ParticleIdx);
		}

		// Update particle positions per bone
		for (auto& BonePair : BoneToParticleIndices)
		{
			const FName& BoneName = BonePair.Key;
			const TArray<int32>& BoneParticleIndices = BonePair.Value;

			int32 BoneIndex = SkelMesh->GetBoneIndex(BoneName);
			if (BoneIndex == INDEX_NONE)
			{
				continue;
			}

			FTransform CurrentBoneTransform = SkelMesh->GetBoneTransform(BoneIndex);

			for (int32 ParticleIdx : BoneParticleIndices)
			{
				FFluidParticle& Particle = Particles[ParticleIdx];
				FVector OldWorldPosition = CurrentBoneTransform.TransformPosition(Particle.AttachedLocalOffset);
				FVector BoneDelta = OldWorldPosition - Particle.Position;
				Particle.Position += BoneDelta;
			}
		}
	}
}

//=============================================================================
// Statistics Collection
//=============================================================================

void UKawaiiFluidSimulationContext::CollectSimulationStats(
	const TArray<FFluidParticle>& Particles,
	const UKawaiiFluidPresetDataAsset* Preset,
	int32 SubstepCount,
	bool bIsGPU)
{
	FKawaiiFluidSimulationStatsCollector& Stats = GetFluidStatsCollector();

	if (!Stats.IsEnabled())
	{
		return;
	}

	// Begin new frame of stats collection
	Stats.BeginFrame();
	Stats.SetGPUSimulation(bIsGPU);
	Stats.SetSubstepCount(SubstepCount);

	if (Preset)
	{
		Stats.SetRestDensity(Preset->Density);
		Stats.SetSolverIterations(Preset->SolverIterations);
	}

	// Count particle types
	int32 TotalCount = Particles.Num();
	int32 AttachedCount = 0;
	int32 GroundCount = 0;

	for (const FFluidParticle& Particle : Particles)
	{
		// Velocity sample
		float VelMag = static_cast<float>(Particle.Velocity.Size());
		Stats.AddVelocitySample(VelMag);

		// Density sample
		Stats.AddDensitySample(Particle.Density);

		// Neighbor count sample
		Stats.AddNeighborCountSample(Particle.NeighborIndices.Num());

		// Count attached particles
		if (Particle.bIsAttached)
		{
			AttachedCount++;
		}

		// Count ground contact
		if (Particle.bNearGround)
		{
			GroundCount++;
		}
	}

	// Set particle counts
	int32 ActiveCount = TotalCount - AttachedCount;
	Stats.SetParticleCounts(TotalCount, ActiveCount, AttachedCount);

	// Set ground contact count as collision stat
	for (int32 i = 0; i < GroundCount; ++i)
	{
		Stats.AddGroundContact();
	}

	// End frame and finalize statistics
	Stats.EndFrame();
}

void UKawaiiFluidSimulationContext::CollectGPUSimulationStats(
	const UKawaiiFluidPresetDataAsset* Preset,
	int32 ParticleCount,
	int32 SubstepCount)
{
	FKawaiiFluidSimulationStatsCollector& Stats = GetFluidStatsCollector();

	if (!Stats.IsEnabled())
	{
		return;
	}

	// Begin new frame of stats collection
	Stats.BeginFrame();
	Stats.SetGPUSimulation(true);

	if (Preset)
	{
		Stats.SetRestDensity(Preset->Density);
		Stats.SetSolverIterations(Preset->SolverIterations);

		// Use actual substep count from simulation
		Stats.SetSubstepCount(SubstepCount);
	}

	// GPU mode: Basic stats only (no sync readback for performance)
	// Detailed stats with Density/Velocity would require sync GPU readback which is expensive
	Stats.SetParticleCounts(ParticleCount, ParticleCount, 0);

	// End frame and finalize statistics
	Stats.EndFrame();
}

//========================================
// Landscape Heightmap Collision
//========================================

void UKawaiiFluidSimulationContext::UpdateLandscapeHeightmapCollision(
	const FKawaiiFluidSimulationParams& Params,
	const UKawaiiFluidPresetDataAsset* Preset)
{
	if (!GPUSimulator.IsValid())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		GPUSimulator->SetHeightmapCollisionEnabled(false);
		return;
	}

	// Only rebuild if dirty (level load, world changed, etc.)
	// Note: bLandscapeHeightmapDirty is set by AppendGPUWorldCollisionPrimitives when world changes
	if (!bLandscapeHeightmapDirty)
	{
		// Already uploaded, just ensure it's enabled
		return;
	}

	// Find all landscapes in the world
	TArray<ALandscapeProxy*> Landscapes;
	FLandscapeHeightmapExtractor::FindLandscapesInWorld(World, Landscapes);

	if (Landscapes.IsEmpty())
	{
		// No landscapes - disable heightmap collision
		GPUSimulator->SetHeightmapCollisionEnabled(false);
		CachedLandscapeHeightmap.Empty();
		CachedHeightmapWidth = 0;
		CachedHeightmapHeight = 0;
		bLandscapeHeightmapDirty = false;
		return;
	}

	// Extract combined heightmap from all landscapes
	// TODO: Currently samples entire world Landscapes. Should clamp to Volume bounds for better precision.
	// - Current: Entire Landscape (10km) -> 1024 texels = 9.77m/texel
	// - Improved: Volume area only (100m) -> 1024 texels = 0.097m/texel
	const int32 Resolution = 1024;  // Default resolution, could be exposed as parameter

	bool bSuccess = FLandscapeHeightmapExtractor::ExtractCombinedHeightmap(
		Landscapes,
		CachedLandscapeHeightmap,
		CachedHeightmapWidth,
		CachedHeightmapHeight,
		CachedHeightmapBounds,
		Resolution);

	if (!bSuccess || CachedLandscapeHeightmap.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to extract landscape heightmap"));
		GPUSimulator->SetHeightmapCollisionEnabled(false);
		bLandscapeHeightmapDirty = false;
		return;
	}

	// Build collision parameters
	const float ParticleRadius = Preset ? Preset->ParticleRadius : 5.0f;
	const float HeightmapFriction = Preset ? Preset->Friction : 0.3f;
	const float HeightmapRestitution = 0.1f;  // Low restitution for terrain collision (no bounce)

	FGPUHeightmapCollisionParams HeightmapParams = FLandscapeHeightmapExtractor::BuildCollisionParams(
		CachedHeightmapBounds,
		CachedHeightmapWidth,
		CachedHeightmapHeight,
		ParticleRadius,
		HeightmapFriction,
		HeightmapRestitution);

	// Upload to GPU
	GPUSimulator->SetHeightmapCollisionParams(HeightmapParams);
	GPUSimulator->UploadHeightmapData(CachedLandscapeHeightmap, CachedHeightmapWidth, CachedHeightmapHeight);
	GPUSimulator->SetHeightmapCollisionEnabled(true);

	UE_LOG(LogTemp, Log, TEXT("Landscape heightmap collision enabled: %dx%d, Bounds: (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)"),
		CachedHeightmapWidth, CachedHeightmapHeight,
		CachedHeightmapBounds.Min.X, CachedHeightmapBounds.Min.Y, CachedHeightmapBounds.Min.Z,
		CachedHeightmapBounds.Max.X, CachedHeightmapBounds.Max.Y, CachedHeightmapBounds.Max.Z);

	bLandscapeHeightmapDirty = false;
}
