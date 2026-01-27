// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// GPUFluidSimulator - Simulation Pass Functions

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/Managers/GPUBoundarySkinningManager.h"
#include "GPU/Managers/GPUZOrderSortManager.h"
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
	// Cohesion Force Parameters (moved from PostSimulation to Phase 2)
	// This prevents jittering by letting the solver consider cohesion forces
	//=========================================================================
	PassParameters->CohesionStrength = Params.CohesionStrength;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	
	// MaxCohesionForce: stability clamp based on physical parameters
	const float h_m = Params.SmoothingRadius * 0.01f;  // cm to m
	PassParameters->MaxCohesionForce = Params.CohesionStrength * Params.RestDensity * h_m * h_m * h_m * 1000.0f;

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

	// Debug: log cohesion state
	static int32 DebugCounter = 0;
	if (++DebugCounter % 60 == 0)
	{
		UE_LOG(LogGPUFluidSimulator, Log, 
			TEXT("PredictPositions: Cohesion=%s (Strength=%.2f, PrevCache=%d particles)"),
			bCanUsePrevNeighborCache ? TEXT("ON") : TEXT("OFF"),
			Params.CohesionStrength,
			PrevNeighborBufferParticleCount);
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
	PassParameters->Particles = InParticlesUAV;
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

	// =========================================================================
	// Boundary Particles for density contribution (Akinci 2012)
	// Two separate boundary types: Skinned (SkeletalMesh) and Static (StaticMesh)
	// =========================================================================

	// Check available boundary types
	const bool bHasSkinnedBoundary = SpatialData.bSkinnedBoundaryPerformed && SpatialData.SkinnedBoundarySRV != nullptr;
	const bool bHasStaticBoundary = SpatialData.bStaticBoundaryAvailable && SpatialData.StaticBoundarySRV != nullptr;

	// Debug: Log boundary status (every 120 frames)
	static int32 BoundaryDebugCounter = 0;
	if (++BoundaryDebugCounter % 120 == 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BoundaryDebug] Skinned=%d (Count=%d), Static=%d (Count=%d)"),
			bHasSkinnedBoundary ? 1 : 0, SpatialData.SkinnedBoundaryParticleCount,
			bHasStaticBoundary ? 1 : 0, SpatialData.StaticBoundaryParticleCount);
	}

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

	// Debug: Log boundary path and Z-Order status (every 120 frames)
	if (BoundaryDebugCounter % 120 == 1)
	{
		const TCHAR* BoundaryPath = bHasSkinnedBoundary ? TEXT("Skinned") :
			(bHasStaticBoundary ? TEXT("Static") : TEXT("NONE"));
		const TCHAR* ZOrderPath = bUseSkinnedZOrder ? TEXT("Skinned") :
			(bUseStaticZOrder ? TEXT("Static") : TEXT("Disabled"));
		UE_LOG(LogTemp, Warning, TEXT("[BoundaryDebug] BoundaryPath=%s, ZOrderPath=%s, BoundaryCount=%d, bUseDensity=%d, bUseZOrder=%d"),
			BoundaryPath, ZOrderPath, PassParameters->BoundaryParticleCount, PassParameters->bUseBoundaryDensity, PassParameters->bUseBoundaryZOrder);
	}

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
// Apply Viscosity Pass
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

	// Debug: Log Viscosity boundary Z-Order status (every 120 frames)
	static int32 ViscosityDebugCounter = 0;
	ViscosityDebugCounter++;
	if (ViscosityDebugCounter % 120 == 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ViscosityDebug] BoundaryCount=%d, bUseBoundaryViscosity=%d, bUseZOrder=%d, SkinnedZOrder=%d, StaticZOrder=%d, ForceStr=%.2f, VelStr=%.2f, Radius=%.1f"),
			PassParameters->BoundaryParticleCount,
			PassParameters->bUseBoundaryViscosity,
			PassParameters->bUseBoundaryZOrder,
			bUseSkinnedZOrder ? 1 : 0,
			bUseStaticZOrder ? 1 : 0,
			PassParameters->AdhesionForceStrength,
			PassParameters->AdhesionVelocityStrength,
			PassParameters->AdhesionRadius);
	}

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
// Combined Viscosity and Cohesion Pass (Optimized)
//=============================================================================

