// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Interfaces/IKawaiiFluidRenderer.h"
#include "Core/FluidParticle.h"
#include "Core/KawaiiRenderParticle.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "KawaiiFluidSSFRRenderer.generated.h"

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
 * Note: This is NOT an ActorComponent - it's owned internally by RenderController.
 * Pure UObject implementation (no component dependencies).
 */
UCLASS()
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidSSFRRenderer : public UObject, public IKawaiiFluidRenderer
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

	// IKawaiiFluidRenderer interface
	virtual void UpdateRendering(const IKawaiiFluidDataProvider* DataProvider, float DeltaTime) override;
	virtual bool IsEnabled() const override { return bEnabled; }
	virtual EKawaiiFluidRenderingMode GetRenderingMode() const override { return EKawaiiFluidRenderingMode::SSFR; }
	virtual void SetEnabled(bool bInEnabled) override { bEnabled = bInEnabled; }

	//========================================
	// GPU Resource Access (for ViewExtension)
	//========================================

	/** Get GPU render resource (for ViewExtension access) */
	FKawaiiFluidRenderResource* GetFluidRenderResource() const;

	/** Check if rendering is active and resource is valid */
	bool IsRenderingActive() const;

	/** Get cached particle radius (for ViewExtension access) */
	float GetCachedParticleRadius() const { return CachedParticleRadius; }

	//========================================
	// Enable Control
	//========================================

	/** Enable/disable this renderer */
	bool bEnabled = true;

	//========================================
	// Appearance
	//========================================

	/** Fluid color */
	FLinearColor FluidColor = FLinearColor(0.2f, 0.4f, 0.8f, 1.0f);

	/** Metallic (metalness) */
	float Metallic = 0.0f;

	/** Roughness */
	float Roughness = 0.1f;

	/** Refractive index (IOR) */
	float RefractiveIndex = 1.33f; // Water IOR

	//========================================
	// Performance Options
	//========================================

	/** Maximum particles to render */
	int32 MaxRenderParticles = 50000;

	/** Depth buffer resolution scale (1.0 = screen resolution) */
	float DepthBufferScale = 1.0f;

	/** Use thickness buffer */
	bool bUseThicknessBuffer = true;

	//========================================
	// Filtering Options
	//========================================

	/** Depth smoothing iterations */
	int32 DepthSmoothingIterations = 3;

	/** Bilateral filter radius */
	float FilterRadius = 3.0f;

	//========================================
	// Advanced Options
	//========================================

	/** Surface tension */
	float SurfaceTension = 0.5f;

	/** Foam generation threshold */
	float FoamThreshold = 5.0f;

	/** Foam color */
	FLinearColor FoamColor = FLinearColor::White;

	//========================================
	// Debug Options
	//========================================

	/** Debug visualization mode */
	bool bShowDebugVisualization = false;

	/** Show render targets (Depth, Thickness, Normal, etc.) */
	bool bShowRenderTargets = false;

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
};
