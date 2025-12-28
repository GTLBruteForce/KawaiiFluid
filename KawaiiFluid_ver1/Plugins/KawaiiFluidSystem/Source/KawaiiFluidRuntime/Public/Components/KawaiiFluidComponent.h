// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/KawaiiFluidSimulationTypes.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "KawaiiFluidComponent.generated.h"

class UKawaiiFluidRenderingModule;

/**
 * Particle hit event delegate
 * Called when a particle collides with an actor
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(
	FOnFluidParticleHitComponent,
	int32, ParticleIndex,
	AActor*, HitActor,
	FVector, HitLocation,
	FVector, HitNormal,
	float, HitSpeed
);

/**
 * Kawaii Fluid Component (통합 컴포넌트)
 *
 * 유체 시뮬레이션을 위한 통합 컴포넌트입니다.
 * 모듈 기반 설계로 시뮬레이션/렌더링/충돌을 분리하여 관리합니다.
 *
 * 시뮬레이션 API는 SimulationModule을 통해 직접 접근:
 * - Component->SimulationModule->SpawnParticles(...)
 * - Component->SimulationModule->ApplyExternalForce(...)
 *
 * 사용:
 * @code
 * FluidComponent = CreateDefaultSubobject<UKawaiiFluidComponent>(TEXT("FluidComponent"));
 * FluidComponent->Preset = MyPreset;
 *
 * // Blueprint/C++에서 모듈 직접 접근
 * FluidComponent->SimulationModule->SpawnParticles(Location, 100, 50.0f);
 * @endcode
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent, DisplayName="Kawaii Fluid"))
class KAWAIIFLUIDRUNTIME_API UKawaiiFluidComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UKawaiiFluidComponent();

	//========================================
	// UActorComponent Interface
	//========================================

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	//========================================
	// Modules (Blueprint 직접 접근 가능)
	//========================================

	/** 시뮬레이션 모듈 - 파티클/콜라이더/외력 등 모든 API 제공 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Instanced, Category = "Fluid")
	TObjectPtr<UKawaiiFluidSimulationModule> SimulationModule;

	//========================================
	// Rendering Settings
	//========================================

	/** Enable rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering")
	bool bEnableRendering = true;

	/** ISM Renderer Settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "ISM Settings"))
	FKawaiiFluidISMRendererSettings ISMSettings;

	/** SSFR Renderer Settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "SSFR Settings"))
	FKawaiiFluidSSFRRendererSettings SSFRSettings;

	//========================================
	// Auto Spawn
	//========================================

	/** Spawn on begin play */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn")
	bool bSpawnOnBeginPlay = false;

	/** Auto spawn count */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "1", ClampMax = "5000", EditCondition = "bSpawnOnBeginPlay"))
	int32 AutoSpawnCount = 100;

	/** Auto spawn radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "1.0", ClampMax = "500.0", EditCondition = "bSpawnOnBeginPlay"))
	float AutoSpawnRadius = 50.0f;

	//========================================
	// Continuous Spawn
	//========================================

	/** Enable continuous particle spawning */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn")
	bool bContinuousSpawn = true;

	/** Particles to spawn per second */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "0.1", ClampMax = "1000.0", EditCondition = "bContinuousSpawn"))
	float ParticlesPerSecond = 10.0f;

	/** Maximum particle count (0 = unlimited) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "0", EditCondition = "bContinuousSpawn"))
	int32 MaxParticleCount = 500;

	/** Spawn radius for continuous spawn */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (ClampMin = "0.0", ClampMax = "100.0", EditCondition = "bContinuousSpawn"))
	float ContinuousSpawnRadius = 10.0f;

	/** Spawn offset from component location */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (EditCondition = "bContinuousSpawn"))
	FVector SpawnOffset = FVector::ZeroVector;

	/** Initial velocity for spawned particles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid|Spawn", meta = (EditCondition = "bContinuousSpawn"))
	FVector SpawnVelocity = FVector::ZeroVector;

	//========================================
	// Events
	//========================================

	/** Particle hit event (Blueprint bindable) */
	UPROPERTY(BlueprintAssignable, Category = "Fluid|Events")
	FOnFluidParticleHitComponent OnParticleHit;

private:
	//========================================
	// Rendering Module (Internal)
	//========================================

	/** Rendering module - renderer management */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidRenderingModule> RenderingModule;

	//========================================
	// Continuous Spawn
	//========================================

	float SpawnAccumulatedTime = 0.0f;
	void ProcessContinuousSpawn(float DeltaTime);

	//========================================
	// Event System
	//========================================

	/** Handle collision event from Module callback */
	void HandleCollisionEvent(const FKawaiiFluidCollisionEvent& Event);

	//=========================`===============
	// Subsystem Registration
	//========================================

	void RegisterToSubsystem();
	void UnregisterFromSubsystem();
};
