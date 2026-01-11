// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/FluidRenderingParameters.h"
#include "Core/FluidAnisotropy.h"
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
	bool bEnabled = false;

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

	/** Particle color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled"))
	FLinearColor ParticleColor = FLinearColor(0.2f, 0.5f, 1.0f, 0.8f);
};

/**
 * Metaball Renderer Settings (Editor Configuration)
 *
 * Lightweight struct for configuring Metaball renderer in Details panel.
 * These settings are copied to UKawaiiFluidMetaballRenderer at initialization.
 */
USTRUCT(BlueprintType)
struct KAWAIIFLUIDRUNTIME_API FKawaiiFluidMetaballRendererSettings
{
	GENERATED_BODY()

	//========================================
	// Enable Control
	//========================================

	/** Enable/disable Metaball renderer */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bEnabled = false;

	//========================================
	// Rendering
	//========================================

	/** Pipeline type - how the fluid surface is computed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering",
		meta = (EditCondition = "bEnabled", DisplayName = "Pipeline Type"))
	EMetaballPipelineType PipelineType = EMetaballPipelineType::ScreenSpace;

	/** Shading mode - how the fluid surface is lit/rendered */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering",
		meta = (EditCondition = "bEnabled", DisplayName = "Shading Mode"))
	EMetaballShadingMode ShadingMode = EMetaballShadingMode::PostProcess;

	/** Use simulation particle radius for rendering (if true, ignores ParticleRenderRadius) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (EditCondition = "bEnabled"))
	bool bUseSimulationRadius = false;

	/** Particle render radius (screen space, cm) - only used when bUseSimulationRadius is false */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (EditCondition = "bEnabled && !bUseSimulationRadius", ClampMin = "0.5", ClampMax = "100.0"))
	float ParticleRenderRadius = 15.0f;

	/** Render only surface particles (for slime - reduces particle count while maintaining surface) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (EditCondition = "bEnabled"))
	bool bRenderSurfaceOnly = false;

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

	/** Depth smoothing filter type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnabled"))
	EDepthSmoothingFilter SmoothingFilter = EDepthSmoothingFilter::NarrowRange;

	/** Bilateral filter radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnabled", ClampMin = "1", ClampMax = "100"))
	int32 BilateralFilterRadius = 20;

	//========================================
	// Anisotropy
	//========================================

	/** Anisotropy parameters for ellipsoid rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anisotropy", meta = (EditCondition = "bEnabled"))
	FFluidAnisotropyParams AnisotropyParams;

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

	/** Metallic value for GBuffer (G-Buffer shading mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "bEnabled && ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float Metallic = 0.1f;

	/** Roughness value for GBuffer (G-Buffer shading mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "bEnabled && ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float Roughness = 0.3f;

	/** Subsurface scattering opacity (G-Buffer shading mode only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|GBuffer",
		meta = (EditCondition = "bEnabled && ShadingMode == EMetaballShadingMode::GBuffer", ClampMin = "0.0", ClampMax = "1.0"))
	float SubsurfaceOpacity = 0.5f;

	//========================================
	// Shadow Casting (VSM) - Legacy
	//========================================

	/** Enable fluid shadow casting onto other objects (Legacy VSM approach) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow",
		meta = (EditCondition = "bEnabled", DisplayName = "Enable Shadow Casting (Legacy)"))
	bool bEnableShadowCasting = false;

	/** VSM shadow map resolution (higher = sharper shadows but more expensive) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow",
		meta = (EditCondition = "bEnabled && bEnableShadowCasting", ClampMin = "256", ClampMax = "4096"))
	int32 VSMResolution = 1024;

	/** VSM blur radius (higher = softer shadows) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow",
		meta = (EditCondition = "bEnabled && bEnableShadowCasting", ClampMin = "0.0", ClampMax = "16.0"))
	float VSMBlurRadius = 4.0f;

	/** Number of VSM blur iterations */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow",
		meta = (EditCondition = "bEnabled && bEnableShadowCasting", ClampMin = "0", ClampMax = "3"))
	int32 VSMBlurIterations = 1;

	/** Shadow intensity (0 = no shadow, 1 = full black) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow",
		meta = (EditCondition = "bEnabled && bEnableShadowCasting", ClampMin = "0.0", ClampMax = "1.0"))
	float ShadowIntensity = 0.5f;
};

// Backwards compatibility alias
using FKawaiiFluidSSFRRendererSettings = FKawaiiFluidMetaballRendererSettings;
