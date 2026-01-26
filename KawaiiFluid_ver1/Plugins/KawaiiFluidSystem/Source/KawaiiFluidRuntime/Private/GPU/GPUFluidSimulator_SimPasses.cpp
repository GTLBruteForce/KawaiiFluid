// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// GPUFluidSimulator - Simulation Pass Functions

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "GPU/Managers/GPUBoundarySkinningManager.h"
#include "GPU/Managers/GPUZOrderSortManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Async/Async.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"

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

	// Debug: log gravity and delta time
	static int32 DebugCounter = 0;
	if (++DebugCounter % 60 == 0)
	{
		//UE_LOG(LogGPUFluidSimulator, Log, TEXT("PredictPositions: Gravity=(%.2f, %.2f, %.2f), DeltaTime=%.4f, Particles=%d"),Params.Gravity.X, Params.Gravity.Y, Params.Gravity.Z, Params.DeltaTime, CurrentParticleCount);
	}

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FPredictPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::PredictPositions"),
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

	// Skip if both viscosity and cohesion are disabled
	if (Params.ViscosityCoefficient <= 0.0f && Params.CohesionStrength <= 0.0f)
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
	PassParameters->CohesionStrength = Params.CohesionStrength;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->MaxSurfaceTensionForce = Params.CohesionStrength * Params.RestDensity * h_m * h_m * h_m * 1000.0f;

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

//=============================================================================
// Boundary Attachment Update Pass
// Manages strong position constraints between fluid and boundary particles
//=============================================================================

