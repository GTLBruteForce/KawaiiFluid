// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Tests/DamBreakTestActor.h"
#include "Tests/FluidMetricsCollector.h"
#include "Components/KawaiiFluidComponent.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Components/BoxComponent.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"

ADamBreakTestActor::ADamBreakTestActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// Create root component
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// Create container bounds visualization
	ContainerBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("ContainerBounds"));
	ContainerBounds->SetupAttachment(RootComponent);
	ContainerBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ContainerBounds->SetBoxExtent(ContainerSize * 0.5f);
	ContainerBounds->SetVisibility(true);
	ContainerBounds->SetHiddenInGame(false);

	// Create fluid component
	FluidComponent = CreateDefaultSubobject<UKawaiiFluidComponent>(TEXT("FluidComponent"));
	FluidComponent->SetupAttachment(RootComponent);
}

void ADamBreakTestActor::BeginPlay()
{
	Super::BeginPlay();

	// Initialize checkpoints
	InitializeCheckpoints();

	// Update container bounds visualization
	if (ContainerBounds)
	{
		ContainerBounds->SetBoxExtent(ContainerSize * 0.5f);
	}

	// Configure fluid component with preset
	if (FluidComponent && FluidPreset)
	{
		if (UKawaiiFluidSimulationModule* Module = FluidComponent->GetSimulationModule())
		{
			Module->SetPreset(FluidPreset);
		}
	}
}

void ADamBreakTestActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (TestResult != EDamBreakTestResult::InProgress)
	{
		return;
	}

	ElapsedTime += DeltaTime;

	// Collect metrics
	CollectMetrics();

	// Get current metrics
	FFluidTestMetrics CurrentMetrics = GetCurrentMetrics();

	// Notify Blueprint
	OnMetricsUpdated(CurrentMetrics);

	// Validate state
	if (!ValidateState(CurrentMetrics))
	{
		// Test failed - FailTest was already called in ValidateState
		return;
	}

	// Process checkpoints
	ProcessCheckpoints();

	// Log metrics periodically
	TimeSinceLastLog += DeltaTime;
	if (bLogMetrics && TimeSinceLastLog >= MetricsLogInterval)
	{
		TimeSinceLastLog = 0.0f;
		UE_LOG(LogTemp, Log, TEXT("[DamBreak] t=%.2fs: %s"),
			ElapsedTime, *CurrentMetrics.GetSummary());
	}

	// Draw debug info
	if (bShowContainerBounds || bShowMetricsOnScreen)
	{
		DrawDebugInfo();
	}

	// Check if test duration completed
	if (ElapsedTime >= TestDuration)
	{
		// Final validation
		if (FFluidMetricsCollector::IsInEquilibrium(MetricsHistory, EquilibriumVelocityThreshold))
		{
			PassTest();
		}
		else
		{
			// Still moving but time's up - check if generally stable
			if (CurrentMetrics.IsNumericallyStable() &&
			    CurrentMetrics.IsDensityStable(RestDensity, DensityTolerancePercent * 1.5f))
			{
				PassTest();
			}
			else
			{
				FailTest(TEXT("Did not reach equilibrium within test duration"));
			}
		}
	}
}

#if WITH_EDITOR
void ADamBreakTestActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Update container bounds when size changes
	if (ContainerBounds && PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ADamBreakTestActor, ContainerSize))
	{
		ContainerBounds->SetBoxExtent(ContainerSize * 0.5f);
	}
}
#endif

void ADamBreakTestActor::InitializeCheckpoints()
{
	Checkpoints.Empty();

	// Checkpoint 1: Initial collapse (0.5s)
	{
		FDamBreakCheckpoint CP;
		CP.Time = 0.5f;
		CP.Description = TEXT("Block collapse initiated - particles spreading along floor");
		Checkpoints.Add(CP);
	}

	// Checkpoint 2: Mid-flow (1.0s)
	{
		FDamBreakCheckpoint CP;
		CP.Time = 1.0f;
		CP.Description = TEXT("Leading edge advancing toward opposite wall");
		Checkpoints.Add(CP);
	}

	// Checkpoint 3: Wall impact (2.0s)
	{
		FDamBreakCheckpoint CP;
		CP.Time = 2.0f;
		CP.Description = TEXT("Impact with opposite wall, rebound wave forming");
		Checkpoints.Add(CP);
	}

	// Checkpoint 4: Settling (5.0s)
	{
		FDamBreakCheckpoint CP;
		CP.Time = 5.0f;
		CP.Description = TEXT("Oscillations damping, approaching equilibrium");
		Checkpoints.Add(CP);
	}

	// Checkpoint 5: Equilibrium (test duration)
	{
		FDamBreakCheckpoint CP;
		CP.Time = TestDuration;
		CP.Description = TEXT("Final equilibrium - uniform layer at container bottom");
		Checkpoints.Add(CP);
	}
}

