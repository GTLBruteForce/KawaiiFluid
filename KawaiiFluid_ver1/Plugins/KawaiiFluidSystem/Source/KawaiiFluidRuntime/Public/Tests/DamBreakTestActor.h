// Copyright KawaiiFluid Team. All Rights Reserved.
// Dam Break Scenario Test Actor
// Validates basic fluid dynamics behavior based on PBF paper

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Tests/FluidTestMetrics.h"
#include "DamBreakTestActor.generated.h"

class UKawaiiFluidComponent;
class UKawaiiFluidSimulationModule;
class UBoxComponent;
class UKawaiiFluidPresetDataAsset;

/**
 * Test validation result
 */
UENUM(BlueprintType)
enum class EDamBreakTestResult : uint8
{
	NotStarted,
	InProgress,
	Passed,
	Failed
};

/**
 * Dam Break test checkpoint
 */
USTRUCT(BlueprintType)
struct FDamBreakCheckpoint
{
	GENERATED_BODY()

	/** Time at which to check (seconds since start) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Checkpoint")
	float Time = 0.0f;

	/** Expected behavior description */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Checkpoint")
	FString Description;

	/** Has this checkpoint been passed? */
	UPROPERTY(BlueprintReadOnly, Category = "Checkpoint")
	bool bPassed = false;

	/** Metrics captured at this checkpoint */
	UPROPERTY(BlueprintReadOnly, Category = "Checkpoint")
	FFluidTestMetrics CapturedMetrics;
};

/**
 * ADamBreakTestActor
 *
 * Test actor that creates a classic dam break scenario for validating
 * Position Based Fluids implementation. Places a block of fluid particles
 * against one wall and releases them to flow across the container.
 *
 * Expected Behavior (from PBF paper):
 * - 0.0s: Particles in stationary block formation
 * - 0.5s: Block collapses, spreading along floor
 * - 1.0s: Leading edge reaches opposite wall
 * - 2.0s: Rebound wave travels back
 * - 5.0s: Equilibrium reached (uniform layer at bottom)
 *
 * Validation Criteria:
 * - Density maintained within 90-110% of RestDensity
 * - Maximum density under 200% of RestDensity
 * - Volume conserved within ±20%
 * - No particles escape bounds
 * - Numerical stability (no NaN/Inf values)
 */
UCLASS(Blueprintable, ClassGroup = "KawaiiFluid|Tests")
class KAWAIIFLUIDRUNTIME_API ADamBreakTestActor : public AActor
{
	GENERATED_BODY()

public:
	ADamBreakTestActor();

	//=========================================================================
	// Test Configuration
	//=========================================================================

	/** Fluid preset to use for simulation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Setup")
	TObjectPtr<UKawaiiFluidPresetDataAsset> FluidPreset;

	/** Number of particles to spawn (default 1000 for quick test) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Setup", meta = (ClampMin = "100", ClampMax = "50000"))
	int32 ParticleCount = 1000;

	/** Container dimensions (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Setup")
	FVector ContainerSize = FVector(200.0f, 100.0f, 100.0f);

	/** Initial fluid block size ratio (0-1 of container) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Setup", meta = (ClampMin = "0.1", ClampMax = "0.9"))
	FVector FluidBlockRatio = FVector(0.3f, 1.0f, 0.8f);

	/** Test duration in seconds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Setup", meta = (ClampMin = "1.0", ClampMax = "60.0"))
	float TestDuration = 10.0f;

	/** Particle spacing multiplier (1.0 = optimal for rest density) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Setup", meta = (ClampMin = "0.3", ClampMax = "1.0"))
	float ParticleSpacingMultiplier = 0.5f;

	//=========================================================================
	// Validation Thresholds
	//=========================================================================

	/** Acceptable density range (percentage of RestDensity) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Validation")
	float DensityTolerancePercent = 10.0f;

	/** Maximum allowed density (percentage of RestDensity) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Validation")
	float MaxDensityPercent = 200.0f;

	/** Volume conservation tolerance (percentage) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Validation")
	float VolumeTolerancePercent = 20.0f;

	/** Equilibrium velocity threshold (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Validation")
	float EquilibriumVelocityThreshold = 5.0f;

	//=========================================================================
	// Test State
	//=========================================================================

	/** Current test result */
	UPROPERTY(BlueprintReadOnly, Category = "Test|State")
	EDamBreakTestResult TestResult = EDamBreakTestResult::NotStarted;

