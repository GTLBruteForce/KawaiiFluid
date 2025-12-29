// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shaders/FluidRayMarchShaders.h"

IMPLEMENT_GLOBAL_SHADER(FFluidRayMarchVS,
	"/Plugin/KawaiiFluidSystem/Private/FluidRayMarching.usf",
	"MainVS",
	SF_Vertex);

IMPLEMENT_GLOBAL_SHADER(FFluidRayMarchPS,
	"/Plugin/KawaiiFluidSystem/Private/FluidRayMarching.usf",
	"MainPS",
	SF_Pixel);
