// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Components/KawaiiFluidEmitterComponent.h"
#include "Actors/KawaiiFluidEmitter.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Core/KawaiiFluidSimulationStats.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "DrawDebugHelpers.h"
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"  // For TActorIterator (editor volume finding)
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

UKawaiiFluidEmitterComponent::UKawaiiFluidEmitterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	bTickInEditor = true;  // Enable tick in editor for wireframe visualization
}

void UKawaiiFluidEmitterComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITORONLY_DATA

	// Check if we need to create BillboardComponent
	const bool bNeedBillboard = !BillboardComponent ||
		(BillboardComponent->GetOwner() != GetOwner()) ||
		(BillboardComponent->GetAttachParent() != this);

	if (bNeedBillboard && GetWorld() && !GetWorld()->IsGameWorld())
	{
		// Clear stale reference (may point to original component after copy)
		BillboardComponent = nullptr;

		BillboardComponent = NewObject<UBillboardComponent>(GetOwner(), NAME_None, RF_Transactional);
		if (BillboardComponent)
		{
			BillboardComponent->SetupAttachment(this);
			BillboardComponent->bIsScreenSizeScaled = true;
			BillboardComponent->SetRelativeScale3D(FVector(0.3f));

			// Load custom emitter icon texture
			static UTexture2D* CachedIcon = LoadObject<UTexture2D>(
				nullptr, TEXT("/KawaiiFluidSystem/Textures/T_KawaiiFluidEmitter_Icon"));
			if (CachedIcon)
			{
				BillboardComponent->SetSprite(CachedIcon);
			}

			BillboardComponent->RegisterComponent();
		}
	}

	// Same logic for VelocityArrow
	const bool bNeedArrow = !VelocityArrow ||
		(VelocityArrow->GetOwner() != GetOwner()) ||
		(VelocityArrow->GetAttachParent() != this);

	if (bNeedArrow && GetWorld() && !GetWorld()->IsGameWorld())
	{
		VelocityArrow = nullptr;

		VelocityArrow = NewObject<UArrowComponent>(GetOwner(), NAME_None, RF_Transactional);
		if (VelocityArrow)
		{
			VelocityArrow->SetupAttachment(this);
			VelocityArrow->SetArrowColor(FLinearColor(0.0f, 0.5f, 1.0f)); // Cyan blue
			VelocityArrow->bIsScreenSizeScaled = true;
			VelocityArrow->SetVisibility(false); // Initially hidden
			VelocityArrow->RegisterComponent();
		}
	}

	// Auto-find nearest volume when placed in editor (if not already set)
	if (GetWorld() && !GetWorld()->IsGameWorld() && !TargetVolume && bAutoFindVolume)
	{
		TargetVolume = FindNearestVolume();
	}
#endif
}

void UKawaiiFluidEmitterComponent::OnUnregister()
{
#if WITH_EDITORONLY_DATA
	if (BillboardComponent)
	{
		BillboardComponent->DestroyComponent();
		BillboardComponent = nullptr;
	}

	if (VelocityArrow)
	{
		VelocityArrow->DestroyComponent();
		VelocityArrow = nullptr;
	}
#endif

	Super::OnUnregister();
}

void UKawaiiFluidEmitterComponent::BeginPlay()
{
	Super::BeginPlay();

	// Auto-find volume if not set
	if (!TargetVolume && bAutoFindVolume)
	{
		TargetVolume = FindNearestVolume();

		// If still not found, defer search to next tick (Volume's BeginPlay may not have run yet)
		if (!TargetVolume)
		{
			bPendingVolumeSearch = true;
			UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Volume not found, deferring search to next tick"),
				*GetName());
		}
	}

	// Allocate SourceID from Subsystem (0~63 range, compatible with GPU counters)
	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			CachedSourceID = Subsystem->AllocateSourceID();
			UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Allocated SourceID = %d"),
				*GetName(), CachedSourceID);
		}
	}

	RegisterToVolume();

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: BeginPlay - TargetVolume=%s"),
		*GetName(), TargetVolume ? *TargetVolume->GetName() : TEXT("None"));

	// Distance optimization: Check initial activation state
	if (bUseDistanceOptimization)
	{
		APawn* PlayerPawn = GetPlayerPawn();
		if (PlayerPawn)
		{
			const FVector PlayerLocation = PlayerPawn->GetActorLocation();
			const FVector EmitterLocation = GetComponentLocation();
			const float DistanceSq = FVector::DistSquared(PlayerLocation, EmitterLocation);

			// Initially inactive if player is outside activation distance
			bDistanceActivated = (DistanceSq <= ActivationDistance * ActivationDistance);

			UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Distance Optimization - Initial state: %s (Distance: %.1f cm, Threshold: %.1f cm)"),
				*GetName(),
				bDistanceActivated ? TEXT("Active") : TEXT("Inactive"),
				FMath::Sqrt(DistanceSq),
				ActivationDistance);
		}
		else
		{
			// Player pawn not found yet, defer check to tick
			bDistanceActivated = false;
			UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Distance Optimization - Player not found, starting inactive"),
				*GetName());
		}
	}

	// Auto start spawning (only if we have a target volume and distance allows)
	if (bEnabled && bAutoStartSpawning && TargetVolume)
	{
		// Skip if distance optimization is enabled and we're outside range
		if (bUseDistanceOptimization && !bDistanceActivated)
		{
			UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Skipping auto spawn - outside activation distance"),
				*GetName());
		}
		else
		{
			if (IsFillMode() && !bAutoSpawnExecuted)
			{
				SpawnFill();
			}
			else if (IsStreamMode())
			{
				StartStreamSpawn();
			}
		}
	}
}

void UKawaiiFluidEmitterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromVolume();

	// Release SourceID back to Subsystem
	if (CachedSourceID >= 0)
	{
		if (UWorld* World = GetWorld())
		{
			if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
			{
				Subsystem->ReleaseSourceID(CachedSourceID);
				UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Released SourceID = %d"),
					*GetName(), CachedSourceID);
			}
		}
		CachedSourceID = -1;
	}

	Super::EndPlay(EndPlayReason);
}

void UKawaiiFluidEmitterComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const bool bIsGameWorld = World->IsGameWorld();

	// Deferred volume search (handles BeginPlay order issues where Volume's BeginPlay hasn't run yet)
	if (bPendingVolumeSearch && bIsGameWorld)
	{
		bPendingVolumeSearch = false;

		if (!TargetVolume && bAutoFindVolume)
		{
			TargetVolume = FindNearestVolume();

			if (TargetVolume)
			{
				UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Deferred volume search found TargetVolume=%s"),
					*GetName(), *TargetVolume->GetName());

				// Register to the newly found volume
				RegisterToVolume();

				// Start auto spawning now that we have a volume (respecting distance optimization)
				if (bEnabled && bAutoStartSpawning)
				{
					// Skip if distance optimization is enabled and we're outside range
					if (bUseDistanceOptimization && !bDistanceActivated)
					{
						UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Skipping deferred auto spawn - outside activation distance"),
							*GetName());
					}
					else
					{
						if (IsFillMode() && !bAutoSpawnExecuted)
						{
							SpawnFill();
						}
						else if (IsStreamMode())
						{
							StartStreamSpawn();
						}
					}
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("UKawaiiFluidEmitterComponent [%s]: Deferred volume search failed - no volume found"),
					*GetName());
			}
		}
	}

	// === Distance Optimization Check ===
	if (bIsGameWorld && bUseDistanceOptimization)
	{
		UpdateDistanceOptimization(DeltaTime);

		// Skip all processing if deactivated by distance
		if (!bDistanceActivated)
		{
			return;
		}
	}

	// Request readback if this emitter needs particle count tracking (for MaxParticleCount or Recycle)
	if (bIsGameWorld && MaxParticleCount > 0)
	{
		GetFluidStatsCollector().SetReadbackRequested(true);
	}

	// Wireframe visualization (editor only, when selected and not hidden via eye icon)
