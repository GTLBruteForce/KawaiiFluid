// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "GPU/FluidAnisotropyComputeShader.h"
#include "GPU/GPUFluidParticle.h"  // Contains FGPUBoundaryParticle, FGPUCollisionSphere, FGPUCollisionCapsule, FGPUCollisionBox
#include "GPU/GPUBoundaryAttachment.h"  // Contains FGPUBoneDeltaAttachment
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "GlobalShader.h"

//=============================================================================
// Shader Implementation
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FFluidAnisotropyCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidAnisotropyCompute.usf",
	"MainCS", SF_Compute);

//=============================================================================
// Pass Builder Implementation
//=============================================================================

/**
 * @brief Add anisotropy calculation pass to RDG.
 * @param GraphBuilder RDG builder.
 * @param Params Anisotropy compute parameters (buffers and settings).
 */
void FFluidAnisotropyPassBuilder::AddAnisotropyPass(
	FRDGBuilder& GraphBuilder,
	const FAnisotropyComputeParams& Params)
{
	if (Params.ParticleCount <= 0 || !Params.PositionsSRV || !Params.PackedVelocitiesSRV || !Params.FlagsSRV)
	{
		return;
	}

	if (!Params.OutAxis1UAV || !Params.OutAxis2UAV || !Params.OutAxis3UAV)
	{
		return;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// Create permutation vector based on grid resolution preset
	// CRITICAL: Hybrid Tiled Z-Order uses fixed 21-bit keys, so use Medium preset (2^21 cells)
	const EGridResolutionPreset EffectivePreset = Params.bUseHybridTiledZOrder ? EGridResolutionPreset::Medium : Params.GridResolutionPreset;
	FFluidAnisotropyCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FGridResolutionDim>(GridResolutionPermutation::FromPreset(EffectivePreset));
	TShaderMapRef<FFluidAnisotropyCS> ComputeShader(GlobalShaderMap, PermutationVector);

	FFluidAnisotropyCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFluidAnisotropyCS::FParameters>();

	// Create dummy buffers for optional inputs that are null
	// RDG requires all shader parameters to have valid buffers
	// Must use QueueBufferUpload to mark buffer as "produced"
	FRDGBufferSRVRef AttachmentsSRV = Params.AttachmentsSRV;
	// TODO(KHJ): Remove legacy CellCounts/ParticleIndices - bUseZOrderSorting is always true
	FRDGBufferSRVRef CellCountsSRV = Params.CellCountsSRV;
	FRDGBufferSRVRef ParticleIndicesSRV = Params.ParticleIndicesSRV;

	// Morton-sorted buffers (ParticleBuffer is already sorted when bUseZOrderSorting=true)
	FRDGBufferSRVRef CellStartSRV = Params.CellStartSRV;
	FRDGBufferSRVRef CellEndSRV = Params.CellEndSRV;

	if (!AttachmentsSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(
			sizeof(FGPUParticleAttachment), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyAttachmentBuffer"));
		FGPUParticleAttachment ZeroData;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(FGPUParticleAttachment));
		AttachmentsSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// TODO: Remove legacy dummy buffer creation - bUseZOrderSorting is always true
	if (!CellCountsSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCellCountBuffer"));
		uint32 ZeroData = 0;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(uint32));
		CellCountsSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	if (!ParticleIndicesSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyParticleIndicesBuffer"));
		uint32 ZeroData = 0;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(uint32));
		ParticleIndicesSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// Create dummy buffers for Morton-sorted inputs if not provided
	if (!CellStartSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCellStartBuffer"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &InvalidIndex, sizeof(uint32));
		CellStartSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	if (!CellEndSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCellEndBuffer"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &InvalidIndex, sizeof(uint32));
		CellEndSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// =========================================================================
	// Boundary Particles for anisotropy calculation
	// =========================================================================
	FRDGBufferSRVRef BoundaryParticlesSRV = Params.BoundaryParticlesSRV;
	FRDGBufferSRVRef SortedBoundaryParticlesSRV = Params.SortedBoundaryParticlesSRV;
	FRDGBufferSRVRef BoundaryCellStartSRV = Params.BoundaryCellStartSRV;
	FRDGBufferSRVRef BoundaryCellEndSRV = Params.BoundaryCellEndSRV;

	// Create dummy buffer for legacy BoundaryParticles if not provided
	if (!BoundaryParticlesSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoundaryParticles_Anisotropy"));
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));
		BoundaryParticlesSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// Create dummy buffers for Z-Order sorted Boundary if not provided
	if (!SortedBoundaryParticlesSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoundaryParticle), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummySortedBoundaryParticles_Anisotropy"));
		FGPUBoundaryParticle ZeroBoundary = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroBoundary, sizeof(FGPUBoundaryParticle));
		SortedBoundaryParticlesSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	if (!BoundaryCellStartSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoundaryCellStart_Anisotropy"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &InvalidIndex, sizeof(uint32));
		BoundaryCellStartSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	if (!BoundaryCellEndSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoundaryCellEnd_Anisotropy"));
		uint32 InvalidIndex = 0xFFFFFFFF;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &InvalidIndex, sizeof(uint32));
		BoundaryCellEndSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// =========================================================================
	// Temporal Smoothing buffers
	// =========================================================================
	FRDGBufferSRVRef PrevAxis1SRV = Params.PrevAxis1SRV;
	FRDGBufferSRVRef PrevAxis2SRV = Params.PrevAxis2SRV;
	FRDGBufferSRVRef PrevAxis3SRV = Params.PrevAxis3SRV;

	if (!PrevAxis1SRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPrevAnisotropyAxis1"));
		FVector4f DefaultAxis = FVector4f(1, 0, 0, 1);  // Default identity axis
		GraphBuilder.QueueBufferUpload(DummyBuffer, &DefaultAxis, sizeof(FVector4f));
		PrevAxis1SRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	if (!PrevAxis2SRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPrevAnisotropyAxis2"));
		FVector4f DefaultAxis = FVector4f(0, 1, 0, 1);  // Default identity axis
		GraphBuilder.QueueBufferUpload(DummyBuffer, &DefaultAxis, sizeof(FVector4f));
		PrevAxis2SRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	if (!PrevAxis3SRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyPrevAnisotropyAxis3"));
		FVector4f DefaultAxis = FVector4f(0, 0, 1, 1);  // Default identity axis
		GraphBuilder.QueueBufferUpload(DummyBuffer, &DefaultAxis, sizeof(FVector4f));
		PrevAxis3SRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// Input buffers (SoA)
	PassParameters->InPositions = Params.PositionsSRV;
	PassParameters->InPackedVelocities = Params.PackedVelocitiesSRV;  // B plan
	PassParameters->InFlags = Params.FlagsSRV;
	PassParameters->InAttachments = AttachmentsSRV;

	// TODO: Remove legacy hash-based binding - bUseZOrderSorting is always true
	// Legacy hash-based spatial lookup
	PassParameters->CellCounts = CellCountsSRV;
	PassParameters->ParticleIndices = ParticleIndicesSRV;

	// Morton-sorted spatial lookup
	PassParameters->CellStart = CellStartSRV;
	PassParameters->CellEnd = CellEndSRV;

	// Output buffers
	PassParameters->OutAnisotropyAxis1 = Params.OutAxis1UAV;
	PassParameters->OutAnisotropyAxis2 = Params.OutAxis2UAV;
	PassParameters->OutAnisotropyAxis3 = Params.OutAxis3UAV;

	// Render offset for surface particles
	FRDGBufferUAVRef OutRenderOffsetUAV = Params.OutRenderOffsetUAV;
	if (!OutRenderOffsetUAV)
	{
		// Create dummy buffer (written by shader but unused)
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), FMath::Max(1, Params.ParticleCount));
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyRenderOffset"));
		OutRenderOffsetUAV = GraphBuilder.CreateUAV(DummyBuffer);
	}
	PassParameters->OutRenderOffset = OutRenderOffsetUAV;
	PassParameters->ParticleRadius = Params.ParticleRadius;

	// Parameters
	PassParameters->ParticleCount = static_cast<uint32>(Params.ParticleCount);
	PassParameters->AnisotropyMode = static_cast<uint32>(Params.Mode);
	PassParameters->VelocityStretchFactor = Params.VelocityStretchFactor;
	PassParameters->AnisotropyScale = Params.Strength;
	PassParameters->AnisotropyMin = Params.MinStretch;
	PassParameters->AnisotropyMax = Params.MaxStretch;
	PassParameters->DensityWeight = Params.DensityWeight;
	PassParameters->SmoothingRadius = Params.SmoothingRadius;
	PassParameters->CellSize = Params.CellSize;

	// TODO(KHJ): Remove bUseZOrderSorting - always 1, legacy path is dead code
	// Morton code params
	PassParameters->bUseZOrderSorting = Params.bUseZOrderSorting ? 1 : 0;
	PassParameters->MortonBoundsMin = Params.MortonBoundsMin;
	PassParameters->bUseHybridTiledZOrder = Params.bUseHybridTiledZOrder ? 1 : 0;

	// Attached particle anisotropy params
	PassParameters->AttachedFlattenScale = Params.AttachedFlattenScale;
	PassParameters->AttachedStretchScale = Params.AttachedStretchScale;

	// Boundary Particles for anisotropy calculation
	PassParameters->BoundaryParticles = BoundaryParticlesSRV;
	PassParameters->BoundaryParticleCount = Params.BoundaryParticleCount;
	PassParameters->bUseBoundaryAnisotropy = Params.bUseBoundaryAnisotropy ? 1 : 0;

	// Boundary Z-Order sorted buffers
	PassParameters->SortedBoundaryParticles = SortedBoundaryParticlesSRV;
	PassParameters->BoundaryCellStart = BoundaryCellStartSRV;
	PassParameters->BoundaryCellEnd = BoundaryCellEndSRV;
	PassParameters->bUseBoundaryZOrder = Params.bUseBoundaryZOrder ? 1 : 0;
	PassParameters->BoundaryWeight = Params.BoundaryWeight;

	// Temporal Smoothing
	PassParameters->PrevAnisotropyAxis1 = PrevAxis1SRV;
	PassParameters->PrevAnisotropyAxis2 = PrevAxis2SRV;
	PassParameters->PrevAnisotropyAxis3 = PrevAxis3SRV;
	PassParameters->bEnableTemporalSmoothing = Params.bEnableTemporalSmoothing ? 1 : 0;
	PassParameters->TemporalSmoothFactor = Params.TemporalSmoothFactor;
	PassParameters->bHasPreviousFrame = Params.bHasPreviousFrame ? 1 : 0;

	// Surface Normal Anisotropy for NEAR_BOUNDARY particles
	FRDGBufferSRVRef BoneDeltaAttachmentsSRV = Params.BoneDeltaAttachmentsSRV;

	// Create dummy buffer for BoneDeltaAttachments if not provided
	if (!BoneDeltaAttachmentsSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBoneDeltaAttachment), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyBoneDeltaAttachments_Anisotropy"));
		FGPUBoneDeltaAttachment ZeroData;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(FGPUBoneDeltaAttachment));
		BoneDeltaAttachmentsSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	PassParameters->InBoneDeltaAttachments = BoneDeltaAttachmentsSRV;
	PassParameters->bEnableSurfaceNormalAnisotropy = Params.bEnableSurfaceNormalAnisotropy ? 1 : 0;

	// =========================================================================
	// Collision Primitives for direct surface normal calculation
	// NOTE: Colliders are ALREADY in world space (transformed by C++ before upload)
	// =========================================================================
	FRDGBufferSRVRef CollisionSpheresSRV = Params.CollisionSpheresSRV;
	FRDGBufferSRVRef CollisionCapsulesSRV = Params.CollisionCapsulesSRV;
	FRDGBufferSRVRef CollisionBoxesSRV = Params.CollisionBoxesSRV;

	// Create dummy buffer for CollisionSpheres if not provided
	if (!CollisionSpheresSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUCollisionSphere), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCollisionSpheres_Anisotropy"));
		FGPUCollisionSphere ZeroData = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(FGPUCollisionSphere));
		CollisionSpheresSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// Create dummy buffer for CollisionCapsules if not provided
	if (!CollisionCapsulesSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUCollisionCapsule), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCollisionCapsules_Anisotropy"));
		FGPUCollisionCapsule ZeroData = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(FGPUCollisionCapsule));
		CollisionCapsulesSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// Create dummy buffer for CollisionBoxes if not provided
	if (!CollisionBoxesSRV)
	{
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUCollisionBox), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("DummyCollisionBoxes_Anisotropy"));
		FGPUCollisionBox ZeroData = {};
		GraphBuilder.QueueBufferUpload(DummyBuffer, &ZeroData, sizeof(FGPUCollisionBox));
		CollisionBoxesSRV = GraphBuilder.CreateSRV(DummyBuffer);
	}

	PassParameters->CollisionSpheres = CollisionSpheresSRV;
	PassParameters->CollisionCapsules = CollisionCapsulesSRV;
	PassParameters->CollisionBoxes = CollisionBoxesSRV;
	PassParameters->SphereCount = Params.SphereCount;
	PassParameters->CapsuleCount = Params.CapsuleCount;
	PassParameters->BoxCount = Params.BoxCount;
	PassParameters->ColliderSearchRadius = Params.ColliderSearchRadius;

	// Volume Preservation mode (Yu & Turk vs FleX style)
	PassParameters->bPreserveVolume = Params.bPreserveVolume ? 1 : 0;
	PassParameters->FlexRenderScale = Params.FlexRenderScale;

	const int32 ThreadGroupSize = FFluidAnisotropyCS::ThreadGroupSize;
	const int32 NumGroups = FMath::DivideAndRoundUp(Params.ParticleCount, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FluidAnisotropy(%d,Preset=%d,mode=%d,Hybrid=%d)",
			Params.ParticleCount, static_cast<int32>(EffectivePreset), static_cast<int32>(Params.Mode), Params.bUseHybridTiledZOrder ? 1 : 0),
		ComputeShader,
		PassParameters,
		FIntVector(NumGroups, 1, 1));
}

