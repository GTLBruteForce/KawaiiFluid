// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/RayMarching/FluidVolumeBuilder.h"
#include "Rendering/RayMarching/FluidRayMarchingShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"

//=============================================================================
// Shader Implementations
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FBuildDensityVolumeCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidBuildDensityVolume.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildSDFVolumeCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidBuildSDFVolume.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildOccupancyMaskCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidBuildOccupancyMask.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildMinMaxMipLevel0CS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidBuildMinMaxMipmap.usf", "BuildLevel0CS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildMinMaxMipChainCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidBuildMinMaxMipmap.usf", "BuildMipChainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FComputeFluidAABBCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidComputeFluidAABB.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FInitFluidAABBCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidComputeFluidAABB.usf", "InitCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClearVolumeCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidClearVolume.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMarkVoxelOccupancyCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidMarkVoxelOccupancy.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBuildSDFVolumeSparseCS, "/Plugin/KawaiiFluidSystem/Private/RayMarching/FluidBuildSDFVolumeSparse.usf", "MainCS", SF_Compute);

//=============================================================================
// FluidVolumeBuilder Implementation
//=============================================================================

FFluidVolumeBuilder::FFluidVolumeBuilder()
{
}

FFluidVolumeBuilder::~FFluidVolumeBuilder()
{
}

FFluidVolumeTextures FFluidVolumeBuilder::BuildVolumes(
	FRDGBuilder& GraphBuilder,
	const FFluidVolumeInput& Input,
	const FFluidVolumeConfig& Config)
{
	// Cache shader map
	GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// NOTE: Tight AABB 기능 비활성화 - Simulation Volume만 사용
	// Tight AABB는 RDG 버퍼 수명 문제로 크래시 발생하여 제거됨

	FFluidVolumeTextures Result;
	Result.VolumeResolution = Config.VolumeResolution;
	Result.VolumeBoundsMin = Input.BoundsMin;
	Result.VolumeBoundsMax = Input.BoundsMax;

	// Build volume based on mode (SDF or Density)
	if (Config.bBuildSDF)
	{
		// SDF mode: Build SDF volume for Sphere Tracing
		if (Config.bUseSparseVoxel)
		{
			Result.SDFVolume = BuildSDFVolumeSparse(GraphBuilder, Input, Config);
		}
		else
		{
			// Always use Simulation Volume bounds (no Tight AABB)
			Result.SDFVolume = BuildSDFVolume(GraphBuilder, Input, Config, nullptr);
		}
	}
	else
	{
		// Legacy Density mode
		Result.DensityVolume = BuildDensityVolume(GraphBuilder, Input, Config);

		// Build occupancy bitmask (optional, for density mode only)
		if (Config.bBuildOccupancyMask)
		{
			Result.OccupancyMask = BuildOccupancyMask(GraphBuilder, Result.DensityVolume, Config);
		}

		// Build MinMax mipmap chain (optional, for density mode only)
		if (Config.bBuildMinMaxMipmap)
		{
			Result.MinMaxMipmap = BuildMinMaxMipmap(GraphBuilder, Result.DensityVolume, Config);
		}
	}

	CachedVolumes = Result;
	return Result;
}

