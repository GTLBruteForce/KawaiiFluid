// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/FluidRendererSubsystem.h"
#include "Rendering/FluidSceneViewExtension.h"
#include "Rendering/FluidShadowUtils.h"
#include "Modules/KawaiiFluidRenderingModule.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

bool UFluidRendererSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!IsValid(Outer))
	{
		return false;
	}

	UWorld* World = CastChecked<UWorld>(Outer);
	const EWorldType::Type WorldType = World->WorldType;

	// Support all world types that might need fluid rendering
	// Including EditorPreview for Preset Editor viewport
	return WorldType == EWorldType::Game ||
	       WorldType == EWorldType::Editor ||
	       WorldType == EWorldType::PIE ||
	       WorldType == EWorldType::EditorPreview ||
	       WorldType == EWorldType::GamePreview;
}

void UFluidRendererSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Scene View Extension 생성 및 등록
	ViewExtension = FSceneViewExtensions::NewExtension<FFluidSceneViewExtension>(this);

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem Initialized"));
}

void UFluidRendererSubsystem::Deinitialize()
{
	// View Extension 해제
	ViewExtension.Reset();

	RegisteredRenderingModules.Empty();

	Super::Deinitialize();

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem Deinitialized"));
}

//========================================
// RenderingModule 관리
//========================================

void UFluidRendererSubsystem::RegisterRenderingModule(UKawaiiFluidRenderingModule* Module)
{
	if (!Module)
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem: RegisterRenderingModule - Module is null"));
		return;
	}

	if (RegisteredRenderingModules.Contains(Module))
	{
		UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem: RenderingModule already registered: %s"),
			*Module->GetName());
		return;
	}

	RegisteredRenderingModules.Add(Module);

	UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem: Registered RenderingModule %s (Total: %d)"),
		*Module->GetName(),
		RegisteredRenderingModules.Num());
}

void UFluidRendererSubsystem::UnregisterRenderingModule(UKawaiiFluidRenderingModule* Module)
{
	if (!Module)
	{
		return;
	}

	int32 Removed = RegisteredRenderingModules.Remove(Module);

	if (Removed > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("FluidRendererSubsystem: Unregistered RenderingModule %s (Remaining: %d)"),
			*Module->GetName(),
			RegisteredRenderingModules.Num());
	}
}

//========================================
// Cached Light Direction (Game Thread)
//========================================

/**
 * @brief Update cached light direction from the main DirectionalLight in the world.
 * Must be called from game thread before rendering.
 */
void UFluidRendererSubsystem::UpdateCachedLightDirection()
{
	check(IsInGameThread());

	UWorld* World = GetWorld();
	if (!World)
	{
		bHasCachedLightData = false;
		return;
	}

	// Find main DirectionalLight using TActorIterator (game thread only)
	ADirectionalLight* MainLight = FluidShadowUtils::FindMainDirectionalLight(World);

	if (MainLight)
	{
		UDirectionalLightComponent* LightComp = Cast<UDirectionalLightComponent>(MainLight->GetLightComponent());
		if (LightComp)
		{
			// Get light direction (forward vector points toward the light source, negate for shadow direction)
			FVector LightDir = -LightComp->GetForwardVector();
			CachedLightDirection = FVector3f(LightDir);

			// Calculate light view-projection matrix for shadow mapping
			// Use a default bounds for now (TODO: use actual fluid bounds)
			FBox FluidBounds(FVector(-1000, -1000, -1000), FVector(1000, 1000, 1000));
			FBox ExpandedBounds = FluidBounds.ExpandBy(FluidBounds.GetExtent().Size() * 0.5f);

			FMatrix ViewMatrix, ProjectionMatrix;
			FluidShadowUtils::CalculateDirectionalLightMatrices(LightDir, ExpandedBounds, ViewMatrix, ProjectionMatrix);
			CachedLightViewProjectionMatrix = FMatrix44f(ViewMatrix * ProjectionMatrix);

			bHasCachedLightData = true;
			return;
		}
	}

	// Fallback: use default sun direction
	CachedLightDirection = FVector3f(0.5f, 0.5f, -0.707f).GetSafeNormal();

	FBox FluidBounds(FVector(-1000, -1000, -1000), FVector(1000, 1000, 1000));
	FBox ExpandedBounds = FluidBounds.ExpandBy(FluidBounds.GetExtent().Size() * 0.5f);

	FMatrix ViewMatrix, ProjectionMatrix;
	FluidShadowUtils::CalculateDirectionalLightMatrices(FVector(CachedLightDirection), ExpandedBounds, ViewMatrix, ProjectionMatrix);
	CachedLightViewProjectionMatrix = FMatrix44f(ViewMatrix * ProjectionMatrix);

	bHasCachedLightData = true;
}

