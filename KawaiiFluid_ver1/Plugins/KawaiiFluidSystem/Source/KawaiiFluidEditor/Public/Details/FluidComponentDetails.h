// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class UKawaiiFluidComponent;

/**
 * KawaiiFluidComponent detail panel customization
 * Adds brush mode start/stop buttons
 */
class FFluidComponentDetails : public IDetailCustomization
{
public:
	/** IDetailCustomization factory */
	static TSharedRef<IDetailCustomization> MakeInstance();

	//~ Begin IDetailCustomization Interface
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
	//~ End IDetailCustomization Interface

private:
	/** Target component */
	TWeakObjectPtr<UKawaiiFluidComponent> TargetComponent;

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
