// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Composite/FluidCompositePassFactory.h"
#include "Rendering/Composite/FluidCustomComposite.h"
#include "Rendering/Composite/FluidGBufferComposite.h"
#include "Rendering/Composite/FluidRayMarchComposite.h"

TSharedPtr<IFluidCompositePass> FFluidCompositePassFactory::Create(ESSFRRenderingMode Mode)
{
	switch (Mode)
	{
	case ESSFRRenderingMode::Custom:
		return MakeShared<FFluidCustomComposite>();

	case ESSFRRenderingMode::GBuffer:
		return MakeShared<FFluidGBufferComposite>();

	case ESSFRRenderingMode::RayMarching:
		return MakeShared<FFluidRayMarchComposite>();

	default:
		UE_LOG(LogTemp, Error, TEXT("Unknown SSFRRenderingMode: %d, defaulting to Custom"), static_cast<int>(Mode));
		return MakeShared<FFluidCustomComposite>();
	}
}
