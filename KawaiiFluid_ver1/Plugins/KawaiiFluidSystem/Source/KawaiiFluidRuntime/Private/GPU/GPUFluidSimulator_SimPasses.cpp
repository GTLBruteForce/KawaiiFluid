// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// GPUFluidSimulator - Simulation Pass Functions

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/Managers/GPUBoundarySkinningManager.h"
#include "GPU/Managers/GPUZOrderSortManager.h"
#include "GPU/Managers/GPUCollisionManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

//=============================================================================
// Predict Positions Pass
//=============================================================================

void FGPUFluidSimulator::AddPredictPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (CurrentParticleCount <= 0) return;

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FPredictPositionsCS> ComputeShader(ShaderMap);

	FPredictPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPredictPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->Gravity = Params.Gravity;
	PassParameters->ExternalForce = ExternalForce;

	//=========================================================================
	// Cohesion Force Parameters (Akinci 2013)
	// Uses C(r) spline kernel with K_ij particle deficiency correction
	// CohesionStrength from Preset->Cohesion (0~50)
	//=========================================================================
	PassParameters->CohesionStrength = Params.CohesionStrength;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;

	// MaxCohesionForce: stability clamp based on physical parameters
	const float h_m = Params.SmoothingRadius * 0.01f;  // cm to m
	PassParameters->MaxCohesionForce = Params.CohesionStrength * Params.RestDensity * h_m * h_m * h_m * 1000.0f;

	//=========================================================================
	// Viscosity Parameters (moved from PostSimulation Phase 5 for optimization)
	// Now calculated together with Cohesion in single neighbor loop
	// This reduces memory bandwidth by ~50%
	//=========================================================================
	PassParameters->ViscosityCoefficient = Params.ViscosityCoefficient;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	
	// Laplacian viscosity coefficient: 45 / (PI * h^6) where h is in meters
	const float h6 = h_m * h_m * h_m * h_m * h_m * h_m;
	PassParameters->ViscLaplacianCoeff = 45.0f / (PI * h6);

	//=========================================================================
	// Previous Frame Neighbor Cache (Double Buffering for Cohesion)
	// Standard practice: use previous frame's neighbor list to avoid dependency
	//=========================================================================
	const bool bCanUsePrevNeighborCache = bPrevNeighborCacheValid && 
	                                       PrevNeighborListBuffer.IsValid() && 
	                                       PrevNeighborCountsBuffer.IsValid() &&
	                                       PrevNeighborBufferParticleCount > 0;

	if (bCanUsePrevNeighborCache)
	{
		// Register previous frame's persistent neighbor cache buffers
		FRDGBufferRef PrevNeighborListRDG = GraphBuilder.RegisterExternalBuffer(
			PrevNeighborListBuffer, TEXT("GPUFluidPrevNeighborList"));
		FRDGBufferRef PrevNeighborCountsRDG = GraphBuilder.RegisterExternalBuffer(
			PrevNeighborCountsBuffer, TEXT("GPUFluidPrevNeighborCounts"));

		PassParameters->PrevNeighborList = GraphBuilder.CreateSRV(PrevNeighborListRDG);
		PassParameters->PrevNeighborCounts = GraphBuilder.CreateSRV(PrevNeighborCountsRDG);
		PassParameters->bUsePrevNeighborCache = 1;
		PassParameters->PrevParticleCount = PrevNeighborBufferParticleCount;
	}
	else
	{
		// First frame or invalid cache: create dummy buffers, skip cohesion
		// This is safe - cohesion is not critical for the first frame
		FRDGBufferRef DummyList = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("DummyPrevNeighborList"));
		FRDGBufferRef DummyCounts = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("DummyPrevNeighborCounts"));

		// Initialize dummy buffers to zero
		uint32 Zero = 0;
		GraphBuilder.QueueBufferUpload(DummyList, &Zero, sizeof(uint32));
		GraphBuilder.QueueBufferUpload(DummyCounts, &Zero, sizeof(uint32));

		PassParameters->PrevNeighborList = GraphBuilder.CreateSRV(DummyList);
		PassParameters->PrevNeighborCounts = GraphBuilder.CreateSRV(DummyCounts);
		PassParameters->bUsePrevNeighborCache = 0;
		PassParameters->PrevParticleCount = 0;
	}

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FPredictPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PredictPositions (Cohesion=%s)", bCanUsePrevNeighborCache ? TEXT("ON") : TEXT("OFF")),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Extract Positions Pass
//=============================================================================