//========================================
// VSM Buffer Management
//========================================

void UFluidRendererSubsystem::SwapVSMBuffers()
{
	// Swap VSM buffers: previous frame's write buffer becomes current frame's read buffer
	VSMTexture_Read = VSMTexture_Write;
	LightVPMatrix_Read = LightVPMatrix_Write;
}

//========================================
// ISM Shadow Management
//========================================

/**
 * @brief Create or destroy the ISM shadow component based on settings.
 */
void UFluidRendererSubsystem::UpdateShadowProxyState()
{
	check(IsInGameThread());

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Note: Actor and ISM creation is now fully handled in UpdateShadowInstances()
	// This function only handles cleanup when shadow is disabled

	if (!bEnableVSMIntegration && (IsValid(ShadowProxyActor) || IsValid(ShadowInstanceComponent)))
	{
		// Destroy ISM component
		if (IsValid(ShadowInstanceComponent))
		{
			ShadowInstanceComponent->ClearInstances();
			ShadowInstanceComponent->DestroyComponent();
		}
		ShadowInstanceComponent = nullptr;

		if (IsValid(ShadowProxyActor))
		{
			ShadowProxyActor->Destroy();
		}
		ShadowProxyActor = nullptr;

		// Release resources
		ShadowSphereMesh = nullptr;
		CachedInstanceTransforms.Empty();
	}
}

/**
 * @brief Update shadow instances from particle positions.
 * @param ParticlePositions Array of particle world positions.
 * @param NumParticles Number of particles.
 */