void ADamBreakTestActor::SpawnParticles()
{
	if (!FluidComponent)
	{
		UE_LOG(LogTemp, Error, TEXT("[DamBreak] No fluid component found"));
		return;
	}

	UKawaiiFluidSimulationModule* Module = FluidComponent->GetSimulationModule();
	if (!Module)
	{
		UE_LOG(LogTemp, Error, TEXT("[DamBreak] No simulation module found"));
		return;
	}

	// Get smoothing radius from preset
	float SmoothingRadius = 20.0f;  // Default
	if (const UKawaiiFluidPresetDataAsset* Preset = Module->GetPreset())
	{
		SmoothingRadius = Preset->SmoothingRadius;
		RestDensity = Preset->RestDensity;
	}

	// Calculate particle spacing (typically 0.5 * h for rest density)
	const float Spacing = SmoothingRadius * ParticleSpacingMultiplier;

	// Calculate block dimensions
	const FVector BlockSize = ContainerSize * FluidBlockRatio;

	// Calculate start position (bottom-left corner of container, offset for block)
	const FVector ActorLoc = GetActorLocation();
	const FVector ContainerMin = ActorLoc - ContainerSize * 0.5f;
	const FVector BlockCenter = ContainerMin + FVector(BlockSize.X * 0.5f, ContainerSize.Y * 0.5f, BlockSize.Z * 0.5f);
	const FVector BlockExtent = BlockSize * 0.5f;

	UE_LOG(LogTemp, Log, TEXT("[DamBreak] Spawning particles (block: %.0f x %.0f x %.0f cm, spacing: %.1f cm)"),
		BlockSize.X, BlockSize.Y, BlockSize.Z, Spacing);

	// Spawn particles using box distribution
	int32 SpawnedCount = Module->SpawnParticlesBoxByCount(
		BlockCenter,
		BlockExtent,
		ParticleCount,
		true,  // bJitter
		0.2f,  // JitterAmount
		FVector::ZeroVector,  // Initial velocity
		FRotator::ZeroRotator
	);

	// Calculate initial volume
	const float ParticleMass = 1.0f;  // Assuming unit mass
	InitialVolume = (SpawnedCount * ParticleMass / RestDensity) * 1e6f;  // cm³

	UE_LOG(LogTemp, Log, TEXT("[DamBreak] Spawned %d particles, Initial volume: %.2f cm³"),
		SpawnedCount, InitialVolume);

	// Setup containment for the fluid
	if (Module)
	{
		Module->SetContainment(
			true,
			ActorLoc,
			ContainerSize * 0.5f,
			FQuat::Identity,
			0.3f,  // Restitution
			0.1f   // Friction
		);
	}
}

void ADamBreakTestActor::CollectMetrics()
{
	if (!FluidComponent)
	{
		return;
	}

	// Collect metrics from component
	FFluidTestMetrics Metrics = FFluidMetricsCollector::CollectFromComponent(FluidComponent);
	Metrics.SimulationElapsedTime = ElapsedTime;
	Metrics.FrameNumber = GFrameCounter;

	// Add to history
	MetricsHistory.AddSample(Metrics);
}

FFluidTestMetrics ADamBreakTestActor::GetCurrentMetrics() const
{
	if (MetricsHistory.Samples.Num() > 0)
	{
		return MetricsHistory.Samples.Last();
	}
	return FFluidTestMetrics();
}

float ADamBreakTestActor::GetTestProgress() const
{
	if (TestDuration <= 0.0f)
	{
		return 0.0f;
	}
	return FMath::Clamp(ElapsedTime / TestDuration, 0.0f, 1.0f);
}

