// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "SceneTextureParameters.h" 

/**
 * Composite 패스를 위한 셰이더 파라미터
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidCompositeParameters, )
    // ------------------------------------------------------
    // Fluid Input Textures
    // ------------------------------------------------------
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FluidDepthTexture)     // Linear Depth (R32F)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FluidNormalTexture)    // Reconstructed Normal
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FluidThicknessTexture) // Thickness

    // ------------------------------------------------------
    // Scene Input
    // ------------------------------------------------------
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
    SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

    SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)

    // ------------------------------------------------------
    // Matrices
    // ------------------------------------------------------
    SHADER_PARAMETER(FMatrix44f, InverseProjectionMatrix)
    SHADER_PARAMETER(FMatrix44f, ProjectionMatrix)
    SHADER_PARAMETER(FMatrix44f, ViewMatrix)

    // ------------------------------------------------------
    // Fluid Settings
    // ------------------------------------------------------
    SHADER_PARAMETER(FLinearColor, FluidColor)
    SHADER_PARAMETER(float, FresnelStrength)
    SHADER_PARAMETER(float, RefractiveIndex)
    SHADER_PARAMETER(float, AbsorptionCoefficient)
    SHADER_PARAMETER(float, SpecularStrength)
    SHADER_PARAMETER(float, SpecularRoughness)

    // ------------------------------------------------------
    // Render Targets (Output)
    // ------------------------------------------------------
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FFluidCompositeVS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFluidCompositeVS);
    SHADER_USE_PARAMETER_STRUCT(FFluidCompositeVS, FGlobalShader);
    
    using FParameters = FFluidCompositeParameters;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

/**
 * Fluid Composite Pixel Shader
 */
class FFluidCompositePS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFluidCompositePS);
    SHADER_USE_PARAMETER_STRUCT(FFluidCompositePS, FGlobalShader);

    using FParameters = FFluidCompositeParameters;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};