/**
 * @brief Create anisotropy output buffers.
 * @param GraphBuilder RDG builder.
 * @param ParticleCount Number of particles.
 * @param OutAxis1 Output axis 1 buffer (direction.xyz + scale.w).
 * @param OutAxis2 Output axis 2 buffer.
 * @param OutAxis3 Output axis 3 buffer.
 */
void FFluidAnisotropyPassBuilder::CreateAnisotropyBuffers(
	FRDGBuilder& GraphBuilder,
	int32 ParticleCount,
	FRDGBufferRef& OutAxis1,
	FRDGBufferRef& OutAxis2,
	FRDGBufferRef& OutAxis3)
{
	if (ParticleCount <= 0)
	{
		OutAxis1 = nullptr;
		OutAxis2 = nullptr;
		OutAxis3 = nullptr;
		return;
	}

	// Each axis is float4 (direction.xyz + scale.w)
	FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(
		sizeof(FVector4f), ParticleCount);

	OutAxis1 = GraphBuilder.CreateBuffer(BufferDesc, TEXT("FluidAnisotropyAxis1"));
	OutAxis2 = GraphBuilder.CreateBuffer(BufferDesc, TEXT("FluidAnisotropyAxis2"));
	OutAxis3 = GraphBuilder.CreateBuffer(BufferDesc, TEXT("FluidAnisotropyAxis3"));
}
