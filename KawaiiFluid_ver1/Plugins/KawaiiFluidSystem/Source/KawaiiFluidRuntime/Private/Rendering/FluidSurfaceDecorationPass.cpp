// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Rendering/FluidSurfaceDecorationPass.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "Engine/Texture2D.h"
#include "RHIStaticStates.h"
#include "TextureResource.h"
#include "Engine/Engine.h"
#include "EngineGlobals.h"
#include "Engine/Texture.h"

//=============================================================================
// Console Variables
//=============================================================================

static TAutoConsoleVariable<int32> CVarFluidSurfaceDecorationDebug(
	TEXT("r.Fluid.SurfaceDecorationDebug"),
	0,
	TEXT("Surface Decoration debug visualization mode.\n")
	TEXT("0: Off (normal rendering)\n")
	TEXT("1: AccumulatedFlow (Red=X, Green=Y magnitude)\n")
	TEXT("2: Velocity (Blue=velocity magnitude)\n")
	TEXT("3: Both (AccumulatedFlow + Velocity)"),
	ECVF_RenderThreadSafe);

//=============================================================================
// Surface Decoration Compute Shader
//=============================================================================

class FFluidSurfaceDecorationCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFluidSurfaceDecorationCS);
	SHADER_USE_PARAMETER_STRUCT(FFluidSurfaceDecorationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		// Input textures from SSFR (RDG textures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ThicknessTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VelocityMapTexture)  // Screen-space velocity (RDG, from Depth pass)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AccumulatedFlowTexture)  // Accumulated flow offset in world units (RDG)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OcclusionMaskTexture)  // Occlusion mask (R8: 1.0=visible, 0.0=occluded by scene geometry)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearWrapSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearMirrorSampler)  // For mirror addressing mode
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearClampSampler)  // For screen-space textures

		// Decoration textures (non-RDG, use engine textures as fallback)
		SHADER_PARAMETER_TEXTURE(Texture2D, FoamTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, LayerTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, LayerNormalMap)

		// Parameters
		SHADER_PARAMETER(FVector2f, TextureSize)
		SHADER_PARAMETER(FVector2f, SSFRTextureSize)  // Size of SSFR intermediate textures (may differ from TextureSize)
		SHADER_PARAMETER(FVector4f, ViewRect)  // xy = Min, zw = Max
		SHADER_PARAMETER(float, Time)

		// View reconstruction
		SHADER_PARAMETER(FMatrix44f, InvViewProjectionMatrix)
		SHADER_PARAMETER(FMatrix44f, ViewMatrix)
		SHADER_PARAMETER(FMatrix44f, InvViewMatrix)
		SHADER_PARAMETER(FMatrix44f, InvProjectionMatrix)
		SHADER_PARAMETER(FVector3f, CameraPosition)

		// Global
		SHADER_PARAMETER(float, GlobalOpacity)
		SHADER_PARAMETER(float, BlendWithFluidColor)

		// Foam
		SHADER_PARAMETER(int32, bFoamEnabled)
		SHADER_PARAMETER(FLinearColor, FoamColor)
		SHADER_PARAMETER(float, FoamVelocityThreshold)
		SHADER_PARAMETER(float, FoamIntensity)
		SHADER_PARAMETER(float, FoamTilingScale)
		SHADER_PARAMETER(int32, bWaveCrestFoam)
		SHADER_PARAMETER(float, FoamWaveCrestStrength)
		SHADER_PARAMETER(int32, FoamAddressingMode)  // 0 = Wrap, 1 = Mirror

		// Foam - Thickness-based
		SHADER_PARAMETER(int32, bThicknessFoam)
		SHADER_PARAMETER(float, FoamThicknessThreshold)
		SHADER_PARAMETER(float, FoamThicknessStrength)

		// Foam - Jitter (UV animation)
		SHADER_PARAMETER(int32, bFoamJitterEnabled)
		SHADER_PARAMETER(float, FoamJitterStrength)
		SHADER_PARAMETER(float, FoamJitterSpeed)

		// Foam - Flow Animation
		SHADER_PARAMETER(int32, bFoamUseFlowAnimation)

		// Emissive
		SHADER_PARAMETER(int32, bEmissiveEnabled)
		SHADER_PARAMETER(FLinearColor, EmissiveColor)
		SHADER_PARAMETER(float, EmissiveIntensity)
		SHADER_PARAMETER(int32, bVelocityEmissive)
		SHADER_PARAMETER(float, VelocitySensitivity)
		SHADER_PARAMETER(float, MinEmissive)
		SHADER_PARAMETER(float, PulsePeriod)
		SHADER_PARAMETER(float, PulseAmplitude)

		// Flow
		SHADER_PARAMETER(int32, bFlowEnabled)
		SHADER_PARAMETER(int32, bUseAccumulatedFlow)  // Use accumulated flow (velocity-based) instead of Time-based
		SHADER_PARAMETER(float, FlowSpeed)
		SHADER_PARAMETER(float, FlowDistortionStrength)

		// Layer
		SHADER_PARAMETER(int32, bLayerEnabled)
		SHADER_PARAMETER(float, LayerTilingScale)
		SHADER_PARAMETER(int32, LayerAddressingMode)  // 0 = Wrap, 1 = Mirror
		SHADER_PARAMETER(float, LayerOpacity)
		SHADER_PARAMETER(float, LayerNormalZThreshold)
		SHADER_PARAMETER(float, LayerFlowInfluence)
		SHADER_PARAMETER(FVector2f, LayerScrollSpeed)

		// Layer Normal Map
		SHADER_PARAMETER(int32, bLayerNormalMapEnabled)
		SHADER_PARAMETER(float, LayerNormalStrength)

		// Layer - Jitter (UV animation)
		SHADER_PARAMETER(int32, bLayerJitterEnabled)
		SHADER_PARAMETER(float, LayerJitterStrength)
		SHADER_PARAMETER(float, LayerJitterSpeed)

		// Layer - Flow Animation
		SHADER_PARAMETER(int32, bLayerUseFlowAnimation)

		// Texture Lighting
		SHADER_PARAMETER(int32, bApplyLightingToTextures)
		SHADER_PARAMETER(FVector3f, LightDirection)
		SHADER_PARAMETER(FVector3f, LightColor)
		SHADER_PARAMETER(FVector3f, AmbientColor)
		SHADER_PARAMETER(float, TextureSpecularStrength)
		SHADER_PARAMETER(float, TextureSpecularRoughness)

		// Debug (0=off, 1=AccumulatedFlow, 2=Velocity, 3=Both)
		SHADER_PARAMETER(int32, DebugMode)

		// View uniform buffer for accessing scene lighting (View.DirectionalLightDirection etc.)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFluidSurfaceDecorationCS,
                        "/Plugin/KawaiiFluidSystem/Private/FluidSurfaceDecoration.usf", "SurfaceDecorationCS",
                        SF_Compute);

