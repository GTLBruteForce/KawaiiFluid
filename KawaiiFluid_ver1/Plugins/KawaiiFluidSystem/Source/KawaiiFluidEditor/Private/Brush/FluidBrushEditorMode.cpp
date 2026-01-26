// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Brush/FluidBrushEditorMode.h"
#include "Components/KawaiiFluidComponent.h"
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

#define LOCTEXT_NAMESPACE "FluidBrushEditorMode"

const FEditorModeID FFluidBrushEditorMode::EM_FluidBrush = TEXT("EM_FluidBrush");

FFluidBrushEditorMode::FFluidBrushEditorMode()
{
	// FEdMode 멤버 명시적 참조
	FEdMode::Info = FEditorModeInfo(
		EM_FluidBrush,
		LOCTEXT("FluidBrushModeName", "Fluid Brush"),
		FSlateIcon(),
		false  // 툴바에 표시 안함
	);
}

FFluidBrushEditorMode::~FFluidBrushEditorMode()
{
}

void FFluidBrushEditorMode::Enter()
{
	FEdMode::Enter();

	// 선택 변경 델리게이트 바인딩
	if (GEditor)
	{
		SelectionChangedHandle = USelection::SelectionChangedEvent.AddRaw(
			this, &FFluidBrushEditorMode::OnSelectionChanged);
	}

	UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode Entered"));
}

void FFluidBrushEditorMode::Exit()
{
	// 선택 변경 델리게이트 언바인딩
	if (SelectionChangedHandle.IsValid())
	{
		USelection::SelectionChangedEvent.Remove(SelectionChangedHandle);
		SelectionChangedHandle.Reset();
	}

	// Component 모드 정리
	if (TargetComponent.IsValid())
	{
		TargetComponent->bBrushModeActive = false;
	}
	TargetComponent.Reset();

	// Volume 모드 정리
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

void FFluidBrushEditorMode::SetTargetComponent(UKawaiiFluidComponent* Component)
{
	// 기존 Volume 타겟 클리어
	TargetVolume.Reset();
	TargetVolumeComponent.Reset();

	TargetComponent = Component;
	if (Component)
	{
		Component->bBrushModeActive = true;
		TargetOwnerActor = Component->GetOwner();
	}
	else
	{
		TargetOwnerActor.Reset();
	}
}

void FFluidBrushEditorMode::SetTargetVolume(AKawaiiFluidVolume* Volume)
{
	// 기존 Component 타겟 클리어
	TargetComponent.Reset();

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

bool FFluidBrushEditorMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                      FKey Key, EInputEvent Event)
{
	// Component 또는 Volume 중 하나가 유효해야 함
	const bool bHasComponent = TargetComponent.IsValid();
	const bool bHasVolume = TargetVolume.IsValid() && TargetVolumeComponent.IsValid();

	if (!bHasComponent && !bHasVolume)
	{
		return false;
	}

	// BrushSettings 참조 (분기 처리)
	FFluidBrushSettings& Settings = bHasVolume 
		? TargetVolumeComponent->BrushSettings 
		: TargetComponent->BrushSettings;

	// 좌클릭: 페인팅
	if (Key == EKeys::LeftMouseButton)
	{
		// Alt + 좌클릭 = 카메라 회전, 패스스루
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
		// ESC: 종료
		if (Key == EKeys::Escape)
		{
			GetModeManager()->DeactivateMode(EM_FluidBrush);
			return true;
		}

		// [ ]: 크기 조절
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

		// 1, 2: 모드 전환
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

bool FFluidBrushEditorMode::HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy,
                                         const FViewportClick& Click)
{
	// 좌클릭은 브러시로 처리, 선택 동작 막음
	if (Click.GetKey() == EKeys::LeftMouseButton && !InViewportClient->IsAltPressed())
	{
		return true;  // 클릭 처리됨 - 선택 막음
	}
	return false;
}

bool FFluidBrushEditorMode::StartTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	// 트래킹 모드 사용 안 함 - InputKey에서 직접 처리
	return false;
}

bool FFluidBrushEditorMode::EndTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport)
{
	return false;
}

