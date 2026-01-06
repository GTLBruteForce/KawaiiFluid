// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/FluidSceneViewExtension.h"
#include "Rendering/FluidShadowHistoryManager.h"
#include "Rendering/FluidShadowUtils.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "EngineUtils.h"

bool UFluidRendererSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!IsValid(Outer))
	{
		return false;
	}

	UWorld* World = CastChecked<UWorld>(Outer);
	const EWorldType::Type WorldType = World->WorldType;

	// Support all world types that might need fluid rendering
	// Including EditorPreview for Preset Editor viewport
	return WorldType == EWorldType::Game ||
	       WorldType == EWorldType::Editor ||
	       WorldType == EWorldType::PIE ||
	       WorldType == EWorldType::EditorPreview ||
	       WorldType == EWorldType::GamePreview;
}

void UFluidRendererSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Scene View Extension 생성 및 등록
	ViewExtension = FSceneViewExtensions::NewExtension<FFluidSceneViewExtension>(this);

	// Shadow History Manager 생성
	ShadowHistoryManager = MakeUnique<FFluidShadowHistoryManager>();

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem Initialized"));
}

void UFluidRendererSubsystem::Deinitialize()
{
	// View Extension 해제
	ViewExtension.Reset();

	// Shadow History Manager 해제
	ShadowHistoryManager.Reset();

	RegisteredRenderingModules.Empty();

	Super::Deinitialize();

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem Deinitialized"));
}

//========================================
// RenderingModule 관리
//========================================

void UFluidRendererSubsystem::RegisterRenderingModule(UKawaiiFluidRenderingModule* Module)
{
	if (!Module)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem: RegisterRenderingModule - Module is null"));
		return;
	}

	if (RegisteredRenderingModules.Contains(Module))
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem: RenderingModule already registered: %s"),
			*Module->GetName());
		return;
	}

	RegisteredRenderingModules.Add(Module);

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem: Registered RenderingModule %s (Total: %d)"),
		*Module->GetName(),
		RegisteredRenderingModules.Num());
}

void UFluidRendererSubsystem::UnregisterRenderingModule(UKawaiiFluidRenderingModule* Module)
{
	if (!Module)
	{
		return;
	}

	int32 Removed = RegisteredRenderingModules.Remove(Module);

	if (Removed > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem: Unregistered RenderingModule %s (Remaining: %d)"),
			*Module->GetName(),
			RegisteredRenderingModules.Num());
	}
}

//========================================
// Cached Light Direction (Game Thread)
//========================================

/**
 * @brief Update cached light direction from the main DirectionalLight in the world.
 * Must be called from game thread before rendering.
 */
void UFluidRendererSubsystem::UpdateCachedLightDirection()
{
	check(IsInGameThread());

	UWorld* World = GetWorld();
	if (!World)
	{
		bHasCachedLightData = false;
		return;
	}

	// Find main DirectionalLight using TActorIterator (game thread only)
	ADirectionalLight* MainLight = FluidShadowUtils::FindMainDirectionalLight(World);

	if (MainLight)
	{
		UDirectionalLightComponent* LightComp = Cast<UDirectionalLightComponent>(MainLight->GetLightComponent());
		if (LightComp)
		{
			// Get light direction (forward vector points toward the light source, negate for shadow direction)
			FVector LightDir = -LightComp->GetForwardVector();
			CachedLightDirection = FVector3f(LightDir);

			// Calculate light view-projection matrix for shadow mapping
			// Use a default bounds for now (TODO: use actual fluid bounds)
			FBox FluidBounds(FVector(-1000, -1000, -1000), FVector(1000, 1000, 1000));
			FBox ExpandedBounds = FluidBounds.ExpandBy(FluidBounds.GetExtent().Size() * 0.5f);

			FMatrix ViewMatrix, ProjectionMatrix;
			FluidShadowUtils::CalculateDirectionalLightMatrices(LightDir, ExpandedBounds, ViewMatrix, ProjectionMatrix);
			CachedLightViewProjectionMatrix = FMatrix44f(ViewMatrix * ProjectionMatrix);

			bHasCachedLightData = true;
			return;
		}
	}

	// Fallback: use default sun direction
	CachedLightDirection = FVector3f(0.5f, 0.5f, -0.707f).GetSafeNormal();

	FBox FluidBounds(FVector(-1000, -1000, -1000), FVector(1000, 1000, 1000));
	FBox ExpandedBounds = FluidBounds.ExpandBy(FluidBounds.GetExtent().Size() * 0.5f);

	FMatrix ViewMatrix, ProjectionMatrix;
	FluidShadowUtils::CalculateDirectionalLightMatrices(FVector(CachedLightDirection), ExpandedBounds, ViewMatrix, ProjectionMatrix);
	CachedLightViewProjectionMatrix = FMatrix44f(ViewMatrix * ProjectionMatrix);

	bHasCachedLightData = true;
}

//========================================
// VSM Buffer Management
//========================================

void UFluidRendererSubsystem::SwapVSMBuffers()
{
	// Swap VSM buffers: previous frame's write buffer becomes current frame's read buffer
	VSMTexture_Read = VSMTexture_Write;
	LightVPMatrix_Read = LightVPMatrix_Write;
}

