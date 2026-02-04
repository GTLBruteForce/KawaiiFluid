// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Components/KawaiiFluidVolumeComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

UKawaiiFluidVolumeComponent::UKawaiiFluidVolumeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Enable ticking in editor for debug visualization
	bTickInEditor = true;

	// UBoxComponent configuration for editor visualization and selection
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCollisionResponseToAllChannels(ECR_Ignore);
	SetGenerateOverlapEvents(false);

	// Wireframe visualization settings
	LineThickness = 2.0f;
	ShapeColor = FColor::Green;
	bHiddenInGame = true;  // Hide wireframe at runtime by default

	// Load default Preset (DA_KF_Water)
	static ConstructorHelpers::FObjectFinder<UKawaiiFluidPresetDataAsset> DefaultPresetFinder(
		TEXT("/KawaiiFluidSystem/Preset/DA_KF_Water.DA_KF_Water"));
	if (DefaultPresetFinder.Succeeded())
	{
		Preset = DefaultPresetFinder.Object;
	}

	// Initialize default volume size based on Medium Z-Order preset and default CellSize (20.0f)
	// Formula: GridResolution(Medium) * CellSize = 128 * 20 = 2560
	// CellSize will be automatically derived from Preset->SmoothingRadius when Preset is set
	const float MediumGridResolution = static_cast<float>(GridResolutionPresetHelper::GetGridResolution(EGridResolutionPreset::Medium));
	const float DefaultCellSize = 20.0f;  // Default fallback when no Preset is set
	const float DefaultVolumeSize = MediumGridResolution * DefaultCellSize;
	UniformVolumeSize = DefaultVolumeSize;
	VolumeSize = FVector(DefaultVolumeSize);

	// Initialize BoxExtent directly (don't call SetBoxExtent in constructor)
	// SetBoxExtent() will be called in OnRegister after the component is fully constructed
	BoxExtent = FVector(DefaultVolumeSize * 0.5f);

	// Initialize grid parameters (without calling RecalculateBounds which uses SetBoxExtent)
	CellSize = DefaultCellSize;
	GridResolutionPreset = EGridResolutionPreset::Medium;
	GridAxisBits = GridResolutionPresetHelper::GetAxisBits(GridResolutionPreset);
	GridResolution = GridResolutionPresetHelper::GetGridResolution(GridResolutionPreset);
	MaxCells = GridResolutionPresetHelper::GetMaxCells(GridResolutionPreset);
	BoundsExtent = static_cast<float>(GridResolution) * CellSize;
}

void UKawaiiFluidVolumeComponent::OnRegister()
{
	Super::OnRegister();
	RecalculateBounds();
}

void UKawaiiFluidVolumeComponent::OnUnregister()
{
	UnregisterFromSubsystem();
	Super::OnUnregister();
}

void UKawaiiFluidVolumeComponent::BeginPlay()
{
	Super::BeginPlay();
	RegisterToSubsystem();
	RecalculateBounds();
}

void UKawaiiFluidVolumeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromSubsystem();
	Super::EndPlay(EndPlayReason);
}

void UKawaiiFluidVolumeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Update bounds if component moved
	RecalculateBounds();

	// Update wireframe color based on selection
	if (AActor* Owner = GetOwner())
	{
		ShapeColor = Owner->IsSelected() ? FColor::Yellow : BoundsColor;
	}

	// Draw additional debug visualization (Z-Order space, info text)
	DrawBoundsVisualization();
}