	/** Time elapsed since test started */
	UPROPERTY(BlueprintReadOnly, Category = "Test|State")
	float ElapsedTime = 0.0f;

	/** Reason for failure (if failed) */
	UPROPERTY(BlueprintReadOnly, Category = "Test|State")
	FString FailureReason;

	/** Test checkpoints */
	UPROPERTY(BlueprintReadOnly, Category = "Test|State")
	TArray<FDamBreakCheckpoint> Checkpoints;

	/** Metrics history for analysis */
	UPROPERTY(BlueprintReadOnly, Category = "Test|State")
	FFluidTestMetricsHistory MetricsHistory;

	/** Initial volume for conservation check */
	UPROPERTY(BlueprintReadOnly, Category = "Test|State")
	float InitialVolume = 0.0f;

	//=========================================================================
	// Debug Visualization
	//=========================================================================

	/** Show container bounds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Debug")
	bool bShowContainerBounds = true;

	/** Show metrics on screen */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Debug")
	bool bShowMetricsOnScreen = true;

	/** Log metrics to output log */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Debug")
	bool bLogMetrics = true;

	/** Metrics logging interval (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Debug", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float MetricsLogInterval = 0.5f;

	//=========================================================================
	// Blueprint Events
	//=========================================================================

	/** Called when test starts */
	UFUNCTION(BlueprintImplementableEvent, Category = "Test|Events")
	void OnTestStarted();

	/** Called when a checkpoint is reached */
	UFUNCTION(BlueprintImplementableEvent, Category = "Test|Events")
	void OnCheckpointReached(const FDamBreakCheckpoint& Checkpoint);

	/** Called when test completes (pass or fail) */
	UFUNCTION(BlueprintImplementableEvent, Category = "Test|Events")
	void OnTestCompleted(EDamBreakTestResult Result, const FString& Message);

	/** Called each frame with current metrics */
	UFUNCTION(BlueprintImplementableEvent, Category = "Test|Events")
	void OnMetricsUpdated(const FFluidTestMetrics& Metrics);

	//=========================================================================
	// Public Methods
	//=========================================================================

	/** Start the dam break test */
	UFUNCTION(BlueprintCallable, Category = "Test")
	void StartTest();

	/** Stop the test */
	UFUNCTION(BlueprintCallable, Category = "Test")
	void StopTest();

	/** Reset test to initial state */
	UFUNCTION(BlueprintCallable, Category = "Test")
	void ResetTest();

	/** Get current metrics */
	UFUNCTION(BlueprintPure, Category = "Test")
	FFluidTestMetrics GetCurrentMetrics() const;

	/** Get test progress (0-1) */
	UFUNCTION(BlueprintPure, Category = "Test")
	float GetTestProgress() const;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** The fluid component */
	UPROPERTY()
	TObjectPtr<UKawaiiFluidComponent> FluidComponent;

	/** Container collision bounds */
	UPROPERTY()
	TObjectPtr<UBoxComponent> ContainerBounds;

	/** Cached rest density */
	float RestDensity = 1000.0f;

	/** Time since last metrics log */
	float TimeSinceLastLog = 0.0f;

	/** Initialize test checkpoints */
	void InitializeCheckpoints();

	/** Spawn particles in block formation */
	void SpawnParticles();

	/** Collect current metrics */
	void CollectMetrics();

	/** Validate current state against criteria */
	bool ValidateState(const FFluidTestMetrics& Metrics);

	/** Check and process checkpoints */
	void ProcessCheckpoints();

	/** Mark test as failed with reason */
	void FailTest(const FString& Reason);

	/** Mark test as passed */
	void PassTest();

	/** Draw debug visualization */
	void DrawDebugInfo();
};