#if WITH_EDITOR
	const bool bIsSelected = IsOwnerSelected();
	const bool bIsHiddenInEditor = GetOwner() && GetOwner()->IsTemporarilyHiddenInEditor();
	if (bShowSpawnVolumeWireframe && !bIsGameWorld && bIsSelected && !bIsHiddenInEditor)
	{
		DrawSpawnVolumeVisualization();
	}

	// Distance optimization visualization (editor only, when selected)
	if (bUseDistanceOptimization && !bIsGameWorld && bIsSelected && !bIsHiddenInEditor)
	{
		DrawDistanceVisualization();
	}

	// Update velocity arrow visualization (editor only, always visible)
	if (!bIsGameWorld)
	{
		UpdateVelocityArrowVisualization();
	}
#endif

	// Process continuous spawning for Stream mode (game world only)
	if (bEnabled && bIsGameWorld && IsStreamMode() && bStreamSpawning)
	{
		ProcessContinuousSpawn(DeltaTime);
	}
}

AKawaiiFluidEmitter* UKawaiiFluidEmitterComponent::GetOwnerEmitter() const
{
	return Cast<AKawaiiFluidEmitter>(GetOwner());
}

void UKawaiiFluidEmitterComponent::SetTargetVolume(AKawaiiFluidVolume* NewVolume)
{
	if (TargetVolume != NewVolume)
	{
		UnregisterFromVolume();
		TargetVolume = NewVolume;
		RegisterToVolume();
	}
}

float UKawaiiFluidEmitterComponent::GetParticleSpacing() const
{
	if (AKawaiiFluidVolume* Volume = GetTargetVolume())
	{
		return Volume->GetParticleSpacing();
	}
	return 10.0f; // Default fallback
}

void UKawaiiFluidEmitterComponent::StartStreamSpawn()
{
	if (!IsStreamMode())
	{
		UE_LOG(LogTemp, Warning, TEXT("UKawaiiFluidEmitterComponent::StartStreamSpawn - Not in Stream mode"));
		return;
	}

	// Distance optimization guard: prevent spawning when culled
	if (bUseDistanceOptimization && !bDistanceActivated)
	{
		UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent::StartStreamSpawn - Ignored (distance culled)"));
		return;
	}

	bStreamSpawning = true;
}

void UKawaiiFluidEmitterComponent::StopStreamSpawn()
{
	bStreamSpawning = false;
}

void UKawaiiFluidEmitterComponent::SpawnFill()
{
	if (!bEnabled)
	{
		return;
	}

	// Distance optimization guard: prevent spawning when culled
	if (bUseDistanceOptimization && !bDistanceActivated)
	{
		UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent::SpawnFill - Ignored (distance culled)"));
		return;
	}

	if (bAutoSpawnExecuted)
	{
		return;
	}

	AKawaiiFluidVolume* Volume = GetTargetVolume();
	if (!Volume)
	{
		UE_LOG(LogTemp, Warning, TEXT("UKawaiiFluidEmitterComponent::SpawnFill - No target Volume available"));
		return;
	}

	bAutoSpawnExecuted = true;

	const FVector SpawnCenter = GetComponentLocation() + SpawnOffset;
	const FQuat SpawnRotation = GetComponentQuat();
	const float Spacing = GetParticleSpacing();

	// Calculate velocity based on space mode
	FVector VelocityDirection = InitialVelocityDirection.GetSafeNormal();
	if (!bUseWorldSpaceVelocity)
	{
		// Local space: rotate velocity direction with component
		VelocityDirection = SpawnRotation.RotateVector(VelocityDirection);
	}
	const FVector CalculatedVelocity = VelocityDirection * InitialSpeed;

	int32 SpawnedCount = 0;

	// Always use hexagonal pattern (only mode supported)
	switch (ShapeType)
	{
	case EKawaiiFluidEmitterShapeType::Sphere:
		SpawnedCount = SpawnParticlesSphereHexagonal(SpawnCenter, SpawnRotation, SphereRadius, Spacing, CalculatedVelocity);
		break;

	case EKawaiiFluidEmitterShapeType::Cube:
		SpawnedCount = SpawnParticlesCubeHexagonal(SpawnCenter, SpawnRotation, CubeHalfSize, Spacing, CalculatedVelocity);
		break;

	case EKawaiiFluidEmitterShapeType::Cylinder:
		SpawnedCount = SpawnParticlesCylinderHexagonal(SpawnCenter, SpawnRotation, CylinderRadius,
			CylinderHalfHeight, Spacing, CalculatedVelocity);
		break;
	}

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent::SpawnFill - Spawned %d particles"), SpawnedCount);
}

void UKawaiiFluidEmitterComponent::BurstSpawn(int32 Count)
{
	if (!bEnabled)
	{
		return;
	}

	if (Count <= 0 || HasReachedParticleLimit())
	{
		return;
	}

	// Clamp to remaining budget
	if (MaxParticleCount > 0)
	{
		Count = FMath::Min(Count, MaxParticleCount - SpawnedParticleCount);
	}

	// Get effective spacing (0.56 matches Fill mode's HCP compensation)
	float EffectiveSpacing = StreamParticleSpacing;
	if (EffectiveSpacing <= 0.0f)
	{
		if (AKawaiiFluidVolume* Vol = GetTargetVolume())
		{
			if (UKawaiiFluidPresetDataAsset* Pst = Vol->GetPreset())
			{
				EffectiveSpacing = Pst->SmoothingRadius * 0.6f;
			}
		}
	}
	if (EffectiveSpacing <= 0.0f)
	{
		EffectiveSpacing = 10.0f;
	}

	const FVector SpawnPos = GetComponentLocation() + SpawnOffset;
	
	// Layer direction always follows component's local orientation
	FVector LayerDir = InitialVelocityDirection.GetSafeNormal();
	FVector LocalLayerDir = GetComponentQuat().RotateVector(LayerDir);
	
	// Calculate velocity direction based on space mode
	FVector VelocityDir = InitialVelocityDirection.GetSafeNormal();
	if (!bUseWorldSpaceVelocity)
	{
		// Local space: rotate velocity direction with component
		VelocityDir = GetComponentQuat().RotateVector(VelocityDir);
	}

	// Spawn layers instead of random particles
	for (int32 i = 0; i < Count; ++i)
	{
		SpawnStreamLayer(SpawnPos, LocalLayerDir, VelocityDir, InitialSpeed, StreamRadius, EffectiveSpacing);
	}
}