FRDGTextureRef FFluidVolumeBuilder::BuildDensityVolume(
	FRDGBuilder& GraphBuilder,
	const FFluidVolumeInput& Input,
	const FFluidVolumeConfig& Config)
{
	RDG_EVENT_SCOPE(GraphBuilder, "BuildDensityVolume");

	const int32 Resolution = Config.VolumeResolution;

	// Create density volume texture
	FRDGTextureDesc DensityDesc = FRDGTextureDesc::Create3D(
		FIntVector(Resolution, Resolution, Resolution),
		PF_R32_FLOAT,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef DensityVolume = GraphBuilder.CreateTexture(DensityDesc, TEXT("FluidDensityVolume"));

	// Clear volume first
	FRDGTextureUAVRef DensityUAV = GraphBuilder.CreateUAV(DensityVolume);
	ClearVolume(GraphBuilder, DensityUAV, Resolution, 0.0f);

	// Build density from particles
	TShaderMapRef<FBuildDensityVolumeCS> ComputeShader(GlobalShaderMap);

	FBuildDensityVolumeCS::FParameters* PassParams = GraphBuilder.AllocParameters<FBuildDensityVolumeCS::FParameters>();
	PassParams->Particles = GraphBuilder.CreateSRV(Input.SortedParticles);
	PassParams->CellStart = GraphBuilder.CreateSRV(Input.CellStart);
	PassParams->CellEnd = GraphBuilder.CreateSRV(Input.CellEnd);
	PassParams->DensityVolume = DensityUAV;
	PassParams->VolumeResolution = Resolution;
	PassParams->CellSize = Input.CellSize;
	PassParams->SmoothingRadius = Input.SmoothingRadius;
	PassParams->Poly6Coeff = Input.Poly6Coeff;
	PassParams->VolumeBoundsMin = Input.BoundsMin;
	PassParams->VolumeBoundsMax = Input.BoundsMax;

	const int32 GroupSize = FBuildDensityVolumeCS::ThreadGroupSize;
	const FIntVector GroupCount(
		FMath::DivideAndRoundUp(Resolution, GroupSize),
		FMath::DivideAndRoundUp(Resolution, GroupSize),
		FMath::DivideAndRoundUp(Resolution, GroupSize));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("BuildDensityVolume %dx%dx%d", Resolution, Resolution, Resolution),
		ComputeShader,
		PassParams,
		GroupCount);

	return DensityVolume;
}

FRDGTextureRef FFluidVolumeBuilder::BuildSDFVolume(
	FRDGBuilder& GraphBuilder,
	const FFluidVolumeInput& Input,
	const FFluidVolumeConfig& Config,
	FRDGBufferRef AABBBuffer)
{
	RDG_EVENT_SCOPE(GraphBuilder, "BuildSDFVolume");

	const int32 Resolution = Config.VolumeResolution;
	const bool bUseTightAABB = Config.bUseTightAABB && AABBBuffer != nullptr;

	// Create SDF volume texture (R32_FLOAT for signed distance values)
	FRDGTextureDesc SDFDesc = FRDGTextureDesc::Create3D(
		FIntVector(Resolution, Resolution, Resolution),
		PF_R32_FLOAT,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef SDFVolume = GraphBuilder.CreateTexture(SDFDesc, TEXT("FluidSDFVolume"));

	// Clear volume to large positive value (far outside fluid)
	FRDGTextureUAVRef SDFUAV = GraphBuilder.CreateUAV(SDFVolume);
	ClearVolume(GraphBuilder, SDFUAV, Resolution, 1e10f);

	// Build SDF from particles using Z-Order neighbor search
	TShaderMapRef<FBuildSDFVolumeCS> ComputeShader(GlobalShaderMap);

	FBuildSDFVolumeCS::FParameters* PassParams = GraphBuilder.AllocParameters<FBuildSDFVolumeCS::FParameters>();
	PassParams->Particles = GraphBuilder.CreateSRV(Input.SortedParticles);
	PassParams->CellStart = GraphBuilder.CreateSRV(Input.CellStart);
	PassParams->CellEnd = GraphBuilder.CreateSRV(Input.CellEnd);
	PassParams->SDFVolume = SDFUAV;
	PassParams->VolumeResolution = Resolution;
	PassParams->ParticleRadius = Input.ParticleRadius;
	PassParams->SmoothK = Config.SmoothK;
	PassParams->SurfaceOffset = Config.SurfaceOffset;
	PassParams->VolumeBoundsMin = Input.BoundsMin;
	PassParams->VolumeBoundsMax = Input.BoundsMax;
	PassParams->CellSize = Input.CellSize;
	PassParams->MortonBoundsMin = Input.MortonBoundsMin;

	// Tight AABB (GPU-only)
	PassParams->SimulationBoundsMin = Input.BoundsMin;
	PassParams->SimulationBoundsMax = Input.BoundsMax;
	PassParams->bUseTightAABB = bUseTightAABB ? 1 : 0;

	if (bUseTightAABB)
	{
		PassParams->FluidAABB = GraphBuilder.CreateSRV(AABBBuffer, PF_R32_UINT);
	}
	else
	{
		// Dummy buffer (for shader validation)
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 6),
			TEXT("DummyAABB"));
		PassParams->FluidAABB = GraphBuilder.CreateSRV(DummyBuffer, PF_R32_UINT);
	}

	const int32 GroupSize = FBuildSDFVolumeCS::ThreadGroupSize;
	const FIntVector GroupCount(
		FMath::DivideAndRoundUp(Resolution, GroupSize),
		FMath::DivideAndRoundUp(Resolution, GroupSize),
		FMath::DivideAndRoundUp(Resolution, GroupSize));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("BuildSDFVolume %dx%dx%d TightAABB=%d", Resolution, Resolution, Resolution, bUseTightAABB ? 1 : 0),
		ComputeShader,
		PassParams,
		GroupCount);

	return SDFVolume;
}