bool FFluidBrushEditorMode::MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                       int32 x, int32 y)
{
	UpdateBrushLocation(ViewportClient, x, y);

	// 페인팅 중이면 브러시 적용
	if (bPainting && bValidLocation)
	{
		ApplyBrush();
	}

	return false;
}

bool FFluidBrushEditorMode::CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                               int32 InMouseX, int32 InMouseY)
{
	UpdateBrushLocation(ViewportClient, InMouseX, InMouseY);

	if (bPainting && bValidLocation)
	{
		ApplyBrush();
	}

	return bPainting;
}

bool FFluidBrushEditorMode::UpdateBrushLocation(FEditorViewportClient* ViewportClient,
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

	// Volume 박스 정보 미리 계산
	FBox VolumeBounds;
	float tEntry = -1.0f;  // 진입점 (카메라→박스)
	float tExit = -1.0f;   // 출구점 (박스 반대편)
	int32 entryAxis = -1;
	int32 exitAxis = -1;
	bool bEntryMinSide = false;
	bool bExitMinSide = false;
	bool bHasVolumeIntersection = false;
	bool bCameraInsideBox = false;

	if (TargetVolumeComponent.IsValid())
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

	// 라인트레이스로 스태틱 메시 검사
	FHitResult Hit;
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = true;

	if (World->LineTraceSingleByChannel(Hit, Origin, Origin + Direction * 50000.0f, ECC_Visibility, QueryParams))
	{
		// 히트가 박스 내부에 있는지 확인
		if (bHasVolumeIntersection && VolumeBounds.IsInsideOrOn(Hit.Location))
		{
			BrushLocation = Hit.Location;
			BrushNormal = Hit.ImpactNormal;
			bValidLocation = true;
			return true;
		}
		// 히트가 박스 밖이면 fallthrough해서 박스 면 사용
	}

	// 박스 면에 브러시 위치
	if (bHasVolumeIntersection)
	{
		float tHit;
		int32 hitAxis;
		bool bMinSide;

		if (bCameraInsideBox)
		{
			// 카메라가 박스 안 → 출구점(반대편 면) 사용
			tHit = tExit;
			hitAxis = exitAxis;
			bMinSide = bExitMinSide;
		}
		else if (tEntry >= 0.0f)
		{
			// 카메라가 박스 밖 → 진입점 사용
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
				// 노말은 카메라를 향해야 함 (박스 안쪽으로 향하는 노말)
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

	// 히트 실패 시 브러시 비활성화
	bValidLocation = false;
	return false;
}

void FFluidBrushEditorMode::ApplyBrush()
{
	if (!bValidLocation)
	{
		return;
	}

	// Component 또는 Volume 중 하나가 유효해야 함
	const bool bHasComponent = TargetComponent.IsValid();
	const bool bHasVolume = TargetVolume.IsValid() && TargetVolumeComponent.IsValid();

	if (!bHasComponent && !bHasVolume)
	{
		return;
	}

	// BrushSettings 참조 (분기 처리)
	const FFluidBrushSettings& Settings = bHasVolume 
		? TargetVolumeComponent->BrushSettings 
		: TargetComponent->BrushSettings;

	// 스트로크 간격
	double Now = FPlatformTime::Seconds();
	if (Now - LastStrokeTime < Settings.StrokeInterval)
	{
		return;
	}
	LastStrokeTime = Now;

	// Volume 모드
	if (bHasVolume)
	{
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
				TargetVolume->RemoveParticlesInRadius(BrushLocation, Settings.Radius);
				break;
		}
	}
	// Component 모드
	else
	{
		TargetComponent->Modify();
		switch (Settings.Mode)
		{
			case EFluidBrushMode::Add:
				TargetComponent->AddParticlesInRadius(
					BrushLocation,
					Settings.Radius,
					Settings.ParticlesPerStroke,
					Settings.InitialVelocity,
					Settings.Randomness,
					BrushNormal
				);
				break;

			case EFluidBrushMode::Remove:
				TargetComponent->RemoveParticlesInRadius(BrushLocation, Settings.Radius);
				break;
		}
	}
}

void FFluidBrushEditorMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	FEdMode::Render(View, Viewport, PDI);

	const bool bHasTarget = TargetComponent.IsValid() || (TargetVolume.IsValid() && TargetVolumeComponent.IsValid());
	if (bValidLocation && bHasTarget)
	{
		DrawBrushPreview(PDI);
	}
}

