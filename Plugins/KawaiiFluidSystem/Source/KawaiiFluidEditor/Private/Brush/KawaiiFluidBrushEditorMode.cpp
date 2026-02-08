// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Brush/KawaiiFluidBrushEditorMode.h"
#include "Components/KawaiiFluidVolumeComponent.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "SceneView.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "EditorModeManager.h"
#include "LevelEditorViewport.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "ScopedTransaction.h"
#include "Selection.h"

#define LOCTEXT_NAMESPACE "KawaiiFluidBrushEditorMode"

const FEditorModeID FKawaiiFluidBrushEditorMode::EM_FluidBrush = TEXT("EM_FluidBrush");

FKawaiiFluidBrushEditorMode::FKawaiiFluidBrushEditorMode()
{
	// Explicit FEdMode member reference
	FEdMode::Info = FEditorModeInfo(
		EM_FluidBrush,
		LOCTEXT("FluidBrushModeName", "Fluid Brush"),
		FSlateIcon(),
		false  // Do not show in toolbar
	);
}

FKawaiiFluidBrushEditorMode::~FKawaiiFluidBrushEditorMode()
{
}

void FKawaiiFluidBrushEditorMode::Enter()
{
	FEdMode::Enter();

	// Bind selection changed delegate
	if (GEditor)
	{
		SelectionChangedHandle = USelection::SelectionChangedEvent.AddRaw(
			this, &FKawaiiFluidBrushEditorMode::OnSelectionChanged);
	}

	UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode Entered"));
}

void FKawaiiFluidBrushEditorMode::Exit()
{
	// Unbind selection changed delegate
	if (SelectionChangedHandle.IsValid())
	{
		USelection::SelectionChangedEvent.Remove(SelectionChangedHandle);
		SelectionChangedHandle.Reset();
	}

	// Cleanup Volume mode
	if (TargetVolumeComponent.IsValid())
	{
		TargetVolumeComponent->bBrushModeActive = false;
	}
	TargetVolume.Reset();
	TargetVolumeComponent.Reset();

	TargetOwnerActor.Reset();
	bPainting = false;

	FEdMode::Exit();
	UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode Exited"));
}

void FKawaiiFluidBrushEditorMode::SetTargetVolume(AKawaiiFluidVolume* Volume)
{
	TargetVolume = Volume;
	if (Volume)
	{
		TargetVolumeComponent = Volume->GetVolumeComponent();
		if (TargetVolumeComponent.IsValid())
		{
			TargetVolumeComponent->bBrushModeActive = true;
		}
		TargetOwnerActor = Volume;
	}
	else
	{
		TargetVolumeComponent.Reset();
		TargetOwnerActor.Reset();
	}
}

bool FKawaiiFluidBrushEditorMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                      FKey Key, EInputEvent Event)
{
	if (!TargetVolume.IsValid() || !TargetVolumeComponent.IsValid())
	{
		return false;
	}

	FFluidBrushSettings& Settings = TargetVolumeComponent->BrushSettings;

	// Left click: Painting
	if (Key == EKeys::LeftMouseButton)
	{
		// Alt + Left click = Camera rotation, pass through
		if (ViewportClient->IsAltPressed())
		{
			return false;
		}

		if (Event == IE_Pressed)
		{
			bPainting = true;
			LastStrokeTime = 0.0;

			if (bValidLocation)
			{
				ApplyBrush();
			}
			return true;
		}
		else if (Event == IE_Released)
		{
			bPainting = false;
			return true;
		}
	}

	if (Event == IE_Pressed)
	{
		// ESC: Exit
		if (Key == EKeys::Escape)
		{
			GetModeManager()->DeactivateMode(EM_FluidBrush);
			return true;
		}

		// [ ]: Adjust size
		if (Key == EKeys::LeftBracket)
		{
			Settings.Radius = FMath::Max(10.0f, Settings.Radius - 10.0f);
			return true;
		}
		if (Key == EKeys::RightBracket)
		{
			Settings.Radius = FMath::Min(500.0f, Settings.Radius + 10.0f);
			return true;
		}

		// 1, 2: Switch mode
		if (Key == EKeys::One)
		{
			Settings.Mode = EFluidBrushMode::Add;
			return true;
		}
		if (Key == EKeys::Two)
		{
			Settings.Mode = EFluidBrushMode::Remove;
			return true;
		}
	}

	return false;
}