#if WITH_EDITOR
void UKawaiiFluidVolumeComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property ?
		PropertyChangedEvent.Property->GetFName() : NAME_None;

	// MemberProperty is the outer property when editing nested struct members (e.g., FVector.X/Y/Z)
	const FName MemberPropertyName = PropertyChangedEvent.MemberProperty ?
		PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;

	// Sync size values when toggling Uniform Size mode
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, bUniformSize))
	{
		if (bUniformSize)
		{
			// Switching to uniform mode: use max of VolumeSize components
			UniformVolumeSize = FMath::Max3(VolumeSize.X, VolumeSize.Y, VolumeSize.Z);
		}
		else
		{
			// Switching to non-uniform mode: copy UniformVolumeSize to all axes
			VolumeSize = FVector(UniformVolumeSize);
		}
	}

	// Apply minimum size constraint (max is handled by RecalculateBounds with rotation awareness)
	// In Hybrid Tiled Z-Order mode, only apply minimum constraint - no maximum clamping
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, UniformVolumeSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, bUniformSize))
	{
		UniformVolumeSize = FMath::Max(UniformVolumeSize, 10.0f);
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, VolumeSize) ||
		MemberPropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, VolumeSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, bUniformSize))
	{
		VolumeSize.X = FMath::Max(VolumeSize.X, 10.0f);
		VolumeSize.Y = FMath::Max(VolumeSize.Y, 10.0f);
		VolumeSize.Z = FMath::Max(VolumeSize.Z, 10.0f);
	}

	// When bUseHybridTiledZOrder is disabled, reset bUseUnlimitedSize to false
	// This ensures size controls remain accessible when switching modes
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, bUseHybridTiledZOrder))
	{
		if (!bUseHybridTiledZOrder && bUseUnlimitedSize)
		{
			bUseUnlimitedSize = false;
			// Restore wireframe visibility
			bShowBoundsInEditor = true;
			bShowBoundsAtRuntime = false;
			SetVisibility(true);
		}
	}

	// When bUseUnlimitedSize changes, ensure wireframe visibility is updated
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, bUseUnlimitedSize))
	{
		// In Unlimited Size mode, hide the wireframe box
		if (bUseUnlimitedSize)
		{
			bShowBoundsInEditor = false;
			bShowBoundsAtRuntime = false;
			SetVisibility(false);
		}
		else
		{
			bShowBoundsInEditor = true;
			bShowBoundsAtRuntime = false;
			SetVisibility(true);
		}
	}

	// Handle size-related property changes or Preset change (which affects CellSize)
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, bUniformSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, UniformVolumeSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, VolumeSize) ||
		MemberPropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, VolumeSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, Preset))
	{
		RecalculateBounds();

		// Notify all registered modules to update their volume info display
		for (TWeakObjectPtr<UKawaiiFluidSimulationModule>& WeakModule : RegisteredModules)
		{
			if (UKawaiiFluidSimulationModule* Module = WeakModule.Get())
			{
				Module->UpdateVolumeInfoDisplay();
			}
		}
	}

	// Update MetaballRenderer's preset when Preset property changes
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, Preset))
	{
		if (AKawaiiFluidVolume* Volume = Cast<AKawaiiFluidVolume>(GetOwner()))
		{
			if (UKawaiiFluidRenderingModule* RenderingMod = Volume->GetRenderingModule())
			{
				if (UKawaiiFluidMetaballRenderer* MR = RenderingMod->GetMetaballRenderer())
				{
					MR->SetPreset(Preset);
				}
			}
		}
	}

	// Update wireframe appearance
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, BoundsColor))
	{
		ShapeColor = BoundsColor;
	}
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidVolumeComponent, BoundsLineThickness))
	{
		SetLineThickness(BoundsLineThickness);
	}
}
#endif

