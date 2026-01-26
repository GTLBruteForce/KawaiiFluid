// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Data/KawaiiFluidPresetDataAsset.h"

#include "EditorFramework/ThumbnailInfo.h"
#include "GPU/GPUFluidSimulatorShaders.h"  // For GPU_MORTON_GRID_AXIS_BITS

UKawaiiFluidPresetDataAsset::UKawaiiFluidPresetDataAsset()
{
	// Default values are set in header
	// Calculate initial derived parameters
	RecalculateDerivedParameters();
}

void UKawaiiFluidPresetDataAsset::RecalculateDerivedParameters()
{
	//========================================
	// Resolution Parameters (from ParticleRadius + SpacingRatio)
	//========================================

	// Clamp inputs to valid range
	ParticleRadius = FMath::Max(ParticleRadius, 0.1f);
	SpacingRatio = FMath::Clamp(SpacingRatio, 0.1f, 0.7f);

	// ParticleSpacing = ParticleRadius * 2 (cm)
	ParticleSpacing = ParticleRadius * 2.0f;

	// SmoothingRadius = ParticleSpacing / SpacingRatio (reverse calculation)
	SmoothingRadius = ParticleSpacing / SpacingRatio;
	SmoothingRadius = FMath::Clamp(SmoothingRadius, 1.0f, 200.0f);

	// Convert spacing to meters for mass calculation
	const float Spacing_m = ParticleSpacing * 0.01f;

	// ParticleMass = Density * d³ (kg)
	// This ensures uniform grid at spacing d achieves Density
	ParticleMass = Density * Spacing_m * Spacing_m * Spacing_m;
	ParticleMass = FMath::Max(ParticleMass, 0.001f);

	// Estimate neighbor count: N ≈ (4/3)π × (h/d)³ = (4/3)π × (1/SpacingRatio)³
	const float HOverD = 1.0f / SpacingRatio;
	EstimatedNeighborCount = FMath::RoundToInt((4.0f / 3.0f) * PI * HOverD * HOverD * HOverD);
}

#if WITH_EDITOR
void UKawaiiFluidPresetDataAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Check which property changed
	const FName PropertyName = PropertyChangedEvent.Property ?
		PropertyChangedEvent.Property->GetFName() : NAME_None;

	// Recalculate derived parameters when relevant properties change
	// - Resolution: ParticleRadius, SpacingRatio → SmoothingRadius, ParticleSpacing, etc.
	// - Mass: Density affects ParticleMass
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidPresetDataAsset, ParticleRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidPresetDataAsset, Density) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidPresetDataAsset, SpacingRatio))
	{
		RecalculateDerivedParameters();

		// Notify subscribers (e.g., SimulationModules) about property changes
		OnPropertyChanged.Broadcast(this);
	}
	
	// Notify thumbnail if available
	if (ThumbnailInfo)
	{
		// This call invalidates thumbnail cache and triggers re-rendering
		ThumbnailInfo->PostEditChange();
	}
}
#endif