bool FKawaiiFluidBrushEditorMode::HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy,
                                         const FViewportClick& Click)
{
	// Left click is handled by brush, block selection behavior
	if (Click.GetKey() == EKeys::LeftMouseButton && !InViewportClient->IsAltPressed())
	{
		return true;  // Click handled - block selection
	}
	return false;
}

bool FKawaiiFluidBrushEditorMode::StartTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// Tracking mode not used - handled directly in InputKey
	return false;
}

bool FKawaiiFluidBrushEditorMode::EndTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	return false;
}

bool FKawaiiFluidBrushEditorMode::MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                       int32 x, int32 y)
{
	UpdateBrushLocation(ViewportClient, x, y);

	// Apply brush if painting
	if (bPainting && bValidLocation)
	{
		ApplyBrush();
	}

	return false;
}

bool FKawaiiFluidBrushEditorMode::CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                               int32 InMouseX, int32 InMouseY)
{
	UpdateBrushLocation(ViewportClient, InMouseX, InMouseY);

	if (bPainting && bValidLocation)
	{
		ApplyBrush();
	}

	return bPainting;
}

bool FKawaiiFluidBrushEditorMode::UpdateBrushLocation(FEditorViewportClient* ViewportClient,
                                                 int32 MouseX, int32 MouseY)
{
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		ViewportClient->Viewport,
		ViewportClient->GetScene(),
		ViewportClient->EngineShowFlags
	));

	FSceneView* View = ViewportClient->CalcSceneView(&ViewFamily);
	if (!View)
	{
		bValidLocation = false;
		return false;
	}

	FVector Origin, Direction;
	View->DeprojectFVector2D(FVector2D(MouseX, MouseY), Origin, Direction);

	UWorld* World = GetWorld();
	if (!World)
	{
		bValidLocation = false;
		return false;
	}

	// Check if unlimited size mode is enabled
	const bool bUnlimitedSize = TargetVolumeComponent.IsValid() && TargetVolumeComponent->bUseUnlimitedSize;

	// Pre-calculate Volume box information (only needed if not unlimited size)
	FBox VolumeBounds;
	float tEntry = -1.0f;  // Entry point (camera→box)
	float tExit = -1.0f;   // Exit point (far side of box)
	int32 entryAxis = -1;
	int32 exitAxis = -1;
	bool bEntryMinSide = false;
	bool bExitMinSide = false;
	bool bHasVolumeIntersection = false;
	bool bCameraInsideBox = false;

	if (!bUnlimitedSize && TargetVolumeComponent.IsValid())
	{
		VolumeBounds = TargetVolumeComponent->Bounds.GetBox();
		if (VolumeBounds.IsValid)
		{
			const FVector BoxMin = VolumeBounds.Min;
			const FVector BoxMax = VolumeBounds.Max;

			float tMin = -FLT_MAX;
			float tMax = FLT_MAX;

			for (int32 i = 0; i < 3; ++i)
			{
				const float dirComp = Direction[i];
				const float originComp = Origin[i];

				if (FMath::Abs(dirComp) < KINDA_SMALL_NUMBER)
				{
					if (originComp < BoxMin[i] || originComp > BoxMax[i])
					{
						tMin = FLT_MAX;
						break;
					}
				}
				else
				{
					float t1 = (BoxMin[i] - originComp) / dirComp;
					float t2 = (BoxMax[i] - originComp) / dirComp;

					bool bT1IsEntry = (t1 < t2);
					if (!bT1IsEntry)
					{
						float temp = t1;
						t1 = t2;
						t2 = temp;
					}

					if (t1 > tMin)
					{
						tMin = t1;
						entryAxis = i;
						bEntryMinSide = bT1IsEntry;
					}
					if (t2 < tMax)
					{
						tMax = t2;
						exitAxis = i;
						bExitMinSide = !bT1IsEntry;
					}
				}
			}

			if (tMin <= tMax)
			{
				bHasVolumeIntersection = true;
				tEntry = tMin;
				tExit = tMax;
				bCameraInsideBox = (tMin < 0.0f && tMax > 0.0f);
			}
		}
	}

	// Line trace to check static meshes
	FHitResult Hit;
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = true;

	if (World->LineTraceSingleByChannel(Hit, Origin, Origin + Direction * 50000.0f, ECC_Visibility, QueryParams))
	{
		// Unlimited size mode: accept any world hit
		if (bUnlimitedSize)
		{
			BrushLocation = Hit.Location;
			BrushNormal = Hit.ImpactNormal;
			bValidLocation = true;
			return true;
		}

		// Limited size mode: check if hit is inside the box
		if (bHasVolumeIntersection && VolumeBounds.IsInsideOrOn(Hit.Location))
		{
			BrushLocation = Hit.Location;
			BrushNormal = Hit.ImpactNormal;
			bValidLocation = true;
			return true;
		}
		// If hit is outside box, fall through to use box face
	}

	// Unlimited size mode: disable brush if no world hit
	if (bUnlimitedSize)
	{
		bValidLocation = false;
		return false;
	}

	// Limited size mode: position brush on box face
	if (bHasVolumeIntersection)
	{
		float tHit;
		int32 hitAxis;
		bool bMinSide;

		if (bCameraInsideBox)
		{
			// Camera inside box → use exit point (far side face)
			tHit = tExit;
			hitAxis = exitAxis;
			bMinSide = bExitMinSide;
		}
		else if (tEntry >= 0.0f)
		{
			// Camera outside box → use entry point
			tHit = tEntry;
			hitAxis = entryAxis;
			bMinSide = bEntryMinSide;
		}
		else
		{
			bValidLocation = false;
			return false;
		}

		if (tHit >= 0.0f && tHit <= 50000.0f)
		{
			BrushLocation = Origin + Direction * tHit;

			BrushNormal = FVector::ZeroVector;
			if (hitAxis >= 0)
			{
				// Normal should face the camera (inward-facing normal)
				BrushNormal[hitAxis] = bMinSide ? 1.0f : -1.0f;
			}
			else
			{
				BrushNormal = FVector::UpVector;
			}

			bValidLocation = true;
			return true;
		}
	}

	// Disable brush on hit failure
	bValidLocation = false;
	return false;
}

