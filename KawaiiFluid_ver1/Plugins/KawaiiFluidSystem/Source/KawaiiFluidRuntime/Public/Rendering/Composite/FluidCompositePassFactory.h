// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/FluidRenderingParameters.h"

class IFluidCompositePass;

/**
 * Factory for creating composite pass instances based on rendering mode
 */
class FFluidCompositePassFactory
{
public:
	/**
	 * Create composite pass instance for specified rendering mode
	 *
	 * @param Mode Rendering mode (Custom or GBuffer)
	 * @return Composite pass instance (never null)
	 */
	static TSharedPtr<IFluidCompositePass> Create(ESSFRRenderingMode Mode);
};
