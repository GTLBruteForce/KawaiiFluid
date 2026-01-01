// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Components/KawaiiFluidTestDataComponent.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/KawaiiFluidISMRenderer.h"
#include "Rendering/KawaiiFluidMetaballRenderer.h"
#include "Engine/World.h"

UKawaiiFluidTestDataComponent::UKawaiiFluidTestDataComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// Create default subobject for rendering module (Instanced pattern)
	RenderingModule = CreateDefaultSubobject<UKawaiiFluidRenderingModule>(TEXT("RenderingModule"));
}

void UKawaiiFluidTestDataComponent::BeginPlay()
{
	Super::BeginPlay();

	// Generate test data
	GenerateTestParticles();

	// Initialize rendering module if rendering is enabled
	if (bEnableRendering && RenderingModule)
	{
		// ActorComponent이므로 Owner의 RootComponent에 ISM 부착
		RenderingModule->Initialize(GetWorld(), GetOwner()->GetRootComponent(), this);

		// Apply settings from structs to renderers
		if (UKawaiiFluidISMRenderer* ISMRenderer = RenderingModule->GetISMRenderer())
		{
			ISMRenderer->ApplySettings(ISMSettings);
		}

		if (UKawaiiFluidMetaballRenderer* MetaballRenderer = RenderingModule->GetMetaballRenderer())
		{
			MetaballRenderer->ApplySettings(MetaballSettings);
		}

		// Register with subsystem
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* Subsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				Subsystem->RegisterRenderingModule(RenderingModule);
			}
		}

		UE_LOG(LogTemp, Log, TEXT("KawaiiFluidTestDataComponent: Initialized with %d particles and RenderingModule (ISM: %s, Metaball: %s)"),
			TestParticles.Num(),
			ISMSettings.bEnabled ? TEXT("Enabled") : TEXT("Disabled"),
			MetaballSettings.bEnabled ? TEXT("Enabled") : TEXT("Disabled"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("KawaiiFluidTestDataComponent: Initialized with %d particles (No Rendering)"),
			TestParticles.Num());
	}
}

void UKawaiiFluidTestDataComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (RenderingModule)
	{
		// Unregister from subsystem
		if (UWorld* World = GetWorld())
		{
			if (UFluidRendererSubsystem* Subsystem = World->GetSubsystem<UFluidRendererSubsystem>())
			{
				Subsystem->UnregisterRenderingModule(RenderingModule);
			}
		}

		// Cleanup rendering module
		RenderingModule->Cleanup();
		RenderingModule = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void UKawaiiFluidTestDataComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Update animation
	if (TestParticles.Num() > 0 && !bAnimationPaused)
	{
		if (DataMode == EKawaiiFluidDummyGenMode::Animated || DataMode == EKawaiiFluidDummyGenMode::Wave)
		{
			UpdateAnimatedParticles(DeltaTime);
		}
	}

	// Update rendering
	if (RenderingModule)
	{
		RenderingModule->UpdateRenderers();
	}
}

void UKawaiiFluidTestDataComponent::RegenerateTestData()
{
	GenerateTestParticles();
	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidTestDataComponent: Regenerated %d particles"), TestParticles.Num());
}

void UKawaiiFluidTestDataComponent::SetTestPattern(EKawaiiFluidDummyGenMode NewMode)
{
	if (DataMode != NewMode)
	{
		DataMode = NewMode;
		RegenerateTestData();
	}
}

void UKawaiiFluidTestDataComponent::SetParticleCount(int32 NewCount)
{
	NewCount = FMath::Clamp(NewCount, 1, 10000);
	if (ParticleCount != NewCount)
	{
		ParticleCount = NewCount;
		RegenerateTestData();
	}
}

void UKawaiiFluidTestDataComponent::ToggleAnimation()
{
	bAnimationPaused = !bAnimationPaused;
	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidTestDataComponent: Animation %s"),
		bAnimationPaused ? TEXT("Paused") : TEXT("Resumed"));
}

void UKawaiiFluidTestDataComponent::GenerateTestParticles()
{
	switch (DataMode)
	{
	case EKawaiiFluidDummyGenMode::Static:
		GenerateStaticData();
		break;

	case EKawaiiFluidDummyGenMode::Animated:
		GenerateStaticData();
		break;

	case EKawaiiFluidDummyGenMode::GridPattern:
		GenerateGridPattern();
		break;

	case EKawaiiFluidDummyGenMode::Sphere:
		GenerateSpherePattern();
		break;

	case EKawaiiFluidDummyGenMode::Wave:
		GenerateGridPattern();
		break;
	}

	UE_LOG(LogTemp, Log, TEXT("KawaiiFluidTestDataComponent: Generated %d test particles"), TestParticles.Num());
}

