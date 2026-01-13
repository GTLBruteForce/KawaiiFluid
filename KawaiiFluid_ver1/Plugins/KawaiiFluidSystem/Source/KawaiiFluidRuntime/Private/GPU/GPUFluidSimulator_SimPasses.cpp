// Copyright KawaiiFluid Team. All Rights Reserved.
// GPUFluidSimulator - Simulation Pass Functions

#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGPUFluidSimulator, Log, All);

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
// Compute Density Pass
//=============================================================================

void FGPUFluidSimulator::AddComputeDensityPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FComputeDensityCS> ComputeShader(ShaderMap);

	FComputeDensityCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComputeDensityCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->Poly6Coeff = Params.Poly6Coeff;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Compliance = Params.Compliance;
	PassParameters->DeltaTimeSq = Params.DeltaTimeSq;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FComputeDensityCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ComputeDensity"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

//=============================================================================
// Solve Pressure Pass
//=============================================================================

void FGPUFluidSimulator::AddSolvePressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSolvePressureCS> ComputeShader(ShaderMap);

	FSolvePressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSolvePressureCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->RestDensity = Params.RestDensity;
	PassParameters->SpikyCoeff = Params.SpikyCoeff;
	PassParameters->CellSize = Params.CellSize;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FSolvePressureCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::SolvePressure"),
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
	const FGPUFluidSimulationParams& Params)
{
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FSolveDensityPressureCS> ComputeShader(ShaderMap);

	FSolveDensityPressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSolveDensityPressureCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	// Hash table mode (legacy)
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;
	// Z-Order sorted mode (new)
	PassParameters->CellStart = InCellStartSRV;
	PassParameters->CellEnd = InCellEndSRV;
	// Use member variable directly - don't check nullptr because dummy buffers are created for RDG validation
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
	// Priority: GPU skinned buffer > CPU uploaded buffer
	const bool bUseGPUSkinning = IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();
	const bool bUseCPUBoundary = !bUseGPUSkinning && IsBoundaryAdhesionEnabled() && CachedBoundaryParticles.Num() > 0;

	if (bUseGPUSkinning)
	{
		// Use GPU-skinned world boundary buffer (populated by AddBoundarySkinningPass)
		FRDGBufferRef BoundaryBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_Density"));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		PassParameters->bUseBoundaryDensity = 1;
	}
	else if (bUseCPUBoundary)
	{
		// Use CPU-uploaded boundary particles (legacy path)
		const int32 BoundaryCount = CachedBoundaryParticles.Num();
		FRDGBufferRef BoundaryBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles_Density"),
			sizeof(FGPUBoundaryParticle),
			BoundaryCount,
			CachedBoundaryParticles.GetData(),
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
	const FGPUFluidSimulationParams& Params)
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
	// Priority: GPU skinned buffer > CPU uploaded buffer
	const bool bUseGPUSkinning = IsGPUBoundarySkinningEnabled() && PersistentWorldBoundaryBuffer.IsValid();
	const bool bUseCPUBoundary = !bUseGPUSkinning && IsBoundaryAdhesionEnabled() && CachedBoundaryParticles.Num() > 0;

	if (bUseGPUSkinning)
	{
		// Use GPU-skinned world boundary buffer (populated by AddBoundarySkinningPass)
		FRDGBufferRef BoundaryBuffer = GraphBuilder.RegisterExternalBuffer(PersistentWorldBoundaryBuffer, TEXT("GPUFluidBoundaryParticles_Viscosity"));
		PassParameters->BoundaryParticles = GraphBuilder.CreateSRV(BoundaryBuffer);
		PassParameters->BoundaryParticleCount = TotalLocalBoundaryParticleCount;
		PassParameters->bUseBoundaryViscosity = 1;
	}
	else if (bUseCPUBoundary)
	{
		// Use CPU-uploaded boundary particles (legacy path)
		const int32 BoundaryCount = CachedBoundaryParticles.Num();
		FRDGBufferRef BoundaryBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("GPUFluidBoundaryParticles_Viscosity"),
			sizeof(FGPUBoundaryParticle),
			BoundaryCount,
			CachedBoundaryParticles.GetData(),
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
// Stack Pressure Pass (for attached particles)
//=============================================================================

void FGPUFluidSimulator::AddStackPressurePass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef InParticlesUAV,
	FRDGBufferSRVRef InAttachmentSRV,
	FRDGBufferSRVRef InCellCountsSRV,
	FRDGBufferSRVRef InParticleIndicesSRV,
	const FGPUFluidSimulationParams& Params)
{
	// Skip if stack pressure is disabled or no attachments
	if (Params.StackPressureScale <= 0.0f || !InAttachmentSRV)
	{
		return;
	}

	// Skip if no bone colliders (no attachments possible)
	if (!bBoneTransformsValid || CachedBoneTransforms.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FStackPressureCS> ComputeShader(ShaderMap);

	// Create collision primitive buffers (same as Adhesion pass)
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(4, 1);

	FRDGBufferRef SpheresBuffer = CachedSpheres.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Spheres"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), sizeof(FGPUCollisionSphere) * CachedSpheres.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheres"));

	FRDGBufferRef CapsulesBuffer = CachedCapsules.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Capsules"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), sizeof(FGPUCollisionCapsule) * CachedCapsules.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsules"));

	FRDGBufferRef BoxesBuffer = CachedBoxes.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Boxes"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), sizeof(FGPUCollisionBox) * CachedBoxes.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxes"));

	FRDGBufferRef ConvexesBuffer = CachedConvexHeaders.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_Convexes"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), sizeof(FGPUCollisionConvex) * CachedConvexHeaders.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexes"));

	FRDGBufferRef ConvexPlanesBuffer = CachedConvexPlanes.Num() > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("StackPressure_ConvexPlanes"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), sizeof(FGPUConvexPlane) * CachedConvexPlanes.Num())
		: GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanes"));

	FStackPressureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStackPressureCS::FParameters>();
	PassParameters->Particles = InParticlesUAV;
	PassParameters->Attachments = InAttachmentSRV;
	PassParameters->CellCounts = InCellCountsSRV;
	PassParameters->ParticleIndices = InParticleIndicesSRV;

	// Collision primitives for surface normal calculation
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);

	// Parameters
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->StackPressureScale = Params.StackPressureScale;
	PassParameters->CellSize = Params.CellSize;
	PassParameters->Gravity = FVector3f(Params.Gravity);
	PassParameters->DeltaTime = Params.DeltaTime;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FStackPressureCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::StackPressure"),
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