bool UKawaiiFluidEmitterComponent::HasReachedParticleLimit() const
{
	if (MaxParticleCount <= 0)
	{
		return false;  // No limit set
	}
	
	// Recycle mode (Stream only): don't block spawning, let recycle handle overflow
	if (bRecycleOldestParticles && IsStreamMode())
	{
		return false;
	}
	
	// Use actual per-source particle count from GPU readback
	UKawaiiFluidSimulationModule* Module = GetSimulationModule();
	if (Module && CachedSourceID >= 0)
	{
		const int32 ActualCount = Module->GetParticleCountForSource(CachedSourceID);
		if (ActualCount >= 0)  // Readback data ready
		{
			return ActualCount >= MaxParticleCount;
		}
	}
	
	// If readback not ready, allow spawning (don't block based on SpawnedParticleCount)
	// SpawnedParticleCount only goes up and doesn't account for particle deaths
	return false;
}

void UKawaiiFluidEmitterComponent::ProcessContinuousSpawn(float DeltaTime)
{
	// Check per-emitter particle limit (MaxParticleCount)
	if (HasReachedParticleLimit())
	{
		return;
	}

	// Only Stream mode is supported
	ProcessStreamEmitter(DeltaTime);
}

void UKawaiiFluidEmitterComponent::ProcessStreamEmitter(float DeltaTime)
{
	// Calculate Spacing exactly like KawaiiFluidComponent does:
	// Use StreamParticleSpacing if set, otherwise Preset->SmoothingRadius * 0.56f
	// Note: 0.56 (not 0.5) to match Fill mode's HCP compensation (1.122x)
	// This prevents initial high-density causing solver stress at spawn
	float EffectiveSpacing = StreamParticleSpacing;
	if (EffectiveSpacing <= 0.0f)
	{
		if (AKawaiiFluidVolume* Vol = GetTargetVolume())
		{
			if (UKawaiiFluidPresetDataAsset* Pst = Vol->GetPreset())
			{
				EffectiveSpacing = Pst->SmoothingRadius * 0.56f;
			}
		}
	}
	if (EffectiveSpacing <= 0.0f)
	{
		EffectiveSpacing = 10.0f;  // fallback
	}
	const float LayerSpacing = EffectiveSpacing * StreamLayerSpacingRatio;

	// Velocity-based layer spawning (accumulate distance traveled)
	const float DistanceThisFrame = InitialSpeed * DeltaTime;
	LayerDistanceAccumulator += DistanceThisFrame;

	if (LayerDistanceAccumulator < LayerSpacing)
	{
		return;  // Not enough distance accumulated for a layer
	}

	// Calculate number of layers to spawn
	// Clamp to MaxLayersPerFrame to prevent particle explosion on frame drops
	// When frame drops occur, DeltaTime increases causing many layers to spawn at once
	// This creates overlapping particles that repel each other violently
	const int32 RawLayerCount = FMath::FloorToInt(LayerDistanceAccumulator / LayerSpacing);
	const int32 LayerCount = FMath::Min(RawLayerCount, MaxLayersPerFrame);

	// IMPORTANT: Discard ALL excess accumulated distance, not just what we spawned
	// Only keep the fractional residual from LayerSpacing, discarding any "skipped" layers
	// This prevents catch-up spawning in subsequent frames which would still cause overlap
	// Example: If RawLayerCount=5 but we only spawn 1, we discard 4 layers worth of distance
	const float ResidualDistance = FMath::Fmod(LayerDistanceAccumulator, LayerSpacing);

	if (LayerCount <= 0)
	{
		return;
	}

	// === Direction calculation ===
	const FVector BaseLocation = GetComponentLocation() + SpawnOffset;
	const FQuat ComponentQuat = GetComponentQuat();
	
	// Calculate layer and velocity directions based on bUseWorldSpaceVelocity
	FVector LayerDir;
	FVector VelocityDir;
	FVector OffsetDir;  // Direction for layer position offset
	
	if (bUseWorldSpaceVelocity)
	{
		// World space: velocity direction is used directly (not rotated by component)
		VelocityDir = InitialVelocityDirection.GetSafeNormal();
		LayerDir = VelocityDir;  // Layer perpendicular to velocity
		OffsetDir = VelocityDir;  // Offset along velocity direction
	}
	else
	{
		// Local space: rotate directions with component
		FVector LocalDir = InitialVelocityDirection.GetSafeNormal();
		LayerDir = ComponentQuat.RotateVector(LocalDir);
		VelocityDir = LayerDir;
		OffsetDir = LayerDir;
	}

	// === Batch collection arrays ===
	TArray<FVector> AllPositions;
	TArray<FVector> AllVelocities;
	
	// Reserve estimated capacity
	const float RadiusSq = StreamRadius * StreamRadius;
	const int32 EstimatedPerLayer = FMath::CeilToInt((PI * RadiusSq) / (EffectiveSpacing * EffectiveSpacing));
	AllPositions.Reserve(EstimatedPerLayer * LayerCount);
	AllVelocities.Reserve(EstimatedPerLayer * LayerCount);

	// === Spawn layers with position offset (reverse order - oldest first) ===
	// Like UKawaiiFluidComponent: apply position offset to each layer to prevent overlap
	for (int32 i = LayerCount - 1; i >= 0; --i)
	{
		// Calculate position offset for each layer
		// i = LayerCount-1: oldest layer (farthest from spawn point)
		// i = 0: newest layer (closest to spawn point)
		const float PositionOffset = static_cast<float>(i) * LayerSpacing + ResidualDistance;
		const FVector OffsetLocation = BaseLocation + OffsetDir * PositionOffset;

		SpawnStreamLayerBatch(OffsetLocation, LayerDir, VelocityDir, InitialSpeed,
			StreamRadius, EffectiveSpacing, AllPositions, AllVelocities);
	}

	// === Send all layers in single batch ===
	if (AllPositions.Num() > 0)
	{
		QueueSpawnRequest(AllPositions, AllVelocities);
	}

	// === Update accumulator with residual distance ===
	LayerDistanceAccumulator = ResidualDistance;

	// === Recycle (Stream mode only): After spawning, remove oldest particles if over MaxParticleCount ===
	// TODO: Implement Quota system for multiple emitters sharing a Volume
	//       - Currently each emitter recycles its own particles when Volume is near limit
	//       - Problem: If Emitter A spawns more aggressively, it dominates the Volume
	//       - Solution: Per-emitter quota based on (EmitterMax / Sum of all EmitterMax) * VolumeMax
	//       - Or priority-based quota allocation
	if (bRecycleOldestParticles && MaxParticleCount > 0)
	{
		UKawaiiFluidSimulationModule* Module = GetSimulationModule();
		if (Module && CachedSourceID >= 0)
		{
			const int32 CurrentCount = Module->GetParticleCountForSource(CachedSourceID);
			// -1 = readback not ready, skip recycle this frame
			if (CurrentCount >= 0)
			{
				const int32 SpawnedThisFrame = AllPositions.Num();
				const int32 Margin = SpawnedThisFrame * 3;

				// Check Volume total particle count
				int32 VolumeMax = MaxParticleCount;
				int32 VolumeTotalCount = CurrentCount;
				if (FGPUFluidSimulator* GPUSim = Module->GetGPUSimulator())
				{
					VolumeMax = GPUSim->GetMaxParticleCount();
					VolumeTotalCount = GPUSim->GetParticleCount();
				}

				// Recycle if approaching either limit:
				// 1. This emitter's particle count near EmitterMax
				// 2. Volume total particle count near VolumeMax
				const bool bEmitterNearLimit = (CurrentCount >= MaxParticleCount - Margin);
				const bool bVolumeNearLimit = (VolumeTotalCount >= VolumeMax - Margin);

				if (bEmitterNearLimit || bVolumeNearLimit)
				{
					const int32 ToRemove = SpawnedThisFrame;
					Module->RemoveOldestParticlesForSource(CachedSourceID, ToRemove);
				}
			}
		}
	}
}

