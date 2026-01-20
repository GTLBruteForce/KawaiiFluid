// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "SceneTextureParameters.h"

/** Maximum number of lights supported for fluid composite shading. */
static constexpr int32 FLUID_MAX_LIGHTS = 8;

/**
 * @brief Shader parameters for composite pass.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFluidCompositeParameters, )
    // ------------------------------------------------------
    // Fluid Input Textures
    // ------------------------------------------------------
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FluidDepthTexture)     // Linear Depth (R32F)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FluidNormalTexture)    // Reconstructed Normal
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FluidThicknessTexture) // Thickness
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OcclusionMaskTexture)  // 1.0=visible, 0.0=occluded

    // ------------------------------------------------------
    // Scene Input
    // ------------------------------------------------------
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
    SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

    SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
    SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)  // Point sampling for depth textures

    // UV scaling for SceneColor/SceneDepth (ViewRect / TextureSize)
    // Needed when texture size differs from ViewRect (e.g., Screen Percentage)
    SHADER_PARAMETER(FVector2f, SceneUVScale)

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
    SHADER_PARAMETER(float, F0Override)           // F0 override (0 = use IOR-based, >0 = use this value)
    SHADER_PARAMETER(float, FresnelStrength)
    SHADER_PARAMETER(float, RefractiveIndex)
    SHADER_PARAMETER(float, AbsorptionCoefficient)
    SHADER_PARAMETER(FLinearColor, AbsorptionColorCoefficients)  // Per-channel absorption (Beer's Law)
    SHADER_PARAMETER(float, SpecularStrength)
    SHADER_PARAMETER(float, SpecularRoughness)
    SHADER_PARAMETER(FLinearColor, EnvironmentLightColor)

    // ------------------------------------------------------
    // Multi-Light Support
    // Packed as float4 arrays for shader compatibility:
    // - LightDirectionsAndIntensity[i] = (Direction.xyz, Intensity)
    // - LightColors[i] = (Color.rgb, unused)
    // ------------------------------------------------------
    SHADER_PARAMETER(int, NumLights)  // Number of active lights (0 = use View.DirectionalLight fallback)
    SHADER_PARAMETER_ARRAY(FVector4f, LightDirectionsAndIntensity, [FLUID_MAX_LIGHTS])
    SHADER_PARAMETER_ARRAY(FVector4f, LightColors, [FLUID_MAX_LIGHTS])

    // ------------------------------------------------------
    // Lighting Scale Parameters
    // ------------------------------------------------------
    SHADER_PARAMETER(float, AmbientScale)
    SHADER_PARAMETER(float, TransmittanceScale)
    SHADER_PARAMETER(float, AlphaThicknessScale)
    SHADER_PARAMETER(float, RefractionScale)
    SHADER_PARAMETER(float, FresnelReflectionBlend)

    // ------------------------------------------------------
    // Subsurface Scattering (SSS)
    // ------------------------------------------------------
    SHADER_PARAMETER(float, SSSIntensity)
    SHADER_PARAMETER(FLinearColor, SSSColor)

    // ------------------------------------------------------
    // Reflection Cubemap
    // ------------------------------------------------------
    SHADER_PARAMETER_TEXTURE(TextureCube, ReflectionCubemap)
    SHADER_PARAMETER_SAMPLER(SamplerState, ReflectionCubemapSampler)
    SHADER_PARAMETER(float, ReflectionIntensity)
    SHADER_PARAMETER(float, ReflectionMipLevel)
    SHADER_PARAMETER(int, bUseReflectionCubemap)  // 1 = use Cubemap, 0 = fallback color

    // ------------------------------------------------------
    // Screen Space Reflections (SSR)
    // ------------------------------------------------------
    SHADER_PARAMETER(int, bEnableSSR)           // Enable SSR
    SHADER_PARAMETER(int, SSRMaxSteps)          // Ray march max steps
    SHADER_PARAMETER(float, SSRStepSize)        // Step size (pixels)
    SHADER_PARAMETER(float, SSRThickness)       // Hit detection thickness
    SHADER_PARAMETER(float, SSRIntensity)       // SSR intensity
    SHADER_PARAMETER(float, SSREdgeFade)        // Screen edge fade
    SHADER_PARAMETER(int, SSRDebugMode)         // Debug visualization mode (0=none, 1-9=various debug views)
    SHADER_PARAMETER(FVector2f, ViewportSize)   // Viewport size (pixels)

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