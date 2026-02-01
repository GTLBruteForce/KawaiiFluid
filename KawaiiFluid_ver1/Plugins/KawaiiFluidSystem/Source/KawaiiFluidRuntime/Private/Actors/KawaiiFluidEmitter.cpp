// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Actors/KawaiiFluidEmitter.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Components/KawaiiFluidEmitterComponent.h"

AKawaiiFluidEmitter::AKawaiiFluidEmitter()
{
	PrimaryActorTick.bCanEverTick = false;  // EmitterComponent handles ticking

	// Create emitter component as root
	EmitterComponent = CreateDefaultSubobject<UKawaiiFluidEmitterComponent>(TEXT("KawaiiFluidEmitterComponent"));
	RootComponent = EmitterComponent;
}

void AKawaiiFluidEmitter::BeginPlay()
{
	Super::BeginPlay();
	// EmitterComponent handles volume registration in its BeginPlay
}

void AKawaiiFluidEmitter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// EmitterComponent handles volume unregistration in its EndPlay
	Super::EndPlay(EndPlayReason);
}

//========================================
// Delegate Getters (from EmitterComponent)
//========================================

AKawaiiFluidVolume* AKawaiiFluidEmitter::GetTargetVolume() const
{
	return EmitterComponent ? EmitterComponent->GetTargetVolume() : nullptr;
}

void AKawaiiFluidEmitter::SetTargetVolume(AKawaiiFluidVolume* NewVolume)
{
	if (EmitterComponent)
	{
		EmitterComponent->SetTargetVolume(NewVolume);
	}
}

//========================================
// API (Delegate to EmitterComponent)
//========================================

void AKawaiiFluidEmitter::BurstSpawn(int32 Count)
{
	if (EmitterComponent)
	{
		EmitterComponent->BurstSpawn(Count);
	}
}

int32 AKawaiiFluidEmitter::GetSpawnedParticleCount() const
{
	return EmitterComponent ? EmitterComponent->GetSpawnedParticleCount() : 0;
}

//========================================
// Spawn Control API
//========================================

void AKawaiiFluidEmitter::StartSpawn()
{
	if (!EmitterComponent)
	{
		return;
	}

	if (EmitterComponent->IsFillMode())
	{
		EmitterComponent->SpawnFill();
	}
	else if (EmitterComponent->IsStreamMode())
	{
		EmitterComponent->StartStreamSpawn();
	}
}

void AKawaiiFluidEmitter::StopSpawn()
{
	if (EmitterComponent && EmitterComponent->IsStreamMode())
	{
		EmitterComponent->StopStreamSpawn();
	}
}

void AKawaiiFluidEmitter::ToggleSpawn()
{
	if (!EmitterComponent)
	{
		return;
	}

	if (EmitterComponent->IsStreamMode())
	{
		if (EmitterComponent->IsStreamSpawning())
		{
			EmitterComponent->StopStreamSpawn();
		}
		else
		{
			EmitterComponent->StartStreamSpawn();
		}
	}
	else if (EmitterComponent->IsFillMode())
	{
		// Fill mode: just spawn (one-shot)
		EmitterComponent->SpawnFill();
	}
}

bool AKawaiiFluidEmitter::IsSpawning() const
{
	if (!EmitterComponent)
	{
		return false;
	}

	if (EmitterComponent->IsStreamMode())
	{
		return EmitterComponent->IsStreamSpawning();
	}
	else
	{
		// Fill mode: check if has spawned
		return EmitterComponent->GetSpawnedParticleCount() > 0;
	}
}

void AKawaiiFluidEmitter::SetEnabled(bool bNewEnabled)
{
	if (EmitterComponent)
	{
		EmitterComponent->bEnabled = bNewEnabled;
	}
}

bool AKawaiiFluidEmitter::IsEnabled() const
{
	return EmitterComponent ? EmitterComponent->bEnabled : false;
}