void FGPUFluidSimulator::AddApplyViscosityAndCohesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	FRDGBufferSRVRef InNeighborListSRV,
	FRDGBufferSRVRef InNeighborCountsSRV,
	const FGPUFluidSimulationParams& Params,
	const FSimulationSpatialData& SpatialData)
{
	// Skip if no particles
	if (CurrentParticleCount <= 0) return;

	// Skip if viscosity is disabled
	// Note: Cohesion is now handled in PredictPositions (Phase 2), not here
	// This pass is now "Viscosity-only" (Cohesion code remains but CohesionStrength=0)
	if (Params.ViscosityCoefficient <= 0.0f)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyViscosityAndCohesionCS> ComputeShader(ShaderMap);

	FApplyViscosityAndCohesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyViscosityAndCohesionCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->NeighborList = InNeighborListSRV;
	PassParameters->NeighborCounts = InNeighborCountsSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->bUseNeighborCache = (InNeighborListSRV != nullptr && InNeighborCountsSRV != nullptr) ? 1 : 0;

	// Viscosity parameters
	PassParameters->ViscosityCoefficient = Params.ViscosityCoefficient;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	const float h_m = Params.SmoothingRadius * 0.01f;  // cm to m
	PassParameters->ViscLaplacianCoeff = 45.0f / (UE_PI * FMath::Pow(h_m, 6.0f));

	// Cohesion parameters
	// =====================================================
	// COHESION FORCE MIGRATION: Phase 5 -> Phase 2 (PredictPositions)
	// Cohesion is now applied as a Force in PredictPositions using previous frame's neighbor cache.
	// This prevents the jittering caused by applying Cohesion AFTER the density constraint solver.
	// Set CohesionStrength = 0 here to disable Phase 5 Cohesion (keep only Viscosity).
	// =====================================================
	PassParameters->CohesionStrength = 0.0f;  // Disabled: Cohesion now in Phase 2 (PredictPositions)
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->MaxSurfaceTensionForce = 0.0f;  // Disabled with Cohesion

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
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidBoundaryParticles_ViscCoh_Dummy")
		);
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(DummyBuffer);
		PassParameters->BoundaryParticleCount = 0;
		PassParameters->bUseBoundaryViscosity = 0;
	}

	// AdhesionStrength and AdhesionRadius for boundary viscosity
	if (BoundarySkinningManager.IsValid())
	{
		const FGPUBoundaryAdhesionParams& AdhesionParams = BoundarySkinningManager->GetBoundaryAdhesionParams();
		PassParameters->AdhesionStrength = AdhesionParams.AdhesionForceStrength;
		PassParameters->AdhesionRadius = AdhesionParams.AdhesionRadius;
	}
	else
	{
		PassParameters->AdhesionStrength = 0.0f;
		PassParameters->AdhesionRadius = 0.0f;
	}

	// Z-Order sorted boundary particles
	const bool bUseSkinnedZOrder = bHasSkinnedBoundary && SpatialData.bSkinnedZOrderPerformed
		&& SpatialData.SkinnedZOrderSortedSRV != nullptr;
	const bool bUseStaticZOrder = !bUseSkinnedZOrder && bHasStaticBoundary
		&& SpatialData.StaticZOrderSortedSRV != nullptr;

	if (bUseSkinnedZOrder)
	{
		PassParameters->SortedBoundaryParticles = SpatialData.SkinnedZOrderSortedSRV;
		PassParameters->BoundaryCellStart = SpatialData.SkinnedZOrderCellStartSRV;
		PassParameters->BoundaryCellEnd = SpatialData.SkinnedZOrderCellEndSRV;
		PassParameters->bUseBoundaryZOrder = 1;
	}
	else if (bUseStaticZOrder)
	{
		PassParameters->SortedBoundaryParticles = SpatialData.StaticZOrderSortedSRV;
		PassParameters->BoundaryCellStart = SpatialData.StaticZOrderCellStartSRV;
		PassParameters->BoundaryCellEnd = SpatialData.StaticZOrderCellEndSRV;
		PassParameters->bUseBoundaryZOrder = 1;
	}
	else
	{
		// Create dummy buffers for RDG validation
		FRDGBufferRef DummySortedBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidSortedBoundary_ViscCoh_Dummy"));
		FGPUBoundaryParticle ZeroBoundaryForZOrder = {};
		GraphBuilder.QueueBufferUpload(DummySortedBuffer, &ZeroBoundaryForZOrder, sizeof(FGPUBoundaryParticle));

		FRDGBufferRef DummyCellStartBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("GPUFluidBoundaryCellStart_ViscCoh_Dummy"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyCellStartBuffer, &InvalidIndex, sizeof(uint32));

		FRDGBufferRef DummyCellEndBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("GPUFluidBoundaryCellEnd_ViscCoh_Dummy"));
		GraphBuilder.QueueBufferUpload(DummyCellEndBuffer, &InvalidIndex, sizeof(uint32));

		PassParameters->SortedBoundaryParticles = GraphBuilder.CreateSRV(DummySortedBuffer);
		PassParameters->BoundaryCellStart = GraphBuilder.CreateSRV(DummyCellStartBuffer);
		PassParameters->BoundaryCellEnd = GraphBuilder.CreateSRV(DummyCellEndBuffer);
		PassParameters->bUseBoundaryZOrder = 0;
	}

	PassParameters->MortonBoundsMin = SimulationBoundsMin;

	// Boundary velocity transfer
	PassParameters->BoundaryVelocityTransferStrength = Params.BoundaryVelocityTransferStrength;
	PassParameters->BoundaryDetachSpeedThreshold = Params.BoundaryDetachSpeedThreshold;
	PassParameters->BoundaryMaxDetachSpeed = Params.BoundaryMaxDetachSpeed;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FApplyViscosityAndCohesionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyViscosityAndCohesion"),
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
	FRDGBufferUAVRef ParticlesUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (CurrentParticleCount <= 0) return;

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFinalizePositionsCS> ComputeShader(ShaderMap);

	FFinalizePositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFinalizePositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
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


