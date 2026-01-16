// Copyright KawaiiFluid Team. All Rights Reserved.
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

	// Boundary Particles for density contribution (Akinci 2012)
	// Priority: 1) Same-frame buffer from SpatialData, 2) Persistent buffer, 3) CPU fallback
	const bool bUseSameFrameBuffer = SpatialData.bBoundarySkinningPerformed && SpatialData.WorldBoundarySRV != nullptr;
	const bool bUsePersistentBuffer = !bUseSameFrameBuffer && BoundarySkinningManager.IsValid()
		&& BoundarySkinningManager->IsGPUBoundarySkinningEnabled() && BoundarySkinningManager->HasWorldBoundaryBuffer();
	const bool bUseCPUBoundary = !bUseSameFrameBuffer && !bUsePersistentBuffer
		&& BoundarySkinningManager.IsValid() && BoundarySkinningManager->HasBoundaryParticles();

	// Debug: Log boundary particle path (every 120 frames)
	static int32 BoundaryDebugCounter = 0;
	if (++BoundaryDebugCounter % 120 == 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BoundaryDebug] SameFrame=%d, Persistent=%d, CPU=%d, HasManager=%d, HasParticles=%d, LocalCount=%d"),
			bUseSameFrameBuffer ? 1 : 0,
			bUsePersistentBuffer ? 1 : 0,
			bUseCPUBoundary ? 1 : 0,
			BoundarySkinningManager.IsValid() ? 1 : 0,
			BoundarySkinningManager.IsValid() ? (BoundarySkinningManager->HasBoundaryParticles() ? 1 : 0) : 0,
			BoundarySkinningManager.IsValid() ? BoundarySkinningManager->GetTotalLocalBoundaryParticleCount() : 0);
	}

	if (bUseSameFrameBuffer)
	{
		// Use same-frame buffer created in AddBoundarySkinningPass (works on first frame!)
		PassParameters->BoundaryParticles = SpatialData.WorldBoundarySRV;
		PassParameters->BoundaryParticleCount = SpatialData.WorldBoundaryParticleCount;
		PassParameters->bUseBoundaryDensity = 1;
	}
	else if (bUsePersistentBuffer)
	{
		// Use GPU-skinned world boundary buffer from previous frame
		FRDGBufferRef BoundaryBuffer = GraphBuilder.RegisterExternalBuffer(BoundarySkinningManager->GetWorldBoundaryBuffer(), TEXT("GPUFluidBoundaryParticles_Density"));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = BoundarySkinningManager->GetTotalLocalBoundaryParticleCount();
		PassParameters->bUseBoundaryDensity = 1;
	}
	else if (bUseCPUBoundary)
	{
		// Use CPU-uploaded boundary particles (legacy path)
		const TArray<FGPUBoundaryParticle>& CachedParticles = BoundarySkinningManager->GetCachedBoundaryParticles();
		const int32 BoundaryCount = CachedParticles.Num();
		FRDGBufferRef BoundaryBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles_Density"),
			sizeof(FGPUBoundaryParticle),
			BoundaryCount,
			CachedParticles.GetData(),
			BoundaryCount * sizeof(FGPUBoundaryParticle),
			ERDGInitialDataFlags::NoCopy
		);
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = BoundaryCount;
		PassParameters->bUseBoundaryDensity = 1;
	}
	else
	{
		// Create dummy buffer for RDG validation
		// Must use QueueBufferUpload to mark buffer as "produced" for RDG validation
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1),
			TEXT("GPUFluidBoundaryParticles_Density_Dummy")
		);
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(DummyBuffer);
		PassParameters->BoundaryParticleCount = 0;
		PassParameters->bUseBoundaryDensity = 0;
	}

	// Debug: Log final boundary count being used (every 120 frames)
	if (BoundaryDebugCounter % 120 == 1)
	{
		const TCHAR* PathName = bUseSameFrameBuffer ? TEXT("SameFrame") :
			(bUsePersistentBuffer ? TEXT("Persistent") :
			(bUseCPUBoundary ? TEXT("CPU") : TEXT("NONE")));
		UE_LOG(LogTemp, Warning, TEXT("[BoundaryDebug] Path=%s, BoundaryCount=%d, bUseBoundaryDensity=%d"),
			PathName, PassParameters->BoundaryParticleCount, PassParameters->bUseBoundaryDensity);
	}

	// Z-Order sorted boundary particles (Akinci 2012 + Z-Order optimization)
	const bool bUseBoundaryZOrder = BoundarySkinningManager.IsValid()
		&& BoundarySkinningManager->IsBoundaryZOrderEnabled()
		&& BoundarySkinningManager->HasBoundaryZOrderData();

	if (bUseBoundaryZOrder)
	{
		// Use Z-Order sorted boundary buffer for O(K) neighbor search
		FRDGBufferRef SortedBoundaryBuffer = GraphBuilder.RegisterExternalBuffer(
			BoundarySkinningManager->GetSortedBoundaryBuffer(),
			TEXT("GPUFluidSortedBoundaryParticles_Density"));
		FRDGBufferRef BoundaryCellStartBuffer = GraphBuilder.RegisterExternalBuffer(
			BoundarySkinningManager->GetBoundaryCellStartBuffer(),
			TEXT("GPUFluidBoundaryCellStart_Density"));
		FRDGBufferRef BoundaryCellEndBuffer = GraphBuilder.RegisterExternalBuffer(
			BoundarySkinningManager->GetBoundaryCellEndBuffer(),
			TEXT("GPUFluidBoundaryCellEnd_Density"));

		PassParameters->SortedBoundaryParticles = GraphBuilder.CreateSRV(SortedBoundaryBuffer);
		PassParameters->BoundaryCellStart = GraphBuilder.CreateSRV(BoundaryCellStartBuffer);
		PassParameters->BoundaryCellEnd = GraphBuilder.CreateSRV(BoundaryCellEndBuffer);
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
	PassParameters->CellSize = Params.CellSize;
	PassParameters->bUseNeighborCache = (InNeighborListSRV != nullptr && InNeighborCountsSRV != nullptr) ? 1 : 0;

	// Boundary Particles for viscosity contribution
	// Priority: 1) Same-frame buffer from SpatialData, 2) Persistent buffer, 3) CPU fallback
	const bool bUseViscSameFrameBuffer = SpatialData.bBoundarySkinningPerformed && SpatialData.WorldBoundarySRV != nullptr;
	const bool bUseViscPersistentBuffer = !bUseViscSameFrameBuffer && BoundarySkinningManager.IsValid()
		&& BoundarySkinningManager->IsGPUBoundarySkinningEnabled() && BoundarySkinningManager->HasWorldBoundaryBuffer();
	const bool bUseCPUBoundary = !bUseViscSameFrameBuffer && !bUseViscPersistentBuffer
		&& BoundarySkinningManager.IsValid() && BoundarySkinningManager->HasBoundaryParticles();

	if (bUseViscSameFrameBuffer)
	{
		// Use same-frame buffer created in AddBoundarySkinningPass (works on first frame!)
		PassParameters->BoundaryParticles = SpatialData.WorldBoundarySRV;
		PassParameters->BoundaryParticleCount = SpatialData.WorldBoundaryParticleCount;
		PassParameters->bUseBoundaryViscosity = 1;
	}
	else if (bUseViscPersistentBuffer)
	{
		// Use GPU-skinned world boundary buffer from previous frame
		FRDGBufferRef BoundaryBuffer = GraphBuilder.RegisterExternalBuffer(BoundarySkinningManager->GetWorldBoundaryBuffer(), TEXT("GPUFluidBoundaryParticles_Viscosity"));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = BoundarySkinningManager->GetTotalLocalBoundaryParticleCount();
		PassParameters->bUseBoundaryViscosity = 1;
	}
	else if (bUseCPUBoundary)
	{
		// Use CPU-uploaded boundary particles (legacy path)
		const TArray<FGPUBoundaryParticle>& CachedParticles = BoundarySkinningManager->GetCachedBoundaryParticles();
		const int32 BoundaryCount = CachedParticles.Num();
		FRDGBufferRef BoundaryBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles_Viscosity"),
			sizeof(FGPUBoundaryParticle),
			BoundaryCount,
			CachedParticles.GetData(),
			BoundaryCount * sizeof(FGPUBoundaryParticle),
			ERDGInitialDataFlags::NoCopy
		);
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = BoundaryCount;
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
// Apply Cohesion Pass (Akinci 2013 Surface Tension)
//=============================================================================

void FGPUFluidSimulator::AddApplyCohesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	FRDGBufferSRVRef InNeighborListSRV,
	FRDGBufferSRVRef InNeighborCountsSRV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if cohesion is disabled
	if (Params.CohesionStrength <= 0.0f)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FApplyCohesionCS> ComputeShader(ShaderMap);

	FApplyCohesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FApplyCohesionCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->NeighborList = InNeighborListSRV;
	PassParameters->NeighborCounts = InNeighborCountsSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->CohesionStrength = Params.CohesionStrength;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->bUseNeighborCache = (InNeighborListSRV != nullptr && InNeighborCountsSRV != nullptr) ? 1 : 0;
	// Akinci 2013 surface tension parameters
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	// MaxSurfaceTensionForce: limit force to prevent instability
	// Scale based on particle mass and smoothing radius
	// Typical value: CohesionStrength * RestDensity * h^3 * 1000 (empirical)
	const float h_m = Params.SmoothingRadius * 0.01f;  // cm to m
	PassParameters->MaxSurfaceTensionForce = Params.CohesionStrength * Params.RestDensity * h_m * h_m * h_m * 1000.0f;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FApplyCohesionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ApplyCohesion"),
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