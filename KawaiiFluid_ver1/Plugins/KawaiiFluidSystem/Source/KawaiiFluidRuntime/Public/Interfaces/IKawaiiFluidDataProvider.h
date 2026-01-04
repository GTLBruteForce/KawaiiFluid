// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "IKawaiiFluidDataProvider.generated.h"

struct FFluidParticle;
class FGPUFluidSimulator;

/**
 * UInterface (for Unreal Reflection System)
 */
UINTERFACE(MinimalAPI, Blueprintable)
class UKawaiiFluidDataProvider : public UInterface
{
	GENERATED_BODY()
};

/**
 * Fluid Simulation Data Provider Interface
 *
 * This interface provides simulation particle data to rendering modules.
 * Simulation modules implement this interface to expose their particle data
 * to the rendering layer without creating dependencies on rendering code.
 *
 * Architecture:
 * - SimulationModule (UKawaiiFluidSimulationModule) implements this interface
 * - RenderingModule (UKawaiiFluidRenderingModule) consumes the data
 * - Provides raw simulation data (FFluidParticle) without rendering concerns
 *
 * Implemented by:
 * - UKawaiiFluidSimulationModule (production simulation module)
 * - UKawaiiFluidTestDataComponent (test/dummy data provider)
 *
 * Usage example:
 * @code
 * // RenderingModule initialization
 * RenderingModule->Initialize(World, Owner, SimulationModule);
 *
 * // In rendering code
 * if (DataProvider && DataProvider->IsDataValid())
 * {
 *     const TArray<FFluidParticle>& Particles = DataProvider->GetParticles();
 *     float Radius = DataProvider->GetParticleRadius();
 *     // Render particles...
 * }
 * @endcode
 */
class IKawaiiFluidDataProvider
{
	GENERATED_BODY()

public:
	/**
	 * Get simulation particle data
	 *
	 * Returns raw simulation particle array containing position, velocity,
	 * density, adhesion state, and other simulation-specific data.
	 *
	 * @return Const reference to particle array
	 */
	virtual const TArray<FFluidParticle>& GetParticles() const = 0;

	/**
	 * Get particle count
	 * @return Number of active particles in simulation
	 */
	virtual int32 GetParticleCount() const = 0;

	/**
	 * Get particle radius used in simulation (cm)
	 *
	 * Returns the actual particle radius used for physics calculations.
	 * This is NOT a rendering-specific scale - renderers may apply additional
	 * scaling based on their own settings.
	 *
	 * @return Simulation particle radius in centimeters
	 */
	virtual float GetParticleRadius() const = 0;

	/**
	 * Check if data is valid for rendering
	 * @return True if particle data is available and ready to render
	 */
	virtual bool IsDataValid() const = 0;

	/**
	 * Get debug name for profiling/logging
	 * @return Human-readable identifier for this data provider
	 */
	virtual FString GetDebugName() const = 0;

	//========================================
	// GPU Buffer Access (Phase 2)
	//========================================

	/**
	 * Check if GPU simulation is active
	 * When true, renderers should use GPU buffer directly instead of CPU particles
	 * @return True if GPU simulation is running
	 */
	virtual bool IsGPUSimulationActive() const { return false; }

	/**
	 * Get GPU particle count
	 * @return Number of particles currently in GPU buffer
	 */
	virtual int32 GetGPUParticleCount() const { return 0; }

	/**
	 * Get GPU fluid simulator instance (for advanced GPU operations)
	 * Use this to access GPU buffers directly: GetGPUSimulator()->GetParticleSRV()
	 * @return Pointer to GPU simulator, or nullptr if not using GPU
	 */
	virtual FGPUFluidSimulator* GetGPUSimulator() const { return nullptr; }
};

