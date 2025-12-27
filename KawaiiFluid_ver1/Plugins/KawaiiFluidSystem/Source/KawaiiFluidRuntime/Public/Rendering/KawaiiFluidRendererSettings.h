// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
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
	// Appearance
	//========================================

	/** Fluid color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled"))
	FLinearColor FluidColor = FLinearColor(0.2f, 0.4f, 0.8f, 1.0f);

	/** Metallic (metalness) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float Metallic = 0.0f;

	/** Roughness */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float Roughness = 0.1f;

	/** Refractive index (IOR) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (EditCondition = "bEnabled", ClampMin = "1.0", ClampMax = "2.5"))
	float RefractiveIndex = 1.33f;

	//========================================
	// Performance
	//========================================

	/** Maximum particles to render */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (EditCondition = "bEnabled", ClampMin = "1", ClampMax = "100000"))
	int32 MaxRenderParticles = 50000;

	/** Depth buffer resolution scale */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (EditCondition = "bEnabled", ClampMin = "0.25", ClampMax = "2.0"))
	float DepthBufferScale = 1.0f;

	/** Use thickness buffer */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (EditCondition = "bEnabled"))
	bool bUseThicknessBuffer = true;

	//========================================
	// Filtering
	//========================================

	/** Depth smoothing iterations */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filtering", meta = (EditCondition = "bEnabled", ClampMin = "0", ClampMax = "10"))
	int32 DepthSmoothingIterations = 3;

	/** Bilateral filter radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filtering", meta = (EditCondition = "bEnabled", ClampMin = "1.0", ClampMax = "10.0"))
	float FilterRadius = 3.0f;

	//========================================
	// Advanced
	//========================================

	/** Surface tension */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float SurfaceTension = 0.5f;

	/** Foam generation threshold */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "10.0"))
	float FoamThreshold = 5.0f;

	/** Foam color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (EditCondition = "bEnabled"))
	FLinearColor FoamColor = FLinearColor::White;

	//========================================
	// Debug
	//========================================

	/** Debug visualization mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (EditCondition = "bEnabled"))
	bool bShowDebugVisualization = false;

	/** Show render targets */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (EditCondition = "bEnabled && bShowDebugVisualization"))
	bool bShowRenderTargets = false;
};