void FGPUFluidSimulator::AddExtractPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticlesSRV,
	FRDGBufferUAVRef PositionsUAV,
	int32 ParticleCount,
	bool bUsePredictedPosition)
{
	if (ParticleCount <= 0) return;

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FExtractPositionsCS> ComputeShader(ShaderMap);

	FExtractPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FExtractPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesSRV;
	PassParameters->Positions = PositionsUAV;
	PassParameters->ParticleCount = ParticleCount;
	PassParameters->bUsePredictedPosition = bUsePredictedPosition ? 1 : 0;

	const uint32 NumGroups = FMath::DivideAndRoundUp(ParticleCount, FExtractPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ExtractPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Solve Density Pressure Pass (PBF with Neighbor Cache)
//=============================================================================

void FGPUFluidSimulator::AddSolveDensityPressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	FRDGBufferSRVRef InCellStartSRV,
	FRDGBufferSRVRef InCellEndSRV,
	FRDGBufferUAVRef InNeighborListUAV,
	FRDGBufferUAVRef InNeighborCountsUAV,
	int32 IterationIndex,
	const FGPUFluidSimulationParams& Params,
	const FSimulationSpatialData& SpatialData)
{
	if (CurrentParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Check if Z-Order sorting is enabled (both manager valid AND flag enabled)
	const bool bUseZOrderSorting = ZOrderSortManager.IsValid() && ZOrderSortManager->IsZOrderSortingEnabled();

	// Get GridResolutionPreset for shader permutation (Z-Order neighbor search)
	EGridResolutionPreset GridPreset = EGridResolutionPreset::Medium;
	if (bUseZOrderSorting)
	{
		GridPreset = ZOrderSortManager->GetGridResolutionPreset();
	}

	// Create permutation vector and get shader
	FSolveDensityPressureCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridPreset));
	TShaderMapRef<FSolveDensityPressureCS> ComputeShader(ShaderMap, PermutationVector);

	FSolveDensityPressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSolveDensityPressureCS::FParameters>();
	// SoA (Structure of Arrays) Particle Buffers
	PassParameters->Positions = GraphBuilder.CreateUAV(SpatialData.SoA_Positions, PF_R32_FLOAT);
	PassParameters->PredictedPositions = GraphBuilder.CreateUAV(SpatialData.SoA_PredictedPositions, PF_R32_FLOAT);
	PassParameters->Velocities = GraphBuilder.CreateUAV(SpatialData.SoA_Velocities, PF_R32_FLOAT);
	PassParameters->Masses = GraphBuilder.CreateUAV(SpatialData.SoA_Masses, PF_R32_FLOAT);
	PassParameters->Densities = GraphBuilder.CreateUAV(SpatialData.SoA_Densities, PF_R32_FLOAT);
	PassParameters->Lambdas = GraphBuilder.CreateUAV(SpatialData.SoA_Lambdas, PF_R32_FLOAT);
	PassParameters->Flags = GraphBuilder.CreateUAV(SpatialData.SoA_Flags, PF_R32_UINT);
	PassParameters->NeighborCountsBuffer = GraphBuilder.CreateUAV(SpatialData.SoA_NeighborCounts, PF_R32_UINT);
	// Hash table mode (legacy)
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	// Z-Order sorted mode (new)
	PassParameters->CellStart = InCellStartSRV;
	PassParameters->CellEnd = InCellEndSRV;
	// Use Z-Order sorting only when manager is valid AND enabled
	PassParameters->bUseZOrderSorting = bUseZOrderSorting ? 1 : 0;
	// Morton bounds for Z-Order cell ID calculation (must match FluidMortonCode.usf)
	PassParameters->MortonBoundsMin = SimulationBoundsMin;
	PassParameters->MortonBoundsExtent = SimulationBoundsMax - SimulationBoundsMin;
	// Hybrid Tiled Z-Order mode for unlimited simulation range
	PassParameters->bUseHybridTiledZOrder = (bUseZOrderSorting && ZOrderSortManager->IsHybridTiledZOrderEnabled()) ? 1 : 0;
	// Neighbor caching buffers
	PassParameters->NeighborList = InNeighborListUAV;
	PassParameters->NeighborCounts = InNeighborCountsUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Compliance = Params.Compliance;
	PassParameters->DeltaTimeSq = Params.DeltaTimeSq;
	// Tensile Instability Correction (PBF Eq.13-14)
	PassParameters->bEnableTensileInstability = Params.bEnableTensileInstability;
	PassParameters->TensileK = Params.TensileK;
	PassParameters->TensileN = Params.TensileN;
	PassParameters->InvW_DeltaQ = Params.InvW_DeltaQ;
	// Iteration control for neighbor caching
	PassParameters->IterationIndex = IterationIndex;
	// Relative Velocity Pressure Damping (prevents fluid flying away from fast boundaries)
	PassParameters->bEnableRelativeVelocityDamping = Params.bEnableRelativeVelocityDamping;
	PassParameters->RelativeVelocityDampingStrength = Params.RelativeVelocityDampingStrength;
	
	// Boundary Velocity Transfer (moved from FluidApplyViscosity for optimization)
	// Fluid following moving boundaries - applied during boundary density loop
	PassParameters->bEnableBoundaryVelocityTransfer = Params.bEnableBoundaryVelocityTransfer;
	PassParameters->BoundaryVelocityTransferStrength = Params.BoundaryVelocityTransferStrength;
	PassParameters->BoundaryDetachSpeedThreshold = Params.BoundaryDetachSpeedThreshold;
	PassParameters->BoundaryMaxDetachSpeed = Params.BoundaryMaxDetachSpeed;
	PassParameters->BoundaryAdhesionStrength = FMath::Clamp(Params.BoundaryAdhesionStrength, 0.0f, 1.0f);
	PassParameters->SolverIterationCount = Params.SolverIterations;

	// Position-Based Surface Tension (always enabled)
	// Creates rounded droplets by minimizing surface area
	PassParameters->bEnablePositionBasedSurfaceTension = Params.bEnablePositionBasedSurfaceTension;
	PassParameters->SurfaceTensionStrength = Params.SurfaceTensionStrength;
	PassParameters->SurfaceTensionActivationDistance = Params.SmoothingRadius * Params.SurfaceTensionActivationRatio;
	PassParameters->SurfaceTensionFalloffDistance = Params.SmoothingRadius * Params.SurfaceTensionFalloffRatio;
	PassParameters->SurfaceTensionSurfaceThreshold = Params.SurfaceTensionSurfaceThreshold;
	PassParameters->SurfaceTensionVelocityDamping = Params.SurfaceTensionVelocityDamping;
	PassParameters->SurfaceTensionTolerance = Params.SurfaceTensionTolerance;

	// Surface Tension max correction
	PassParameters->MaxSurfaceTensionCorrection = Params.MaxSurfaceTensionCorrectionPerIteration;

	// =========================================================================
	// Boundary Particles for density contribution (Akinci 2012)
	// Two separate boundary types: Skinned (SkeletalMesh) and Static (StaticMesh)
	// =========================================================================

	// Check available boundary types
	const bool bHasSkinnedBoundary = SpatialData.bSkinnedBoundaryPerformed && SpatialData.SkinnedBoundarySRV != nullptr;
	const bool bHasStaticBoundary = SpatialData.bStaticBoundaryAvailable && SpatialData.StaticBoundarySRV != nullptr;

	// Debug: Log boundary status - disabled for performance
	// static int32 BoundaryDebugCounter = 0;
	// if (++BoundaryDebugCounter % 120 == 1)
	// {
	// 	UE_LOG(LogTemp, Warning, TEXT("[BoundaryDebug] Skinned=%d (Count=%d), Static=%d (Count=%d)"),
	// 		bHasSkinnedBoundary ? 1 : 0, SpatialData.SkinnedBoundaryParticleCount,
	// 		bHasStaticBoundary ? 1 : 0, SpatialData.StaticBoundaryParticleCount);
	// }

	// Determine which boundary to use for density (currently supports one at a time)
	// TODO: Support both simultaneously by modifying shader to loop over both
	if (bHasSkinnedBoundary)
	{
		// Use Skinned boundary (SkeletalMesh - same-frame)
		PassParameters->BoundaryParticles = SpatialData.SkinnedBoundarySRV;
		PassParameters->BoundaryParticleCount = SpatialData.SkinnedBoundaryParticleCount;
		PassParameters->bUseBoundaryDensity = 1;
	}
	else if (bHasStaticBoundary)
	{
		// Use Static boundary (StaticMesh - persistent GPU)
		PassParameters->BoundaryParticles = SpatialData.StaticBoundarySRV;
		PassParameters->BoundaryParticleCount = SpatialData.StaticBoundaryParticleCount;
		PassParameters->bUseBoundaryDensity = 1;
	}
	else
	{
		// No boundary - create dummy buffer for RDG validation
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidBoundaryParticles_Density_Dummy"));
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(DummyBuffer);
		PassParameters->BoundaryParticleCount = 0;
		PassParameters->bUseBoundaryDensity = 0;
	}

	// =========================================================================
	// Z-Order sorted boundary particles (Akinci 2012 + Z-Order optimization)
	// Priority matches boundary selection above
	// =========================================================================
	const bool bUseSkinnedZOrder = bHasSkinnedBoundary && SpatialData.bSkinnedZOrderPerformed
		&& SpatialData.SkinnedZOrderSortedSRV != nullptr;
	const bool bUseStaticZOrder = !bUseSkinnedZOrder && bHasStaticBoundary
		&& SpatialData.StaticZOrderSortedSRV != nullptr;
	const bool bUseBoundaryZOrder = bUseSkinnedZOrder || bUseStaticZOrder;

	if (bUseSkinnedZOrder)
	{
		// Use Skinned Z-Order buffers
		PassParameters->SortedBoundaryParticles = SpatialData.SkinnedZOrderSortedSRV;
		PassParameters->BoundaryCellStart = SpatialData.SkinnedZOrderCellStartSRV;
		PassParameters->BoundaryCellEnd = SpatialData.SkinnedZOrderCellEndSRV;
		PassParameters->bUseBoundaryZOrder = 1;
	}
	else if (bUseStaticZOrder)
	{
		// Use Static Z-Order buffers (persistent GPU)
		PassParameters->SortedBoundaryParticles = SpatialData.StaticZOrderSortedSRV;
		PassParameters->BoundaryCellStart = SpatialData.StaticZOrderCellStartSRV;
		PassParameters->BoundaryCellEnd = SpatialData.StaticZOrderCellEndSRV;
		PassParameters->bUseBoundaryZOrder = 1;
	}
	else
	{
		// Create dummy buffers for RDG validation when Z-Order is disabled
		FRDGBufferRef DummySortedBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidSortedBoundaryParticles_Density_Dummy"));
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummySortedBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));

		FRDGBufferRef DummyCellStartBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("GPUFluidBoundaryCellStart_Density_Dummy"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyCellStartBuffer, &InvalidIndex, sizeof(uint32));

		FRDGBufferRef DummyCellEndBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("GPUFluidBoundaryCellEnd_Density_Dummy"));
		GraphBuilder.QueueBufferUpload(DummyCellEndBuffer, &InvalidIndex, sizeof(uint32));

		PassParameters->SortedBoundaryParticles = GraphBuilder.CreateSRV(DummySortedBuffer);
		PassParameters->BoundaryCellStart = GraphBuilder.CreateSRV(DummyCellStartBuffer);
		PassParameters->BoundaryCellEnd = GraphBuilder.CreateSRV(DummyCellEndBuffer);
		PassParameters->bUseBoundaryZOrder = 0;
	}

	// Debug: Log boundary path and Z-Order status - disabled for performance
	// if (BoundaryDebugCounter % 120 == 1)
	// {
	// 	const TCHAR* BoundaryPath = bHasSkinnedBoundary ? TEXT("Skinned") :
	// 		(bHasStaticBoundary ? TEXT("Static") : TEXT("NONE"));
	// 	const TCHAR* ZOrderPath = bUseSkinnedZOrder ? TEXT("Skinned") :
	// 		(bUseStaticZOrder ? TEXT("Static") : TEXT("Disabled"));
	// 	UE_LOG(LogTemp, Warning, TEXT("[BoundaryDebug] BoundaryPath=%s, ZOrderPath=%s, BoundaryCount=%d, bUseDensity=%d, bUseZOrder=%d"),
	// 		BoundaryPath, ZOrderPath, PassParameters->BoundaryParticleCount, PassParameters->bUseBoundaryDensity, PassParameters->bUseBoundaryZOrder);
	// }

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FSolveDensityPressureCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SolveDensityPressure (Iter %d)", IterationIndex),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// [DEPRECATED] Apply Viscosity Pass
// 
// NOTE: This function is deprecated as of the Cohesion+Viscosity optimization.
// - Fluid Viscosity is now calculated in AddPredictPositionsPass (Phase 2)
// - Boundary Viscosity is now calculated in AddSolveDensityPressurePass (Phase 3)
// This reduces neighbor traversal from 2x to 1x, saving ~400us at 76k particles.
// Kept for backward compatibility but no longer called by ExecutePostSimulation.
//=============================================================================

