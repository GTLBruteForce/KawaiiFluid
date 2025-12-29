// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "KawaiiFluidISMRenderer.generated.h"

class IKawaiiFluidDataProvider;

/**
 * Instanced Static Mesh fluid renderer (UObject-based)
 *
 * Renders fluid particles as instanced static meshes for high performance.
 * Each particle becomes one mesh instance rendered via GPU instancing.
 *
 * Features:
 * - High performance (GPU instancing)
 * - Custom mesh/material support
 * - Velocity-based color and rotation
 * - Absolute world coordinates
 *
 * Note: This is NOT an ActorComponent - it's owned internally by RenderingModule.
 * The ISMComponent inside IS a component, created and attached to the owner actor.
 */
UCLASS()
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidISMRenderer : public UObject
{
	GENERATED_BODY()

public:
	UKawaiiFluidISMRenderer();

	/**
	 * Initialize renderer with world and owner context
	 * @param InWorld World context for subsystem access
	 * @param InOwner Actor owner for component attachment
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
	void ApplySettings(const FKawaiiFluidISMRendererSettings& Settings);

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


	/** Set fluid color (creates dynamic material instance if needed) */
	void SetFluidColor(FLinearColor Color);

	//========================================
	// Configuration
	//========================================

	/** Particle mesh to use (UPROPERTY for GC) */
	UPROPERTY()
	TObjectPtr<UStaticMesh> ParticleMesh;

	/** Particle material (UPROPERTY for GC, nullptr uses mesh default) */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> ParticleMaterial;

	/** Particle scale multiplier */
	float ParticleScale = 1.0f;

	//========================================
	// Performance Options
	//========================================

	/** Maximum particles to render (memory/performance limit) */
	int32 MaxRenderParticles = 10000;

	/** Cull distance (cm) */
	float CullDistance = 10000.0f;

	/** Cast shadows */
	bool bCastShadow = false;

	//========================================
	// Visual Effects
	//========================================

	/** Enable velocity-based rotation */
	bool bRotateByVelocity = false;

	/** Enable velocity-based color */
	bool bColorByVelocity = false;

	/** Minimum velocity color */
	FLinearColor MinVelocityColor = FLinearColor::Blue;

	/** Maximum velocity color */
	FLinearColor MaxVelocityColor = FLinearColor::Red;

	/** Velocity normalization value (treat this as max velocity) */
	float MaxVelocityForColor = 1000.0f;

	//========================================
	// Enable Control
	//========================================

	/** Enable/disable this renderer */
	bool bEnabled = true;

	//========================================
	// Component Access
	//========================================

	/** ISM component instance */
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> ISMComponent;

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
	// ISM-specific Internals
	//========================================

	/** Initialize ISM component */
	void InitializeISM();

	/** Load default particle mesh */
	UStaticMesh* GetDefaultParticleMesh();

	/** Load default material */
	UMaterialInterface* GetDefaultParticleMaterial();
};