int32 UKawaiiFluidEmitterComponent::SpawnParticlesSphereHexagonal(FVector Center, FQuat Rotation, float Radius, float Spacing, FVector InInitialVelocity)
{
	AKawaiiFluidVolume* Volume = GetTargetVolume();
	if (!Volume || Spacing <= 0.0f || Radius <= 0.0f) return 0;

	TArray<FVector> Positions;
	TArray<FVector> Velocities;

	// HCP density compensation (matches KawaiiFluidSimulationModule exactly)
	const float HCPCompensation = 1.122f;
	const float AdjustedSpacing = Spacing * HCPCompensation;

	// Hexagonal close packing offsets
	const float RowSpacingY = AdjustedSpacing * 0.866025f;   // sqrt(3)/2
	const float LayerSpacingZ = AdjustedSpacing * 0.816497f; // sqrt(2/3)
	const float RadiusSq = Radius * Radius;
	const float JitterRange = AdjustedSpacing * JitterAmount;

	// Integer-based grid (matches SimulationModule)
	const int32 GridSize = FMath::CeilToInt(Radius / AdjustedSpacing) + 1;
	const int32 GridSizeY = FMath::CeilToInt(Radius / RowSpacingY) + 1;
	const int32 GridSizeZ = FMath::CeilToInt(Radius / LayerSpacingZ) + 1;

	const float EstimatedCount = (4.0f / 3.0f) * PI * Radius * Radius * Radius / (AdjustedSpacing * AdjustedSpacing * AdjustedSpacing);
	Positions.Reserve(FMath::CeilToInt(EstimatedCount));
	Velocities.Reserve(FMath::CeilToInt(EstimatedCount));

	for (int32 z = -GridSizeZ; z <= GridSizeZ; ++z)
	{
		// Z-layer offset pattern (mod 3) - ABCABC stacking for proper HCP
		const int32 ZMod = ((z + GridSizeZ) % 3);
		const float ZLayerOffsetX = (ZMod == 1) ? AdjustedSpacing * 0.5f : ((ZMod == 2) ? AdjustedSpacing * 0.25f : 0.0f);
		const float ZLayerOffsetY = (ZMod == 1) ? RowSpacingY / 3.0f : ((ZMod == 2) ? RowSpacingY * 2.0f / 3.0f : 0.0f);

		for (int32 y = -GridSizeY; y <= GridSizeY; ++y)
		{
			const float RowOffsetX = (((y + GridSizeY) % 2) == 1) ? AdjustedSpacing * 0.5f : 0.0f;

			for (int32 x = -GridSize; x <= GridSize; ++x)
			{
				// Check MaxParticleCount limit (Fill mode)
				if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
				{
					break;
				}

				FVector LocalPos(
					x * AdjustedSpacing + RowOffsetX + ZLayerOffsetX,
					y * RowSpacingY + ZLayerOffsetY,
					z * LayerSpacingZ
				);

				// Check sphere bounds
				const float DistSq = LocalPos.SizeSquared();
				if (DistSq > RadiusSq)
				{
					continue;
				}

				// Apply rotation to local position, then translate to world
				FVector RotatedPos = Rotation.RotateVector(LocalPos);
				FVector WorldPos = Center + RotatedPos;

				// Apply jitter with distance-based falloff (center: 100%, surface: 0%)
				// This prevents surface particles from protruding and causing jagged edges
				if (bUseJitter && JitterRange > 0.0f)
				{
					const float Dist = FMath::Sqrt(DistSq);
					const float JitterFactor = FMath::Clamp(1.0f - (Dist / Radius), 0.0f, 1.0f);
					const float ActualJitter = JitterRange * JitterFactor;

					if (ActualJitter > 0.0f)
					{
						WorldPos += FVector(
							FMath::FRandRange(-ActualJitter, ActualJitter),
							FMath::FRandRange(-ActualJitter, ActualJitter),
							FMath::FRandRange(-ActualJitter, ActualJitter)
						);
					}
				}

				Positions.Add(WorldPos);
				Velocities.Add(InInitialVelocity);
			}
			
			// Break outer loops if limit reached
			if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
			{
				break;
			}
		}
		
		if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
		{
			break;
		}
	}

	QueueSpawnRequest(Positions, Velocities);
	return Positions.Num();
}