FRDGTextureRef FFluidVolumeBuilder::BuildSDFVolumeSparse(
	FRDGBuilder& GraphBuilder,
	const FFluidVolumeInput& Input,
	const FFluidVolumeConfig& Config)
{
	RDG_EVENT_SCOPE(GraphBuilder, "BuildSDFVolumeSparse");

	const int32 Resolution = Config.VolumeResolution;
	const int32 TotalVoxels = Resolution * Resolution * Resolution;
	const int32 MaskUintCount = (TotalVoxels + 31) / 32;  // Round up to next uint32

	//========================================
	// Pass 1: Mark Voxel Occupancy
	//========================================
	{
		RDG_EVENT_SCOPE(GraphBuilder, "MarkVoxelOccupancy");

		// Create active voxel mask buffer
		FRDGBufferDesc MaskDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaskUintCount);
		FRDGBufferRef ActiveVoxelMask = GraphBuilder.CreateBuffer(MaskDesc, TEXT("SparseActiveVoxelMask"));

		// Clear mask to zero
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ActiveVoxelMask, PF_R32_UINT), 0u);

		// Mark voxels within particle influence
		TShaderMapRef<FMarkVoxelOccupancyCS> MarkShader(GlobalShaderMap);

		FMarkVoxelOccupancyCS::FParameters* MarkParams = GraphBuilder.AllocParameters<FMarkVoxelOccupancyCS::FParameters>();
		MarkParams->Particles = GraphBuilder.CreateSRV(Input.SortedParticles);
		MarkParams->ActiveVoxelMask = GraphBuilder.CreateUAV(ActiveVoxelMask, PF_R32_UINT);
		MarkParams->ParticleCount = Input.ParticleCount;
		MarkParams->VolumeResolution = Resolution;
		MarkParams->SearchRadius = Input.ParticleRadius * 3.0f;  // Influence extends beyond particle
		MarkParams->VolumeBoundsMin = Input.BoundsMin;
		MarkParams->VolumeBoundsMax = Input.BoundsMax;

		const int32 MarkGroupSize = FMarkVoxelOccupancyCS::ThreadGroupSize;
		const int32 MarkGroupCount = FMath::DivideAndRoundUp(Input.ParticleCount, MarkGroupSize);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("MarkOccupancy Particles=%d", Input.ParticleCount),
			MarkShader,
			MarkParams,
			FIntVector(MarkGroupCount, 1, 1));

		//========================================
		// Pass 2: Build SDF for Active Voxels Only
		//========================================
		{
			RDG_EVENT_SCOPE(GraphBuilder, "BuildSDFSparse");

			// Create SDF volume texture
			FRDGTextureDesc SDFDesc = FRDGTextureDesc::Create3D(
				FIntVector(Resolution, Resolution, Resolution),
				PF_R32_FLOAT,
				FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV);

			FRDGTextureRef SDFVolume = GraphBuilder.CreateTexture(SDFDesc, TEXT("FluidSDFVolumeSparse"));

			// Note: We don't need to clear the volume - inactive voxels will be set to 1e10f in shader
			// Active voxels will be computed properly

			FRDGTextureUAVRef SDFUAV = GraphBuilder.CreateUAV(SDFVolume);

			// Build SDF from particles (sparse version)
			TShaderMapRef<FBuildSDFVolumeSparseCS> SparseShader(GlobalShaderMap);

			FBuildSDFVolumeSparseCS::FParameters* SparseParams = GraphBuilder.AllocParameters<FBuildSDFVolumeSparseCS::FParameters>();
			SparseParams->Particles = GraphBuilder.CreateSRV(Input.SortedParticles);
			SparseParams->CellStart = GraphBuilder.CreateSRV(Input.CellStart);
			SparseParams->CellEnd = GraphBuilder.CreateSRV(Input.CellEnd);
			SparseParams->ActiveVoxelMask = GraphBuilder.CreateSRV(ActiveVoxelMask, PF_R32_UINT);
			SparseParams->SDFVolume = SDFUAV;
			SparseParams->VolumeResolution = Resolution;
			SparseParams->ParticleRadius = Input.ParticleRadius;
			SparseParams->SmoothK = Config.SmoothK;
			SparseParams->SurfaceOffset = Config.SurfaceOffset;
			SparseParams->VolumeBoundsMin = Input.BoundsMin;
			SparseParams->VolumeBoundsMax = Input.BoundsMax;
			SparseParams->CellSize = Input.CellSize;
			SparseParams->MortonBoundsMin = Input.MortonBoundsMin;

			const int32 GroupSize = FBuildSDFVolumeSparseCS::ThreadGroupSize;
			const FIntVector GroupCount(
				FMath::DivideAndRoundUp(Resolution, GroupSize),
				FMath::DivideAndRoundUp(Resolution, GroupSize),
				FMath::DivideAndRoundUp(Resolution, GroupSize));

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("BuildSDFSparse %dx%dx%d", Resolution, Resolution, Resolution),
				SparseShader,
				SparseParams,
				GroupCount);

			return SDFVolume;
		}
	}
}