//=============================================================================
// Adhesion Pass Implementations (GPU-based bone attachment)
//=============================================================================

void FGPUFluidSimulator::AddAdhesionPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef AttachmentUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (!IsAdhesionEnabled() || !bBoneTransformsValid || CachedBoneTransforms.Num() == 0 || CurrentParticleCount <= 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FAdhesionCS> ComputeShader(ShaderMap);

	// Upload bone transforms
	FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidBoneTransforms"),
		sizeof(FGPUBoneTransform),
		CachedBoneTransforms.Num(),
		CachedBoneTransforms.GetData(),
		CachedBoneTransforms.Num() * sizeof(FGPUBoneTransform),
		ERDGInitialDataFlags::NoCopy
	);
	FRDGBufferSRVRef BoneTransformsSRVLocal = GraphBuilder.CreateSRV(BoneTransformsBuffer);

	// Upload collision primitives for adhesion check
	FRDGBufferRef SpheresBuffer = nullptr;
	FRDGBufferRef CapsulesBuffer = nullptr;
	FRDGBufferRef BoxesBuffer = nullptr;
	FRDGBufferRef ConvexesBuffer = nullptr;
	FRDGBufferRef ConvexPlanesBuffer = nullptr;

	if (CachedSpheres.Num() > 0)
	{
		SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionSpheres"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), CachedSpheres.Num() * sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedCapsules.Num() > 0)
	{
		CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionCapsules"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), CachedCapsules.Num() * sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedBoxes.Num() > 0)
	{
		BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionBoxes"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), CachedBoxes.Num() * sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexHeaders.Num() > 0)
	{
		ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionConvexes"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), CachedConvexHeaders.Num() * sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexPlanes.Num() > 0)
	{
		ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidConvexPlanes"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), CachedConvexPlanes.Num() * sizeof(FGPUConvexPlane),
			ERDGInitialDataFlags::NoCopy
		);
	}

	// Dummy buffers for empty arrays
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(16, 1);
	if (!SpheresBuffer) SpheresBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheres"));
	if (!CapsulesBuffer) CapsulesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsules"));
	if (!BoxesBuffer) BoxesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxes"));
	if (!ConvexesBuffer) ConvexesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexes"));
	if (!ConvexPlanesBuffer) ConvexPlanesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanes"));

	FAdhesionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FAdhesionCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->ParticleRadius = Params.ParticleRadius;
	PassParameters->Attachments = AttachmentUAV;
	PassParameters->BoneTransforms = BoneTransformsSRVLocal;
	PassParameters->BoneCount = CachedBoneTransforms.Num();
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	PassParameters->AdhesionStrength = CachedAdhesionParams.AdhesionStrength;
	PassParameters->AdhesionRadius = CachedAdhesionParams.AdhesionRadius;
	PassParameters->DetachAccelThreshold = CachedAdhesionParams.DetachAccelThreshold;
	PassParameters->DetachDistanceThreshold = CachedAdhesionParams.DetachDistanceThreshold;
	PassParameters->ColliderContactOffset = CachedAdhesionParams.ColliderContactOffset;
	PassParameters->BoneVelocityScale = CachedAdhesionParams.BoneVelocityScale;
	PassParameters->SlidingFriction = CachedAdhesionParams.SlidingFriction;
	PassParameters->CurrentTime = Params.CurrentTime;
	PassParameters->DeltaTime = Params.DeltaTime;
	PassParameters->bEnableAdhesion = CachedAdhesionParams.bEnableAdhesion;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FAdhesionCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::Adhesion"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddUpdateAttachedPositionsPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferSRVRef AttachmentSRV,
	FRDGBufferSRVRef InBoneTransformsSRV,
	const FGPUFluidSimulationParams& Params)
{
	// This signature is for external calls. We'll use the internal version.
}

void FGPUFluidSimulator::AddUpdateAttachedPositionsPassInternal(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV,
	FRDGBufferUAVRef AttachmentUAV,
	const FGPUFluidSimulationParams& Params)
{
	if (!IsAdhesionEnabled() || !bBoneTransformsValid || CachedBoneTransforms.Num() == 0)
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FUpdateAttachedPositionsCS> ComputeShader(ShaderMap);

	// Upload bone transforms
	FRDGBufferRef BoneTransformsBuffer = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GPUFluidBoneTransformsUpdate"),
		sizeof(FGPUBoneTransform),
		CachedBoneTransforms.Num(),
		CachedBoneTransforms.GetData(),
		CachedBoneTransforms.Num() * sizeof(FGPUBoneTransform),
		ERDGInitialDataFlags::NoCopy
	);
	FRDGBufferSRVRef BoneTransformsSRVLocal = GraphBuilder.CreateSRV(BoneTransformsBuffer);

	// Upload collision primitives for detachment distance check
	FRDGBufferRef SpheresBuffer = nullptr;
	FRDGBufferRef CapsulesBuffer = nullptr;
	FRDGBufferRef BoxesBuffer = nullptr;
	FRDGBufferRef ConvexesBuffer = nullptr;
	FRDGBufferRef ConvexPlanesBuffer = nullptr;

	if (CachedSpheres.Num() > 0)
	{
		SpheresBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionSpheresUpdate"),
			sizeof(FGPUCollisionSphere), CachedSpheres.Num(),
			CachedSpheres.GetData(), CachedSpheres.Num() * sizeof(FGPUCollisionSphere),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedCapsules.Num() > 0)
	{
		CapsulesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionCapsulesUpdate"),
			sizeof(FGPUCollisionCapsule), CachedCapsules.Num(),
			CachedCapsules.GetData(), CachedCapsules.Num() * sizeof(FGPUCollisionCapsule),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedBoxes.Num() > 0)
	{
		BoxesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionBoxesUpdate"),
			sizeof(FGPUCollisionBox), CachedBoxes.Num(),
			CachedBoxes.GetData(), CachedBoxes.Num() * sizeof(FGPUCollisionBox),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexHeaders.Num() > 0)
	{
		ConvexesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidCollisionConvexesUpdate"),
			sizeof(FGPUCollisionConvex), CachedConvexHeaders.Num(),
			CachedConvexHeaders.GetData(), CachedConvexHeaders.Num() * sizeof(FGPUCollisionConvex),
			ERDGInitialDataFlags::NoCopy
		);
	}
	if (CachedConvexPlanes.Num() > 0)
	{
		ConvexPlanesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("GPUFluidConvexPlanesUpdate"),
			sizeof(FGPUConvexPlane), CachedConvexPlanes.Num(),
			CachedConvexPlanes.GetData(), CachedConvexPlanes.Num() * sizeof(FGPUConvexPlane),
			ERDGInitialDataFlags::NoCopy
		);
	}

	// Dummy buffers for empty arrays
	FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(16, 1);
	if (!SpheresBuffer) SpheresBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySpheresUpdate"));
	if (!CapsulesBuffer) CapsulesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCapsulesUpdate"));
	if (!BoxesBuffer) BoxesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoxesUpdate"));
	if (!ConvexesBuffer) ConvexesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyConvexesUpdate"));
	if (!ConvexPlanesBuffer) ConvexPlanesBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPlanesUpdate"));

	FUpdateAttachedPositionsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpdateAttachedPositionsCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;
	PassParameters->Attachments = AttachmentUAV;
	PassParameters->BoneTransforms = BoneTransformsSRVLocal;
	PassParameters->BoneCount = CachedBoneTransforms.Num();
	PassParameters->CollisionSpheres = GraphBuilder.CreateSRV(SpheresBuffer);
	PassParameters->SphereCount = CachedSpheres.Num();
	PassParameters->CollisionCapsules = GraphBuilder.CreateSRV(CapsulesBuffer);
	PassParameters->CapsuleCount = CachedCapsules.Num();
	PassParameters->CollisionBoxes = GraphBuilder.CreateSRV(BoxesBuffer);
	PassParameters->BoxCount = CachedBoxes.Num();
	PassParameters->CollisionConvexes = GraphBuilder.CreateSRV(ConvexesBuffer);
	PassParameters->ConvexCount = CachedConvexHeaders.Num();
	PassParameters->ConvexPlanes = GraphBuilder.CreateSRV(ConvexPlanesBuffer);
	PassParameters->DetachAccelThreshold = CachedAdhesionParams.DetachAccelThreshold;
	PassParameters->DetachDistanceThreshold = CachedAdhesionParams.DetachDistanceThreshold;
	PassParameters->ColliderContactOffset = CachedAdhesionParams.ColliderContactOffset;
	PassParameters->BoneVelocityScale = CachedAdhesionParams.BoneVelocityScale;
	PassParameters->SlidingFriction = CachedAdhesionParams.SlidingFriction;
	PassParameters->DeltaTime = Params.DeltaTime;

	// Gravity sliding parameters
	PassParameters->Gravity = CachedAdhesionParams.Gravity;
	PassParameters->GravitySlidingScale = CachedAdhesionParams.GravitySlidingScale;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FUpdateAttachedPositionsCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::UpdateAttachedPositions"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}

void FGPUFluidSimulator::AddClearDetachedFlagPass(
	FRDGBuilder& GraphBuilder,
	FRDGBufferUAVRef ParticlesUAV)
{
	if (!IsAdhesionEnabled())
	{
		return;
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FClearDetachedFlagCS> ComputeShader(ShaderMap);

	FClearDetachedFlagCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearDetachedFlagCS::FParameters>();
	PassParameters->Particles = ParticlesUAV;
	PassParameters->ParticleCount = CurrentParticleCount;

	const uint32 NumGroups = FMath::DivideAndRoundUp(CurrentParticleCount, FClearDetachedFlagCS::ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("GPUFluid::ClearDetachedFlag"),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1)
	);
}