int32 UKawaiiFluidEmitterComponent::SpawnParticlesCubeHexagonal(FVector Center, FQuat Rotation, FVector HalfSize, float Spacing, FVector InInitialVelocity)
{
	AKawaiiFluidVolume* Volume = GetTargetVolume();
	if (!Volume || Spacing <= 0.0f) return 0;

	TArray<FVector> Positions;
	TArray<FVector> Velocities;

	// HCP density compensation (matches KawaiiFluidSimulationModule exactly)
	const float HCPCompensation = 1.122f;
	const float AdjustedSpacing = Spacing * HCPCompensation;

	// Hexagonal Close Packing constants
	const float RowSpacingY = AdjustedSpacing * 0.866025f;   // sqrt(3)/2
	const float LayerSpacingZ = AdjustedSpacing * 0.816497f; // sqrt(2/3)
	const float JitterRange = AdjustedSpacing * JitterAmount;

	// Calculate grid counts (matches SimulationModule)
	const int32 CountX = FMath::Max(1, FMath::CeilToInt(HalfSize.X * 2.0f / AdjustedSpacing));
	const int32 CountY = FMath::Max(1, FMath::CeilToInt(HalfSize.Y * 2.0f / RowSpacingY));
	const int32 CountZ = FMath::Max(1, FMath::CeilToInt(HalfSize.Z * 2.0f / LayerSpacingZ));

	const int32 EstimatedTotal = CountX * CountY * CountZ;
	Positions.Reserve(EstimatedTotal);
	Velocities.Reserve(EstimatedTotal);

	// Start position (bottom-left-back corner with half-spacing offset)
	const FVector LocalStart(-HalfSize.X + AdjustedSpacing * 0.5f, -HalfSize.Y + RowSpacingY * 0.5f, -HalfSize.Z + LayerSpacingZ * 0.5f);

	for (int32 z = 0; z < CountZ; ++z)
	{
		// Z layer offset for HCP (ABC stacking pattern - mod 3)
		const float ZLayerOffsetX = (z % 3 == 1) ? AdjustedSpacing * 0.5f : ((z % 3 == 2) ? AdjustedSpacing * 0.25f : 0.0f);
		const float ZLayerOffsetY = (z % 3 == 1) ? RowSpacingY / 3.0f : ((z % 3 == 2) ? RowSpacingY * 2.0f / 3.0f : 0.0f);

		for (int32 y = 0; y < CountY; ++y)
		{
			// Row offset for hexagonal pattern in XY plane
			const float RowOffsetX = (y % 2 == 1) ? AdjustedSpacing * 0.5f : 0.0f;

			for (int32 x = 0; x < CountX; ++x)
			{
				// Check MaxParticleCount limit (Fill mode)
				if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
				{
					break;
				}

				FVector LocalPos(
					LocalStart.X + x * AdjustedSpacing + RowOffsetX + ZLayerOffsetX,
					LocalStart.Y + y * RowSpacingY + ZLayerOffsetY,
					LocalStart.Z + z * LayerSpacingZ
				);

				// Check bounds
				if (FMath::Abs(LocalPos.X) > HalfSize.X ||
				    FMath::Abs(LocalPos.Y) > HalfSize.Y ||
				    FMath::Abs(LocalPos.Z) > HalfSize.Z)
				{
					continue;
				}

				// Apply rotation to local position, then translate to world
				FVector RotatedPos = Rotation.RotateVector(LocalPos);
				FVector WorldPos = Center + RotatedPos;

				// Apply jitter with distance-based falloff (center: 100%, surface: 0%)
				// This prevents surface particles from protruding and causing jagged edges
				if (bUseJitter && JitterRange > 0.0f)
				{
					// Calculate distance to nearest face
					const float DistToSurfaceX = HalfSize.X - FMath::Abs(LocalPos.X);
					const float DistToSurfaceY = HalfSize.Y - FMath::Abs(LocalPos.Y);
					const float DistToSurfaceZ = HalfSize.Z - FMath::Abs(LocalPos.Z);
					const float MinDistToSurface = FMath::Min3(DistToSurfaceX, DistToSurfaceY, DistToSurfaceZ);

					// Falloff based on smallest half-size dimension
					const float MaxDist = FMath::Min3(HalfSize.X, HalfSize.Y, HalfSize.Z);
					const float JitterFactor = FMath::Clamp(MinDistToSurface / MaxDist, 0.0f, 1.0f);
					const float ActualJitter = JitterRange * JitterFactor;

					if (ActualJitter > 0.0f)
					{
						WorldPos += FVector(
							FMath::FRandRange(-ActualJitter, ActualJitter),
							FMath::FRandRange(-ActualJitter, ActualJitter),
							FMath::FRandRange(-ActualJitter, ActualJitter)
						);
					}
				}

				Positions.Add(WorldPos);
				Velocities.Add(InInitialVelocity);
			}

			// Break outer loop if limit reached
			if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
			{
				break;
			}
		}

		if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
		{
			break;
		}
	}

	QueueSpawnRequest(Positions, Velocities);
	return Positions.Num();
}

int32 UKawaiiFluidEmitterComponent::SpawnParticlesCylinderHexagonal(FVector Center, FQuat Rotation, float Radius, float HalfHeight, float Spacing, FVector InInitialVelocity)
{
	AKawaiiFluidVolume* Volume = GetTargetVolume();
	if (!Volume || Spacing <= 0.0f || Radius <= 0.0f || HalfHeight <= 0.0f) return 0;

	TArray<FVector> Positions;
	TArray<FVector> Velocities;

	// HCP density compensation (matches KawaiiFluidSimulationModule exactly)
	const float HCPCompensation = 1.122f;
	const float AdjustedSpacing = Spacing * HCPCompensation;

	// Hexagonal Close Packing constants
	const float RowSpacingY = AdjustedSpacing * 0.866025f;   // sqrt(3)/2
	const float LayerSpacingZ = AdjustedSpacing * 0.816497f; // sqrt(2/3)
	const float JitterRange = AdjustedSpacing * JitterAmount;
	const float RadiusSq = Radius * Radius;

	// Integer-based grid (matches SimulationModule)
	const int32 GridSizeXY = FMath::CeilToInt(Radius / AdjustedSpacing) + 1;
	const int32 GridSizeY = FMath::CeilToInt(Radius / RowSpacingY) + 1;
	const int32 GridSizeZ = FMath::CeilToInt(HalfHeight / LayerSpacingZ);

	const float EstimatedCount = PI * Radius * Radius * HalfHeight * 2.0f / (AdjustedSpacing * AdjustedSpacing * AdjustedSpacing);
	Positions.Reserve(FMath::CeilToInt(EstimatedCount));
	Velocities.Reserve(FMath::CeilToInt(EstimatedCount));

	for (int32 z = -GridSizeZ; z <= GridSizeZ; ++z)
	{
		// Z layer offset for HCP (ABC stacking pattern - mod 3)
		const int32 ZMod = ((z + GridSizeZ) % 3);
		const float ZLayerOffsetX = (ZMod == 1) ? AdjustedSpacing * 0.5f : ((ZMod == 2) ? AdjustedSpacing * 0.25f : 0.0f);
		const float ZLayerOffsetY = (ZMod == 1) ? RowSpacingY / 3.0f : ((ZMod == 2) ? RowSpacingY * 2.0f / 3.0f : 0.0f);

		for (int32 y = -GridSizeY; y <= GridSizeY; ++y)
		{
			const float RowOffsetX = (((y + GridSizeY) % 2) == 1) ? AdjustedSpacing * 0.5f : 0.0f;

			for (int32 x = -GridSizeXY; x <= GridSizeXY; ++x)
			{
				// Check MaxParticleCount limit (Fill mode)
				if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
				{
					break;
				}

				FVector LocalPos(
					x * AdjustedSpacing + RowOffsetX + ZLayerOffsetX,
					y * RowSpacingY + ZLayerOffsetY,
					z * LayerSpacingZ
				);

				// Check cylinder bounds (XY plane for radius, Z for height)
				const float XYDistSq = LocalPos.X * LocalPos.X + LocalPos.Y * LocalPos.Y;
				if (XYDistSq > RadiusSq || FMath::Abs(LocalPos.Z) > HalfHeight)
				{
					continue;
				}

				// Apply rotation to local position, then translate to world
				FVector RotatedPos = Rotation.RotateVector(LocalPos);
				FVector WorldPos = Center + RotatedPos;

				// Apply jitter with distance-based falloff (center: 100%, surface: 0%)
				// This prevents surface particles from protruding and causing jagged edges
				if (bUseJitter && JitterRange > 0.0f)
				{
					// Calculate distance to nearest surface (radial or caps)
					const float XYDist = FMath::Sqrt(XYDistSq);
					const float DistToRadialSurface = Radius - XYDist;
					const float DistToCapSurface = HalfHeight - FMath::Abs(LocalPos.Z);
					const float MinDistToSurface = FMath::Min(DistToRadialSurface, DistToCapSurface);

					// Falloff based on smaller dimension (radius or half-height)
					const float MaxDist = FMath::Min(Radius, HalfHeight);
					const float JitterFactor = FMath::Clamp(MinDistToSurface / MaxDist, 0.0f, 1.0f);
					const float ActualJitter = JitterRange * JitterFactor;

					if (ActualJitter > 0.0f)
					{
						WorldPos += FVector(
							FMath::FRandRange(-ActualJitter, ActualJitter),
							FMath::FRandRange(-ActualJitter, ActualJitter),
							FMath::FRandRange(-ActualJitter, ActualJitter)
						);
					}
				}

				Positions.Add(WorldPos);
				Velocities.Add(InInitialVelocity);
			}
			
			// Break outer loop if limit reached
			if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
			{
				break;
			}
		}
		
		if (MaxParticleCount > 0 && Positions.Num() >= MaxParticleCount)
		{
			break;
		}
	}

	QueueSpawnRequest(Positions, Velocities);
	return Positions.Num();
}