FRDGBufferRef FFluidVolumeBuilder::BuildOccupancyMask(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef DensityVolume,
	const FFluidVolumeConfig& Config)
{
	RDG_EVENT_SCOPE(GraphBuilder, "BuildOccupancyMask");

	// Create occupancy bitmask buffer (32³ bits = 1024 uint32 = 4KB)
	FRDGBufferDesc OccupancyDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), OCCUPANCY_UINT_COUNT);
	FRDGBufferRef OccupancyMask = GraphBuilder.CreateBuffer(OccupancyDesc, TEXT("FluidOccupancyMask"));

	// Clear to zero first
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OccupancyMask, PF_R32_UINT), 0u);

	// Build occupancy mask from density volume
	TShaderMapRef<FBuildOccupancyMaskCS> ComputeShader(GlobalShaderMap);

	FBuildOccupancyMaskCS::FParameters* PassParams = GraphBuilder.AllocParameters<FBuildOccupancyMaskCS::FParameters>();
	PassParams->DensityVolume = GraphBuilder.CreateSRV(DensityVolume);
	PassParams->OccupancyMask = GraphBuilder.CreateUAV(OccupancyMask, PF_R32_UINT);
	PassParams->VolumeResolution = Config.VolumeResolution;
	PassParams->DensityThreshold = Config.DensityThreshold;

	// Each thread handles one occupancy cell (32³ = 32768 threads)
	const int32 GroupSize = FBuildOccupancyMaskCS::ThreadGroupSize;
	const FIntVector GroupCount(
		FMath::DivideAndRoundUp(OCCUPANCY_RESOLUTION, GroupSize),
		FMath::DivideAndRoundUp(OCCUPANCY_RESOLUTION, GroupSize),
		FMath::DivideAndRoundUp(OCCUPANCY_RESOLUTION, GroupSize));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("BuildOccupancyMask 32x32x32"),
		ComputeShader,
		PassParams,
		GroupCount);

	return OccupancyMask;
}

