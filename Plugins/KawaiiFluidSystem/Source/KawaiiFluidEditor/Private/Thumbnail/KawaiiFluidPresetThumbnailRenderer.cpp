// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Thumbnail/KawaiiFluidPresetThumbnailRenderer.h"

#include "CanvasTypes.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "SceneView.h"
#include "Misc/App.h"
#include "ShowFlags.h"
#include "ThumbnailHelpers.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "RendererInterface.h"
#include "Modules/ModuleManager.h"
#include "GameTime.h"
#include "LegacyScreenPercentageDriver.h"

/**
 * Internal helper class for 3D thumbnail preview
 */
class FKawaiiFluidPresetThumbnailScene : public FThumbnailPreviewScene
{
public:
	FKawaiiFluidPresetThumbnailScene()
		: FThumbnailPreviewScene()
	{
		// 1. Create sphere mesh component (use Transient Package as Outer to prevent orphan objects)
		PreviewMeshComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);

		UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EditorMeshes/AssetViewer/Sphere.Sphere"));
		if (!SphereMesh)
		{
			// Fallback to basic engine sphere in case path changed
			SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		}

		if (SphereMesh)
		{
			PreviewMeshComponent->SetStaticMesh(SphereMesh);
		}

		// Add to scene
		AddComponent(PreviewMeshComponent, FTransform::Identity);

		// 2. Material setup (load default preview material)
		UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/KawaiiFluidSystem/Material/ThumbnailMaterial"));
		if (BaseMat)
		{
			ThumbnailMID = UMaterialInstanceDynamic::Create(BaseMat, GetTransientPackage());
			PreviewMeshComponent->SetMaterial(0, ThumbnailMID);
		}

		// 3. Lighting setup
		if (DirectionalLight)
		{
			DirectionalLight->SetRelativeRotation(FRotator(-45.0f, -45.0f, 0.0f));
			DirectionalLight->SetIntensity(3.0f);
		}