void UKawaiiFluidEmitterComponent::SpawnStreamLayer(FVector Position, FVector LayerDirection, FVector VelocityDirection, float Speed, float Radius, float Spacing)
{
	AKawaiiFluidVolume* Volume = GetTargetVolume();
	if (!Volume || Spacing <= 0.0f || Radius <= 0.0f) return;

	TArray<FVector> Positions;
	TArray<FVector> Velocities;

	// Use LayerDirection for particle placement (follows component rotation)
	FVector Dir = LayerDirection.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = FVector(0, 0, -1);  // Default: downward
	}

	// Create local coordinate system for particle placement
	FVector Right, Up;
	Dir.FindBestAxisVectors(Right, Up);

	// Calculate velocity (independent of layer direction in world space mode)
	const FVector SpawnVel = VelocityDirection.GetSafeNormal() * Speed;

	// NO HCP compensation for 2D hexagonal layer!
	// This matches SimulationModule::SpawnParticleDirectionalHexLayerBatch exactly
	const float RowSpacing = Spacing * FMath::Sqrt(3.0f) * 0.5f;  // ~0.866 * Spacing
	const float RadiusSq = Radius * Radius;

	// Jitter setup (matches SimulationModule)
	const float Jitter = FMath::Clamp(StreamJitter, 0.0f, 0.5f);
	const float MaxJitterOffset = Spacing * Jitter;
	const bool bApplyJitter = Jitter > KINDA_SMALL_NUMBER;

	// Row count calculation (matches SimulationModule)
	const int32 NumRows = FMath::CeilToInt(Radius / RowSpacing) * 2 + 1;
	const int32 HalfRows = NumRows / 2;

	// Estimate and reserve
	const int32 EstimatedCount = FMath::CeilToInt((PI * RadiusSq) / (Spacing * Spacing));
	Positions.Reserve(EstimatedCount);
	Velocities.Reserve(EstimatedCount);

	// Hexagonal grid iteration (matches SimulationModule exactly)
	for (int32 RowIdx = -HalfRows; RowIdx <= HalfRows; ++RowIdx)
	{
		const float LocalY = RowIdx * RowSpacing;
		const float LocalYSq = LocalY * LocalY;

		// Skip rows outside the circle
		if (LocalYSq > RadiusSq)
		{
			continue;
		}

		const float MaxX = FMath::Sqrt(RadiusSq - LocalYSq);

		// Odd rows get X offset (Hexagonal Packing) - uses Abs like SimulationModule
		const float XOffset = (FMath::Abs(RowIdx) % 2 != 0) ? Spacing * 0.5f : 0.0f;

		// Calculate column count
		const int32 NumCols = FMath::FloorToInt(MaxX / Spacing);

		for (int32 ColIdx = -NumCols; ColIdx <= NumCols; ++ColIdx)
		{
			float LocalX = ColIdx * Spacing + XOffset;
			float LocalYFinal = LocalY;

			// Apply jitter
			if (bApplyJitter)
			{
				LocalX += FMath::FRandRange(-MaxJitterOffset, MaxJitterOffset);
				LocalYFinal += FMath::FRandRange(-MaxJitterOffset, MaxJitterOffset);
			}

			// Check inside circle (after jitter)
			if (LocalX * LocalX + LocalYFinal * LocalYFinal <= RadiusSq)
			{
				FVector SpawnPos = Position + Right * LocalX + Up * LocalYFinal;
				Positions.Add(SpawnPos);
				Velocities.Add(SpawnVel);
			}
		}
	}

	QueueSpawnRequest(Positions, Velocities);
}

void UKawaiiFluidEmitterComponent::SpawnStreamLayerBatch(FVector Position, FVector LayerDirection, 
	FVector VelocityDirection, float Speed, float Radius, float Spacing,
	TArray<FVector>& OutPositions, TArray<FVector>& OutVelocities)
{
	if (Spacing <= 0.0f || Radius <= 0.0f) return;

	// Use LayerDirection for particle placement (follows component rotation)
	FVector Dir = LayerDirection.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = FVector(0, 0, -1);  // Default: downward
	}

	// Create local coordinate system for particle placement
	FVector Right, Up;
	Dir.FindBestAxisVectors(Right, Up);

	// Calculate velocity (independent of layer direction in world space mode)
	const FVector SpawnVel = VelocityDirection.GetSafeNormal() * Speed;

	// NO HCP compensation for 2D hexagonal layer!
	// This matches SimulationModule::SpawnParticleDirectionalHexLayerBatch exactly
	const float RowSpacing = Spacing * FMath::Sqrt(3.0f) * 0.5f;  // ~0.866 * Spacing
	const float RadiusSq = Radius * Radius;

	// Jitter setup (matches SimulationModule)
	const float Jitter = FMath::Clamp(StreamJitter, 0.0f, 0.5f);
	const float MaxJitterOffset = Spacing * Jitter;
	const bool bApplyJitter = Jitter > KINDA_SMALL_NUMBER;

	// Row count calculation (matches SimulationModule)
	const int32 NumRows = FMath::CeilToInt(Radius / RowSpacing) * 2 + 1;
	const int32 HalfRows = NumRows / 2;

	// Hexagonal grid iteration (matches SimulationModule exactly)
	for (int32 RowIdx = -HalfRows; RowIdx <= HalfRows; ++RowIdx)
	{
		const float LocalY = RowIdx * RowSpacing;
		const float LocalYSq = LocalY * LocalY;

		// Skip rows outside the circle
		if (LocalYSq > RadiusSq)
		{
			continue;
		}

		const float MaxX = FMath::Sqrt(RadiusSq - LocalYSq);

		// Odd rows get X offset (Hexagonal Packing) - uses Abs like SimulationModule
		const float XOffset = (FMath::Abs(RowIdx) % 2 != 0) ? Spacing * 0.5f : 0.0f;

		// Calculate column count
		const int32 NumCols = FMath::FloorToInt(MaxX / Spacing);

		for (int32 ColIdx = -NumCols; ColIdx <= NumCols; ++ColIdx)
		{
			float LocalX = ColIdx * Spacing + XOffset;
			float LocalYFinal = LocalY;

			// Apply jitter
			if (bApplyJitter)
			{
				LocalX += FMath::FRandRange(-MaxJitterOffset, MaxJitterOffset);
				LocalYFinal += FMath::FRandRange(-MaxJitterOffset, MaxJitterOffset);
			}

			// Check inside circle (after jitter)
			if (LocalX * LocalX + LocalYFinal * LocalYFinal <= RadiusSq)
			{
				FVector SpawnPos = Position + Right * LocalX + Up * LocalYFinal;
				OutPositions.Add(SpawnPos);
				OutVelocities.Add(SpawnVel);
			}
		}
	}
}