void FKawaiiFluidBrushEditorMode::ApplyBrush()
{
	if (!bValidLocation || !TargetVolume.IsValid() || !TargetVolumeComponent.IsValid())
	{
		return;
	}

	const FFluidBrushSettings& Settings = TargetVolumeComponent->BrushSettings;

	// Stroke interval
	double Now = FPlatformTime::Seconds();
	if (Now - LastStrokeTime < Settings.StrokeInterval)
	{
		return;
	}
	LastStrokeTime = Now;

	TargetVolume->Modify();
	switch (Settings.Mode)
	{
		case EFluidBrushMode::Add:
			TargetVolume->AddParticlesInRadius(
				BrushLocation,
				Settings.Radius,
				Settings.ParticlesPerStroke,
				Settings.InitialVelocity,
				Settings.Randomness,
				BrushNormal
			);
			break;

		case EFluidBrushMode::Remove:
			TargetVolume->RemoveParticlesInRadiusGPU(BrushLocation, Settings.Radius);
			break;
	}
}

void FKawaiiFluidBrushEditorMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	FEdMode::Render(View, Viewport, PDI);

	if (bValidLocation && TargetVolume.IsValid() && TargetVolumeComponent.IsValid())
	{
		DrawBrushPreview(PDI);
	}
}

void FKawaiiFluidBrushEditorMode::DrawBrushPreview(FPrimitiveDrawInterface* PDI)
{
	if (!TargetVolume.IsValid() || !TargetVolumeComponent.IsValid())
	{
		return;
	}

	const FFluidBrushSettings& Settings = TargetVolumeComponent->BrushSettings;
	FColor Color = GetBrushColor().ToFColor(true);

	// Circle based on normal (actual spawn area - hemisphere base)
	FVector Tangent, Bitangent;
	BrushNormal.FindBestAxisVectors(Tangent, Bitangent);
	DrawCircle(PDI, BrushLocation, Tangent, Bitangent, Color, Settings.Radius, 32, SDPG_Foreground);

	// Arrow in normal direction (shows spawn direction)
	FVector ArrowEnd = BrushLocation + BrushNormal * Settings.Radius;
	PDI->DrawLine(BrushLocation, ArrowEnd, Color, SDPG_Foreground, 2.0f);

	// Arrow head
	FVector ArrowHead1 = ArrowEnd - BrushNormal * 15.0f + Tangent * 8.0f;
	FVector ArrowHead2 = ArrowEnd - BrushNormal * 15.0f - Tangent * 8.0f;
	PDI->DrawLine(ArrowEnd, ArrowHead1, Color, SDPG_Foreground, 2.0f);
	PDI->DrawLine(ArrowEnd, ArrowHead2, Color, SDPG_Foreground, 2.0f);

	// Center point
	PDI->DrawPoint(BrushLocation, Color, 8.0f, SDPG_Foreground);
}