void FGPUFluidSimulator::AddApplyViscosityPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	FRDGBufferSRVRef InNeighborListSRV,
	FRDGBufferSRVRef InNeighborCountsSRV,
	const FGPUFluidSimulationParams& Params,
	const FSimulationSpatialData& SpatialData)
{
	if (CurrentParticleCount <= 0) return;

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyViscosityCS> ComputeShader(ShaderMap);

	FApplyViscosityCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyViscosityCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->NeighborList = InNeighborListSRV;
	PassParameters->NeighborCounts = InNeighborCountsSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->ViscosityCoefficient = Params.ViscosityCoefficient;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;

	// Laplacian viscosity coefficient: 45 / (PI * h^6) where h is in meters
	const float h_m = Params.SmoothingRadius * 0.01f;  // cm to meters
	const float h6 = h_m * h_m * h_m * h_m * h_m * h_m;
	PassParameters->ViscLaplacianCoeff = 45.0f / (PI * h6);
	PassParameters->DeltaTime = Params.DeltaTime;

	PassParameters->CellSize = Params.CellSize;
	PassParameters->bUseNeighborCache = (InNeighborListSRV != nullptr && InNeighborCountsSRV != nullptr) ? 1 : 0;

	// Boundary Particles for viscosity contribution
	// Priority: 1) Skinned boundary (same-frame), 2) Static boundary (persistent GPU)
	const bool bHasSkinnedBoundary = SpatialData.bSkinnedBoundaryPerformed && SpatialData.SkinnedBoundarySRV != nullptr;
	const bool bHasStaticBoundary = SpatialData.bStaticBoundaryAvailable && SpatialData.StaticBoundarySRV != nullptr;

	if (bHasSkinnedBoundary)
	{
		// Use Skinned boundary (SkeletalMesh - same-frame buffer)
		PassParameters->BoundaryParticles = SpatialData.SkinnedBoundarySRV;
		PassParameters->BoundaryParticleCount = SpatialData.SkinnedBoundaryParticleCount;
		PassParameters->bUseBoundaryViscosity = 1;
	}
	else if (bHasStaticBoundary)
	{
		// Use Static boundary (StaticMesh - persistent GPU buffer)
		PassParameters->BoundaryParticles = SpatialData.StaticBoundarySRV;
		PassParameters->BoundaryParticleCount = SpatialData.StaticBoundaryParticleCount;
		PassParameters->bUseBoundaryViscosity = 1;
	}
	else
	{
		// Create dummy buffer for RDG validation
		// Must use QueueBufferUpload to mark buffer as "produced" for RDG validation
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidBoundaryParticles_Viscosity_Dummy")
		);
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(DummyBuffer);
		PassParameters->BoundaryParticleCount = 0;
		PassParameters->bUseBoundaryViscosity = 0;
	}

	// Hybrid Adhesion parameters for boundary viscosity
	if (BoundarySkinningManager.IsValid())
	{
		const FGPUBoundaryAdhesionParams& AdhesionParams = BoundarySkinningManager->GetBoundaryAdhesionParams();
		PassParameters->AdhesionForceStrength = AdhesionParams.AdhesionForceStrength;
		PassParameters->AdhesionVelocityStrength = AdhesionParams.AdhesionVelocityStrength;
		PassParameters->AdhesionRadius = AdhesionParams.AdhesionRadius;
	}
	else
	{
		PassParameters->AdhesionForceStrength = 0.0f;
		PassParameters->AdhesionVelocityStrength = 0.0f;
		PassParameters->AdhesionRadius = 0.0f;
	}

	// =========================================================================
	// Z-Order sorted boundary particles (same pattern as AddSolveDensityPressurePass)
	// =========================================================================
	const bool bUseSkinnedZOrder = bHasSkinnedBoundary && SpatialData.bSkinnedZOrderPerformed
		&& SpatialData.SkinnedZOrderSortedSRV != nullptr;
	const bool bUseStaticZOrder = !bUseSkinnedZOrder && bHasStaticBoundary
		&& SpatialData.StaticZOrderSortedSRV != nullptr;
	const bool bUseBoundaryZOrder = bUseSkinnedZOrder || bUseStaticZOrder;

	if (bUseSkinnedZOrder)
	{
		// Use Skinned Z-Order buffers
		PassParameters->SortedBoundaryParticles = SpatialData.SkinnedZOrderSortedSRV;
		PassParameters->BoundaryCellStart = SpatialData.SkinnedZOrderCellStartSRV;
		PassParameters->BoundaryCellEnd = SpatialData.SkinnedZOrderCellEndSRV;
		PassParameters->bUseBoundaryZOrder = 1;
	}
	else if (bUseStaticZOrder)
	{
		// Use Static Z-Order buffers (persistent GPU)
		PassParameters->SortedBoundaryParticles = SpatialData.StaticZOrderSortedSRV;
		PassParameters->BoundaryCellStart = SpatialData.StaticZOrderCellStartSRV;
		PassParameters->BoundaryCellEnd = SpatialData.StaticZOrderCellEndSRV;
		PassParameters->bUseBoundaryZOrder = 1;
	}
	else
	{
		// Create dummy buffers for RDG validation when Z-Order is disabled
		FRDGBufferRef DummySortedBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidSortedBoundaryParticles_Viscosity_Dummy"));
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummySortedBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));

		FRDGBufferRef DummyCellStartBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("GPUFluidBoundaryCellStart_Viscosity_Dummy"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyCellStartBuffer, &InvalidIndex, sizeof(uint32));

		FRDGBufferRef DummyCellEndBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("GPUFluidBoundaryCellEnd_Viscosity_Dummy"));
		GraphBuilder.QueueBufferUpload(DummyCellEndBuffer, &InvalidIndex, sizeof(uint32));

		PassParameters->SortedBoundaryParticles = GraphBuilder.CreateSRV(DummySortedBuffer);
		PassParameters->BoundaryCellStart = GraphBuilder.CreateSRV(DummyCellStartBuffer);
		PassParameters->BoundaryCellEnd = GraphBuilder.CreateSRV(DummyCellEndBuffer);
		PassParameters->bUseBoundaryZOrder = 0;
	}

	// MortonBoundsMin is required for GetMortonCellIDFromCellCoord in shader
	PassParameters->MortonBoundsMin = SimulationBoundsMin;

	// Improved Boundary Velocity Transfer
	PassParameters->BoundaryVelocityTransferStrength = Params.BoundaryVelocityTransferStrength;
	PassParameters->BoundaryDetachSpeedThreshold = Params.BoundaryDetachSpeedThreshold;
	PassParameters->BoundaryMaxDetachSpeed = Params.BoundaryMaxDetachSpeed;

	// Debug: Log Viscosity boundary Z-Order status - disabled for performance
	// static int32 ViscosityDebugCounter = 0;
	// ViscosityDebugCounter++;
	// if (ViscosityDebugCounter % 120 == 1)
	// {
	// 	UE_LOG(LogTemp, Warning, TEXT("[ViscosityDebug] BoundaryCount=%d, bUseBoundaryViscosity=%d, bUseZOrder=%d, SkinnedZOrder=%d, StaticZOrder=%d, ForceStr=%.2f, VelStr=%.2f, Radius=%.1f"),
	// 		PassParameters->BoundaryParticleCount,
	// 		PassParameters->bUseBoundaryViscosity,
	// 		PassParameters->bUseBoundaryZOrder,
	// 		bUseSkinnedZOrder ? 1 : 0,
	// 		bUseStaticZOrder ? 1 : 0,
	// 		PassParameters->AdhesionForceStrength,
	// 		PassParameters->AdhesionVelocityStrength,
	// 		PassParameters->AdhesionRadius);
	// }

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FApplyViscosityCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyViscosity"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Particle Sleeping Pass (NVIDIA Flex Stabilization)
//=============================================================================