bool ADamBreakTestActor::ValidateState(const FFluidTestMetrics& Metrics)
{
	// Check numerical stability
	if (Metrics.InvalidParticles > 0)
	{
		FailTest(FString::Printf(TEXT("Numerical instability: %d particles with NaN/Inf values"),
			Metrics.InvalidParticles));
		return false;
	}

	// Check bounds
	if (Metrics.ParticlesOutOfBounds > 0)
	{
		// Only fail if significant number escaped
		const float EscapeRatio = static_cast<float>(Metrics.ParticlesOutOfBounds) / FMath::Max(1, Metrics.ParticleCount);
		if (EscapeRatio > 0.01f)  // More than 1% escaped
		{
			FailTest(FString::Printf(TEXT("Boundary violation: %d particles (%.1f%%) escaped bounds"),
				Metrics.ParticlesOutOfBounds, EscapeRatio * 100.0f));
			return false;
		}
	}

	// Check maximum density (prevent extreme compression)
	const float MaxAllowedDensity = RestDensity * (MaxDensityPercent / 100.0f);
	if (Metrics.MaxDensity > MaxAllowedDensity)
	{
		FailTest(FString::Printf(TEXT("Extreme compression: Max density %.1f exceeds %.1f (%.0f%% of rest)"),
			Metrics.MaxDensity, MaxAllowedDensity, MaxDensityPercent));
		return false;
	}

	// Check volume conservation (only after settling starts, ~2 seconds)
	if (ElapsedTime > 2.0f && InitialVolume > 0.0f)
	{
		const float VolumeRatio = Metrics.TotalVolume / InitialVolume;
		const float LowerBound = 1.0f - (VolumeTolerancePercent / 100.0f);
		const float UpperBound = 1.0f + (VolumeTolerancePercent / 100.0f);

		if (VolumeRatio < LowerBound || VolumeRatio > UpperBound)
		{
			FailTest(FString::Printf(TEXT("Volume conservation failed: %.1f%% of initial (expected %.0f-%.0f%%)"),
				VolumeRatio * 100.0f, LowerBound * 100.0f, UpperBound * 100.0f));
			return false;
		}
	}

	return true;
}

void ADamBreakTestActor::ProcessCheckpoints()
{
	for (FDamBreakCheckpoint& Checkpoint : Checkpoints)
	{
		if (!Checkpoint.bPassed && ElapsedTime >= Checkpoint.Time)
		{
			Checkpoint.bPassed = true;
			Checkpoint.CapturedMetrics = GetCurrentMetrics();

			UE_LOG(LogTemp, Log, TEXT("[DamBreak] Checkpoint at t=%.1fs: %s"),
				Checkpoint.Time, *Checkpoint.Description);
			UE_LOG(LogTemp, Log, TEXT("  Metrics: %s"), *Checkpoint.CapturedMetrics.GetSummary());

			OnCheckpointReached(Checkpoint);
		}
	}
}

void ADamBreakTestActor::StartTest()
{
	if (TestResult == EDamBreakTestResult::InProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DamBreak] Test already in progress"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[DamBreak] Starting Dam Break test"));
	UE_LOG(LogTemp, Log, TEXT("  Container: %.0f x %.0f x %.0f cm"), ContainerSize.X, ContainerSize.Y, ContainerSize.Z);
	UE_LOG(LogTemp, Log, TEXT("  Particles: %d"), ParticleCount);
	UE_LOG(LogTemp, Log, TEXT("  Duration: %.1f s"), TestDuration);

	// Reset state
	ResetTest();

	// Spawn particles
	SpawnParticles();

	// Enable ticking
	SetActorTickEnabled(true);

	// Set test state
	TestResult = EDamBreakTestResult::InProgress;

	OnTestStarted();
}

void ADamBreakTestActor::StopTest()
{
	SetActorTickEnabled(false);

	if (TestResult == EDamBreakTestResult::InProgress)
	{
		TestResult = EDamBreakTestResult::NotStarted;
		UE_LOG(LogTemp, Log, TEXT("[DamBreak] Test stopped"));
	}
}

