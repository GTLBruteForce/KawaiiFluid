// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidCompositeShaders.h"

IMPLEMENT_GLOBAL_SHADER(FFluidCompositeVS, "/Plugin/KawaiiFluidSystem/Private/FluidComposite.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FFluidCompositePS, "/Plugin/KawaiiFluidSystem/Private/FluidComposite.usf", "MainPS", SF_Pixel);