void UKawaiiFluidVolumeComponent::RecalculateBounds()
{
	// Update CellSize from Preset->SmoothingRadius (or use default fallback)
	if (Preset)
	{
		CellSize = Preset->SmoothingRadius;
	}
	else
	{
		CellSize = 20.0f;  // Default fallback when no Preset is set
	}

	// Ensure valid CellSize
	CellSize = FMath::Max(CellSize, 1.0f);

	// Get user-defined volume size (full size)
	const FVector OriginalHalfExtent = GetEffectiveVolumeSize() * 0.5f;
	FVector WorkingHalfExtent = OriginalHalfExtent;
	FVector EffectiveHalfExtent = WorkingHalfExtent;

	// Get component rotation
	const FQuat ComponentRotation = GetComponentQuat();

	// Helper lambda to compute AABB half-extent from OBB half-extent and rotation
	auto ComputeRotatedAABBHalfExtent = [&ComponentRotation](const FVector& OBBHalfExtent) -> FVector
	{
		if (ComponentRotation.Equals(FQuat::Identity))
		{
			return OBBHalfExtent;
		}

		FVector RotatedCorners[8];
		for (int32 i = 0; i < 8; ++i)
		{
			FVector Corner(
				(i & 1) ? OBBHalfExtent.X : -OBBHalfExtent.X,
				(i & 2) ? OBBHalfExtent.Y : -OBBHalfExtent.Y,
				(i & 4) ? OBBHalfExtent.Z : -OBBHalfExtent.Z
			);
			RotatedCorners[i] = ComponentRotation.RotateVector(Corner);
		}

		FVector AABBMin = RotatedCorners[0];
		FVector AABBMax = RotatedCorners[0];
		for (int32 i = 1; i < 8; ++i)
		{
			AABBMin = AABBMin.ComponentMin(RotatedCorners[i]);
			AABBMax = AABBMax.ComponentMax(RotatedCorners[i]);
		}

		return FVector(
			FMath::Max(FMath::Abs(AABBMin.X), FMath::Abs(AABBMax.X)),
			FMath::Max(FMath::Abs(AABBMin.Y), FMath::Abs(AABBMax.Y)),
			FMath::Max(FMath::Abs(AABBMin.Z), FMath::Abs(AABBMax.Z))
		);
	};

	// In Hybrid Tiled Z-Order mode, skip all size clamping - unlimited range supported
	// In classic mode, clamp to grid capacity
	if (!bUseHybridTiledZOrder)
	{
		// Get the maximum half-extent supported by Large preset
		const float LargeMaxHalfExtent = GridResolutionPresetHelper::GetMaxExtentForPreset(EGridResolutionPreset::Large, CellSize);

		// First pass: Clamp half-extent to Large max (without rotation)
		WorkingHalfExtent = GridResolutionPresetHelper::ClampExtentToMaxSupported(OriginalHalfExtent, CellSize);

		// Calculate the AABB extent for the rotated OBB
		EffectiveHalfExtent = WorkingHalfExtent;

		if (!ComponentRotation.Equals(FQuat::Identity))
		{
			// Compute rotated AABB
			EffectiveHalfExtent = ComputeRotatedAABBHalfExtent(WorkingHalfExtent);

			// Check if rotated AABB exceeds Large preset limits
			const float MaxAABBHalfExtent = FMath::Max3(EffectiveHalfExtent.X, EffectiveHalfExtent.Y, EffectiveHalfExtent.Z);
			if (MaxAABBHalfExtent > LargeMaxHalfExtent)
			{
				// Scale down the original extent proportionally so rotated AABB fits within Large
				const float ScaleFactor = LargeMaxHalfExtent / MaxAABBHalfExtent;
				WorkingHalfExtent = WorkingHalfExtent * ScaleFactor;

				// Recompute rotated AABB with scaled extent
				EffectiveHalfExtent = ComputeRotatedAABBHalfExtent(WorkingHalfExtent);
			}
		}

		// Apply final extent if different from original (update VolumeSize/UniformVolumeSize)
		if (!WorkingHalfExtent.Equals(OriginalHalfExtent, 0.01f))
		{
			const FVector NewSize = WorkingHalfExtent * 2.0f;

#if WITH_EDITOR
			const bool bWasRotated = !ComponentRotation.Equals(FQuat::Identity);
			const FVector OriginalSize = OriginalHalfExtent * 2.0f;

			if (bWasRotated)
			{
				const FVector OriginalRotatedAABB = ComputeRotatedAABBHalfExtent(OriginalHalfExtent);
				const float RotatedAABBMax = FMath::Max3(OriginalRotatedAABB.X, OriginalRotatedAABB.Y, OriginalRotatedAABB.Z);
				UE_LOG(LogTemp, Warning, TEXT("VolumeSize adjusted: Rotated AABB (%.1f cm) exceeds limit (%.1f cm). Size scaled from (%s) to (%s)"),
					RotatedAABBMax * 2.0f, LargeMaxHalfExtent * 2.0f, *OriginalSize.ToString(), *NewSize.ToString());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("VolumeSize exceeds limit (%.1f cm per axis). Clamped from (%s) to (%s)"),
					LargeMaxHalfExtent * 2.0f, *OriginalSize.ToString(), *NewSize.ToString());
			}
#endif

			// Update the stored size values
			VolumeSize = NewSize;
			if (bUniformSize)
			{
				UniformVolumeSize = FMath::Max3(NewSize.X, NewSize.Y, NewSize.Z);
			}
		}
	}
	else
	{
		// Hybrid mode: compute effective half-extent for rotated AABB (for grid preset selection)
		if (!ComponentRotation.Equals(FQuat::Identity))
		{
			EffectiveHalfExtent = ComputeRotatedAABBHalfExtent(WorkingHalfExtent);
		}
	}

	// Final half-extent to use for BoxComponent
	const FVector FinalHalfExtent = WorkingHalfExtent;

	// Sync UBoxComponent's BoxExtent with our VolumeSize
	// Skip if bUseUnlimitedSize is enabled (no box collision/wireframe needed)
	if (!bUseUnlimitedSize)
	{
		// Only call SetBoxExtent after the component is registered (not in constructor)
		if (IsRegistered())
		{
			SetBoxExtent(FinalHalfExtent, false);
		}
		else
		{
			// Direct assignment for pre-registration (constructor) phase
			BoxExtent = FinalHalfExtent;
		}
	}
	else
	{
		// In Unlimited Size mode, set a minimal extent (box is hidden anyway)
		BoxExtent = FVector(1.0f, 1.0f, 1.0f);
	}

	// Auto-select optimal GridResolutionPreset based on rotated AABB size
	// In Hybrid mode, we still use this for internal grid parameters
	GridResolutionPreset = GridResolutionPresetHelper::SelectPresetForExtent(EffectiveHalfExtent, CellSize);

	// Update grid parameters from auto-selected preset
	GridAxisBits = GridResolutionPresetHelper::GetAxisBits(GridResolutionPreset);
	GridResolution = GridResolutionPresetHelper::GetGridResolution(GridResolutionPreset);
	MaxCells = GridResolutionPresetHelper::GetMaxCells(GridResolutionPreset);

	// Calculate actual bounds extent from the selected preset
	// This may be larger than requested to fit Z-Order grid constraints
	BoundsExtent = static_cast<float>(GridResolution) * CellSize;

	// Get component world location
	const FVector ComponentLocation = GetComponentLocation();

	// Calculate world bounds (centered on component)
	// Use the actual BoundsExtent from grid for consistency with Z-Order space
	const float ActualHalfExtent = BoundsExtent * 0.5f;
	WorldBoundsMin = ComponentLocation - FVector(ActualHalfExtent, ActualHalfExtent, ActualHalfExtent);
	WorldBoundsMax = ComponentLocation + FVector(ActualHalfExtent, ActualHalfExtent, ActualHalfExtent);
}

