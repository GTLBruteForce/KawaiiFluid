// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/FluidDepthShaders.h"
#include "ShaderParameterUtils.h"
#include "RenderGraphUtils.h"

// Shader implementation
IMPLEMENT_GLOBAL_SHADER(FFluidDepthVS, "/Plugin/KawaiiFluidSystem/Private/FluidDepth.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FFluidDepthPS, "/Plugin/KawaiiFluidSystem/Private/FluidDepth.usf", "MainPS", SF_Pixel);