FRDGBufferRef FFluidVolumeBuilder::ComputeFluidAABB(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef ParticleBuffer,
	int32 ParticleCount,
	float ParticleRadius)
{
	RDG_EVENT_SCOPE(GraphBuilder, "ComputeFluidAABB");

	// Debug logging
	static int32 AABBDebugCounter = 0;
	if (++AABBDebugCounter % 60 == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ComputeFluidAABB] ParticleCount=%d, ParticleRadius=%.2f"),
			ParticleCount, ParticleRadius);
	}

	// Create AABB buffer (6 uints: min.xyz, max.xyz as sortable uints)
	FRDGBufferDesc AABBDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 6);
	FRDGBufferRef AABBBuffer = GraphBuilder.CreateBuffer(AABBDesc, TEXT("FluidAABB"));

	// Initialize buffer with extreme values:
	// - min values [0,1,2]: 0xFFFFFFFF (largest sortable uint - any real value is smaller)
	// - max values [3,4,5]: 0x00000000 (smallest sortable uint - any real value is larger)
	{
		TShaderMapRef<FInitFluidAABBCS> InitShader(GlobalShaderMap);

		FInitFluidAABBCS::FParameters* InitParams = GraphBuilder.AllocParameters<FInitFluidAABBCS::FParameters>();
		InitParams->FluidAABB = GraphBuilder.CreateUAV(AABBBuffer, PF_R32_UINT);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("InitFluidAABB"),
			InitShader,
			InitParams,
			FIntVector(1, 1, 1));
	}

	// Compute AABB from particles
	TShaderMapRef<FComputeFluidAABBCS> ComputeShader(GlobalShaderMap);

	FComputeFluidAABBCS::FParameters* PassParams = GraphBuilder.AllocParameters<FComputeFluidAABBCS::FParameters>();
	PassParams->Particles = ParticleBuffer;
	PassParams->FluidAABB = GraphBuilder.CreateUAV(AABBBuffer, PF_R32_UINT);
	PassParams->ParticleCount = ParticleCount;
	PassParams->ParticleRadius = ParticleRadius;

	const int32 GroupSize = FComputeFluidAABBCS::ThreadGroupSize;
	const int32 GroupCount = FMath::DivideAndRoundUp(ParticleCount, GroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("ComputeFluidAABB ParticleCount=%d", ParticleCount),
		ComputeShader,
		PassParams,
		FIntVector(GroupCount, 1, 1));

	return AABBBuffer;
}