void UKawaiiFluidEmitterComponent::QueueSpawnRequest(const TArray<FVector>& Positions, const TArray<FVector>& Velocities)
{
	if (!bEnabled)
	{
		return;
	}

	AKawaiiFluidVolume* Volume = GetTargetVolume();
	if (!Volume || Positions.Num() == 0)
	{
		return;
	}

	// Use pre-allocated SourceID (from Subsystem, 0~63 range)
	// Queue spawn requests to Volume's batch queue
	Volume->QueueSpawnRequests(Positions, Velocities, CachedSourceID);

	SpawnedParticleCount += Positions.Num();
}

UKawaiiFluidSimulationModule* UKawaiiFluidEmitterComponent::GetSimulationModule() const
{
	if (AKawaiiFluidVolume* Volume = GetTargetVolume())
	{
		return Volume->GetSimulationModule();
	}
	return nullptr;
}

//========================================
// Volume Registration
//========================================

void UKawaiiFluidEmitterComponent::RegisterToVolume()
{
	if (TargetVolume)
	{
		if (AKawaiiFluidEmitter* Emitter = GetOwnerEmitter())
		{
			TargetVolume->RegisterEmitter(Emitter);
		}
	}
}

void UKawaiiFluidEmitterComponent::UnregisterFromVolume()
{
	if (TargetVolume)
	{
		if (AKawaiiFluidEmitter* Emitter = GetOwnerEmitter())
		{
			TargetVolume->UnregisterEmitter(Emitter);
		}
	}
}

AKawaiiFluidVolume* UKawaiiFluidEmitterComponent::FindNearestVolume() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	const FVector EmitterLocation = GetComponentLocation();
	AKawaiiFluidVolume* NearestVolume = nullptr;
	float NearestDistSq = FLT_MAX;

	// In editor (non-game world), use TActorIterator to find volumes directly
	// because volumes are not registered to Subsystem until BeginPlay
	if (!World->IsGameWorld())
	{
		for (TActorIterator<AKawaiiFluidVolume> It(World); It; ++It)
		{
			AKawaiiFluidVolume* Volume = *It;
			if (Volume && !Volume->IsPendingKillPending())
			{
				const float DistSq = FVector::DistSquared(EmitterLocation, Volume->GetActorLocation());
				if (DistSq < NearestDistSq)
				{
					NearestDistSq = DistSq;
					NearestVolume = Volume;
				}
			}
		}
		return NearestVolume;
	}

	// In game world, use Subsystem for registered volumes
	UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>();
	if (!Subsystem)
	{
		return nullptr;
	}

	for (const TObjectPtr<AKawaiiFluidVolume>& Volume : Subsystem->GetAllVolumes())
	{
		if (Volume)
		{
			const float DistSq = FVector::DistSquared(EmitterLocation, Volume->GetActorLocation());
			if (DistSq < NearestDistSq)
			{
				NearestDistSq = DistSq;
				NearestVolume = Volume;
			}
		}
	}

	return NearestVolume;
}

//========================================
// Distance Optimization (Emitter Distance Culling)
//========================================

void UKawaiiFluidEmitterComponent::UpdateDistanceOptimization(float DeltaTime)
{
	// Throttle: 10Hz
	DistanceCheckAccumulator += DeltaTime;
	if (DistanceCheckAccumulator < DistanceCheckInterval)
	{
		return;
	}
	DistanceCheckAccumulator = 0.0f;

	APawn* PlayerPawn = GetPlayerPawn();
	if (!PlayerPawn)
	{
		return;
	}

	const FVector PlayerLocation = PlayerPawn->GetActorLocation();
	const FVector EmitterLocation = GetComponentLocation();
	const float DistanceSq = FVector::DistSquared(PlayerLocation, EmitterLocation);

	// Hysteresis logic (auto-calculated: 10% of ActivationDistance)
	const float HysteresisBuffer = GetHysteresisDistance();
	bool bShouldBeActive;
	if (bDistanceActivated)
	{
		// Deactivate at (ActivationDistance + Hysteresis)
		const float DeactivationDist = ActivationDistance + HysteresisBuffer;
		bShouldBeActive = (DistanceSq <= DeactivationDist * DeactivationDist);
	}
	else
	{
		// Activate at ActivationDistance
		bShouldBeActive = (DistanceSq <= ActivationDistance * ActivationDistance);
	}

	if (bShouldBeActive != bDistanceActivated)
	{
		OnDistanceActivationChanged(bShouldBeActive);
	}
}

void UKawaiiFluidEmitterComponent::OnDistanceActivationChanged(bool bNewState)
{
	bDistanceActivated = bNewState;

	if (bNewState)
	{
		// === ACTIVATING ===
		UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Distance Activation - Activating (player entered range)"),
			*GetName());

		if (IsStreamMode())
		{
			if (bAutoStartSpawning)
			{
				StartStreamSpawn();
			}
		}
		else if (IsFillMode())
		{
			if (bNeedsRespawnOnReentry && bAutoRespawnOnReentry)
			{
				bAutoSpawnExecuted = false;
				SpawnFill();
				bNeedsRespawnOnReentry = false;
			}
		}
	}
	else
	{
		// === DEACTIVATING ===
		UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: Distance Activation - Deactivating (player left range)"),
			*GetName());

		if (IsStreamMode())
		{
			StopStreamSpawn();
		}

		// Despawn all particles from this emitter
		DespawnAllParticles();

		if (IsFillMode())
		{
			bNeedsRespawnOnReentry = true;
		}
	}
}

void UKawaiiFluidEmitterComponent::DespawnAllParticles()
{
	if (CachedSourceID < 0)
	{
		return;
	}

	UKawaiiFluidSimulationModule* Module = GetSimulationModule();
	if (!Module)
	{
		return;
	}

	const int32 CurrentCount = Module->GetParticleCountForSource(CachedSourceID);
	if (CurrentCount <= 0)
	{
		return;
	}

	// Remove all particles belonging to this emitter
	const int32 RemovedCount = Module->RemoveOldestParticlesForSource(CachedSourceID, CurrentCount);
	SpawnedParticleCount = 0;

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidEmitterComponent [%s]: DespawnAllParticles - Removed %d particles (SourceID=%d)"),
		*GetName(), RemovedCount, CachedSourceID);
}

APawn* UKawaiiFluidEmitterComponent::GetPlayerPawn()
{
	// Return cached pawn if still valid
	if (CachedPlayerPawn.IsValid())
	{
		return CachedPlayerPawn.Get();
	}

	// Find player pawn
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return nullptr;
	}

	APawn* Pawn = PC->GetPawn();
	if (Pawn)
	{
		CachedPlayerPawn = Pawn;
	}

	return Pawn;
}

#if WITH_EDITOR
void UKawaiiFluidEmitterComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property ?
		PropertyChangedEvent.Property->GetFName() : NAME_None;

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidEmitterComponent, TargetVolume))
	{
		// Re-register when target volume changes
		if (HasBegunPlay())
		{
			UnregisterFromVolume();
			RegisterToVolume();
		}
	}

	// Update velocity arrow when relevant properties change
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidEmitterComponent, EmitterMode) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidEmitterComponent, bUseWorldSpaceVelocity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidEmitterComponent, InitialVelocityDirection) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidEmitterComponent, InitialSpeed))
	{
		UpdateVelocityArrowVisualization();
	}
}

