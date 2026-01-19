// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Brush/FluidBrushEditorMode.h"
#include "Components/KawaiiFluidComponent.h"
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

	if (TargetComponent.IsValid())
	{
		TargetComponent->bBrushModeActive = false;
	}
	TargetComponent.Reset();
	TargetOwnerActor.Reset();
	bPainting = false;

	FEdMode::Exit();
	UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode Exited"));
}

void FFluidBrushEditorMode::SetTargetComponent(UKawaiiFluidComponent* Component)
{
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

bool FFluidBrushEditorMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
                                      FKey Key, EInputEvent Event)
{
	if (!TargetComponent.IsValid())
	{
		return false;
	}

	FFluidBrushSettings& Settings = TargetComponent->BrushSettings;

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

			// 드래그 시작 시 트랜잭션 생성 (Undo 한 번에 전체 드래그 취소)
			DragTransaction = MakeUnique<FScopedTransaction>(LOCTEXT("FluidBrushDrag", "Fluid Brush Drag"));

			if (bValidLocation)
			{
				ApplyBrush();
			}
			return true;
		}
		else if (Event == IE_Released)
		{
			bPainting = false;

			// 드래그 종료 시 트랜잭션 커밋
			DragTransaction.Reset();

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

	// 레이캐스트
	UWorld* World = GetWorld();
	if (!World)
	{
		bValidLocation = false;
		return false;
	}

	FHitResult Hit;
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = true;

	if (World->LineTraceSingleByChannel(Hit, Origin, Origin + Direction * 50000.0f, ECC_Visibility, QueryParams))
	{
		BrushLocation = Hit.Location;
		BrushNormal = Hit.ImpactNormal;
		bValidLocation = true;
		return true;
	}

	// 히트 실패 시 브러시 비활성화 (허공에선 스폰 안 함)
	bValidLocation = false;
	return false;
}

void FFluidBrushEditorMode::ApplyBrush()
{
	if (!TargetComponent.IsValid() || !bValidLocation)
	{
		return;
	}

	const FFluidBrushSettings& Settings = TargetComponent->BrushSettings;

	// 스트로크 간격
	double Now = FPlatformTime::Seconds();
	if (Now - LastStrokeTime < Settings.StrokeInterval)
	{
		return;
	}
	LastStrokeTime = Now;

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

void FFluidBrushEditorMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	FEdMode::Render(View, Viewport, PDI);

	if (bValidLocation && TargetComponent.IsValid())
	{
		DrawBrushPreview(PDI);
	}
}

void FFluidBrushEditorMode::DrawBrushPreview(FPrimitiveDrawInterface* PDI)
{
	if (!TargetComponent.IsValid())
	{
		return;
	}

	const FFluidBrushSettings& Settings = TargetComponent->BrushSettings;
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
	if (!TargetComponent.IsValid())
	{
		return FLinearColor::White;
	}

	switch (TargetComponent->BrushSettings.Mode)
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

	if (!Canvas || !TargetComponent.IsValid() || !GEngine)
	{
		return;
	}

	const FFluidBrushSettings& Settings = TargetComponent->BrushSettings;
	FString ModeStr = (Settings.Mode == EFluidBrushMode::Add) ? TEXT("ADD") : TEXT("REMOVE");

	// 파티클 개수 (per-source count: 이 컴포넌트가 소유한 파티클 수)
	int32 ParticleCount = -1;
	if (TargetComponent->GetSimulationModule()) 
	{
		const int32 SourceID = TargetComponent->GetSimulationModule()->GetSourceID();
		ParticleCount = TargetComponent->GetSimulationModule()->GetParticleCountForSource(SourceID);
	}

	FString ParticleStr = (ParticleCount >= 0) ? FString::FromInt(ParticleCount) : TEXT("-");
	FString InfoText = FString::Printf(TEXT("Brush: %s | Radius: %.0f | Particles: %s | [ ] Size | 1/2 Mode | ESC Exit"),
	                               *ModeStr, Settings.Radius, *ParticleStr);

	FCanvasTextItem Text(FVector2D(10, 40), FText::FromString(InfoText),
	                     GEngine->GetSmallFont(), GetBrushColor());
	Canvas->DrawItem(Text);
}

bool FFluidBrushEditorMode::DisallowMouseDeltaTracking() const
{
	if (!TargetComponent.IsValid())
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

	// 조건 2: 타겟 컴포넌트 삭제됨
	if (!TargetComponent.IsValid())
	{
		UE_LOG(LogTemp, Log, TEXT("Fluid Brush Mode: Target component destroyed, exiting"));
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
