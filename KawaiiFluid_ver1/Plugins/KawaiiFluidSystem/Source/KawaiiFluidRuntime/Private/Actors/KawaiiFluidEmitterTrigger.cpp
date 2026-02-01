// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Actors/KawaiiFluidEmitterTrigger.h"
#include "Actors/KawaiiFluidEmitter.h"
#include "Components/BoxComponent.h"
#include "Components/BillboardComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

AKawaiiFluidEmitterTrigger::AKawaiiFluidEmitterTrigger()
{
	PrimaryActorTick.bCanEverTick = false;

	// Create root component
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	// Create trigger box
	TriggerBox = CreateDefaultSubobject<UBoxComponent>(TEXT("TriggerBox"));
	TriggerBox->SetupAttachment(RootComponent);
	TriggerBox->SetBoxExtent(BoxExtent);
	TriggerBox->SetCollisionProfileName(TEXT("Trigger"));
	TriggerBox->SetGenerateOverlapEvents(true);

	// Bind overlap events
	TriggerBox->OnComponentBeginOverlap.AddDynamic(this, &AKawaiiFluidEmitterTrigger::OnTriggerBeginOverlap);
	TriggerBox->OnComponentEndOverlap.AddDynamic(this, &AKawaiiFluidEmitterTrigger::OnTriggerEndOverlap);

#if WITH_EDITORONLY_DATA
	// Create billboard for editor visualization
	BillboardComponent = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Billboard"));
	if (BillboardComponent)
	{
		BillboardComponent->SetupAttachment(RootComponent);

		// Try to load trigger icon
		static ConstructorHelpers::FObjectFinder<UTexture2D> IconFinder(
			TEXT("/Engine/EditorResources/S_TriggerBox"));
		if (IconFinder.Succeeded())
		{
			BillboardComponent->SetSprite(IconFinder.Object);
		}
		BillboardComponent->bIsScreenSizeScaled = true;
	}
#endif
}

void AKawaiiFluidEmitterTrigger::BeginPlay()
{
	Super::BeginPlay();

	// Ensure trigger box has correct extent at runtime
	if (TriggerBox)
	{
		TriggerBox->SetBoxExtent(BoxExtent);
	}

	// Warn if no target emitter assigned
	if (!TargetEmitter)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidEmitterTrigger [%s]: No TargetEmitter assigned!"), *GetName());
	}
}

//========================================
// Manual Trigger API
//========================================

void AKawaiiFluidEmitterTrigger::ExecuteTriggerAction()
{
	if (!TargetEmitter)
	{
		return;
	}

	switch (TriggerAction)
	{
	case EKawaiiFluidTriggerAction::Start:
		TargetEmitter->StartSpawn();
		break;
	case EKawaiiFluidTriggerAction::Stop:
		TargetEmitter->StopSpawn();
		break;
	case EKawaiiFluidTriggerAction::Toggle:
		TargetEmitter->ToggleSpawn();
		break;
	}
}

void AKawaiiFluidEmitterTrigger::ExecuteExitAction()
{
	if (!TargetEmitter)
	{
		return;
	}

	// Only stop if configured to do so
	if (bStopOnExit && TriggerAction == EKawaiiFluidTriggerAction::Start)
	{
		TargetEmitter->StopSpawn();
	}
}

//========================================
// Overlap Handlers
//========================================

void AKawaiiFluidEmitterTrigger::OnTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (!ShouldTriggerFor(OtherActor))
	{
		return;
	}

	ExecuteTriggerAction();
}

void AKawaiiFluidEmitterTrigger::OnTriggerEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	if (!ShouldTriggerFor(OtherActor))
	{
		return;
	}

	ExecuteExitAction();
}

//========================================
// Internal Helpers
//========================================

bool AKawaiiFluidEmitterTrigger::ShouldTriggerFor(AActor* OtherActor) const
{
	if (!OtherActor)
	{
		return false;
	}

	if (!bOnlyPlayer)
	{
		// Any actor can trigger
		return true;
	}

	// Check if it's the player pawn
	APawn* Pawn = Cast<APawn>(OtherActor);
	if (!Pawn)
	{
		return false;
	}

	// Check if this pawn is player-controlled
	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	return PC != nullptr;
}

#if WITH_EDITOR
void AKawaiiFluidEmitterTrigger::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AKawaiiFluidEmitterTrigger, BoxExtent))
	{
		if (TriggerBox)
		{
			TriggerBox->SetBoxExtent(BoxExtent);
		}
	}
}
#endif
