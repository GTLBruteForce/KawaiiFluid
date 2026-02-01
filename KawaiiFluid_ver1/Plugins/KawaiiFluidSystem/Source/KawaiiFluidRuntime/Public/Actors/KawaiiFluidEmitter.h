// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KawaiiFluidEmitter.generated.h"

class AKawaiiFluidVolume;
class UKawaiiFluidEmitterComponent;

/**
 * Kawaii Fluid Emitter
 *
 * A spawn-only actor that emits particles into a target KawaiiFluidVolume.
 * This actor does NOT own simulation or rendering - it only requests particle spawns.
 *
 * All configuration properties are stored in the EmitterComponent.
 * This actor provides the world presence and simulation registration.
 *
 * Usage:
 * 1. Place AKawaiiFluidEmitter in the level
 * 2. Assign a TargetVolume via EmitterComponent
 * 3. Configure spawn parameters in the EmitterComponent
 *
 * The emitter will automatically register with its target volume and
 * use the volume's preset for particle properties (ParticleSpacing, etc.).
 */
UCLASS(Blueprintable, meta = (DisplayName = "Kawaii Fluid Emitter"))
class KAWAIIFLUIDRUNTIME_API AKawaiiFluidEmitter : public AActor
{
	GENERATED_BODY()

public:
	AKawaiiFluidEmitter();

	//========================================
	// AActor Interface
	//========================================

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	//========================================
	// Components
	//========================================

	/** Get the emitter component (contains all configuration) */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	UKawaiiFluidEmitterComponent* GetEmitterComponent() const { return EmitterComponent; }

	//========================================
	// Delegate Getters (from EmitterComponent)
	//========================================

	/** Get the target volume (delegates to EmitterComponent) */
	UFUNCTION(BlueprintPure, Category = "Target")
	AKawaiiFluidVolume* GetTargetVolume() const;

	/** Set the target volume (delegates to EmitterComponent) */
	UFUNCTION(BlueprintCallable, Category = "Target")
	void SetTargetVolume(AKawaiiFluidVolume* NewVolume);

	//========================================
	// API (Delegate to EmitterComponent)
	//========================================

	/** Burst spawn particles (delegates to EmitterComponent) */
	UFUNCTION(BlueprintCallable, Category = "Emitter")
	void BurstSpawn(int32 Count);

	/** Get the number of particles spawned by this emitter */
	UFUNCTION(BlueprintPure, Category = "Emitter")
	int32 GetSpawnedParticleCount() const;

	//========================================
	// Spawn Control API
	//========================================

	/** Start spawning particles.
	 *  Fill mode: executes SpawnFill() once.
	 *  Stream mode: starts continuous spawning. */
	UFUNCTION(BlueprintCallable, Category = "Emitter|Control")
	void StartSpawn();

	/** Stop spawning particles (Stream mode only).
	 *  Fill mode: no effect (already one-shot). */
	UFUNCTION(BlueprintCallable, Category = "Emitter|Control")
	void StopSpawn();

	/** Toggle spawn state.
	 *  If spawning, stops. If stopped, starts. */
	UFUNCTION(BlueprintCallable, Category = "Emitter|Control")
	void ToggleSpawn();

	/** Check if currently spawning (Stream mode) or has spawned (Fill mode) */
	UFUNCTION(BlueprintPure, Category = "Emitter|Control")
	bool IsSpawning() const;

	/** Set emitter enabled state */
	UFUNCTION(BlueprintCallable, Category = "Emitter|Control")
	void SetEnabled(bool bNewEnabled);

	/** Get emitter enabled state */
	UFUNCTION(BlueprintPure, Category = "Emitter|Control")
	bool IsEnabled() const;

protected:
	//========================================
	// Components
	//========================================

	/** Emitter component - handles all spawn logic and settings
	 * Configure target volume, spawn type, shape, velocity, etc. here
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UKawaiiFluidEmitterComponent> EmitterComponent;
};
