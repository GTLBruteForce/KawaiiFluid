// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiRenderParticle.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "Rendering/FluidRenderingParameters.h"
#include "KawaiiFluidMetaballRenderer.generated.h"

class IKawaiiFluidDataProvider;
class UFluidRendererSubsystem;
class FKawaiiFluidRenderResource;
class IKawaiiMetaballRenderingPipeline;

/**
 * Metaball Renderer (UObject-based)
 *
 * Renders fluid particles using GPU-based metaball rendering with
 * screen-space surface reconstruction for realistic fluid appearance.
 *
 * Features:
 * - Realistic fluid surface rendering
 * - GPU Compute Shader based high performance
 * - Reflection/refraction/fresnel effects
 * - Custom rendering pipeline via ViewExtension
 *
 * Note: This is NOT an ActorComponent - it's owned internally by RenderingModule.
 * Pure UObject implementation (no component dependencies).
 */
UCLASS()
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidMetaballRenderer : public UObject
{
	GENERATED_BODY()

public:
	UKawaiiFluidMetaballRenderer();

	/**
	 * Initialize renderer with world and owner context
	 * @param InWorld World context for subsystem access
	 * @param InOwnerComponent Parent scene component (for consistency with ISM)
	 */
	void Initialize(UWorld* InWorld, USceneComponent* InOwnerComponent);

	/**
	 * Cleanup renderer resources
	 */
	void Cleanup();

	/**
	 * Apply settings from struct
	 * @param Settings Editor configuration to apply
	 */
	void ApplySettings(const FKawaiiFluidMetaballRendererSettings& Settings);

	/**
	 * Update rendering
	 * @param DataProvider Particle data provider
	 * @param DeltaTime Frame delta time
	 */
	void UpdateRendering(const IKawaiiFluidDataProvider* DataProvider, float DeltaTime);

	/** Check if rendering is enabled */
	bool IsEnabled() const { return bEnabled; }

	/** Enable or disable rendering */
	void SetEnabled(bool bInEnabled);

	//========================================
	// GPU Resource Access (for ViewExtension)
	//========================================

	/** Get GPU render resource (for ViewExtension access) */
	FKawaiiFluidRenderResource* GetFluidRenderResource() const;

	/** Check if rendering is active and resource is valid */
	bool IsRenderingActive() const;

	/** Get cached particle radius (for ViewExtension access) */
	float GetCachedParticleRadius() const { return CachedParticleRadius; }

	/** Get local rendering parameters for batching */
	const FFluidRenderingParameters& GetLocalParameters() const { return LocalParameters; }

	/** Get rendering pipeline (handles ShadingMode internally) */
	TSharedPtr<IKawaiiMetaballRenderingPipeline> GetPipeline() const { return Pipeline; }

	//========================================
	// Enable Control
	//========================================

	/** Enable/disable this renderer */
	bool bEnabled = true;

	/** Use simulation particle radius for rendering */
	bool bUseSimulationRadius = false;

	/** Render only surface particles (for slime) */
	bool bRenderSurfaceOnly = false;

	/** Local rendering parameters (per-renderer settings) */
	FFluidRenderingParameters LocalParameters;

	//========================================
	// Performance Options
	//========================================

	/** Maximum particles to render */
	int32 MaxRenderParticles = 50000;

	//========================================
	// Runtime Info
	//========================================

	/** Last frame rendered particle count */
	int32 LastRenderedParticleCount = 0;

	/** Metaball rendering active status */
	bool bIsRenderingActive = false;

	//========================================
	// Debug Visualization
	//========================================

	/** Cached SDF volume bounds (for debug drawing) */
	FVector CachedSDFVolumeMin = FVector::ZeroVector;
	FVector CachedSDFVolumeMax = FVector::ZeroVector;
	bool bHasValidSDFVolumeBounds = false;

	/** Set SDF volume bounds (called from render thread) */
	void SetSDFVolumeBounds(const FVector& VolumeMin, const FVector& VolumeMax);

	/** Draw debug visualization (called from game thread) */
	void DrawDebugVisualization();

protected:
	//========================================
	// Common State (from removed base class)
	//========================================

	/** Cached world reference (replaces GetWorld()) */
	UPROPERTY()
	TObjectPtr<UWorld> CachedWorld;

	/** Cached owner component reference */
	UPROPERTY()
	TObjectPtr<USceneComponent> CachedOwnerComponent;

	//========================================
	// Metaball Renderer Internals
	//========================================

	/** Update GPU render resources */
	void UpdateGPUResources(const TArray<FFluidParticle>& Particles, float ParticleRadius);

	/** Update Pipeline based on LocalParameters (Pipeline handles ShadingMode internally) */
	void UpdatePipeline();

private:
	/** Cached particle positions */
	TArray<FVector> CachedParticlePositions;

	/** Cached particle radius */
	float CachedParticleRadius = 5.0f;

	/** Cached renderer subsystem reference (for ViewExtension access) */
	TWeakObjectPtr<UFluidRendererSubsystem> RendererSubsystem;

	//========================================
	// GPU Resources
	//========================================

	/** GPU render resource (manages structured buffers) */
	TSharedPtr<FKawaiiFluidRenderResource> RenderResource;

	/** Converted render particles cache (FFluidParticle -> FKawaiiRenderParticle) */
	TArray<FKawaiiRenderParticle> RenderParticlesCache;

	//========================================
	// Pipeline Architecture
	//========================================

	/** Rendering pipeline (ScreenSpace or RayMarching) - handles ShadingMode internally */
	TSharedPtr<IKawaiiMetaballRenderingPipeline> Pipeline;

	/** Cached pipeline type (to detect changes) */
	EMetaballPipelineType CachedPipelineType = EMetaballPipelineType::ScreenSpace;
};

// Backwards compatibility alias
using UKawaiiFluidSSFRRenderer = UKawaiiFluidMetaballRenderer;
