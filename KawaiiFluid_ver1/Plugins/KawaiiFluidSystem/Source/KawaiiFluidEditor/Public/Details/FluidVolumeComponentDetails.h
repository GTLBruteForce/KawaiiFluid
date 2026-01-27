// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IDetailCustomization.h"

class UKawaiiFluidVolumeComponent;
class AKawaiiFluidVolume;

/**
 * KawaiiFluidVolumeComponent detail panel customization
 * Adds brush mode start/stop buttons and particle count display
 */
class FFluidVolumeComponentDetails : public IDetailCustomization
{
public:
	/** IDetailCustomization factory */
	static TSharedRef<IDetailCustomization> MakeInstance();

	//~ Begin IDetailCustomization Interface
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
	//~ End IDetailCustomization Interface

private:
	/** Target volume component */
	TWeakObjectPtr<UKawaiiFluidVolumeComponent> TargetComponent;

	/** Target volume actor (for Brush API access) */
	TWeakObjectPtr<AKawaiiFluidVolume> TargetVolume;

	/** Brush start button clicked */
	FReply OnStartBrushClicked();

	/** Brush stop button clicked */
	FReply OnStopBrushClicked();

	/** Clear all particles button clicked */
	FReply OnClearParticlesClicked();

	/** Start button visibility */
	EVisibility GetStartVisibility() const;

	/** Stop button visibility */
	EVisibility GetStopVisibility() const;

	/** Check if brush mode is active */
	bool IsBrushActive() const;
};