FRDGTextureRef FFluidVolumeBuilder::BuildMinMaxMipmap(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef DensityVolume,
	const FFluidVolumeConfig& Config)
{
	RDG_EVENT_SCOPE(GraphBuilder, "BuildMinMaxMipmap");

	const int32 DensityResolution = Config.VolumeResolution;
	const int32 MipLevels = Config.MinMaxMipLevels;

	// MinMax mipmap starts at half density resolution
	// L0 = DensityRes/2, L1 = L0/2, etc.
	const int32 MipLevel0Resolution = DensityResolution / 2;

	// Create MinMax mipmap texture with mip chain
	FRDGTextureDesc MinMaxDesc = FRDGTextureDesc::Create3D(
		FIntVector(MipLevel0Resolution, MipLevel0Resolution, MipLevel0Resolution),
		PF_G32R32F,  // float2: (min, max)
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV,
		MipLevels);

	FRDGTextureRef MinMaxMipmap = GraphBuilder.CreateTexture(MinMaxDesc, TEXT("FluidMinMaxMipmap"));

	// Create sampler state for texture sampling
	FSamplerStateRHIRef PointSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// Build Level 0: Density -> MinMax (downsample by 2x)
	{
		TShaderMapRef<FBuildMinMaxMipLevel0CS> ComputeShader(GlobalShaderMap);

		FBuildMinMaxMipLevel0CS::FParameters* PassParams = GraphBuilder.AllocParameters<FBuildMinMaxMipLevel0CS::FParameters>();
		PassParams->DensityVolume = GraphBuilder.CreateSRV(DensityVolume);
		PassParams->DensitySampler = PointSampler;
		PassParams->MinMaxMipLevel0 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(MinMaxMipmap, 0));
		PassParams->InputResolution = DensityResolution;
		PassParams->OutputResolution = MipLevel0Resolution;

		const int32 GroupSize = FBuildMinMaxMipLevel0CS::ThreadGroupSize;
		const FIntVector GroupCount(
			FMath::DivideAndRoundUp(MipLevel0Resolution, GroupSize),
			FMath::DivideAndRoundUp(MipLevel0Resolution, GroupSize),
			FMath::DivideAndRoundUp(MipLevel0Resolution, GroupSize));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("BuildMinMaxMipLevel0 %d -> %d", DensityResolution, MipLevel0Resolution),
			ComputeShader,
			PassParams,
			GroupCount);
	}

	// Build subsequent mip levels
	TShaderMapRef<FBuildMinMaxMipChainCS> MipChainShader(GlobalShaderMap);
	int32 CurrentResolution = MipLevel0Resolution;

	for (int32 MipLevel = 1; MipLevel < MipLevels; ++MipLevel)
	{
		const int32 NextResolution = FMath::Max(CurrentResolution / 2, 1);

		FBuildMinMaxMipChainCS::FParameters* PassParams = GraphBuilder.AllocParameters<FBuildMinMaxMipChainCS::FParameters>();
		PassParams->InputMipLevel = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(MinMaxMipmap, MipLevel - 1));
		PassParams->InputSampler = PointSampler;
		PassParams->OutputMipLevel = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(MinMaxMipmap, MipLevel));
		PassParams->InputResolution = CurrentResolution;
		PassParams->OutputResolution = NextResolution;

		const int32 GroupSize = FBuildMinMaxMipChainCS::ThreadGroupSize;
		const FIntVector GroupCount(
			FMath::DivideAndRoundUp(NextResolution, GroupSize),
			FMath::DivideAndRoundUp(NextResolution, GroupSize),
			FMath::DivideAndRoundUp(NextResolution, GroupSize));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("BuildMinMaxMipLevel%d %d -> %d", MipLevel, CurrentResolution, NextResolution),
			MipChainShader,
			PassParams,
			GroupCount);

		CurrentResolution = NextResolution;
	}

	return MinMaxMipmap;
}

void FFluidVolumeBuilder::ClearVolume(
	FRDGBuilder& GraphBuilder,
	FRDGTextureUAVRef VolumeUAV,
	int32 Resolution,
	float ClearValue)
{
	TShaderMapRef<FClearVolumeCS> ComputeShader(GlobalShaderMap);

	FClearVolumeCS::FParameters* PassParams = GraphBuilder.AllocParameters<FClearVolumeCS::FParameters>();
	PassParams->Volume = VolumeUAV;
	PassParams->VolumeResolution = Resolution;
	PassParams->ClearValue = ClearValue;

	const int32 GroupSize = FClearVolumeCS::ThreadGroupSize;
	const FIntVector GroupCount(
		FMath::DivideAndRoundUp(Resolution, GroupSize),
		FMath::DivideAndRoundUp(Resolution, GroupSize),
		FMath::DivideAndRoundUp(Resolution, GroupSize));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("ClearVolume %dx%dx%d", Resolution, Resolution, Resolution),
		ComputeShader,
		PassParams,
		GroupCount);
}

// NOTE: ApplyTightAABB and UpdateAABBCache removed - GPU-only Tight AABB now
// AABB is computed and used within same frame, no CPU readback or caching needed
