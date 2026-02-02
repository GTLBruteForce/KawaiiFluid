// Copyright 2026 Team_Bruteforce. All Rights Reserved.
// GPU Fluid Spatial Data Structures

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

/**
 * FSimulationSpatialData
 * Contains all spatial hashing and SOA particle buffers for GPU fluid simulation
 */
struct FSimulationSpatialData
{
	// Hash Table buffers (Legacy / Compatibility)
	FRDGBufferRef CellCountsBuffer = nullptr;
	FRDGBufferRef ParticleIndicesBuffer = nullptr;
	FRDGBufferSRVRef CellCountsSRV = nullptr;
	FRDGBufferSRVRef ParticleIndicesSRV = nullptr;

	// Z-Order buffers (Sorted)
	FRDGBufferRef CellStartBuffer = nullptr;
	FRDGBufferRef CellEndBuffer = nullptr;
	FRDGBufferSRVRef CellStartSRV = nullptr;
	FRDGBufferSRVRef CellEndSRV = nullptr;


	// Neighbor Cache buffers
	FRDGBufferRef NeighborListBuffer = nullptr;
	FRDGBufferRef NeighborCountsBuffer = nullptr;
	FRDGBufferSRVRef NeighborListSRV = nullptr;
	FRDGBufferSRVRef NeighborCountsSRV = nullptr;

	// Skinned Boundary Particle buffers (SkeletalMesh - same-frame)
	// Created in AddBoundarySkinningPass, used in AddSolveDensityPressurePass
	FRDGBufferRef SkinnedBoundaryBuffer = nullptr;
	FRDGBufferSRVRef SkinnedBoundarySRV = nullptr;
	int32 SkinnedBoundaryParticleCount = 0;
	bool bSkinnedBoundaryPerformed = false;

	// Skinned Boundary Z-Order buffers (same-frame)
	FRDGBufferRef SkinnedZOrderSortedBuffer = nullptr;
	FRDGBufferRef SkinnedZOrderCellStartBuffer = nullptr;
	FRDGBufferRef SkinnedZOrderCellEndBuffer = nullptr;
	FRDGBufferSRVRef SkinnedZOrderSortedSRV = nullptr;
	FRDGBufferSRVRef SkinnedZOrderCellStartSRV = nullptr;
	FRDGBufferSRVRef SkinnedZOrderCellEndSRV = nullptr;
	int32 SkinnedZOrderParticleCount = 0;
	bool bSkinnedZOrderPerformed = false;

	// Static Boundary Particle buffers (StaticMesh - persistent GPU)
	// Cached on GPU, only re-sorted when dirty
	FRDGBufferSRVRef StaticBoundarySRV = nullptr;
	FRDGBufferSRVRef StaticZOrderSortedSRV = nullptr;
	FRDGBufferSRVRef StaticZOrderCellStartSRV = nullptr;
	FRDGBufferSRVRef StaticZOrderCellEndSRV = nullptr;
	int32 StaticBoundaryParticleCount = 0;
	bool bStaticBoundaryAvailable = false;

	// Bone Delta Attachment buffer (NEW simplified bone-following)
	// Created by EnsureBoneDeltaAttachmentBuffer, used by ApplyBoneTransform & UpdateBoneDeltaAttachment
	FRDGBufferRef BoneDeltaAttachmentBuffer = nullptr;
	FRDGBufferUAVRef BoneDeltaAttachmentUAV = nullptr;
	FRDGBufferSRVRef BoneDeltaAttachmentSRV = nullptr;

	// SoA (Structure of Arrays) Particle Buffers (Memory Bandwidth Optimization)
	// Split after BuildSpatialStructures, Merge after PostSimulation
	//
	// Bandwidth optimization (B plan):
	// - Position: float3 (full precision, critical for stability)
	// - Velocity: half3 packed as uint2 (50% bandwidth reduction)
	// - Density+Lambda: half2 packed as uint (50% bandwidth reduction)
	// - Mass: removed (uniform constant from Preset)

	FRDGBufferRef SoA_Positions = nullptr;           // float3 as 3 floats
	FRDGBufferRef SoA_PredictedPositions = nullptr;  // float3 as 3 floats
	FRDGBufferRef SoA_PackedVelocities = nullptr;    // uint2 = half4 (vel.xy, vel.z, padding)
	FRDGBufferRef SoA_PackedDensityLambda = nullptr; // uint = half2 (density, lambda)
	FRDGBufferRef SoA_Flags = nullptr;               // uint
	FRDGBufferRef SoA_NeighborCounts = nullptr;      // uint
	FRDGBufferRef SoA_ParticleIDs = nullptr;         // int
	FRDGBufferRef SoA_SourceIDs = nullptr;           // int

	// Legacy aliases (for backward compatibility during transition)
	FRDGBufferRef& WorldBoundaryBuffer = SkinnedBoundaryBuffer;
	FRDGBufferSRVRef& WorldBoundarySRV = SkinnedBoundarySRV;
	int32& WorldBoundaryParticleCount = SkinnedBoundaryParticleCount;
	bool& bBoundarySkinningPerformed = bSkinnedBoundaryPerformed;

	FRDGBufferRef& BoundaryZOrderSortedBuffer = SkinnedZOrderSortedBuffer;
	FRDGBufferRef& BoundaryZOrderCellStartBuffer = SkinnedZOrderCellStartBuffer;
	FRDGBufferRef& BoundaryZOrderCellEndBuffer = SkinnedZOrderCellEndBuffer;
	FRDGBufferSRVRef& BoundaryZOrderSortedSRV = SkinnedZOrderSortedSRV;
	FRDGBufferSRVRef& BoundaryZOrderCellStartSRV = SkinnedZOrderCellStartSRV;
	FRDGBufferSRVRef& BoundaryZOrderCellEndSRV = SkinnedZOrderCellEndSRV;
	int32& BoundaryZOrderParticleCount = SkinnedZOrderParticleCount;
	bool& bBoundaryZOrderPerformed = bSkinnedZOrderPerformed;
};
