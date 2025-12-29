// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdMode.h"

class UKawaiiFluidComponent;

/**
 * 플루이드 파티클 브러시 에디터 모드
 * 디테일 패널 버튼으로 활성화되며, 특정 FluidComponent를 타겟으로 동작
 */
class FFluidBrushEditorMode : public FEdMode
{
public:
	static const FEditorModeID EM_FluidBrush;

	FFluidBrushEditorMode();
	virtual ~FFluidBrushEditorMode() override;

	//~ Begin FEdMode Interface
	virtual void Enter() override;
	virtual void Exit() override;
	virtual bool UsesToolkits() const override { return false; }

	virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	                      FKey Key, EInputEvent Event) override;
	virtual bool HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy,
	                         const FViewportClick& Click) override;
	virtual bool StartTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport) override;
	virtual bool EndTracking(FEditorViewportClient* ViewportClient, FViewport* Viewport) override;
	virtual bool MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	                       int32 x, int32 y) override;
	virtual bool CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	                               int32 InMouseX, int32 InMouseY) override;
	virtual void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	                     const FSceneView* View, FCanvas* Canvas) override;
	virtual bool IsSelectionAllowed(AActor* InActor, bool bInSelected) const override { return false; }
	virtual bool ShouldDrawWidget() const override { return false; }
	virtual bool DisallowMouseDeltaTracking() const override;
	//~ End FEdMode Interface

	/** 타겟 컴포넌트 설정 */
	void SetTargetComponent(UKawaiiFluidComponent* Component);

	/** 타겟 컴포넌트 반환 */
	UKawaiiFluidComponent* GetTargetComponent() const { return TargetComponent.Get(); }

private:
	/** 타겟 플루이드 컴포넌트 */
	TWeakObjectPtr<UKawaiiFluidComponent> TargetComponent;

	/** 현재 브러시 위치 */
	FVector BrushLocation {};

	/** 유효한 브러시 위치인지 */
	bool bValidLocation = false;

	/** 페인팅 중인지 */
	bool bPainting = false;

	/** 마지막 스트로크 시간 */
	double LastStrokeTime = 0.0;

	/** 브러시 위치 업데이트 */
	bool UpdateBrushLocation(FEditorViewportClient* ViewportClient, int32 MouseX, int32 MouseY);

	/** 브러시 적용 */
	void ApplyBrush();

	/** 브러시 미리보기 그리기 */
	void DrawBrushPreview(FPrimitiveDrawInterface* PDI);

	/** 브러시 모드별 색상 반환 */
	FLinearColor GetBrushColor() const;
};