		// Skylight for soft shadows
		if (SkyLight)
		{
			SkyLight->SetIntensity(1.0f);
		}
	}
	
	void SetFluidParameters(UKawaiiFluidPresetDataAsset* Preset)
	{
		if (ThumbnailMID && Preset)
		{
			// 1. Set fluid color
			ThumbnailMID->SetVectorParameterValue(TEXT("Base Color"), Preset->RenderingParameters.FluidColor);

			//ThumbnailMID->SetScalarParameterValue(TEXT("Opacity"), Preset->RenderingParameters.ThicknessScale / 10.f);

			// 2. Set texture (if available from SurfaceDecoration Layer)
			if (UTexture2D* TargetTex = Preset->RenderingParameters.SurfaceDecoration.Layer.Texture)
			{
				ThumbnailMID->SetTextureParameterValue(TEXT("FluidTexture"), TargetTex);
			}
			else
			{
				static UTexture2D* DefaultWhite = LoadObject<UTexture2D>(nullptr,
					TEXT("/KawaiiFluidSystem/Textures/ThumbnailDefaultTexture"));
				ThumbnailMID->SetTextureParameterValue(TEXT("FluidTexture"), DefaultWhite);
			}
		}
		PreviewMeshComponent->MarkRenderStateDirty();
	}
	
	// FThumbnailPreviewScene interface implementation (required)
	virtual void GetViewMatrixParameters(const float InFOVDegrees, FVector& OutOrigin, float& OutOrbitPitch, float& OutOrbitYaw, float& OutOrbitZoom) const override
	{
		OutOrigin = FVector::ZeroVector;
		OutOrbitPitch = -45.0f;
		OutOrbitYaw = -135.0f;
		OutOrbitZoom = PreviewMeshComponent ? (PreviewMeshComponent->Bounds.SphereRadius * 2.5f) : 0.0f;
	}

	void Draw(FCanvas* Canvas, const FIntRect& Rect)
	{
		if (!PreviewMeshComponent || !PreviewMeshComponent->GetStaticMesh())
	{
		return;
	}

		// Create FGameTime (UE 5.7 compatible)
		FGameTime GameTime = FGameTime::CreateUndilated(FApp::GetCurrentTime(), FApp::GetDeltaTime());

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			Canvas->GetRenderTarget(),
			GetScene(),
			FEngineShowFlags(ESFIM_Game))
			.SetTime(GameTime));

		ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(ViewFamily, 1.0f));
		
		ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
		ViewFamily.EngineShowFlags.ScreenPercentage = 0; // Disable screen percentage
		ViewFamily.EngineShowFlags.MotionBlur = 0;
		ViewFamily.EngineShowFlags.LOD = 0;

		// Calculate view matrix (use 30 degree FOV to reduce distortion)
		FVector Origin;
		float Pitch, Yaw, Zoom;
		const float FOV = 30.0f;
		GetViewMatrixParameters(FOV, Origin, Pitch, Yaw, Zoom);

		const float HalfFOVRadians = FMath::DegreesToRadians(FOV) * 0.5f;
		// Auto-calculate based on bounds if zoom distance is 0
		const float DistanceFromMesh = (Zoom > 0.0f) ? Zoom : (PreviewMeshComponent->Bounds.SphereRadius / FMath::Tan(HalfFOVRadians));
		
		FSceneViewInitOptions ViewInitOptions;
		ViewInitOptions.ViewFamily = &ViewFamily;
		ViewInitOptions.SetViewRectangle(Rect);
		
		// Apply orbit rotation
		FRotator ViewRotation(Pitch, Yaw, 0.0f);
		ViewInitOptions.ViewOrigin = Origin - (ViewRotation.Vector() * DistanceFromMesh);
		ViewInitOptions.ViewRotationMatrix = FInverseRotationMatrix(ViewRotation) * FMatrix(
			FVector(0, 0, 1),
			FVector(1, 0, 0),
			FVector(0, 1, 0),
			FVector::ZeroVector);

		ViewInitOptions.ProjectionMatrix = FReversedZPerspectiveMatrix(
			FMath::DegreesToRadians(FOV),
			Rect.Width(),
			Rect.Height(),
			0.01f);

		FSceneView* View = new FSceneView(ViewInitOptions);
		ViewFamily.Views.Add(View);

		// Safely invoke the Renderer module
		IRendererModule& RendererModule = FModuleManager::GetModuleChecked<IRendererModule>("Renderer");
		RendererModule.BeginRenderingViewFamily(Canvas, &ViewFamily);
	}

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		FThumbnailPreviewScene::AddReferencedObjects(Collector);
		Collector.AddReferencedObject(PreviewMeshComponent);
		Collector.AddReferencedObject(ThumbnailMID);
	}

private:
	TObjectPtr<UStaticMeshComponent> PreviewMeshComponent;
	TObjectPtr<UMaterialInstanceDynamic> ThumbnailMID;
};

UKawaiiFluidPresetThumbnailRenderer::UKawaiiFluidPresetThumbnailRenderer()
	: Super()
	, ThumbnailScene(nullptr)
{
}

void UKawaiiFluidPresetThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalContext)
{
	UKawaiiFluidPresetDataAsset* Preset = Cast<UKawaiiFluidPresetDataAsset>(Object);
	if (!Preset)
	{
		return;
	}

	if (!ThumbnailScene)
	{
		ThumbnailScene = new FKawaiiFluidPresetThumbnailScene();
	}

	ThumbnailScene->SetFluidParameters(Preset);
	ThumbnailScene->Draw(Canvas, FIntRect(X, Y, X + Width, Y + Height));
	FlushRenderingCommands();
}

void UKawaiiFluidPresetThumbnailRenderer::BeginDestroy()
{
	if (ThumbnailScene)
	{
		delete ThumbnailScene;
		ThumbnailScene = nullptr;
	}

	Super::BeginDestroy();
}