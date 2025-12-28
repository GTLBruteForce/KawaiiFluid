// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiRenderParticle.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "Rendering/FluidRenderingParameters.h"
#include "KawaiiFluidSSFRRenderer.generated.h"

class IKawaiiFluidDataProvider;
class UFluidRendererSubsystem;
class FKawaiiFluidRenderResource;

/**
 * Screen Space Fluid Rendering (SSFR) renderer (UObject-based)
 *
 * Renders fluid particles using GPU-based depth/thickness rendering and
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
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidSSFRRenderer : public UObject
{
	GENERATED_BODY()

public:
	UKawaiiFluidSSFRRenderer();

	/**
	 * Initialize renderer with world and owner context
	 * @param InWorld World context for subsystem access
	 * @param InOwner Actor owner
	 */
	void Initialize(UWorld* InWorld, AActor* InOwner);

	/**
	 * Cleanup renderer resources
	 */
	void Cleanup();

	/**
	 * Apply settings from struct
	 * @param Settings Editor configuration to apply
	 */
	void ApplySettings(const FKawaiiFluidSSFRRendererSettings& Settings);

	/**
	 * Update rendering
	 * @param DataProvider Particle data provider
	 * @param DeltaTime Frame delta time
	 */
	void UpdateRendering(const IKawaiiFluidDataProvider* DataProvider, float DeltaTime);

	/** Check if rendering is enabled */
	bool IsEnabled() const { return bEnabled; }

	/** Enable or disable rendering */
	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }

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

	/** Get composite pass (for ViewExtension access) */
	TSharedPtr<class IFluidCompositePass> GetCompositePass() const { return CompositePass; }

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

	/** SSFR rendering active status */
	bool bIsRenderingActive = false;

protected:
	//========================================
	// Common State (from removed base class)
	//========================================

	/** Cached world reference (replaces GetWorld()) */
	UPROPERTY()
	TObjectPtr<UWorld> CachedWorld;

	/** Cached owner actor reference (replaces GetOwner()) */
	UPROPERTY()
	TObjectPtr<AActor> CachedOwner;

	//========================================
	// SSFR-specific Internals
	//========================================

	/** Update GPU render resources */
	void UpdateGPUResources(const TArray<FFluidParticle>& Particles, float ParticleRadius);

	/** Update composite pass if mode changed */
	void UpdateCompositePass();

private:
	/** Cached particle positions */
	TArray<FVector> CachedParticlePositions;

	/** Cached particle radius */
	float CachedParticleRadius = 5.0f;

	/** Cached renderer subsystem reference (for ViewExtension access) */
	UPROPERTY()
	TObjectPtr<UFluidRendererSubsystem> RendererSubsystem;

	//========================================
	// GPU Resources (SSFR Pipeline)
	//========================================

	/** GPU render resource (manages structured buffers for SSFR) */
	TSharedPtr<FKawaiiFluidRenderResource> RenderResource;

	/** Converted render particles cache (FFluidParticle → FKawaiiRenderParticle) */
	TArray<FKawaiiRenderParticle> RenderParticlesCache;

	//========================================
	// Composite Pass (Custom vs GBuffer rendering)
	//========================================

	/** Composite rendering pass (Custom or GBuffer) */
	TSharedPtr<IFluidCompositePass> CompositePass;

	/** Cached rendering mode (to detect mode changes) */
	ESSFRRenderingMode CachedSSFRMode = ESSFRRenderingMode::Custom;
};
