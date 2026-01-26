// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "FluidRenderingParameters.h"
#include "RenderGraphResources.h"
#include "FluidRendererSubsystem.generated.h"

class FFluidSceneViewExtension;
class UKawaiiFluidRenderingModule;
class UInstancedStaticMeshComponent;
class UStaticMesh;

/**
 * Fluid rendering world subsystem
 *
 * Responsibilities:
 * - Manages UKawaiiFluidRenderingModule integration
 * - Provides SSFR rendering pipeline (ViewExtension)
 * - ISM rendering uses Unreal's default pipeline
 */
UCLASS()
class KAWAIIFLUIDRUNTIME_API UFluidRendererSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End of USubsystem interface

	//========================================
	// RenderingModule Management
	//========================================

	/** Register RenderingModule (called automatically) */
	void RegisterRenderingModule(UKawaiiFluidRenderingModule* Module);

	/** Unregister RenderingModule */
	void UnregisterRenderingModule(UKawaiiFluidRenderingModule* Module);

	/** Get all registered RenderingModules */
	const TArray<TObjectPtr<UKawaiiFluidRenderingModule>>& GetAllRenderingModules() const { return RegisteredRenderingModules; }

	//========================================
	// Rendering Parameters
	//========================================

	/** Global rendering parameters */
	UPROPERTY(EditAnywhere, Transient, BlueprintReadWrite, Category = "Fluid Rendering")
	FFluidRenderingParameters RenderingParameters;

	/** View Extension accessor */
	TSharedPtr<FFluidSceneViewExtension, ESPMode::ThreadSafe> GetViewExtension() const { return ViewExtension; }


	//========================================
	// Cached Shadow Light Data (Game Thread -> Render Thread)
	//========================================

	/** Update cached light direction from DirectionalLight (call from game thread) */
	void UpdateCachedLightDirection();

	/** Get cached light direction (safe to call from render thread) */
	FVector3f GetCachedLightDirection() const { return CachedLightDirection; }

	/** Get cached light view-projection matrix (safe to call from render thread) */
	FMatrix44f GetCachedLightViewProjectionMatrix() const { return CachedLightViewProjectionMatrix; }

	/** Check if cached light data is valid */
	bool HasValidCachedLightData() const { return bHasCachedLightData; }

	//========================================
	// VSM (Variance Shadow Map) Textures - Per-World
	//========================================

	/** Swap VSM buffers (call at BeginRenderViewFamily) */
	void SwapVSMBuffers();

	/** Get VSM read texture */
	TRefCountPtr<IPooledRenderTarget>& GetVSMTextureRead() { return VSMTexture_Read; }

	/** Get VSM write texture reference for extraction */
	TRefCountPtr<IPooledRenderTarget>* GetVSMTextureWritePtr() { return &VSMTexture_Write; }

	/** Get light VP matrix for reading */
	FMatrix44f GetLightVPMatrixRead() const { return LightVPMatrix_Read; }

	/** Set light VP matrix for writing */
	void SetLightVPMatrixWrite(const FMatrix44f& Matrix) { LightVPMatrix_Write = Matrix; }

private:
	/** Registered RenderingModules */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UKawaiiFluidRenderingModule>> RegisteredRenderingModules;

	/** Scene View Extension (rendering pipeline injection) */
	TSharedPtr<FFluidSceneViewExtension, ESPMode::ThreadSafe> ViewExtension;


	//========================================
	// Cached Light Data (updated on game thread, read on render thread)
	//========================================

	/** Cached directional light direction (normalized) */
	FVector3f CachedLightDirection = FVector3f(0.5f, 0.5f, -0.707f);

	/** Cached light view-projection matrix for shadow mapping */
	FMatrix44f CachedLightViewProjectionMatrix = FMatrix44f::Identity;

	/** Whether cached light data is valid */
	bool bHasCachedLightData = false;

	//========================================
	// VSM Textures (Per-World, double-buffered)
	//========================================

	/** VSM texture for writing (current frame) */
	TRefCountPtr<IPooledRenderTarget> VSMTexture_Write;

	/** VSM texture for reading (previous frame) */
	TRefCountPtr<IPooledRenderTarget> VSMTexture_Read;

	/** Light view-projection matrix for writing */
	FMatrix44f LightVPMatrix_Write = FMatrix44f::Identity;

	/** Light view-projection matrix for reading */
	FMatrix44f LightVPMatrix_Read = FMatrix44f::Identity;

	//========================================
	// HISM Shadow (Instanced Mesh Shadow)
	//========================================

public:
	/**
	 * @brief Enable/disable HISM shadow via instanced spheres.
	 * When enabled, creates sphere instances at particle positions for shadow casting.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Shadow|HISM")
	bool bEnableVSMIntegration = true;

	/**
	 * @brief Maximum number of shadow instances to use.
	 * Set to 0 for unlimited (default).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Shadow|HISM", meta = (ClampMin = "0", ClampMax = "100000"))
	int32 MaxShadowInstances = 0;

	/**
	 * @brief Skip factor for particle-to-instance conversion.
	 * Value of 2 means every other particle becomes an instance.
	 * Higher values improve performance but reduce shadow detail.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid Shadow|HISM", meta = (ClampMin = "1", ClampMax = "10"))
	int32 ParticleSkipFactor = 1;

	/**
	 * @brief Cleanup HISM shadow resources when disabled.
	 */
	void UpdateShadowProxyState();

	/**
	 * @brief Update shadow instances from particle positions.
	 * Creates sphere instances at particle locations for shadow casting.
	 * @param ParticlePositions Array of particle world positions.
	 * @param NumParticles Number of particles.
	 * @param ParticleRadius Radius of each particle (used for shadow sphere size).
	 * Uses pooling based on MaxParticles to minimize reallocation
	 */
	void UpdateShadowInstances(const FVector* ParticlePositions, int32 NumParticles, float ParticleRadius, int32 MaxParticles = 0);
	
	/**
	 * @brief Update shadow instances with anisotropy data for ellipsoid shadows.
	 * Creates oriented ellipsoid instances at particle locations.
	 * @param ParticlePositions Array of particle world positions.
	 * @param AnisotropyAxis1 Array of first ellipsoid axis (xyz=direction, w=scale).
	 * @param AnisotropyAxis2 Array of second ellipsoid axis.
	 * @param AnisotropyAxis3 Array of third ellipsoid axis.
	 * @param NumParticles Number of particles.
	 * @param ParticleRadius Radius of each particle (used for shadow sphere size).
	 */
	void UpdateShadowInstancesWithAnisotropy(
		const FVector* ParticlePositions,
		const FVector4* AnisotropyAxis1,
		const FVector4* AnisotropyAxis2,
		const FVector4* AnisotropyAxis3,
		int32 NumParticles,
		float ParticleRadius);

private:
	/** Actor that owns the ISM shadow component. */
	UPROPERTY(Transient)
	TObjectPtr<AActor> ShadowProxyActor = nullptr;

	/** ISM component for instanced sphere shadow casting. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> ShadowInstanceComponent = nullptr;

	/** Sphere mesh used for shadow instances. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> ShadowSphereMesh;

	/** Cached instance transforms for batch update. */
	TArray<FTransform> CachedInstanceTransforms;
};
