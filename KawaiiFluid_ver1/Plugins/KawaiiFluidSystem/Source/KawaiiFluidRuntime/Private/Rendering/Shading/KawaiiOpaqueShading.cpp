// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/Shading/KawaiiOpaqueShading.h"

void FKawaiiOpaqueShading::RenderForScreenSpacePipeline(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	const FMetaballIntermediateTextures& IntermediateTextures,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	// Skeleton: Opaque shading not implemented yet
	UE_LOG(LogTemp, Warning, TEXT("FKawaiiOpaqueShading::RenderForScreenSpacePipeline - Not implemented (experimental)"));
}

void FKawaiiOpaqueShading::RenderForRayMarchingPipeline(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FFluidRenderingParameters& RenderParams,
	FRDGBufferSRVRef ParticleBufferSRV,
	int32 ParticleCount,
	float ParticleRadius,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneColorTexture,
	FScreenPassRenderTarget Output)
{
	// Skeleton: Opaque shading not implemented yet
	UE_LOG(LogTemp, Warning, TEXT("FKawaiiOpaqueShading::RenderForRayMarchingPipeline - Not implemented (experimental)"));
}