void FGPUFluidSimulator::AddParticleSleepingPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferUAVRef InSleepCountersUAV,
	FRDGBufferSRVRef InNeighborListSRV,
	FRDGBufferSRVRef InNeighborCountsSRV,
	const FGPUFluidSimulationParams& Params)
{
	if (CurrentParticleCount <= 0) return;
	if (!Params.bEnableParticleSleeping) return;

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FParticleSleepingCS> ComputeShader(ShaderMap);

	FParticleSleepingCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FParticleSleepingCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->SleepCounters = InSleepCountersUAV;
	PassParameters->NeighborList = InNeighborListSRV;
	PassParameters->NeighborCounts = InNeighborCountsSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SleepVelocityThreshold = Params.SleepVelocityThreshold;
	PassParameters->SleepFrameThreshold = Params.SleepFrameThreshold;
	PassParameters->WakeVelocityThreshold = Params.WakeVelocityThreshold;

	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FParticleSleepingCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FluidParticleSleeping"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Finalize Positions Pass
//=============================================================================

void FGPUFluidSimulator::AddFinalizePositionsPass(
	FRDGBuilder& GraphBuilder,
	const FSimulationSpatialData& SpatialData,
	const FGPUFluidSimulationParams& Params)
{
	if (CurrentParticleCount <= 0) return;

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFinalizePositionsCS> ComputeShader(ShaderMap);

	FFinalizePositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFinalizePositionsCS::FParameters>();
	PassParameters->Positions = GraphBuilder.CreateUAV(SpatialData.SoA_Positions, PF_R32_FLOAT);
	PassParameters->PredictedPositions = GraphBuilder.CreateUAV(SpatialData.SoA_PredictedPositions, PF_R32_FLOAT);
	PassParameters->Velocities = GraphBuilder.CreateUAV(SpatialData.SoA_Velocities, PF_R32_FLOAT);
	PassParameters->Flags = GraphBuilder.CreateUAV(SpatialData.SoA_Flags, PF_R32_UINT);
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->MaxVelocity = MaxVelocity;  // Safety clamp (50000 cm/s = 500 m/s)
	PassParameters->GlobalDamping = Params.GlobalDamping;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FFinalizePositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::FinalizePositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Bone Delta Attachment Passes (NEW simplified bone-following system)
//=============================================================================

void FGPUFluidSimulator::AddApplyBoneTransformPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef BoneDeltaAttachmentSRV,
	FRDGBufferSRVRef LocalBoundaryParticlesSRV,
	int32 BoundaryParticleCount,
	FRDGBufferSRVRef BoneTransformsSRV,
	int32 BoneCount,
	const FMatrix44f& ComponentTransform,
	float DeltaTime)
{
	if (CurrentParticleCount <= 0 || !BoneDeltaAttachmentSRV || !LocalBoundaryParticlesSRV || BoundaryParticleCount <= 0)
	{
		return;
	}

	// BoneTransformsSRV can be null for static meshes - that's OK, we use ComponentTransform as fallback

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyBoneTransformCS> ComputeShader(ShaderMap);

	FApplyBoneTransformCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyBoneTransformCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->BoneDeltaAttachments = BoneDeltaAttachmentSRV;
	PassParameters->LocalBoundaryParticles = LocalBoundaryParticlesSRV;
	PassParameters->BoundaryParticleCount = BoundaryParticleCount;
	PassParameters->BoneTransforms = BoneTransformsSRV;
	PassParameters->BoneCount = BoneCount;
	PassParameters->ComponentTransform = ComponentTransform;
	PassParameters->DeltaTime = DeltaTime;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FApplyBoneTransformCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyBoneTransform(%d particles, %d boundary, %d bones)",
			CurrentParticleCount, BoundaryParticleCount, BoneCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddUpdateBoneDeltaAttachmentPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef BoneDeltaAttachmentUAV,
	FRDGBufferSRVRef SortedBoundaryParticlesSRV,
	FRDGBufferSRVRef BoundaryCellStartSRV,
	FRDGBufferSRVRef BoundaryCellEndSRV,
	int32 BoundaryParticleCount,
	FRDGBufferSRVRef WorldBoundaryParticlesSRV,
	int32 WorldBoundaryParticleCount,
	const FGPUFluidSimulationParams& Params)
{
	if (CurrentParticleCount <= 0 || !BoneDeltaAttachmentUAV)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Get grid resolution preset for Z-Order search
	EGridResolutionPreset GridPreset = EGridResolutionPreset::Medium;
	if (ZOrderSortManager.IsValid())
	{
		GridPreset = ZOrderSortManager->GetGridResolutionPreset();
	}

	// Create permutation vector
	FUpdateBoneDeltaAttachmentCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(GridPreset));
	TShaderMapRef<FUpdateBoneDeltaAttachmentCS> ComputeShader(ShaderMap, PermutationVector);

	FUpdateBoneDeltaAttachmentCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpdateBoneDeltaAttachmentCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->BoneDeltaAttachments = BoneDeltaAttachmentUAV;

	// Boundary particles for attachment search (Z-Order sorted, contains OriginalIndex)
	PassParameters->SortedBoundaryParticles = SortedBoundaryParticlesSRV;
	PassParameters->BoundaryCellStart = BoundaryCellStartSRV;
	PassParameters->BoundaryCellEnd = BoundaryCellEndSRV;
	PassParameters->BoundaryParticleCount = BoundaryParticleCount;

	// World boundary particles (unsorted, for LocalOffset calculation)
	PassParameters->WorldBoundaryParticles = WorldBoundaryParticlesSRV;
	PassParameters->WorldBoundaryParticleCount = WorldBoundaryParticleCount;

	// Attachment parameters
	PassParameters->AttachRadius = Params.BoundaryAttachRadius;  // Use BoundaryAttachRadius from preset
	// DetachDistance = BoundaryAttachRadius * 5.0 (파티클이 AttachRadius의 5배 이상 급격히 이동하면 detach)
	PassParameters->DetachDistance = Params.BoundaryAttachRadius * 5.0f;
	// AdhesionStrength: if 0, no attachment allowed
	PassParameters->AdhesionStrength = Params.BoundaryAdhesionStrength;

	// Z-Order bounds (for cell ID calculation)
	PassParameters->MortonBoundsMin = SimulationBoundsMin;
	PassParameters->CellSize = Params.CellSize;

	// Hybrid Tiled Z-Order mode (for unlimited simulation range)
	bool bUseHybridTiledZOrder = ZOrderSortManager.IsValid() && ZOrderSortManager->IsHybridTiledZOrderEnabled();
	PassParameters->bUseHybridTiledZOrder = bUseHybridTiledZOrder ? 1 : 0;

	//=========================================================================
	// Collision Primitives (for direct collider surface normal calculation)
	// These are the same primitives used by FluidAdhesion
	//=========================================================================
	const TArray<FGPUCollisionSphere>& CachedSpheres = CollisionManager.IsValid() ? CollisionManager->GetCachedSpheres() : TArray<FGPUCollisionSphere>();
	const TArray<FGPUCollisionCapsule>& CachedCapsules = CollisionManager.IsValid() ? CollisionManager->GetCachedCapsules() : TArray<FGPUCollisionCapsule>();
	const TArray<FGPUCollisionBox>& CachedBoxes = CollisionManager.IsValid() ? CollisionManager->GetCachedBoxes() : TArray<FGPUCollisionBox>();
	const TArray<FGPUBoneTransform>& CachedBoneTransforms = CollisionManager.IsValid() ? CollisionManager->GetCachedBoneTransforms() : TArray<FGPUBoneTransform>();

	// DEBUG: Log collider counts and positions
	static int32 ColliderDebugCounter = 0;
	if (++ColliderDebugCounter % 120 == 1)
	{
		UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[BoneDeltaAttachment] Colliders: Spheres=%d, Capsules=%d, Boxes=%d, Bones=%d"),
			CachedSpheres.Num(), CachedCapsules.Num(), CachedBoxes.Num(), CachedBoneTransforms.Num());

		// Log first capsule position
		if (CachedCapsules.Num() > 0)
		{
			const FGPUCollisionCapsule& Cap = CachedCapsules[0];
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT("  Capsule[0]: Start=(%.1f, %.1f, %.1f), End=(%.1f, %.1f, %.1f), Radius=%.1f"),
				Cap.Start.X, Cap.Start.Y, Cap.Start.Z, Cap.End.X, Cap.End.Y, Cap.End.Z, Cap.Radius);
		}
		// Log first box position
		if (CachedBoxes.Num() > 0)
		{
			const FGPUCollisionBox& Box = CachedBoxes[0];
			UE_LOG(LogGPUFluidSimulator, Warning, TEXT("  Box[0]: Center=(%.1f, %.1f, %.1f), Extent=(%.1f, %.1f, %.1f)"),
				Box.Center.X, Box.Center.Y, Box.Center.Z, Box.Extent.X, Box.Extent.Y, Box.Extent.Z);
		}
	}

	// Create dummy data for empty buffers (RDG requires valid buffers)
	static FGPUCollisionSphere DummySphere;
	static FGPUCollisionCapsule DummyCapsule;
	static FGPUCollisionBox DummyBox;
	static FGPUBoneTransform DummyBone;

	// Spheres buffer
	FRDGBufferRef SpheresBuffer = CachedSpheres.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("BoneDeltaSpheres"), sizeof(FGPUCollisionSphere), CachedSpheres.Num(), CachedSpheres.GetData(), CachedSpheres.Num() * sizeof(FGPUCollisionSphere))
		: CreateStructuredBuffer(GraphBuilder, TEXT("BoneDeltaDummySpheres"), sizeof(FGPUCollisionSphere), 1, &DummySphere, sizeof(FGPUCollisionSphere));
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();

	// Capsules buffer
	FRDGBufferRef CapsulesBuffer = CachedCapsules.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("BoneDeltaCapsules"), sizeof(FGPUCollisionCapsule), CachedCapsules.Num(), CachedCapsules.GetData(), CachedCapsules.Num() * sizeof(FGPUCollisionCapsule))
		: CreateStructuredBuffer(GraphBuilder, TEXT("BoneDeltaDummyCapsules"), sizeof(FGPUCollisionCapsule), 1, &DummyCapsule, sizeof(FGPUCollisionCapsule));
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();

	// Boxes buffer
	FRDGBufferRef BoxesBuffer = CachedBoxes.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("BoneDeltaBoxes"), sizeof(FGPUCollisionBox), CachedBoxes.Num(), CachedBoxes.GetData(), CachedBoxes.Num() * sizeof(FGPUCollisionBox))
		: CreateStructuredBuffer(GraphBuilder, TEXT("BoneDeltaDummyBoxes"), sizeof(FGPUCollisionBox), 1, &DummyBox, sizeof(FGPUCollisionBox));
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();

	// Bone transforms buffer
	FRDGBufferRef BoneTransformsBuffer = CachedBoneTransforms.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("BoneDeltaBoneTransforms"), sizeof(FGPUBoneTransform), CachedBoneTransforms.Num(), CachedBoneTransforms.GetData(), CachedBoneTransforms.Num() * sizeof(FGPUBoneTransform))
		: CreateStructuredBuffer(GraphBuilder, TEXT("BoneDeltaDummyBoneTransforms"), sizeof(FGPUBoneTransform), 1, &DummyBone, sizeof(FGPUBoneTransform));
	PassParameters->BoneTransforms = GraphBuilder.CreateSRV(BoneTransformsBuffer);
	PassParameters->BoneCount = CachedBoneTransforms.Num();

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FUpdateBoneDeltaAttachmentCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::UpdateBoneDeltaAttachment(%d particles)", CurrentParticleCount),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