void FFluidBrushEditorMode::DrawBrushPreview(FPrimitiveDrawInterface* PDI)
{
	// Component 또는 Volume 중 하나가 유효해야 함
	const bool bHasComponent = TargetComponent.IsValid();
	const bool bHasVolume = TargetVolume.IsValid() && TargetVolumeComponent.IsValid();

	if (!bHasComponent && !bHasVolume)
	{
		return;
	}

	// BrushSettings 참조 (분기 처리)
	const FFluidBrushSettings& Settings = bHasVolume 
		? TargetVolumeComponent->BrushSettings 
		: TargetComponent->BrushSettings;
	FColor Color = GetBrushColor().ToFColor(true);

	// 노말 기준 원 (실제 스폰 영역 - 반구의 바닥면)
	FVector Tangent, Bitangent;
	BrushNormal.FindBestAxisVectors(Tangent, Bitangent);
	DrawCircle(PDI, BrushLocation, Tangent, Bitangent, Color, Settings.Radius, 32, SDPG_Foreground);

	// 노말 방향 화살표 (스폰 방향 표시)
	FVector ArrowEnd = BrushLocation + BrushNormal * Settings.Radius;
	PDI->DrawLine(BrushLocation, ArrowEnd, Color, SDPG_Foreground, 2.0f);

	// 화살표 머리
	FVector ArrowHead1 = ArrowEnd - BrushNormal * 15.0f + Tangent * 8.0f;
	FVector ArrowHead2 = ArrowEnd - BrushNormal * 15.0f - Tangent * 8.0f;
	PDI->DrawLine(ArrowEnd, ArrowHead1, Color, SDPG_Foreground, 2.0f);
	PDI->DrawLine(ArrowEnd, ArrowHead2, Color, SDPG_Foreground, 2.0f);

	// 중심점
	PDI->DrawPoint(BrushLocation, Color, 8.0f, SDPG_Foreground);
}