FLinearColor FKawaiiFluidBrushEditorMode::GetBrushColor() const
{
	if (!TargetVolume.IsValid() || !TargetVolumeComponent.IsValid())
	{
		return FLinearColor::White;
	}

	EFluidBrushMode Mode = TargetVolumeComponent->BrushSettings.Mode;

	switch (Mode)
	{
		case EFluidBrushMode::Add:
			return FLinearColor(0.2f, 0.9f, 0.3f, 0.8f);  // Green
		case EFluidBrushMode::Remove:
			return FLinearColor(0.9f, 0.2f, 0.2f, 0.8f);  // Red
		default:
			return FLinearColor::White;
	}
}

void FKawaiiFluidBrushEditorMode::DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                     const FSceneView* View, FCanvas* Canvas)
{
	FEdMode::DrawHUD(ViewportClient, Viewport, View, Canvas);

	if (!Canvas || !TargetVolume.IsValid() || !TargetVolumeComponent.IsValid() || !GEngine)
	{
		return;
	}

	const FFluidBrushSettings& Settings = TargetVolumeComponent->BrushSettings;
	FString ModeStr = (Settings.Mode == EFluidBrushMode::Add) ? TEXT("ADD") : TEXT("REMOVE");

	int32 ParticleCount = -1;
	if (UKawaiiFluidSimulationModule* SimModule = TargetVolume->GetSimulationModule())
	{
		ParticleCount = SimModule->GetParticleCount();
	}

	FString ParticleStr = (ParticleCount >= 0) ? FString::FromInt(ParticleCount) : TEXT("-");
	FString InfoText = FString::Printf(TEXT("[Volume] Brush: %s | Radius: %.0f | Particles: %s | [ ] Size | 1/2 Mode | ESC Exit"),
	                               *ModeStr, Settings.Radius, *ParticleStr);

	FCanvasTextItem Text(FVector2D(10, 40), FText::FromString(InfoText),
	                     GEngine->GetSmallFont(), GetBrushColor());
	Canvas->DrawItem(Text);
}

bool FKawaiiFluidBrushEditorMode::DisallowMouseDeltaTracking() const
{
	if (!TargetVolume.IsValid() || !TargetVolumeComponent.IsValid())
	{
		return false;
	}

	// Allow camera manipulation with RMB/MMB
	const TSet<FKey>& PressedButtons = FSlateApplication::Get().GetPressedMouseButtons();
	if (PressedButtons.Contains(EKeys::RightMouseButton) || PressedButtons.Contains(EKeys::MiddleMouseButton))
	{
		return false;
	}

	// Allow camera orbit when Alt is pressed
	if (FSlateApplication::Get().GetModifierKeys().IsAltDown())
	{
		return false;
	}

	// Otherwise (LMB only) = brush mode so disable camera tracking
	return true;
}

void FKawaiiFluidBrushEditorMode::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{
	FEdMode::Tick(ViewportClient, DeltaTime);

	// Target destroyed
	if (!TargetVolume.IsValid() || !TargetVolumeComponent.IsValid())
	{
		UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode: Target destroyed, exiting"));
		GetModeManager()->DeactivateMode(EM_FluidBrush);
		return;
	}

	// Condition 5: Check viewport focus loss
	if (ViewportClient && !ViewportClient->Viewport->HasFocus())
	{
		// Exit only when focus moves to another window (ignore brief focus loss)
		// Currently omitted as it's only important for viewport switching
		// Can implement with timer to exit after certain duration without focus
	}
}

void FKawaiiFluidBrushEditorMode::OnSelectionChanged(UObject* Object)
{
	// Ignore selection changes while painting
	if (bPainting)
	{
		return;
	}

	if (!GEditor)
	{
		return;
	}

	USelection* Selection = GEditor->GetSelectedActors();
	if (!Selection)
	{
		return;
	}

	// Nothing selected -> exit
	if (Selection->Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode: Selection cleared, exiting"));
		GetModeManager()->DeactivateMode(EM_FluidBrush);
		return;
	}

	// Check if target actor is still selected
	if (TargetOwnerActor.IsValid())
	{
		bool bTargetStillSelected = Selection->IsSelected(TargetOwnerActor.Get());
		if (!bTargetStillSelected)
		{
			UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode: Different actor selected, exiting"));
			GetModeManager()->DeactivateMode(EM_FluidBrush);
			return;
		}
	}
}

#undef LOCTEXT_NAMESPACE
