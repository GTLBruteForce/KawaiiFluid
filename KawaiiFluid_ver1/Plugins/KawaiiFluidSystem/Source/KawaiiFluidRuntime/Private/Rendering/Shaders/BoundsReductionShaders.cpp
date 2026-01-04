// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shaders/BoundsReductionShaders.h"

IMPLEMENT_GLOBAL_SHADER(FBoundsReductionCS,
	"/Plugin/KawaiiFluidSystem/Private/FluidBoundsReduction.usf",
	"CalculateBoundsCS",
	SF_Compute);