//=============================================================================
// Helper: Get texture RHI or fallback to engine default texture
//=============================================================================

static FRHITexture* GetTextureRHIOrDefault(UTexture2D* Texture, FRHITexture* Default)
{
	if (Texture && Texture->GetResource() && Texture->GetResource()->GetTexture2DRHI())
	{
		return Texture->GetResource()->GetTexture2DRHI();
	}
	return Default;
}

//=============================================================================
// Pass Implementation
//=============================================================================

/**
 * @brief Renders surface decoration effects on the fluid.
 */
void RenderFluidSurfaceDecorationPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FSurfaceDecorationParams& Params,
	FRDGTextureRef DepthTexture,
	FRDGTextureRef NormalTexture,
	FRDGTextureRef ThicknessTexture,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef VelocityMapTexture,
	FRDGTextureRef AccumulatedFlowTexture,
	FRDGTextureRef OcclusionMaskTexture,
	const FIntRect& OutputViewRect,
	FRDGTextureRef& OutDecoratedTexture)
{
	// Early out if decoration is disabled
	if (!Params.bEnabled)
	{
		OutDecoratedTexture = SceneColorTexture;
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "FluidSurfaceDecoration");

	check(DepthTexture);
	check(NormalTexture);
	check(ThicknessTexture);
	check(SceneColorTexture);

	// Use SceneColorTexture size to match output
	// Note: SSFR intermediate textures (Depth, Normal, Thickness) may be different size
	FIntPoint TextureSize = SceneColorTexture->Desc.Extent;

	// Create output texture
	FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
		TextureSize,
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);
	OutDecoratedTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("FluidDecoratedColor"));

	// Note: No CopyTexture needed - compute shader handles background pixels directly
	// by outputting SceneColor for pixels with no fluid depth

	// Get shader
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FFluidSurfaceDecorationCS> ComputeShader(GlobalShaderMap);

	// Prepare parameters
	auto* PassParameters = GraphBuilder.AllocParameters<FFluidSurfaceDecorationCS::FParameters>();

	// Input textures (RDG)
	PassParameters->DepthTexture = DepthTexture;
	PassParameters->NormalTexture = NormalTexture;
	PassParameters->ThicknessTexture = ThicknessTexture;
	PassParameters->SceneColorTexture = SceneColorTexture;
	PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->BilinearWrapSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Wrap>::GetRHI();
	PassParameters->BilinearMirrorSampler = TStaticSamplerState<SF_Bilinear, AM_Mirror, AM_Mirror>::GetRHI();
	PassParameters->BilinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();

	// Decoration textures (non-RDG) - use engine global textures as fallback
	FRHITexture* WhiteFallback = GWhiteTexture->TextureRHI;
	FRHITexture* BlackFallback = GBlackTexture->TextureRHI;
	// For normal map fallback, use a flat normal (0.5, 0.5, 1.0) - white works as simple fallback
	FRHITexture* NormalFallback = GWhiteTexture->TextureRHI;

	PassParameters->FoamTexture = GetTextureRHIOrDefault(Params.Foam.FoamTexture.Get(), WhiteFallback);
	PassParameters->LayerTexture = GetTextureRHIOrDefault(Params.Layer.Texture.Get(), WhiteFallback);
	PassParameters->LayerNormalMap = GetTextureRHIOrDefault(Params.Layer.NormalMap.Get(), NormalFallback);

	// Velocity and AccumulatedFlow textures (RDG)
	// Create properly formatted dummy textures with producer pass when not provided
	auto CreateAndClearDummyTexture = [&GraphBuilder](const TCHAR* Name) -> FRDGTextureRef
	{
		// Must match the format expected by the shader (PF_G16R16F for velocity/flow data)
		FRDGTextureDesc DummyDesc = FRDGTextureDesc::Create2D(
			FIntPoint(4, 4),  // Small but not 1x1 to avoid potential edge cases
			PF_G16R16F,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		FRDGTextureRef DummyTexture = GraphBuilder.CreateTexture(DummyDesc, Name);
		// Add clear pass as producer - this makes the texture valid for reading
		AddClearRenderTargetPass(GraphBuilder, DummyTexture, FLinearColor::Black);
		return DummyTexture;
	};

	if (VelocityMapTexture)
	{
		PassParameters->VelocityMapTexture = VelocityMapTexture;
	}
	else
	{
		PassParameters->VelocityMapTexture = CreateAndClearDummyTexture(TEXT("DummyVelocityMap"));
	}

	if (AccumulatedFlowTexture)
	{
		PassParameters->AccumulatedFlowTexture = AccumulatedFlowTexture;
	}
	else
	{
		PassParameters->AccumulatedFlowTexture = CreateAndClearDummyTexture(TEXT("DummyAccumulatedFlow"));
	}

	// Occlusion mask texture binding
	// Create R8 dummy texture for occlusion mask (1.0 = visible by default)
	auto CreateAndClearOcclusionDummyTexture = [&GraphBuilder]() -> FRDGTextureRef
	{
		FRDGTextureDesc DummyDesc = FRDGTextureDesc::Create2D(
			FIntPoint(4, 4),
			PF_R8,  // Single channel for occlusion
			FClearValueBinding(FLinearColor::White),  // 1.0 = visible (no occlusion)
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		FRDGTextureRef DummyTexture = GraphBuilder.CreateTexture(DummyDesc, TEXT("DummyOcclusionMask"));
		AddClearRenderTargetPass(GraphBuilder, DummyTexture, FLinearColor::White);
		return DummyTexture;
	};

	if (OcclusionMaskTexture)
	{
		PassParameters->OcclusionMaskTexture = OcclusionMaskTexture;
	}
	else
	{
		PassParameters->OcclusionMaskTexture = CreateAndClearOcclusionDummyTexture();
	}

	// Auto-enable if textures are assigned (user convenience)
	const bool bLayerHasTexture = Params.Layer.Texture.Get() != nullptr;
	const bool bFoamHasTexture = Params.Foam.FoamTexture.Get() != nullptr;

	// Basic parameters
	PassParameters->TextureSize = FVector2f(TextureSize.X, TextureSize.Y);
	// SSFR texture size (Depth, Normal, Thickness) may differ from output size
	FIntPoint SSFRSize = DepthTexture->Desc.Extent;
	PassParameters->SSFRTextureSize = FVector2f(SSFRSize.X, SSFRSize.Y);
	// Use OutputViewRect - this is where fluid was rendered in SceneColorTexture
	// SSFR textures cover this area: SSFR(0,0) = OutputViewRect.Min, SSFR(SSFRSize) = OutputViewRect.Max
	PassParameters->ViewRect = FVector4f(OutputViewRect.Min.X, OutputViewRect.Min.Y, OutputViewRect.Max.X, OutputViewRect.Max.Y);
	PassParameters->Time = View.Family->Time.GetWorldTimeSeconds();

	// View matrices
	FMatrix InvViewProj = View.ViewMatrices.GetInvViewProjectionMatrix();
	FMatrix ViewMat = View.ViewMatrices.GetViewMatrix();
	FMatrix InvViewMat = View.ViewMatrices.GetInvViewMatrix();
	FMatrix InvProjMat = View.ViewMatrices.GetInvProjectionMatrix();
	PassParameters->InvViewProjectionMatrix = FMatrix44f(InvViewProj);
	PassParameters->ViewMatrix = FMatrix44f(ViewMat);
	PassParameters->InvViewMatrix = FMatrix44f(InvViewMat);
	PassParameters->InvProjectionMatrix = FMatrix44f(InvProjMat);
	PassParameters->CameraPosition = FVector3f(View.ViewMatrices.GetViewOrigin());

	// Layer Blending
	PassParameters->GlobalOpacity = Params.LayerFinalOpacity;
	PassParameters->BlendWithFluidColor = Params.LayerBlendWithFluidColor;

	// Foam respect bEnabled flag only
	PassParameters->bFoamEnabled = Params.Foam.bEnabled ? 1 : 0;
	PassParameters->FoamColor = Params.Foam.FoamColor;
	PassParameters->FoamVelocityThreshold = Params.Foam.VelocityThreshold;
	PassParameters->FoamIntensity = Params.Foam.Intensity;
	PassParameters->FoamTilingScale = Params.Foam.TilingScale;
	PassParameters->bWaveCrestFoam = Params.Foam.bWaveCrestFoam ? 1 : 0;
	PassParameters->FoamWaveCrestStrength = Params.Foam.WaveCrestFoamStrength;
	PassParameters->FoamAddressingMode = static_cast<int32>(Params.Foam.AddressingMode);

	// Foam - Thickness-based
	PassParameters->bThicknessFoam = Params.Foam.bThicknessFoam ? 1 : 0;
	PassParameters->FoamThicknessThreshold = Params.Foam.ThicknessThreshold;
	PassParameters->FoamThicknessStrength = Params.Foam.ThicknessFoamStrength;

	// Foam - Jitter
	PassParameters->bFoamJitterEnabled = Params.Foam.bJitterEnabled ? 1 : 0;
	PassParameters->FoamJitterStrength = Params.Foam.JitterStrength;
	PassParameters->FoamJitterSpeed = Params.Foam.JitterSpeed;

	// Foam - Flow Animation
	PassParameters->bFoamUseFlowAnimation = Params.Foam.bUseFlowAnimation ? 1 : 0;

	// Emissive
	PassParameters->bEmissiveEnabled = Params.Emissive.bEnabled ? 1 : 0;
	PassParameters->EmissiveColor = Params.Emissive.EmissiveColor;
	PassParameters->EmissiveIntensity = Params.Emissive.Intensity;
	PassParameters->bVelocityEmissive = Params.Emissive.bVelocityEmissive ? 1 : 0;
	PassParameters->VelocitySensitivity = Params.Emissive.VelocitySensitivity;
	PassParameters->MinEmissive = Params.Emissive.MinEmissive;
	PassParameters->PulsePeriod = Params.Emissive.PulsePeriod;
	PassParameters->PulseAmplitude = Params.Emissive.PulseAmplitude;

	// Flow
	PassParameters->bFlowEnabled = Params.FlowMap.bEnabled ? 1 : 0;
	// Use accumulated flow when flow is enabled and we have accumulated flow texture
	PassParameters->bUseAccumulatedFlow = (Params.FlowMap.bEnabled && AccumulatedFlowTexture != nullptr) ? 1 : 0;
	PassParameters->FlowSpeed = Params.FlowMap.FlowSpeed;
	PassParameters->FlowDistortionStrength = Params.FlowMap.DistortionStrength;

	// Layer respect bEnabled flag only
	PassParameters->bLayerEnabled = Params.Layer.bEnabled ? 1 : 0;
	PassParameters->LayerTilingScale = Params.Layer.TilingScale;
	PassParameters->LayerAddressingMode = static_cast<int32>(Params.Layer.AddressingMode);
	PassParameters->LayerOpacity = Params.Layer.Opacity;
	PassParameters->LayerNormalZThreshold = Params.Layer.NormalZThreshold;
	PassParameters->LayerFlowInfluence = Params.Layer.FlowInfluence;
	PassParameters->LayerScrollSpeed = FVector2f(Params.Layer.ScrollSpeed.X, Params.Layer.ScrollSpeed.Y);

	// Layer Normal Map
	const bool bLayerHasNormalMap = Params.Layer.NormalMap.Get() != nullptr;
	PassParameters->bLayerNormalMapEnabled = bLayerHasNormalMap ? 1 : 0;
	PassParameters->LayerNormalStrength = Params.Layer.NormalStrength;

	// Layer - Jitter
	PassParameters->bLayerJitterEnabled = Params.Layer.bJitterEnabled ? 1 : 0;
	PassParameters->LayerJitterStrength = Params.Layer.JitterStrength;
	PassParameters->LayerJitterSpeed = Params.Layer.JitterSpeed;

	// Layer - Flow Animation
	PassParameters->bLayerUseFlowAnimation = Params.Layer.bUseFlowAnimation ? 1 : 0;

	// Layer Lighting
	PassParameters->bApplyLightingToTextures = Params.bApplyLightingToLayer ? 1 : 0;
	// Use default lighting direction (sunlight from above-right)
	// This provides a reasonable default; future enhancement could find DirectionalLight in scene
	// Direction is FROM light source (shader uses NoL = dot(N, -LightDir))
	FVector3f LightDir = FVector3f(0.577f, 0.577f, -0.577f);  // Normalized (1,1,-1) - light from upper right
	FVector3f LightCol = FVector3f(1.0f, 0.98f, 0.95f);       // Slightly warm white
	FVector3f AmbientCol = FVector3f(0.15f, 0.15f, 0.18f);    // Slightly cool ambient
	PassParameters->LightDirection = LightDir;
	PassParameters->LightColor = LightCol;
	PassParameters->AmbientColor = AmbientCol;
	PassParameters->TextureSpecularStrength = Params.LayerSpecularStrength;
	PassParameters->TextureSpecularRoughness = Params.LayerSpecularRoughness;

	// View uniform buffer for scene lighting access
	PassParameters->View = View.ViewUniformBuffer;

	// Debug mode: 0=off, 1=AccumulatedFlow, 2=Velocity, 3=Both
	// Use console variable: r.Fluid.SurfaceDecorationDebug
	// - Mode 1: Red/Green shows AccumulatedFlow.xy magnitude
	// - Mode 2: Blue shows Velocity magnitude
	// - Mode 3: Shows both
	PassParameters->DebugMode = CVarFluidSurfaceDecorationDebug.GetValueOnRenderThread();

	// Output
	PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutDecoratedTexture);

	// Dispatch based on texture size
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SurfaceDecoration"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(TextureSize, 8));
}