FLinearColor FFluidBrushEditorMode::GetBrushColor() const
{
	// Component 또는 Volume 중 하나가 유효해야 함
	const bool bHasComponent = TargetComponent.IsValid();
	const bool bHasVolume = TargetVolume.IsValid() && TargetVolumeComponent.IsValid();

	if (!bHasComponent && !bHasVolume)
	{
		return FLinearColor::White;
	}

	// BrushSettings 참조 (분기 처리)
	EFluidBrushMode Mode = bHasVolume 
		? TargetVolumeComponent->BrushSettings.Mode 
		: TargetComponent->BrushSettings.Mode;

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

void FFluidBrushEditorMode::DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                     const FSceneView* View, FCanvas* Canvas)
{
	FEdMode::DrawHUD(ViewportClient, Viewport, View, Canvas);

	// Component 또는 Volume 중 하나가 유효해야 함
	const bool bHasComponent = TargetComponent.IsValid();
	const bool bHasVolume = TargetVolume.IsValid() && TargetVolumeComponent.IsValid();

	if (!Canvas || (!bHasComponent && !bHasVolume) || !GEngine)
	{
		return;
	}

	// BrushSettings 참조 (분기 처리)
	const FFluidBrushSettings& Settings = bHasVolume 
		? TargetVolumeComponent->BrushSettings 
		: TargetComponent->BrushSettings;
	FString ModeStr = (Settings.Mode == EFluidBrushMode::Add) ? TEXT("ADD") : TEXT("REMOVE");

	// 파티클 개수 (분기 처리)
	int32 ParticleCount = -1;
	if (bHasVolume)
	{
		// Volume 모드: Volume의 SimulationModule에서 전체 파티클 수
		if (UKawaiiFluidSimulationModule* SimModule = TargetVolume->GetSimulationModule())
		{
			ParticleCount = SimModule->GetParticleCount();
		}
	}
	else
	{
		// Component 모드: 이 컴포넌트가 소유한 파티클 수 (per-source count)
		if (TargetComponent->GetSimulationModule()) 
		{
			const int32 SourceID = TargetComponent->GetSimulationModule()->GetSourceID();
			ParticleCount = TargetComponent->GetSimulationModule()->GetParticleCountForSource(SourceID);
		}
	}

	FString ParticleStr = (ParticleCount >= 0) ? FString::FromInt(ParticleCount) : TEXT("-");
	FString TargetTypeStr = bHasVolume ? TEXT("Volume") : TEXT("Component");
	FString InfoText = FString::Printf(TEXT("[%s] Brush: %s | Radius: %.0f | Particles: %s | [ ] Size | 1/2 Mode | ESC Exit"),
	                               *TargetTypeStr, *ModeStr, Settings.Radius, *ParticleStr);

	FCanvasTextItem Text(FVector2D(10, 40), FText::FromString(InfoText),
	                     GEngine->GetSmallFont(), GetBrushColor());
	Canvas->DrawItem(Text);
}

bool FFluidBrushEditorMode::DisallowMouseDeltaTracking() const
{
	// Component 또는 Volume 중 하나가 유효해야 함
	const bool bHasComponent = TargetComponent.IsValid();
	const bool bHasVolume = TargetVolume.IsValid() && TargetVolumeComponent.IsValid();

	if (!bHasComponent && !bHasVolume)
	{
		return false;
	}

	// RMB/MMB는 카메라 조작 허용
	const TSet<FKey>& PressedButtons = FSlateApplication::Get().GetPressedMouseButtons();
	if (PressedButtons.Contains(EKeys::RightMouseButton) || PressedButtons.Contains(EKeys::MiddleMouseButton))
	{
		return false;
	}

	// Alt 누르면 카메라 오빗 허용
	if (FSlateApplication::Get().GetModifierKeys().IsAltDown())
	{
		return false;
	}

	// 그 외 (LMB 단독) = 브러시 모드이므로 카메라 트래킹 비활성화
	return true;
}

void FFluidBrushEditorMode::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{
	FEdMode::Tick(ViewportClient, DeltaTime);

	// Component 또는 Volume 중 하나가 유효해야 함
	const bool bHasComponent = TargetComponent.IsValid();
	const bool bHasVolume = TargetVolume.IsValid() && TargetVolumeComponent.IsValid();

	// 타겟이 삭제됨
	if (!bHasComponent && !bHasVolume)
	{
		UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode: Target destroyed, exiting"));
		GetModeManager()->DeactivateMode(EM_FluidBrush);
		return;
	}

	// 조건 5: 뷰포트 포커스 잃음 체크
	if (ViewportClient && !ViewportClient->Viewport->HasFocus())
	{
		// 다른 창으로 포커스 이동 시에만 종료 (짧은 포커스 손실은 무시)
		// 실제로는 뷰포트 전환 시에만 중요하므로 일단 생략
		// 필요하면 타이머로 일정 시간 포커스 없으면 종료하도록 구현
	}
}

void FFluidBrushEditorMode::OnSelectionChanged(UObject* Object)
{
	// 페인팅 중에는 선택 변경 무시
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

	// 아무것도 선택 안 됨 -> 종료
	if (Selection->Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode: Selection cleared, exiting"));
		GetModeManager()->DeactivateMode(EM_FluidBrush);
		return;
	}

	// 타겟 액터가 여전히 선택되어 있는지 확인
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