void UKawaiiFluidTestDataComponent::GenerateStaticData()
{
	TestParticles.Empty();
	TestParticles.Reserve(ParticleCount);

	const FVector BaseLocation = GetOwner()->GetActorLocation();
	const FQuat ComponentRotation = GetOwner()->GetActorQuat();

	for (int32 i = 0; i < ParticleCount; ++i)
	{
		FFluidParticle& Particle = TestParticles.AddDefaulted_GetRef();

		FVector LocalOffset = FVector(
			FMath::FRandRange(-SpawnExtent.X, SpawnExtent.X),
			FMath::FRandRange(-SpawnExtent.Y, SpawnExtent.Y),
			FMath::FRandRange(-SpawnExtent.Z, SpawnExtent.Z)
		);

		FVector WorldOffset = ComponentRotation.RotateVector(LocalOffset);
		Particle.Position = BaseLocation + WorldOffset;
		Particle.Velocity = FVector::ZeroVector;
		Particle.Mass = 1.0f;
		Particle.Density = 1000.0f; // 기본 물 밀도
	}
}

void UKawaiiFluidTestDataComponent::GenerateGridPattern()
{
	TestParticles.Empty();

	const int32 GridSize = FMath::CeilToInt(FMath::Pow(ParticleCount, 1.0f / 3.0f));
	
	const FVector BaseLocation = GetOwner()->GetActorLocation();
	const FQuat ComponentRotation = GetOwner()->GetActorQuat();
	const float Spacing = ParticleRadius * 2.5f;

	int32 Count = 0;
	for (int32 x = 0; x < GridSize && Count < ParticleCount; ++x)
	{
		for (int32 y = 0; y < GridSize && Count < ParticleCount; ++y)
		{
			for (int32 z = 0; z < GridSize && Count < ParticleCount; ++z)
			{
				FFluidParticle& Particle = TestParticles.AddDefaulted_GetRef();

				FVector LocalPos = FVector(
					(x - GridSize / 2) * Spacing,
					(y - GridSize / 2) * Spacing,
					(z - GridSize / 2) * Spacing
				);

				FVector WorldPos = ComponentRotation.RotateVector(LocalPos);
				Particle.Position = BaseLocation + WorldPos;
				Particle.Velocity = FVector::ZeroVector;
				Particle.Mass = 1.0f;
				Particle.Density = 1000.0f;

				Count++;
			}
		}
	}
}

void UKawaiiFluidTestDataComponent::GenerateSpherePattern()
{
	TestParticles.Empty();
	TestParticles.Reserve(ParticleCount);

	const FVector BaseLocation = GetOwner()->GetActorLocation();
	const FQuat ComponentRotation = GetOwner()->GetActorQuat();
	const float SphereRadius = SpawnExtent.X;

	for (int32 i = 0; i < ParticleCount; ++i)
	{
		FFluidParticle& Particle = TestParticles.AddDefaulted_GetRef();

		// Fibonacci Sphere
		float Phi = FMath::Acos(1.0f - 2.0f * (i + 0.5f) / ParticleCount);
		float Theta = PI * (1.0f + FMath::Sqrt(5.0f)) * i;

		FVector LocalPos = FVector(
			FMath::Cos(Theta) * FMath::Sin(Phi),
			FMath::Sin(Theta) * FMath::Sin(Phi),
			FMath::Cos(Phi)
		) * SphereRadius;

		FVector WorldPos = ComponentRotation.RotateVector(LocalPos);
		Particle.Position = BaseLocation + WorldPos;
		Particle.Velocity = FVector::ZeroVector;
		Particle.Mass = 1.0f;
		Particle.Density = 1000.0f;
	}
}

void UKawaiiFluidTestDataComponent::UpdateAnimatedParticles(float DeltaTime)
{
	AnimationTime += DeltaTime * AnimationSpeed;

	const FVector BaseLocation = GetOwner()->GetActorLocation();

	if (DataMode == EKawaiiFluidDummyGenMode::Animated)
	{
		// 회전 애니메이션
		for (int32 i = 0; i < TestParticles.Num(); ++i)
		{
			FFluidParticle& Particle = TestParticles[i];

			FVector LocalPos = Particle.Position - BaseLocation;

			// Y축 회전
			FRotator Rotation(0.0f, AnimationTime * 50.0f, 0.0f);
			FVector RotatedPos = Rotation.RotateVector(LocalPos);

			Particle.Position = BaseLocation + RotatedPos;
		}
	}
	else if (DataMode == EKawaiiFluidDummyGenMode::Wave)
	{
		// Z축 파동
		for (int32 i = 0; i < TestParticles.Num(); ++i)
		{
			FFluidParticle& Particle = TestParticles[i];

			FVector LocalPos = Particle.Position - BaseLocation;

			// 원래 위치 저장
			static TArray<FVector> OriginalLocalPositions;
			if (OriginalLocalPositions.Num() != TestParticles.Num())
			{
				OriginalLocalPositions.SetNum(TestParticles.Num());
				for (int32 j = 0; j < TestParticles.Num(); ++j)
				{
					OriginalLocalPositions[j] = TestParticles[j].Position - BaseLocation;
				}
			}

			float Wave = FMath::Sin(OriginalLocalPositions[i].X * WaveFrequency * 0.01f + AnimationTime) * WaveAmplitude;
			LocalPos.Z = OriginalLocalPositions[i].Z + Wave;

			Particle.Position = BaseLocation + LocalPos;
		}
	}
}

