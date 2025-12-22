// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidSceneViewExtension.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/FluidDepthPass.h"
#include "Rendering/FluidSmoothingPass.h"
#include "Core/FluidSimulator.h"
#include "SceneView.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

FFluidSceneViewExtension::FFluidSceneViewExtension(const FAutoRegister& AutoRegister, UFluidRendererSubsystem* InSubsystem)
	: FSceneViewExtensionBase(AutoRegister)
	, Subsystem(InSubsystem)
{
	UE_LOG(LogTemp, Log, TEXT("FluidSceneViewExtension Created"));
}

FFluidSceneViewExtension::~FFluidSceneViewExtension()
{
	UE_LOG(LogTemp, Log, TEXT("FluidSceneViewExtension Destroyed"));
}

void FFluidSceneViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
	// Subsystem 유효성 검사
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	// 렌더링 비활성화 체크
	if (!SubsystemPtr->RenderingParameters.bEnableRendering)
	{
		return;
	}

	// 등록된 시뮬레이터가 없으면 스킵
	const TArray<AFluidSimulator*>& Simulators = SubsystemPtr->GetRegisteredSimulators();
	if (Simulators.Num() == 0)
	{
		return;
	}

	// SSFR 렌더링 파이프라인 실행

	// Depth Pass - Sphere ray-casting으로 깊이 버퍼 생성
	FRDGTextureRef DepthTexture = nullptr;
	RenderFluidDepthPass(GraphBuilder, View, SubsystemPtr, DepthTexture);

	// Depth texture가 생성되지 않았으면 스킵
	if (!DepthTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidDepthPass: DepthTexture not created!"));
		return;
	}

	// UE_LOG(LogTemp, Warning, TEXT("FluidDepthPass: DepthTexture created successfully - Size: %dx%d"),
	// 	DepthTexture->Desc.Extent.X, DepthTexture->Desc.Extent.Y);

	// Smoothing Pass - Bilateral blur로 깊이 부드럽게
	FRDGTextureRef SmoothedDepthTexture = nullptr;
	float BlurRadius = static_cast<float>(SubsystemPtr->RenderingParameters.BilateralFilterRadius);
	float DepthFalloff = SubsystemPtr->RenderingParameters.DepthThreshold;

	// UE_LOG(LogTemp, Warning, TEXT("FluidSmoothingPass: Starting with BlurRadius=%.1f, DepthFalloff=%.3f"),
	// 	BlurRadius, DepthFalloff);

	RenderFluidSmoothingPass(GraphBuilder, View, DepthTexture, SmoothedDepthTexture, BlurRadius, DepthFalloff);

	if (SmoothedDepthTexture)
	{
		// UE_LOG(LogTemp, Warning, TEXT("FluidSmoothingPass: SmoothedDepthTexture created - Size: %dx%d"),
		// 	SmoothedDepthTexture->Desc.Extent.X, SmoothedDepthTexture->Desc.Extent.Y);
	}

	// 3. Normal Reconstruction Pass (TODO)
	// RenderNormalPass(GraphBuilder, View, SmoothedDepthTexture);

	// 4. Thickness Pass (TODO)
	// RenderThicknessPass(GraphBuilder, View);

	// 5. Final Shading Pass (TODO)
	// RenderShadingPass(GraphBuilder, View, Inputs);
}

void FFluidSceneViewExtension::RenderDepthPass(FRDGBuilder& GraphBuilder, const FSceneView& View)
{
	UFluidRendererSubsystem* SubsystemPtr = Subsystem.Get();
	if (!SubsystemPtr)
	{
		return;
	}

	FRDGTextureRef DepthTexture = nullptr;
	RenderFluidDepthPass(GraphBuilder, View, SubsystemPtr, DepthTexture);
}

void FFluidSceneViewExtension::RenderSmoothingPass(FRDGBuilder& GraphBuilder, const FSceneView& View)
{
	// TODO: 다음 단계에서 구현
}

void FFluidSceneViewExtension::RenderNormalPass(FRDGBuilder& GraphBuilder, const FSceneView& View)
{
	// TODO: 다음 단계에서 구현
}

void FFluidSceneViewExtension::RenderThicknessPass(FRDGBuilder& GraphBuilder, const FSceneView& View)
{
	// TODO: 다음 단계에서 구현
}

void FFluidSceneViewExtension::RenderShadingPass(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
	// TODO: 다음 단계에서 구현
}