void FGPUFluidSimulator::AddBoundaryAttachmentUpdatePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	const FSimulationSpatialData& SpatialData,
	const FGPUFluidSimulationParams& Params)
{
	// Debug: Log all conditions (every 60 frames)
	static int32 AttachmentDebugCounter = 0;
	const bool bAttachmentDebugLog = (++AttachmentDebugCounter % 60 == 1);

	// Skip if no particles or no boundary particles
	if (CurrentParticleCount <= 0)
	{
		if (bAttachmentDebugLog) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[AttachmentPass] SKIP: CurrentParticleCount <= 0"));
		return;
	}

	// Check if boundary attachment is enabled via Params
	if (!Params.bEnableBoundaryAttachment)
	{
		if (bAttachmentDebugLog) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[AttachmentPass] SKIP: Params.bEnableBoundaryAttachment=false"));
		return;
	}

	// Check if BoundarySkinningManager is available
	if (!BoundarySkinningManager.IsValid())
	{
		if (bAttachmentDebugLog) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[AttachmentPass] SKIP: BoundarySkinningManager not valid"));
		return;
	}

	const FGPUBoundaryAdhesionParams& AdhesionParams = BoundarySkinningManager->GetBoundaryAdhesionParams();
	if (!AdhesionParams.bEnabled)
	{
		if (bAttachmentDebugLog) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[AttachmentPass] SKIP: AdhesionParams.bEnabled=false"));
		return;
	}

	// Require Z-Order sorted boundary particles
	const bool bHasSkinnedZOrder = SpatialData.bSkinnedZOrderPerformed
		&& SpatialData.SkinnedZOrderSortedSRV != nullptr
		&& SpatialData.SkinnedZOrderCellStartSRV != nullptr
		&& SpatialData.SkinnedZOrderCellEndSRV != nullptr;

	if (!bHasSkinnedZOrder)
	{
		if (bAttachmentDebugLog)
		{
			UE_LOG(LogGPUFluidSimulator, Warning,
				TEXT("[AttachmentPass] SKIP: bHasSkinnedZOrder=false (bSkinnedZOrderPerformed=%d, SortedSRV=%p, CellStartSRV=%p, CellEndSRV=%p)"),
				SpatialData.bSkinnedZOrderPerformed ? 1 : 0,
				SpatialData.SkinnedZOrderSortedSRV,
				SpatialData.SkinnedZOrderCellStartSRV,
				SpatialData.SkinnedZOrderCellEndSRV);
		}
		return;
	}

	const int32 BoundaryParticleCount = SpatialData.SkinnedZOrderParticleCount;
	if (BoundaryParticleCount <= 0)
	{
		if (bAttachmentDebugLog) UE_LOG(LogGPUFluidSimulator, Warning, TEXT("[AttachmentPass] SKIP: BoundaryParticleCount <= 0"));
		return;
	}

	// All conditions passed!
	if (bAttachmentDebugLog)
	{
		const bool bHasOwnerTransform = BoundarySkinningManager->HasOwnerTransformBuffer();
		const int32 MaxOwnerID = bHasOwnerTransform ? BoundarySkinningManager->GetMaxOwnerID() : -1;
		const int32 CellRadius = FMath::CeilToInt(Params.BoundaryAttachRadius / Params.CellSize);
		const int32 PrimaryBoneCount = BoundarySkinningManager->GetPrimaryBoneCount();
		const FVector3f& BoundaryZOrderBounds = BoundarySkinningManager->GetZOrderBoundsMin();
		UE_LOG(LogGPUFluidSimulator, Warning,
			TEXT("[AttachmentPass] RUNNING: ParticleCount=%d, BoundaryCount=%d, AttachRadius=%.2f, CellSize=%.2f, CellRadius=%d, DetachMult=%.2f, ConstraintBlend=%.2f, MaxOwnerID=%d, HasOwnerTransform=%d, BoneCount=%d, BoundaryZOrderBounds=(%.1f,%.1f,%.1f), SimBounds=(%.1f,%.1f,%.1f)"),
			CurrentParticleCount, BoundaryParticleCount,
			Params.BoundaryAttachRadius, Params.CellSize, CellRadius,
			Params.BoundaryDetachDistanceMultiplier, Params.BoundaryAttachConstraintBlend,
			MaxOwnerID, bHasOwnerTransform ? 1 : 0, PrimaryBoneCount,
			BoundaryZOrderBounds.X, BoundaryZOrderBounds.Y, BoundaryZOrderBounds.Z,
			SimulationBoundsMin.X, SimulationBoundsMin.Y, SimulationBoundsMin.Z);
	}

	// Attachment data is now embedded in FGPUFluidParticle (96 bytes)
	// No separate AttachmentBuffer needed - attachment reorders with particle during Z-Order sort

	// Get shader
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Use permutation based on grid resolution
	EGridResolutionPreset GridPreset = EGridResolutionPreset::Medium;
	if (ZOrderSortManager.IsValid())
	{
		GridPreset = ZOrderSortManager->GetGridResolutionPreset();
	}

	FBoundaryAttachmentUpdateCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FGridResolutionDim>(static_cast<int32>(GridPreset));
	TShaderMapRef<FBoundaryAttachmentUpdateCS> ComputeShader(ShaderMap, PermutationVector);

	// Setup parameters
	FBoundaryAttachmentUpdateCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FBoundaryAttachmentUpdateCS::FParameters>();

	// Create counter buffer for debug (20 counters for detailed diagnostics)
	// [0] = Attached particle count (final)
	// [1] = Particles that found a boundary (nearestBoundaryIdx >= 0)
	// [2] = Particles within AttachRadius
	// [3] = Particles with valid OwnerID
	// [4] = New attachments this frame
	// [5] = Detachments this frame
	// [6] = Particles in cooldown
	// [7] = Particles already attached (maintained)
	// [8] = Total cells searched (cumulative)
	// [9] = Non-empty cells found (cumulative)
	// [10] = Boundary particles checked (cumulative)
	// [11] = Boundary particles within radius (distance check only)
	// [12] = Boundary particles with valid OwnerID (after distance check)
	// [13] = Minimum distance squared (as uint bits)
	// [14] = First boundary OwnerID found
	// [15] = Count of OwnerID == -1
	// [16] = MaxOwnerID as seen by shader
	constexpr int32 NumCounters = 20;
	FRDGBufferDesc CounterDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumCounters);
	FRDGBufferRef CounterBuffer = GraphBuilder.CreateBuffer(CounterDesc, TEXT("GPUFluid.AttachmentCounter"));
	
	// Initialize counters: most to 0, but [13] and [14] need special values for InterlockedMin/CompareExchange
	TArray<uint32> InitialCounters;
	InitialCounters.SetNumZeroed(NumCounters);
	InitialCounters[13] = 0x7F7FFFFF;  // Large float as uint (for InterlockedMin on min distance)
	InitialCounters[14] = 0xFFFFFFFF;  // Sentinel for InterlockedCompareExchange
	GraphBuilder.QueueBufferUpload(CounterBuffer, InitialCounters.GetData(), NumCounters * sizeof(uint32));

	// Particles buffer now includes embedded attachment data (96 bytes)
	// No separate Attachments buffer needed - attachment is reordered with particle during Z-Order sort
	PassParameters->Particles = ParticlesUAV;
	PassParameters->SortedBoundaryParticles = SpatialData.SkinnedZOrderSortedSRV;
	PassParameters->BoundaryCellStart = SpatialData.SkinnedZOrderCellStartSRV;
	PassParameters->BoundaryCellEnd = SpatialData.SkinnedZOrderCellEndSRV;
	PassParameters->AttachmentCounter = GraphBuilder.CreateUAV(CounterBuffer, PF_R32_UINT);

	// Owner Transform buffer (for local-to-world conversion)
	// UpdateOwnerTransformBuffer returns the RDG buffer directly for use in the same frame
	int32 CurrentMaxOwnerID = -1;
	FRDGBufferRef OwnerTransformBuffer = BoundarySkinningManager->UpdateOwnerTransformBuffer(GraphBuilder, CurrentMaxOwnerID);
	if (OwnerTransformBuffer)
	{
		PassParameters->OwnerTransforms = GraphBuilder.CreateSRV(OwnerTransformBuffer);
		PassParameters->MaxOwnerID = CurrentMaxOwnerID;
	}
	else
	{
		// Create dummy buffer if no owners
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryOwnerTransform), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("GPUFluid.OwnerTransforms.Dummy"));
		PassParameters->OwnerTransforms = GraphBuilder.CreateSRV(DummyBuffer);
		PassParameters->MaxOwnerID = -1;
	}

	// Bone Transforms buffer (for skeletal mesh attachment - follows bone animation)
	// InverseBoneTransforms is pre-computed on CPU for accurate inverse (handles scale correctly)
	int32 BoneCount = 0;
	FRDGBufferRef InverseBoneTransformsBuffer = nullptr;
	FRDGBufferRef BoneTransformsBuffer = BoundarySkinningManager->CreateCombinedBoneTransformsBuffer(GraphBuilder, BoneCount, InverseBoneTransformsBuffer);
	PassParameters->BoneTransforms = GraphBuilder.CreateSRV(BoneTransformsBuffer);
	PassParameters->InverseBoneTransforms = GraphBuilder.CreateSRV(InverseBoneTransformsBuffer);
	PassParameters->BoneCount = BoneCount;

	// Particle counts
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->BoundaryParticleCount = BoundaryParticleCount;

	// Attachment parameters (from FGPUFluidSimulationParams, populated by Preset)
	PassParameters->AttachRadius = Params.BoundaryAttachRadius;
	PassParameters->DetachDistanceMultiplier = Params.BoundaryDetachDistanceMultiplier;
	PassParameters->DetachSpeedThreshold = Params.BoundaryAttachDetachSpeedThreshold;
	PassParameters->AttachCooldown = Params.BoundaryAttachCooldown;
	PassParameters->ConstraintBlend = Params.BoundaryAttachConstraintBlend;

	// AdhesionStrength for drift-based detach threshold
	// AdhesionParams already declared at function start (line ~721)
	PassParameters->AdhesionStrength = AdhesionParams.AdhesionForceStrength;

	// Time parameters
	PassParameters->CurrentTime = Params.CurrentTime;
	PassParameters->DeltaTime = Params.DeltaTime;

	// Z-Order parameters - CRITICAL: Must use the same bounds as Boundary Z-Order sorting!
	// Using SimulationBoundsMin causes cell ID mismatch when bounds differ from Boundary Z-Order
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->MortonBoundsMin = BoundarySkinningManager->GetZOrderBoundsMin();

	// Dispatch
	const int32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FBoundaryAttachmentUpdateCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::BoundaryAttachmentUpdate"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);

	// Debug: Extract counter buffer for readback (available next frame)
	GraphBuilder.QueueBufferExtraction(CounterBuffer, &BoundaryAttachmentCounterBuffer);

	// Debug: Check previous frame's readback result
	if (bAttachmentDebugLog)
	{
		// Check if previous readback is ready
		if (BoundaryAttachmentCounterReadback.IsValid() && BoundaryAttachmentCounterReadback->IsReady())
		{
			const uint32* ResultData = static_cast<const uint32*>(BoundaryAttachmentCounterReadback->Lock(NumCounters * sizeof(uint32)));
			if (ResultData)
			{
				// Read all 8 counters
				const uint32 FinalAttached = ResultData[0];
				const uint32 FoundBoundary = ResultData[1];
				const uint32 WithinRadius = ResultData[2];
				const uint32 ValidOwnerID = ResultData[3];
				const uint32 NewAttach = ResultData[4];
				const uint32 Detached = ResultData[5];
				const uint32 InCooldown = ResultData[6];
				const uint32 Maintained = ResultData[7];
				const uint32 CellsSearched = ResultData[8];
				const uint32 NonEmptyCells = ResultData[9];
				const uint32 BoundaryChecked = ResultData[10];
				const uint32 BndWithinRadius = ResultData[11];
				const uint32 BndValidOwnerID = ResultData[12];
				const uint32 MinDistSqBits = ResultData[13];
				const uint32 FirstOwnerIDBits = ResultData[14];
				const uint32 OwnerNegOneCount = ResultData[15];

				// Convert min distance from uint bits back to float
				const float MinDistSq = *reinterpret_cast<const float*>(&MinDistSqBits);
				const float MinDist = FMath::Sqrt(MinDistSq);

				// First OwnerID (stored as uint, convert back to int)
				const int32 FirstOwnerID = (FirstOwnerIDBits == 0xFFFFFFFF) ? -999 : static_cast<int32>(FirstOwnerIDBits);

				LastBoundaryAttachedParticleCount = static_cast<int32>(FinalAttached);

				UE_LOG(LogGPUFluidSimulator, Warning,
					TEXT("[AttachmentPass] READBACK: Final=%u, FoundBnd=%u, InRadius=%u, ValidOwner=%u, NewAttach=%u, Detach=%u, Cooldown=%u, Maintained=%u / Total=%d"),
					FinalAttached, FoundBoundary, WithinRadius, ValidOwnerID, NewAttach, Detached, InCooldown, Maintained, CurrentParticleCount);

				UE_LOG(LogGPUFluidSimulator, Warning,
					TEXT("[AttachmentPass] SEARCH: CellsSearched=%u, NonEmptyCells=%u, BndChecked=%u, BndInRadius=%u, BndValidOwner=%u"),
					CellsSearched, NonEmptyCells, BoundaryChecked, BndWithinRadius, BndValidOwnerID);

				const int32 ShaderMaxOwnerID = static_cast<int32>(ResultData[16]);

				UE_LOG(LogGPUFluidSimulator, Warning,
					TEXT("[AttachmentPass] DEBUG: MinDist=%.2f (AttachRadius=%.2f), FirstOwnerID=%d, MaxOwnerID(C++)=%d, MaxOwnerID(Shader)=%d, OwnerID==-1 count=%u"),
					MinDist, Params.BoundaryAttachRadius, FirstOwnerID, BoundarySkinningManager->GetMaxOwnerID(), ShaderMaxOwnerID, OwnerNegOneCount);

				// Diagnosis hints
				if (NonEmptyCells == 0)
				{
					UE_LOG(LogGPUFluidSimulator, Warning, TEXT("  -> PROBLEM: All searched cells are EMPTY. Z-Order cell mismatch!"));
				}
				else if (BoundaryChecked == 0)
				{
					UE_LOG(LogGPUFluidSimulator, Warning, TEXT("  -> PROBLEM: NonEmptyCells=%u but no boundary particles checked. Cell range issue?"), NonEmptyCells);
				}
				else if (BndWithinRadius == 0)
				{
					UE_LOG(LogGPUFluidSimulator, Warning, TEXT("  -> PROBLEM: MinDist=%.2f > AttachRadius=%.2f. Particles too far from boundary!"), MinDist, Params.BoundaryAttachRadius);
				}
				else if (BndValidOwnerID == 0)
				{
					UE_LOG(LogGPUFluidSimulator, Warning, TEXT("  -> PROBLEM: %u particles within radius but ALL have invalid OwnerID. FirstOwnerID=%d, OwnerID==-1 count=%u"),
						BndWithinRadius, FirstOwnerID, OwnerNegOneCount);
				}
			}
			BoundaryAttachmentCounterReadback->Unlock();
		}
	}

	// Enqueue readback for this frame's counter (using RDG pass for proper synchronization)
	if (bAttachmentDebugLog)
	{
		if (!BoundaryAttachmentCounterReadback.IsValid())
		{
			BoundaryAttachmentCounterReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("BoundaryAttachmentCounterReadback"));
		}

		// Use AddEnqueueCopyPass for proper RDG synchronization
		FRHIGPUBufferReadback* Readback = BoundaryAttachmentCounterReadback.Get();
		AddEnqueueCopyPass(GraphBuilder, Readback, CounterBuffer, NumCounters * sizeof(uint32));
	}
}