void ADamBreakTestActor::ResetTest()
{
	StopTest();

	// Clear metrics history
	MetricsHistory.Clear();

	// Reset checkpoints
	for (FDamBreakCheckpoint& Checkpoint : Checkpoints)
	{
		Checkpoint.bPassed = false;
		Checkpoint.CapturedMetrics = FFluidTestMetrics();
	}

	// Reset state
	ElapsedTime = 0.0f;
	TimeSinceLastLog = 0.0f;
	InitialVolume = 0.0f;
	FailureReason.Empty();
	TestResult = EDamBreakTestResult::NotStarted;

	// Clear particles
	if (FluidComponent)
	{
		FluidComponent->ClearAllParticles();
	}

	UE_LOG(LogTemp, Log, TEXT("[DamBreak] Test reset"));
}

void ADamBreakTestActor::FailTest(const FString& Reason)
{
	TestResult = EDamBreakTestResult::Failed;
	FailureReason = Reason;

	UE_LOG(LogTemp, Error, TEXT("[DamBreak] TEST FAILED at t=%.2fs: %s"), ElapsedTime, *Reason);

	SetActorTickEnabled(false);
	OnTestCompleted(TestResult, Reason);
}

void ADamBreakTestActor::PassTest()
{
	TestResult = EDamBreakTestResult::Passed;

	const FFluidTestMetrics FinalMetrics = GetCurrentMetrics();
	const FString Message = FString::Printf(
		TEXT("Test completed successfully. Final: %s"),
		*FinalMetrics.GetSummary());

	UE_LOG(LogTemp, Log, TEXT("[DamBreak] TEST PASSED at t=%.2fs"), ElapsedTime);
	UE_LOG(LogTemp, Log, TEXT("  %s"), *Message);

	SetActorTickEnabled(false);
	OnTestCompleted(TestResult, Message);
}

void ADamBreakTestActor::DrawDebugInfo()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector ActorLoc = GetActorLocation();

	// Draw container bounds
	if (bShowContainerBounds)
	{
		DrawDebugBox(World, ActorLoc, ContainerSize * 0.5f, FColor::White, false, -1.0f, 0, 2.0f);

		// Draw initial fluid block region
		const FVector BlockSize = ContainerSize * FluidBlockRatio;
		const FVector ContainerMin = ActorLoc - ContainerSize * 0.5f;
		const FVector BlockCenter = ContainerMin + FVector(BlockSize.X * 0.5f, ContainerSize.Y * 0.5f, BlockSize.Z * 0.5f);

		DrawDebugBox(World, BlockCenter, BlockSize * 0.5f, FColor::Blue, false, -1.0f, 0, 1.0f);
	}

	// Draw metrics on screen
	if (bShowMetricsOnScreen && GEngine)
	{
		const FFluidTestMetrics Metrics = GetCurrentMetrics();

		FString StatusStr;
		FColor StatusColor = FColor::White;

		switch (TestResult)
		{
		case EDamBreakTestResult::NotStarted:
			StatusStr = TEXT("NOT STARTED");
			StatusColor = FColor::White;
			break;
		case EDamBreakTestResult::InProgress:
			StatusStr = TEXT("IN PROGRESS");
			StatusColor = FColor::Yellow;
			break;
		case EDamBreakTestResult::Passed:
			StatusStr = TEXT("PASSED");
			StatusColor = FColor::Green;
			break;
		case EDamBreakTestResult::Failed:
			StatusStr = TEXT("FAILED");
			StatusColor = FColor::Red;
			break;
		}

		GEngine->AddOnScreenDebugMessage(-1, 0.0f, StatusColor,
			FString::Printf(TEXT("Dam Break Test: %s (%.1f%%)"), *StatusStr, GetTestProgress() * 100.0f));

		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::White,
			FString::Printf(TEXT("Time: %.2f / %.2f s"), ElapsedTime, TestDuration));

		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("Particles: %d | Density: %.1f (%.1f%%)"),
				Metrics.ParticleCount, Metrics.AverageDensity, Metrics.DensityRatio * 100.0f));

		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("Velocity: %.1f cm/s | Lambda: %.4f"),
				Metrics.AverageVelocity, Metrics.AverageLambda));

		if (TestResult == EDamBreakTestResult::Failed)
		{
			GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Red, FailureReason);
		}
	}
}