void UFluidRendererSubsystem::UpdateShadowInstances(const FVector* ParticlePositions, int32 NumParticles, float ParticleRadius)
{
	check(IsInGameThread());

	// Clear HISM instances if no particles (do this even if VSM is disabled)
	if (NumParticles <= 0 || !ParticlePositions)
	{
		if (IsValid(ShadowInstanceComponent))
		{
			ShadowInstanceComponent->ClearInstances();
			ShadowInstanceComponent->MarkRenderStateDirty();
		}
		return;
	}

	// Early return if VSM integration is disabled (but clearing above still happens)
	if (!bEnableVSMIntegration)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || World->bIsTearingDown || !World->bActorsInitialized)
	{
		return;
	}

	// Lazy creation of Actor (moved here from UpdateShadowProxyState for better timing)
	// Use IsValid() to catch stale pointers from previous PIE sessions
	if (!IsValid(ShadowProxyActor))
	{
		ShadowProxyActor = nullptr;  // Clear stale pointer
		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = TEXT("FluidShadowProxyActor");
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ShadowProxyActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (ShadowProxyActor)
		{
			ShadowProxyActor->SetActorLabel(TEXT("FluidShadowProxy"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem: Failed to spawn shadow proxy actor"));
			return;
		}
	}

	// Lazy creation of ISM component (ISM is better than HISM for dynamic updates)
	// Use IsValid() to catch stale pointers from previous PIE sessions
	if (!IsValid(ShadowInstanceComponent))
	{
		ShadowInstanceComponent = nullptr;  // Clear stale pointer

		// Load sphere mesh if not already loaded (also check validity)
		if (!IsValid(ShadowSphereMesh))
		{
			ShadowSphereMesh = nullptr;
			ShadowSphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			if (!ShadowSphereMesh)
			{
				UE_LOG(LogTemp, Warning, TEXT("FluidRendererSubsystem: Failed to load sphere mesh for shadow"));
				return;
			}
		}

		// Create ISM component for instanced shadow spheres
		ShadowInstanceComponent = NewObject<UInstancedStaticMeshComponent>(ShadowProxyActor, TEXT("ShadowInstanceComponent"));
		if (ShadowInstanceComponent)
		{
			// Set as root component so it's properly attached to the actor
			ShadowProxyActor->SetRootComponent(ShadowInstanceComponent);

			ShadowInstanceComponent->SetStaticMesh(ShadowSphereMesh);

			// Shadow settings - cast shadow but invisible in main pass
			ShadowInstanceComponent->CastShadow = true;
			ShadowInstanceComponent->bCastDynamicShadow = true;
			ShadowInstanceComponent->bCastStaticShadow = false;
			ShadowInstanceComponent->bAffectDynamicIndirectLighting = false;
			ShadowInstanceComponent->bAffectDistanceFieldLighting = false;

			// Hidden but casts shadow (both editor and game)
			ShadowInstanceComponent->SetVisibility(false);
			ShadowInstanceComponent->SetHiddenInGame(true);
			ShadowInstanceComponent->bCastHiddenShadow = true;

			// Performance settings
			ShadowInstanceComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			ShadowInstanceComponent->SetGenerateOverlapEvents(false);
			ShadowInstanceComponent->bDisableCollision = true;
			ShadowInstanceComponent->SetMobility(EComponentMobility::Movable);

			// Register component to the world
			ShadowInstanceComponent->RegisterComponent();
		}
	}

	if (!IsValid(ShadowInstanceComponent))
	{
		return;
	}

	// Calculate number of instances based on skip factor and max limit
	const int32 SkipFactor = FMath::Max(1, ParticleSkipFactor);
	int32 NumInstances = (NumParticles + SkipFactor - 1) / SkipFactor;

	if (MaxShadowInstances > 0)
	{
		NumInstances = FMath::Min(NumInstances, MaxShadowInstances);
	}

	// Prepare transforms
	CachedInstanceTransforms.SetNumUninitialized(NumInstances);

	const FVector SphereScale = FVector(ParticleRadius / 50.0f);  // Default sphere is 100 units diameter

	for (int32 i = 0, InstanceIdx = 0; i < NumParticles && InstanceIdx < NumInstances; i += SkipFactor, ++InstanceIdx)
	{
		FTransform& InstanceTransform = CachedInstanceTransforms[InstanceIdx];

		// Validate position - skip particles with NaN/Inf positions
		const FVector& Position = ParticlePositions[i];
		if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z))
		{
			InstanceTransform.SetLocation(FVector::ZeroVector);
			InstanceTransform.SetRotation(FQuat::Identity);
			InstanceTransform.SetScale3D(FVector::ZeroVector);  // Zero scale = invisible
			continue;
		}

		InstanceTransform.SetLocation(Position);
		InstanceTransform.SetRotation(FQuat::Identity);
		InstanceTransform.SetScale3D(SphereScale);
	}

	const int32 CurrentInstanceCount = ShadowInstanceComponent->GetInstanceCount();
	const int32 TargetInstanceCount = NumInstances;

	if (TargetInstanceCount == 0)
	{
		if (CurrentInstanceCount > 0)
		{
			ShadowInstanceComponent->ClearInstances();
		}
		return;
	}

	// Optimized update strategy for ISM:
	// - If count matches: use BatchUpdateInstancesTransforms (most efficient)
	// - If count differs: rebuild entirely (ClearInstances + AddInstances)
	if (CurrentInstanceCount == TargetInstanceCount)
	{
		// Batch update all transforms at once - much faster than individual updates
		ShadowInstanceComponent->BatchUpdateInstancesTransforms(
			0,
			CachedInstanceTransforms,
			false,  // bWorldSpace
			true,   // bMarkRenderStateDirty
			false   // bTeleport
		);
	}
	else
	{
		// Instance count changed - rebuild entirely
		ShadowInstanceComponent->ClearInstances();
		ShadowInstanceComponent->AddInstances(CachedInstanceTransforms, false, true);
	}
}

