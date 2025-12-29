// Copyright KawaiiFluid Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class UKawaiiFluidComponent;

/**
 * KawaiiFluidComponent 디테일 패널 커스터마이제이션
 * 브러시 모드 시작/종료 버튼 추가
 */
class FFluidComponentDetails : public IDetailCustomization
{
public:
	/** IDetailCustomization 팩토리 */
	static TSharedRef<IDetailCustomization> MakeInstance();

	//~ Begin IDetailCustomization Interface
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
	//~ End IDetailCustomization Interface

private:
	/** 타겟 컴포넌트 */
	TWeakObjectPtr<UKawaiiFluidComponent> TargetComponent;

	/** 브러시 시작 버튼 클릭 */
	FReply OnStartBrushClicked();

	/** 브러시 종료 버튼 클릭 */
	FReply OnStopBrushClicked();

	/** 파티클 전체 삭제 버튼 클릭 */
	FReply OnClearParticlesClicked();

	/** Start 버튼 표시 여부 */
	EVisibility GetStartVisibility() const;

	/** Stop 버튼 표시 여부 */
	EVisibility GetStopVisibility() const;

	/** 브러시 모드 활성화 상태 확인 */
	bool IsBrushActive() const;
};
