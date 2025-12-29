// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/KawaiiSlimeComponent.h"
#include "Core/KawaiiFluidSimulatorSubsystem.h"
#include "Modules/KawaiiSlimeSimulationModule.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/KawaiiFluidSSFRRenderer.h"

UKawaiiSlimeComponent::UKawaiiSlimeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;  // Subsystem 시뮬레이션 이후 렌더링

	// 슬라임 시뮬레이션 모듈 생성
	SlimeModule = CreateDefaultSubobject<UKawaiiSlimeSimulationModule>(TEXT("SlimeSimulationModule"));

	// 렌더링 모듈 생성
	RenderingModule = CreateDefaultSubobject<UKawaiiFluidRenderingModule>(TEXT("RenderingModule"));
}

void UKawaiiSlimeComponent::BeginPlay()
{
	Super::BeginPlay();

	if (SlimeModule)
	{
		// Owner Actor 설정
		SlimeModule->SetOwnerActor(GetOwner());

		// Preset 설정 (Component에 설정된 Preset 사용)
		if (Preset)
		{
			SlimeModule->SetPreset(Preset);
		}
		else
		{
			// Preset 없으면 기본 생성
			Preset = NewObject<UKawaiiFluidPresetDataAsset>(this, NAME_None, RF_Transient);
			SlimeModule->SetPreset(Preset);
			UE_LOG(LogTemp, Warning, TEXT("KawaiiSlimeComponent [%s]: No Preset assigned, using default values"), *GetName());
		}

		// 모듈 초기화
		SlimeModule->Initialize(Preset);

		// 이벤트 바인딩
		SlimeModule->OnGroundContact.AddDynamic(this, &UKawaiiSlimeComponent::HandleGroundContact);
		SlimeModule->OnObjectEntered.AddDynamic(this, &UKawaiiSlimeComponent::HandleObjectEntered);
		SlimeModule->OnObjectExited.AddDynamic(this, &UKawaiiSlimeComponent::HandleObjectExited);
	}

	// 렌더링 모듈 초기화
	if (bEnableRendering && RenderingModule && SlimeModule)
	{
		RenderingModule->Initialize(GetWorld(), GetOwner(), SlimeModule);

		// ISM 렌더러 설정 적용
		if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
		{
			ISMRenderer->ApplySettings(ISMSettings);
		}

		// SSFR 렌더러 설정 적용
		if (UKawaiiFluidSSFRRenderer* SSFRRenderer = RenderingModule->GetSSFRRenderer())
		{
			SSFRRenderer->ApplySettings(SSFRSettings);
		}

		// FluidRendererSubsystem에 등록
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				RendererSubsystem->RegisterRenderingModule(RenderingModule);
			}
		}
	}

	// Subsystem에 Module 등록
	RegisterToSubsystem();

	// 자동 스폰 (균일 분포로 스폰하여 ShapeMatching이 유지할 수 있도록)
	if (bSpawnOnBeginPlay && AutoSpawnCount > 0 && SlimeModule)
	{
		SlimeModule->SpawnParticlesUniform(GetOwner()->GetActorLocation(), AutoSpawnCount, AutoSpawnRadius);
	}

	UE_LOG(LogTemp, Log, TEXT("UKawaiiSlimeComponent BeginPlay: %s (Module-based)"), *GetName());
}

void UKawaiiSlimeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Subsystem에서 등록 해제
	UnregisterFromSubsystem();

	// 이벤트 해제
	if (SlimeModule)
	{
		SlimeModule->OnGroundContact.RemoveDynamic(this, &UKawaiiSlimeComponent::HandleGroundContact);
		SlimeModule->OnObjectEntered.RemoveDynamic(this, &UKawaiiSlimeComponent::HandleObjectEntered);
		SlimeModule->OnObjectExited.RemoveDynamic(this, &UKawaiiSlimeComponent::HandleObjectExited);
	}

	OnGroundContact.Clear();
	OnObjectEntered.Clear();
	OnObjectExited.Clear();

	// 렌더링 모듈 정리
	if (RenderingModule)
	{
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* RendererSubsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				RendererSubsystem->UnregisterRenderingModule(RenderingModule);
			}
		}
		RenderingModule->Cleanup();
		RenderingModule = nullptr;
	}

	// 시뮬레이션 모듈 정리
	if (SlimeModule)
	{
		SlimeModule->Shutdown();
	}

	Super::EndPlay(EndPlayReason);

	UE_LOG(LogTemp, Log, TEXT("UKawaiiSlimeComponent EndPlay: %s"), *GetName());
}

void UKawaiiSlimeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 슬라임 전용 로직 Tick
	if (SlimeModule)
	{
		// 디버그: 모듈 상태 확인
		static bool bLogged = false;
		if (!bLogged && SlimeModule->GetParticleCount() > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("SlimeComponent Tick: Particles=%d, SpatialHash=%s, Independent=%s"),
				SlimeModule->GetParticleCount(),
				SlimeModule->GetSpatialHash() ? TEXT("Valid") : TEXT("NULL"),
				SlimeModule->IsIndependentSimulation() ? TEXT("Yes") : TEXT("No"));
			bLogged = true;
		}

		SlimeModule->TickSlime(DeltaTime);
	}

	// 렌더링 업데이트
	if (RenderingModule)
	{
		RenderingModule->UpdateRenderers();
	}
}

//========================================
// Blueprint API (모듈에 위임)
//========================================

void UKawaiiSlimeComponent::ApplyMovementInput(FVector Input)
{
	if (SlimeModule)
	{
		SlimeModule->ApplyMovementInput(Input);
	}
}

void UKawaiiSlimeComponent::ApplyJumpImpulse()
{
	if (SlimeModule)
	{
		SlimeModule->ApplyJumpImpulse();
	}
}

void UKawaiiSlimeComponent::SetDecomposeMode(bool bEnable)
{
	if (SlimeModule)
	{
		SlimeModule->SetDecomposeMode(bEnable);
	}
}

FVector UKawaiiSlimeComponent::GetMainClusterCenter() const
{
	if (SlimeModule)
	{
		return SlimeModule->GetMainClusterCenter();
	}
	return FVector::ZeroVector;
}

int32 UKawaiiSlimeComponent::GetMainClusterParticleCount() const
{
	if (SlimeModule)
	{
		return SlimeModule->GetMainClusterParticleCount();
	}
	return 0;
}

bool UKawaiiSlimeComponent::IsActorInsideSlime(AActor* Actor) const
{
	if (SlimeModule)
	{
		return SlimeModule->IsActorInsideSlime(Actor);
	}
	return false;
}

bool UKawaiiSlimeComponent::IsGrounded() const
{
	if (SlimeModule)
	{
		return SlimeModule->IsGrounded();
	}
	return true;
}

FVector UKawaiiSlimeComponent::GetNucleusPosition() const
{
	if (SlimeModule)
	{
		return SlimeModule->NucleusPosition;
	}
	return FVector::ZeroVector;
}

int32 UKawaiiSlimeComponent::GetParticleCount() const
{
	if (SlimeModule)
	{
		return SlimeModule->GetParticleCount();
	}
	return 0;
}

//========================================
// Subsystem Registration
//========================================

void UKawaiiSlimeComponent::RegisterToSubsystem()
{
	if (!SlimeModule)
	{
		UE_LOG(LogTemp, Error, TEXT("RegisterToSubsystem: SlimeModule is NULL!"));
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			// Module을 직접 등록
			Subsystem->RegisterModule(SlimeModule);
			UE_LOG(LogTemp, Warning, TEXT("RegisterToSubsystem: Module registered! AllModules count=%d"),
				Subsystem->GetAllModules().Num());
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("RegisterToSubsystem: Subsystem is NULL!"));
		}
	}
}

void UKawaiiSlimeComponent::UnregisterFromSubsystem()
{
	if (!SlimeModule)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (UKawaiiFluidSimulatorSubsystem* Subsystem = World->GetSubsystem<UKawaiiFluidSimulatorSubsystem>())
		{
			Subsystem->UnregisterModule(SlimeModule);
		}
	}
}

//========================================
// Event Handlers
//========================================

void UKawaiiSlimeComponent::HandleGroundContact(FVector Location, FVector Normal)
{
	if (OnGroundContact.IsBound())
	{
		OnGroundContact.Broadcast(Location, Normal);
	}
}

void UKawaiiSlimeComponent::HandleObjectEntered(AActor* Object)
{
	if (OnObjectEntered.IsBound())
	{
		OnObjectEntered.Broadcast(Object);
	}
}

void UKawaiiSlimeComponent::HandleObjectExited(AActor* Object)
{
	if (OnObjectExited.IsBound())
	{
		OnObjectExited.Broadcast(Object);
	}
}