void UKawaiiFluidEmitterComponent::DrawSpawnVolumeVisualization()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector Location = GetComponentLocation() + SpawnOffset;
	const FQuat Rotation = GetComponentRotation().Quaternion();
	const FColor SpawnColor = SpawnVolumeWireframeColor;
	const float Duration = -1.0f;  // Redraw each frame
	const uint8 DepthPriority = 0;
	const float Thickness = WireframeThickness;

	if (IsFillMode())
	{
		// Fill Volume visualization
		switch (ShapeType)
		{
		case EKawaiiFluidEmitterShapeType::Sphere:
			{
				// Draw sphere using 3 circles (one per axis) like Point Light
				const int32 NumSegments = 24;
				
				// XY plane circle (around Z axis)
				DrawDebugCircle(World, Location, SphereRadius, NumSegments, SpawnColor, false, Duration, DepthPriority, Thickness, FVector(1, 0, 0), FVector(0, 1, 0), false);
				
				// XZ plane circle (around Y axis)
				DrawDebugCircle(World, Location, SphereRadius, NumSegments, SpawnColor, false, Duration, DepthPriority, Thickness, FVector(1, 0, 0), FVector(0, 0, 1), false);
				
				// YZ plane circle (around X axis)
				DrawDebugCircle(World, Location, SphereRadius, NumSegments, SpawnColor, false, Duration, DepthPriority, Thickness, FVector(0, 1, 0), FVector(0, 0, 1), false);
			}
			break;

		case EKawaiiFluidEmitterShapeType::Cube:
			// Cube with rotation
			DrawDebugBox(World, Location, CubeHalfSize, Rotation, SpawnColor, false, Duration, DepthPriority, Thickness);
			break;

		case EKawaiiFluidEmitterShapeType::Cylinder:
			{
				const float Radius = CylinderRadius;
				const float HalfHeight = CylinderHalfHeight;

				// Local coordinate cylinder vertices, then apply rotation
				const FVector LocalTopCenter = FVector(0, 0, HalfHeight);
				const FVector LocalBottomCenter = FVector(0, 0, -HalfHeight);

				const int32 NumSegments = 24;
				for (int32 i = 0; i < NumSegments; ++i)
				{
					const float Angle1 = (float)i / NumSegments * 2.0f * PI;
					const float Angle2 = (float)(i + 1) / NumSegments * 2.0f * PI;

					// Calculate local positions
					const FVector LocalTopP1 = LocalTopCenter + FVector(FMath::Cos(Angle1), FMath::Sin(Angle1), 0) * Radius;
					const FVector LocalTopP2 = LocalTopCenter + FVector(FMath::Cos(Angle2), FMath::Sin(Angle2), 0) * Radius;
					const FVector LocalBottomP1 = LocalBottomCenter + FVector(FMath::Cos(Angle1), FMath::Sin(Angle1), 0) * Radius;
					const FVector LocalBottomP2 = LocalBottomCenter + FVector(FMath::Cos(Angle2), FMath::Sin(Angle2), 0) * Radius;

					// Apply rotation and transform to world position
					const FVector TopP1 = Location + Rotation.RotateVector(LocalTopP1);
					const FVector TopP2 = Location + Rotation.RotateVector(LocalTopP2);
					const FVector BottomP1 = Location + Rotation.RotateVector(LocalBottomP1);
					const FVector BottomP2 = Location + Rotation.RotateVector(LocalBottomP2);

					DrawDebugLine(World, TopP1, TopP2, SpawnColor, false, Duration, DepthPriority, Thickness);
					DrawDebugLine(World, BottomP1, BottomP2, SpawnColor, false, Duration, DepthPriority, Thickness);
				}

				// Vertical lines (4 lines connecting top and bottom)
				for (int32 i = 0; i < 4; ++i)
				{
					const float Angle = (float)i / 4 * 2.0f * PI;
					const FVector LocalTopP = LocalTopCenter + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Radius;
					const FVector LocalBottomP = LocalBottomCenter + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Radius;

					const FVector TopP = Location + Rotation.RotateVector(LocalTopP);
					const FVector BottomP = Location + Rotation.RotateVector(LocalBottomP);
					DrawDebugLine(World, TopP, BottomP, SpawnColor, false, Duration, DepthPriority, Thickness);
				}
			}
			break;
		}
	}
	else // Stream mode
	{
		// Stream radius circle (always follows component orientation, independent of velocity space mode)
		if (StreamRadius > 0.0f)
		{
			// Use local direction to draw circle (always rotates with component)
			FVector LocalDir = InitialVelocityDirection.GetSafeNormal();
			FVector WorldDir = Rotation.RotateVector(LocalDir);
			
			FVector Right, Up;
			WorldDir.FindBestAxisVectors(Right, Up);

			const int32 NumSegments = 24;
			for (int32 i = 0; i < NumSegments; ++i)
			{
				const float Angle1 = (float)i / NumSegments * 2.0f * PI;
				const float Angle2 = (float)(i + 1) / NumSegments * 2.0f * PI;

				const FVector P1 = Location + (Right * FMath::Cos(Angle1) + Up * FMath::Sin(Angle1)) * StreamRadius;
				const FVector P2 = Location + (Right * FMath::Cos(Angle2) + Up * FMath::Sin(Angle2)) * StreamRadius;

				DrawDebugLine(World, P1, P2, SpawnColor, false, Duration, DepthPriority, Thickness);
			}
		}
	}
}

void UKawaiiFluidEmitterComponent::UpdateVelocityArrowVisualization()
{
#if WITH_EDITORONLY_DATA
	if (!VelocityArrow)
	{
		return;
	}

	VelocityArrow->SetVisibility(true);

	// Calculate velocity direction based on space mode (same for both Fill and Stream)
	FVector VelocityDir = InitialVelocityDirection.GetSafeNormal();
	if (!bUseWorldSpaceVelocity)
	{
		// Local space: arrow is in local space, no need to rotate
		VelocityArrow->SetRelativeRotation(VelocityDir.Rotation());
	}
	else
	{
		// World space: set world rotation directly
		VelocityArrow->SetWorldRotation(VelocityDir.Rotation());
	}

	// Set arrow length (fixed length, independent of speed)
	const float ArrowLength = 1.2f;
	VelocityArrow->SetRelativeScale3D(FVector(ArrowLength, 1.0f, 1.0f));
#endif
}

void UKawaiiFluidEmitterComponent::DrawDistanceVisualization()
{
	if (!bUseDistanceOptimization)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || World->IsGameWorld())
	{
		return;
	}

	const FVector Location = GetComponentLocation();
	const float Duration = -1.0f;  // Redraw each frame
	const uint8 DepthPriority = 0;

	// Activation distance (Green) - Player enters this range to activate
	// Note: Hysteresis (10% buffer) is applied internally but not visualized
	DrawDebugSphere(World, Location, ActivationDistance, 32,
		FColor::Green, false, Duration, DepthPriority, 1.0f);
}
#endif
