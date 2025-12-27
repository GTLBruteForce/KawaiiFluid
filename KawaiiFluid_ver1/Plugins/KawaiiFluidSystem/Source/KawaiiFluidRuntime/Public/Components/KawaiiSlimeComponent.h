// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Modules/KawaiiSlimeSimulationModule.h"
#include "Rendering/KawaiiFluidRendererSettings.h"
#include "KawaiiSlimeComponent.generated.h"

class UKawaiiFluidRenderingModule;
class UKawaiiFluidPresetDataAsset;

/**
 * Kawaii Slime Component (모듈 기반)
 *
 * 슬라임 시뮬레이션을 위한 통합 컴포넌트입니다.
 * UKawaiiSlimeSimulationModule을 통해 슬라임 전용 기능을 제공합니다.
 *
 * 사용:
 * @code
 * SlimeComponent = CreateDefaultSubobject<UKawaiiSlimeComponent>(TEXT("SlimeComponent"));
 * SlimeComponent->SlimeModule->ApplyMovementInput(Input);
 * @endcode
 */
UCLASS(ClassGroup=(KawaiiFluid), meta=(BlueprintSpawnableComponent, DisplayName="Kawaii Slime"))
class KAWAIIFLUIDRUNTIME_API UKawaiiSlimeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UKawaiiSlimeComponent();

	//========================================
	// UActorComponent Interface
	//========================================

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	//========================================
	// Modules (Blueprint 직접 접근 가능)
	//========================================

	/** 슬라임 시뮬레이션 모듈 - 슬라임 전용 API 제공 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Instanced, Category = "Slime")
	TObjectPtr<UKawaiiSlimeSimulationModule> SlimeModule;

	//========================================
	// Preset (SimulationModule에 전달)
	//========================================

	/** Fluid preset data asset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Configuration")
	TObjectPtr<UKawaiiFluidPresetDataAsset> Preset;

	//========================================
	// Rendering Settings
	//========================================

	/** Enable rendering */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Rendering")
	bool bEnableRendering = true;

	/** ISM Renderer Settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "ISM Settings"))
	FKawaiiFluidISMRendererSettings ISMSettings;

	/** SSFR Renderer Settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Rendering", meta = (EditCondition = "bEnableRendering", DisplayName = "SSFR Settings"))
	FKawaiiFluidSSFRRendererSettings SSFRSettings;

	//========================================
	// Auto Spawn
	//========================================

	/** Spawn on begin play */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Spawn")
	bool bSpawnOnBeginPlay = true;

	/** Auto spawn count */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Spawn", meta = (ClampMin = "1", ClampMax = "5000", EditCondition = "bSpawnOnBeginPlay"))
	int32 AutoSpawnCount = 100;

	/** Auto spawn radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slime|Spawn", meta = (ClampMin = "1.0", ClampMax = "500.0", EditCondition = "bSpawnOnBeginPlay"))
	float AutoSpawnRadius = 50.0f;

	//========================================
	// Blueprint API (모듈에 위임)
	//========================================

	/** Apply movement input (call from Pawn) */
	UFUNCTION(BlueprintCallable, Category = "Slime|Input")
	void ApplyMovementInput(FVector Input);

	/** Apply jump impulse */
	UFUNCTION(BlueprintCallable, Category = "Slime|Input")
	void ApplyJumpImpulse();

	/** Toggle decompose mode */
	UFUNCTION(BlueprintCallable, Category = "Slime|Input")
	void SetDecomposeMode(bool bEnable);

	/** Get center of mass for main cluster */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	FVector GetMainClusterCenter() const;

	/** Get particle count for main cluster */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	int32 GetMainClusterParticleCount() const;

	/** Check if actor is inside slime */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	bool IsActorInsideSlime(AActor* Actor) const;

	/** Check if slime is grounded */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	bool IsGrounded() const;

	/** Get nucleus position */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	FVector GetNucleusPosition() const;

	/** Get particle count */
	UFUNCTION(BlueprintCallable, Category = "Slime|Query")
	int32 GetParticleCount() const;

	//========================================
	// Events (모듈 이벤트 바인딩용)
	//========================================

	/** Fired when slime contacts ground */
	UPROPERTY(BlueprintAssignable, Category = "Slime|Events")
	FOnSlimeGroundContactModule OnGroundContact;

	/** Fired when object enters slime */
	UPROPERTY(BlueprintAssignable, Category = "Slime|Events")
	FOnSlimeObjectEnteredModule OnObjectEntered;

	/** Fired when object exits slime */
	UPROPERTY(BlueprintAssignable, Category = "Slime|Events")
	FOnSlimeObjectExitedModule OnObjectExited;

private:
	//========================================
	// Rendering Module
	//========================================

	UPROPERTY()
	TObjectPtr<UKawaiiFluidRenderingModule> RenderingModule;

	//========================================
	// Subsystem Registration
	//========================================

	void RegisterToSubsystem();
	void UnregisterFromSubsystem();

	//========================================
	// Event Binding
	//========================================

	UFUNCTION()
	void HandleGroundContact(FVector Location, FVector Normal);

	UFUNCTION()
	void HandleObjectEntered(AActor* Object);

	UFUNCTION()
	void HandleObjectExited(AActor* Object);
};
