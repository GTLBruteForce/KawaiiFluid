// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KawaiiFluidEmitterTrigger.generated.h"

class AKawaiiFluidEmitter;
class AKawaiiFluidVolume;
class UBoxComponent;
class UBillboardComponent;

/**
 * Trigger action type for KawaiiFluidEmitterTrigger
 */
UENUM(BlueprintType)
enum class EKawaiiFluidTriggerAction : uint8
{
	/** Start spawning when triggered */
	Start UMETA(DisplayName = "Start Spawn"),

	/** Stop spawning when triggered */
	Stop UMETA(DisplayName = "Stop Spawn"),

	/** Toggle spawn state when triggered */
	Toggle UMETA(DisplayName = "Toggle Spawn")
};

/**
 * Kawaii Fluid Emitter Trigger
 *
 * A trigger actor that controls a KawaiiFluidEmitter based on overlap events.
 * Place in the world, assign a target emitter, and the trigger will automatically
 * start/stop/toggle spawning when actors enter the trigger volume.
 *
 * Usage:
 * 1. Place AKawaiiFluidEmitterTrigger in the level
 * 2. Set TargetEmitter to the emitter you want to control
 * 3. Configure TriggerAction and other settings
 * 4. Adjust the trigger box size in the viewport
 *
 * The target emitter should have bAutoStartSpawning = false for manual control.
 */
UCLASS(Blueprintable, meta = (DisplayName = "Kawaii Fluid Emitter Trigger"))
class KAWAIIFLUIDRUNTIME_API AKawaiiFluidEmitterTrigger : public AActor
{
	GENERATED_BODY()

public:
	AKawaiiFluidEmitterTrigger();

	//========================================
	// AActor Interface
	//========================================

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	//========================================
	// Trigger Settings
	//========================================

	/** The emitters to control when triggered.
	 *  Assign these in the editor by selecting emitters from the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger")
	TArray<TObjectPtr<AKawaiiFluidEmitter>> TargetEmitters;

	/** Action to perform when an actor enters the trigger */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger")
	EKawaiiFluidTriggerAction TriggerAction = EKawaiiFluidTriggerAction::Start;

	/** If true, automatically stop spawning when the actor exits the trigger.
	 *  Only applies when TriggerAction is Start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger",
		meta = (EditCondition = "TriggerAction == EKawaiiFluidTriggerAction::Start", EditConditionHides))
	bool bStopOnExit = true;

	/** If true, clear all particles when the actor exits the trigger.
	 *  Useful for demo maps where you want particles to disappear when leaving the area.
	 *  Sends clear requests over multiple frames to handle GPU readback latency. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger",
		meta = (EditCondition = "TriggerAction == EKawaiiFluidTriggerAction::Start", EditConditionHides))
	bool bClearParticlesOnExit = false;

	/** Number of frames to repeat the clear request on exit.
	 *  GPU despawn is async, so repeating ensures all particles are caught. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger",
		meta = (EditCondition = "bClearParticlesOnExit", EditConditionHides, ClampMin = "1", ClampMax = "30"))
	int32 ClearParticleFrameCount = 5;

	/** If true, only the player pawn can trigger this.
	 *  If false, any actor with collision can trigger. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger")
	bool bOnlyPlayer = true;

	/** Trigger box extent (half-size in each axis) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trigger|Shape",
		meta = (MakeEditWidget = true))
	FVector BoxExtent = FVector(100.0f, 100.0f, 100.0f);

	//========================================
	// Components
	//========================================

	/** Get the trigger box component */
	UFUNCTION(BlueprintPure, Category = "Trigger")
	UBoxComponent* GetTriggerBox() const { return TriggerBox; }

	//========================================
	// Manual Trigger API
	//========================================

	/** Manually execute the trigger action (for BP scripting) */
	UFUNCTION(BlueprintCallable, Category = "Trigger")
	void ExecuteTriggerAction();

	/** Manually execute exit action (for BP scripting) */
	UFUNCTION(BlueprintCallable, Category = "Trigger")
	void ExecuteExitAction();

protected:
	//========================================
	// Components
	//========================================

	/** Root scene component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> SceneRoot;

	/** Trigger box for overlap detection */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UBoxComponent> TriggerBox;

#if WITH_EDITORONLY_DATA
	/** Billboard icon for editor visualization */
	UPROPERTY(Transient)
	TObjectPtr<UBillboardComponent> BillboardComponent;
#endif

	//========================================
	// Overlap Handlers
	//========================================

	/** Called when an actor enters the trigger */
	UFUNCTION()
	void OnTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	/** Called when an actor exits the trigger */
	UFUNCTION()
	void OnTriggerEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

	//========================================
	// Internal Helpers
	//========================================

	/** Check if the overlapping actor should trigger */
	bool ShouldTriggerFor(AActor* OtherActor) const;

	/** Remaining frames to send clear requests */
	int32 ClearFramesRemaining = 0;

	/** Update trigger box extent when property changes */
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