bool UKawaiiFluidVolumeComponent::IsPositionInBounds(const FVector& WorldPosition) const
{
	return WorldPosition.X >= WorldBoundsMin.X && WorldPosition.X <= WorldBoundsMax.X &&
	       WorldPosition.Y >= WorldBoundsMin.Y && WorldPosition.Y <= WorldBoundsMax.Y &&
	       WorldPosition.Z >= WorldBoundsMin.Z && WorldPosition.Z <= WorldBoundsMax.Z;
}

void UKawaiiFluidVolumeComponent::GetSimulationBounds(FVector& OutMin, FVector& OutMax) const
{
	OutMin = WorldBoundsMin;
	OutMax = WorldBoundsMax;
}

void UKawaiiFluidVolumeComponent::RegisterModule(UKawaiiFluidSimulationModule* Module)
{
	if (Module && !RegisteredModules.Contains(Module))
	{
		RegisteredModules.Add(Module);
	}
}

void UKawaiiFluidVolumeComponent::UnregisterModule(UKawaiiFluidSimulationModule* Module)
{
	if (Module)
	{
		RegisteredModules.Remove(Module);
	}
}

void UKawaiiFluidVolumeComponent::RegisterToSubsystem()
{
	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			Subsystem->RegisterVolumeComponent(this);
		}
	}
}

void UKawaiiFluidVolumeComponent::UnregisterFromSubsystem()
{
	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			Subsystem->UnregisterVolumeComponent(this);
		}
	}
}

void UKawaiiFluidVolumeComponent::DrawBoundsVisualization()
{
	// Skip all visualization in Unlimited Size mode (no box, no text)
	if (bUseUnlimitedSize)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector ComponentLocation = GetComponentLocation();

	// Optionally draw internal Z-Order space (advanced debug) - different from user-defined volume
	if (bShowZOrderSpaceWireframe)
	{
		const FVector ZOrderCenter = (WorldBoundsMin + WorldBoundsMax) * 0.5f;
		const FVector ZOrderExtent = (WorldBoundsMax - WorldBoundsMin) * 0.5f;

		DrawDebugBox(
			World,
			ZOrderCenter,
			ZOrderExtent,
			FQuat::Identity,
			ZOrderSpaceWireframeColor,
			false,
			-1.0f,
			0,
			1.0f  // Thinner line for internal grid
		);
	}

	// Draw info text at center (editor only)
#if WITH_EDITOR
	if (!World->IsGameWorld() && bShowBoundsInEditor)
	{
		const FVector UserExtent = GetVolumeHalfExtent();
		const FVector EffectiveSize = GetEffectiveVolumeSize();
		const FString InfoText = FString::Printf(
			TEXT("Size: %.0fx%.0fx%.0f cm\nBounce: %.1f, Friction: %.1f"),
			EffectiveSize.X, EffectiveSize.Y, EffectiveSize.Z,
			GetWallBounce(), GetWallFriction()
		);
		DrawDebugString(World, ComponentLocation + FVector(0, 0, UserExtent.Z + 50.0f), InfoText, nullptr, ShapeColor, -1.0f, true);
	}
#endif
}

//========================================
// Preset & Simulation
//========================================

float UKawaiiFluidVolumeComponent::GetParticleSpacing() const
{
	if (Preset)
	{
		return Preset->ParticleRadius * 2.0f;
	}
	return 10.0f;  // Default fallback
}

float UKawaiiFluidVolumeComponent::GetWallBounce() const
{
	if (Preset)
	{
		return Preset->Bounciness;
	}
	return 0.0f;  // Default fallback
}

float UKawaiiFluidVolumeComponent::GetWallFriction() const
{
	if (Preset)
	{
		return Preset->Friction;
	}
	return 0.5f;  // Default fallback
}

//========================================
// Debug Methods
//========================================

void UKawaiiFluidVolumeComponent::SetDebugDrawMode(EKawaiiFluidDebugDrawMode Mode)
{
	DebugDrawMode = Mode;
}

void UKawaiiFluidVolumeComponent::DisableDebugDraw()
{
	DebugDrawMode = EKawaiiFluidDebugDrawMode::None;
}
