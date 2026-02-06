// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Actors/KawaiiFluidEmitterTrigger.h"
#include "Actors/KawaiiFluidEmitter.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Components/BoxComponent.h"
#include "Components/BillboardComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

AKawaiiFluidEmitterTrigger::AKawaiiFluidEmitterTrigger()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

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

	// Warn if no target emitters assigned
	if (TargetEmitters.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("KawaiiFluidEmitterTrigger [%s]: No TargetEmitters assigned!"), *GetName());
	}
}

void AKawaiiFluidEmitterTrigger::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (ClearFramesRemaining > 0)
	{
		for (AKawaiiFluidEmitter* Emitter : TargetEmitters)
		{
			if (Emitter)
			{
				Emitter->ClearSpawnedParticles();
			}
		}
		--ClearFramesRemaining;

		if (ClearFramesRemaining <= 0)
		{
			SetActorTickEnabled(false);
		}
	}
	else
	{
		// Safety: disable tick if no work
		ClearFramesRemaining = 0;
		SetActorTickEnabled(false);
	}
}

//========================================
// Manual Trigger API
//========================================

void AKawaiiFluidEmitterTrigger::ExecuteTriggerAction()
{
	for (AKawaiiFluidEmitter* Emitter : TargetEmitters)
	{
		if (!Emitter)
		{
			continue;
		}

		switch (TriggerAction)
		{
		case EKawaiiFluidTriggerAction::Start:
			Emitter->StartSpawn();
			break;
		case EKawaiiFluidTriggerAction::Stop:
			Emitter->StopSpawn();
			break;
		case EKawaiiFluidTriggerAction::Toggle:
			Emitter->ToggleSpawn();
			break;
		}
	}
}

void AKawaiiFluidEmitterTrigger::ExecuteExitAction()
{
	// Only process exit actions when TriggerAction is Start
	if (TriggerAction != EKawaiiFluidTriggerAction::Start)
	{
		return;
	}

	for (AKawaiiFluidEmitter* Emitter : TargetEmitters)
	{
		if (!Emitter)
		{
			continue;
		}

		// Stop spawning if configured
		if (bStopOnExit)
		{
			Emitter->StopSpawn();
		}

		// TODO: Multi-frame Tick clear is a workaround for GPU readback latency.
		// Replace with GPU-side SourceID bulk despawn or despawn completion callback.
		if (bClearParticlesOnExit)
		{
			Emitter->ClearSpawnedParticles();
		}
	}

	if (bClearParticlesOnExit)
	{
		ClearFramesRemaining = ClearParticleFrameCount - 1;
		if (ClearFramesRemaining > 0)
		{
			SetActorTickEnabled(true);
		}
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
