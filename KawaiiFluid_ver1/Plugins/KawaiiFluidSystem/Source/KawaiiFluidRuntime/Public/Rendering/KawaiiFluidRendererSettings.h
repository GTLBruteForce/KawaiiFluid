// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/FluidRenderingParameters.h"
#include "KawaiiFluidRendererSettings.generated.h"

/**
 * ISM Renderer Settings (Editor Configuration)
 *
 * Lightweight struct for configuring ISM renderer in Details panel.
 * These settings are copied to UKawaiiFluidISMRenderer at initialization.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FKawaiiFluidISMRendererSettings
{
	GENERATED_BODY()

	// Constructor to set default mesh and material
	FKawaiiFluidISMRendererSettings();

	//========================================
	// Enable Control
	//========================================

	/** Enable/disable ISM renderer */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bEnabled = true;

	//========================================
	// Configuration
	//========================================

	/** Particle mesh to use */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Configuration", meta = (EditCondition = "bEnabled"))
	TObjectPtr<UStaticMesh> ParticleMesh;

	/** Particle material (nullptr uses mesh default) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Configuration", meta = (EditCondition = "bEnabled"))
	TObjectPtr<UMaterialInterface> ParticleMaterial;

	/** Particle scale multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Configuration", meta = (EditCondition = "bEnabled", ClampMin = "0.01", ClampMax = "10.0"))
	float ParticleScale = 1.0f;
};

/**
 * SSFR Renderer Settings (Editor Configuration)
 *
 * Lightweight struct for configuring SSFR renderer in Details panel.
 * These settings are copied to UKawaiiFluidSSFRRenderer at initialization.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FKawaiiFluidSSFRRendererSettings
{
	GENERATED_BODY()

	//========================================
	// Enable Control
	//========================================

	/** Enable/disable SSFR renderer */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bEnabled = false;
	
	//========================================
	// Rendering
	//========================================

	/** SSFR rendering mode selection */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (EditCondition = "bEnabled", DisplayName = "SSFR Mode"))
	ESSFRRenderingMode SSFRMode = ESSFRRenderingMode::Custom;

	/** Use simulation particle radius for rendering (if true, ignores ParticleRenderRadius) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (EditCondition = "bEnabled"))
	bool bUseSimulationRadius = false;

	/** Render only surface particles (for slime - reduces particle count while maintaining surface) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (EditCondition = "bEnabled"))
	bool bRenderSurfaceOnly = false;

	/** Particle render radius (screen space, cm) - only used when bUseSimulationRadius is false */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (EditCondition = "bEnabled && !bUseSimulationRadius", ClampMin = "1.0", ClampMax = "100.0"))
	float ParticleRenderRadius = 15.0f;

	//========================================
	// Visual Appearance
	//========================================

	/** Fluid color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled"))
	FLinearColor FluidColor = FLinearColor(0.2f, 0.4f, 0.8f, 1.0f);

	/** Fresnel strength */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float FresnelStrength = 0.7f;

	/** Refractive index (IOR) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled", ClampMin = "1.0", ClampMax = "2.5"))
	float RefractiveIndex = 1.33f;

	/** Absorption coefficient */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "10.0"))
	float AbsorptionCoefficient = 2.0f;

	/** Specular strength */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "2.0"))
	float SpecularStrength = 1.0f;

	/** Specular roughness */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled", ClampMin = "0.01", ClampMax = "1.0"))
	float SpecularRoughness = 0.2f;

	//========================================
	// Smoothing
	//========================================

	/** Bilateral filter radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnabled", ClampMin = "1", ClampMax = "50"))
	int32 BilateralFilterRadius = 20;

	//========================================
	// Performance
	//========================================

	/** Maximum particles to render */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (EditCondition = "bEnabled", ClampMin = "1", ClampMax = "100000"))
	int32 MaxRenderParticles = 50000;

	/** Render target resolution scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (EditCondition = "bEnabled", ClampMin = "0.25", ClampMax = "2.0"))
	float RenderTargetScale = 1.0f;

	/** Thickness scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (EditCondition = "bEnabled", ClampMin = "0.1", ClampMax = "10.0"))
	float ThicknessScale = 1.0f;
	
	//========================================
	// G-Buffer Mode Parameters
	//========================================

	/** Metallic value for GBuffer (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "bEnabled && SSFRMode == ESSFRRenderingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float Metallic = 0.1f;

	/** Roughness value for GBuffer (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "bEnabled && SSFRMode == ESSFRRenderingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float Roughness = 0.3f;

	/** Subsurface scattering opacity (G-Buffer mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "bEnabled && SSFRMode == ESSFRRenderingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float SubsurfaceOpacity = 0.5f;
};