/**
 * @brief Update shadow instances with anisotropy data for ellipsoid shadows.
 *
 * Note: This function is currently not used because anisotropy-based shadows cause flickering.
 * The flickering is caused by GPU Morton sorting which reorders particle indices every frame,
 * resulting in mismatched position/anisotropy data pairs. Until a stable particle ID system
 * is implemented, use UpdateShadowInstances() with uniform spheres instead.
 *
 * @param ParticlePositions Array of particle world positions.
 * @param AnisotropyAxis1 Array of first ellipsoid axis (xyz=direction, w=scale).
 * @param AnisotropyAxis2 Array of second ellipsoid axis.
 * @param AnisotropyAxis3 Array of third ellipsoid axis.
 * @param NumParticles Number of particles.
 */
void UFluidRendererSubsystem::UpdateShadowInstancesWithAnisotropy(
	const FVector* ParticlePositions,
	const FVector4* AnisotropyAxis1,
	const FVector4* AnisotropyAxis2,
	const FVector4* AnisotropyAxis3,
	int32 NumParticles,
	float ParticleRadius)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FluidShadow_UpdateInstancesWithAnisotropy);

	check(IsInGameThread());

	// Early return if VSM integration is disabled
	if (!bEnableVSMIntegration)
	{
		return;
	}

	// Clear ISM instances if no particles
	if (NumParticles <= 0 || !ParticlePositions)
	{
		if (IsValid(ShadowInstanceComponent))
		{
			ShadowInstanceComponent->ClearInstances();
		}
		return;
	}

	UWorld* World = GetWorld();
	if (!World || World->bIsTearingDown || !World->bActorsInitialized)
	{
		return;
	}

	// Lazy creation of Actor
	if (!IsValid(ShadowProxyActor))
	{
		ShadowProxyActor = nullptr;
		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = TEXT("FluidShadowProxyActor");
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ShadowProxyActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (ShadowProxyActor)
		{
			ShadowProxyActor->SetActorLabel(TEXT("FluidShadowProxy"));
		}
		else
		{
			return;
		}
	}

	// Lazy creation of ISM component
	if (!IsValid(ShadowInstanceComponent))
	{
		ShadowInstanceComponent = nullptr;

		if (!IsValid(ShadowSphereMesh))
		{
			ShadowSphereMesh = nullptr;
			ShadowSphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			if (!ShadowSphereMesh)
			{
				return;
			}
		}

		ShadowInstanceComponent = NewObject<UInstancedStaticMeshComponent>(ShadowProxyActor, TEXT("ShadowInstanceComponent"));
		if (ShadowInstanceComponent)
		{
			ShadowProxyActor->SetRootComponent(ShadowInstanceComponent);
			ShadowInstanceComponent->SetStaticMesh(ShadowSphereMesh);

			ShadowInstanceComponent->CastShadow = true;
			ShadowInstanceComponent->bCastDynamicShadow = true;
			ShadowInstanceComponent->bCastStaticShadow = false;
			ShadowInstanceComponent->bAffectDynamicIndirectLighting = false;
			ShadowInstanceComponent->bAffectDistanceFieldLighting = false;

			ShadowInstanceComponent->SetVisibility(false);
			ShadowInstanceComponent->SetHiddenInGame(true);
			ShadowInstanceComponent->bCastHiddenShadow = true;

			ShadowInstanceComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			ShadowInstanceComponent->SetGenerateOverlapEvents(false);
			ShadowInstanceComponent->bDisableCollision = true;
			ShadowInstanceComponent->SetMobility(EComponentMobility::Movable);

			ShadowInstanceComponent->RegisterComponent();
		}
	}

	if (!IsValid(ShadowInstanceComponent))
	{
		return;
	}

	// Calculate number of instances
	const int32 SkipFactor = FMath::Max(1, ParticleSkipFactor);
	int32 NumInstances = (NumParticles + SkipFactor - 1) / SkipFactor;

	if (MaxShadowInstances > 0)
	{
		NumInstances = FMath::Min(NumInstances, MaxShadowInstances);
	}

	// Prepare transforms with anisotropy
	CachedInstanceTransforms.SetNumUninitialized(NumInstances);

	const float BaseScale = ParticleRadius / 50.0f;
	const bool bHasAnisotropy = (AnisotropyAxis1 != nullptr && AnisotropyAxis2 != nullptr && AnisotropyAxis3 != nullptr);

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FluidShadow_ComputeTransforms);

		for (int32 i = 0, InstanceIdx = 0; i < NumParticles && InstanceIdx < NumInstances; i += SkipFactor, ++InstanceIdx)
		{
			FTransform& InstanceTransform = CachedInstanceTransforms[InstanceIdx];

			// Validate position - skip particles with NaN/Inf positions
			const FVector& Position = ParticlePositions[i];
			if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z))
			{
				// Use a safe default transform for invalid particles
				InstanceTransform.SetLocation(FVector::ZeroVector);
				InstanceTransform.SetRotation(FQuat::Identity);
				InstanceTransform.SetScale3D(FVector::ZeroVector);  // Zero scale = invisible
				continue;
			}

			InstanceTransform.SetLocation(Position);

			if (bHasAnisotropy)
			{
				// Build rotation from anisotropy axes (eigenvectors)
				const FVector4& Axis1 = AnisotropyAxis1[i];
				const FVector4& Axis2 = AnisotropyAxis2[i];
				const FVector4& Axis3 = AnisotropyAxis3[i];

				// Validate scale values - FMath::Clamp does NOT handle NaN properly!
				// NaN comparisons always return false, so Clamp(NaN, min, max) returns NaN
				const bool bValidScales =
					FMath::IsFinite(Axis1.W) &&
					FMath::IsFinite(Axis2.W) &&
					FMath::IsFinite(Axis3.W);

				if (bValidScales)
				{
					// Extract scale from W components (eigenvalues) with clamping
					const float Scale1 = FMath::Clamp(Axis1.W, 0.1f, 3.0f);
					const float Scale2 = FMath::Clamp(Axis2.W, 0.1f, 3.0f);
					const float Scale3 = FMath::Clamp(Axis3.W, 0.1f, 3.0f);

					// Build rotation matrix from eigenvectors
					FMatrix RotationMatrix = FMatrix::Identity;
					RotationMatrix.SetAxis(0, FVector(Axis1.X, Axis1.Y, Axis1.Z).GetSafeNormal());
					RotationMatrix.SetAxis(1, FVector(Axis2.X, Axis2.Y, Axis2.Z).GetSafeNormal());
					RotationMatrix.SetAxis(2, FVector(Axis3.X, Axis3.Y, Axis3.Z).GetSafeNormal());

					InstanceTransform.SetRotation(RotationMatrix.ToQuat());
					InstanceTransform.SetScale3D(FVector(Scale1, Scale2, Scale3) * BaseScale);
				}
				else
				{
					// Invalid anisotropy data - fall back to uniform sphere
					InstanceTransform.SetRotation(FQuat::Identity);
					InstanceTransform.SetScale3D(FVector(BaseScale));
				}
			}
			else
			{
				InstanceTransform.SetRotation(FQuat::Identity);
				InstanceTransform.SetScale3D(FVector(BaseScale));
			}
		}
	}

	// Optimized ISM update
	TRACE_CPUPROFILER_EVENT_SCOPE(FluidShadow_ISMUpdate);
	const int32 CurrentInstanceCount = ShadowInstanceComponent->GetInstanceCount();
	const int32 TargetInstanceCount = NumInstances;

	if (TargetInstanceCount == 0)
	{
		if (CurrentInstanceCount > 0)
		{
			ShadowInstanceComponent->ClearInstances();
		}
		return;
	}

	if (CurrentInstanceCount == TargetInstanceCount)
	{
		// Batch update all transforms at once
		ShadowInstanceComponent->BatchUpdateInstancesTransforms(
			0,
			CachedInstanceTransforms,
			false,  // bWorldSpace
			true,   // bMarkRenderStateDirty
			false   // bTeleport
		);
	}
	else
	{
		// Instance count changed - rebuild entirely
		ShadowInstanceComponent->ClearInstances();
		ShadowInstanceComponent->AddInstances(CachedInstanceTransforms, false, true);
	}
}
