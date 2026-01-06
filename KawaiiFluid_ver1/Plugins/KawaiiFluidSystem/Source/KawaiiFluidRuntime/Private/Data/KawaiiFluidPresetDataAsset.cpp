// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Data/KawaiiFluidPresetDataAsset.h"

UKawaiiFluidPresetDataAsset::UKawaiiFluidPresetDataAsset()
{
	// Default values are set in header
	// Calculate initial derived parameters
	RecalculateDerivedParameters();
}

void UKawaiiFluidPresetDataAsset::RecalculateDerivedParameters()
{
	// Clamp SpacingRatio to valid range
	SpacingRatio = FMath::Clamp(SpacingRatio, 0.3f, 0.7f);

	// ParticleSpacing = SmoothingRadius * SpacingRatio (cm)
	ParticleSpacing = SmoothingRadius * SpacingRatio;

	// Convert spacing to meters for mass calculation
	const float Spacing_m = ParticleSpacing * 0.01f;

	// ParticleMass = RestDensity * d³ (kg)
	// This ensures uniform grid at spacing d achieves RestDensity
	ParticleMass = RestDensity * Spacing_m * Spacing_m * Spacing_m;

	// Ensure minimum mass
	ParticleMass = FMath::Max(ParticleMass, 0.001f);

	// ParticleRadius = Spacing / 2 (cm)
	// Renders particles with slight overlap for continuous fluid appearance
	ParticleRadius = ParticleSpacing * 0.5f;

	// Ensure minimum radius
	ParticleRadius = FMath::Max(ParticleRadius, 0.1f);

	// Estimate neighbor count: N ≈ (4/3)π × (h/d)³ = (4/3)π × (1/SpacingRatio)³
	const float HOverD = 1.0f / SpacingRatio;
	EstimatedNeighborCount = FMath::RoundToInt((4.0f / 3.0f) * PI * HOverD * HOverD * HOverD);
}

#if WITH_EDITOR
void UKawaiiFluidPresetDataAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Validate parameters
	if (SmoothingRadius < 1.0f)
	{
		SmoothingRadius = 1.0f;
	}

	// Check which property changed
	const FName PropertyName = PropertyChangedEvent.Property ?
		PropertyChangedEvent.Property->GetFName() : NAME_None;

	// Recalculate derived parameters when relevant properties change
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidPresetDataAsset, SmoothingRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidPresetDataAsset, RestDensity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidPresetDataAsset, SpacingRatio))
	{
		RecalculateDerivedParameters();
	}
}
#endif
