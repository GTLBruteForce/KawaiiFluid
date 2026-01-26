// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Modules/KawaiiFluidSimulationModule.h"
#include "Core/SpatialHash.h"
#include "Collision/FluidCollider.h"
#include "Components/FluidInteractionComponent.h"
#include "Components/KawaiiFluidComponent.h"
#include "Components/KawaiiFluidVolumeComponent.h"
#include "Actors/KawaiiFluidVolume.h"
#include "Data/KawaiiFluidPresetDataAsset.h"
#include "GPU/GPUFluidSimulator.h"
#include "GPU/GPUFluidSimulatorShaders.h"  // For GPU_MORTON_GRID_AXIS_BITS
#include "GPU/GPUFluidParticle.h"  // For FGPUSpawnRequest
#include "UObject/UObjectGlobals.h"  // For FCoreUObjectDelegates
#include "UObject/ObjectSaveContext.h"  // For FObjectPreSaveContext
#include "Engine/World.h"

#if WITH_EDITOR
#include "Editor.h"  // For FEditorDelegates
#endif

UKawaiiFluidSimulationModule::UKawaiiFluidSimulationModule()
{
	// Initialize default volume size based on Medium Z-Order preset and CellSize
	// Formula: GridResolution(Medium) * CellSize = 128 * CellSize
	const float MediumGridResolution = static_cast<float>(GridResolutionPresetHelper::GetGridResolution(EGridResolutionPreset::Medium));
	const float DefaultVolumeSize = MediumGridResolution * CellSize;
	UniformVolumeSize = DefaultVolumeSize;
	VolumeSize = FVector(DefaultVolumeSize);

	// Calculate initial bounds
	RecalculateVolumeBounds();
}

void UKawaiiFluidSimulationModule::PostInitProperties()
{
	Super::PostInitProperties();

#if WITH_EDITOR
	// CDO가 아닌 경우에만 델리게이트 바인딩
	// PostLoad는 디스크에서 로드할 때만 호출되므로,
	// CreateDefaultSubobject로 생성된 경우를 위해 여기서도 바인딩
	if (!HasAnyFlags(RF_ClassDefaultObject) && !PreBeginPIEHandle.IsValid())
	{
		PreBeginPIEHandle = FEditorDelegates::PreBeginPIE.AddUObject(this, &UKawaiiFluidSimulationModule::OnPreBeginPIE);
	}
#endif
}

void UKawaiiFluidSimulationModule::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	// Bind to preset property changes when loading in editor
	if (Preset)
	{
		BindToPresetPropertyChanged();
	}

	// Bind to objects replaced event (for asset reload detection)
	BindToObjectsReplaced();

	// Bind to PreBeginPIE for GPU→CPU sync before PIE duplication
	if (!PreBeginPIEHandle.IsValid())
	{
		PreBeginPIEHandle = FEditorDelegates::PreBeginPIE.AddUObject(this, &UKawaiiFluidSimulationModule::OnPreBeginPIE);
	}

	// Update volume info to fix any mismatch between CellSize and VolumeSize
	// This ensures internal grid preset is calculated correctly based on current Preset
	UpdateVolumeInfoDisplay();
#endif
}

void UKawaiiFluidSimulationModule::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	// PIE로 복제될 때 EditorWorld의 객체를 가리키는 stale pointer 초기화
	// 이 포인터들은 새 World의 BeginPlay에서 올바른 객체로 다시 설정됨
	CachedSimulationContext = nullptr;
	WeakGPUSimulator.Reset();
	bGPUSimulationActive = false;

	// ⚠️ Particles 배열은 유지! (PreBeginPIE에서 캐시한 GPU 데이터
	// UPROPERTY TArray라서 자동으로 딥카피됨
	const int32 PreservedParticleCount = Particles.Num();

	// SpatialHash는 Initialize에서 다시 생성됨
	SpatialHash.Reset();

	// OwnedVolumeComponent도 다른 World의 객체이므로 리셋
	OwnedVolumeComponent = nullptr;
	PreviousRegisteredVolume.Reset();

	bIsInitialized = false;  // BeginPlay에서 다시 초기화

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidSimulationModule::PostDuplicate - Preserved %d particles, cleared stale pointers (PIE=%d)"),
		PreservedParticleCount, bDuplicateForPIE ? 1 : 0);
}

//========================================
// Serialization (PreSave)
//========================================

void UKawaiiFluidSimulationModule::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	// 저장 전 GPU 데이터를 CPU Particles 배열로 동기화
	SyncGPUParticlesToCPU();
}

//========================================
// GPU ↔ CPU Particle Sync
//========================================

void UKawaiiFluidSimulationModule::SyncGPUParticlesToCPU()
{
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (!GPUSim)
	{
		// GPU 시뮬레이터 없으면 기존 CPU Particles 유지
		return;
	}

	const int32 GPUCount = GPUSim->GetParticleCount();
	if (GPUCount <= 0)
	{
		// GPU에 파티클 없으면 기존 유지
		return;
	}

	TArray<FFluidParticle> MyParticles;

	if (GPUSim->IsReady())
	{
		// 내 SourceID 파티클만 필터링해서 가져오기 (배칭 환경 대응)
		if (GPUSim->GetParticlesBySourceID(CachedSourceID, MyParticles))
		{
			Particles = MoveTemp(MyParticles);
		}
	}
}

void UKawaiiFluidSimulationModule::UploadCPUParticlesToGPU()
{
	if (Particles.Num() == 0)
	{
		return;
	}

	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (!GPUSim || !GPUSim->IsReady())
	{
		UE_LOG(LogTemp, Warning, TEXT("UploadCPUParticlesToGPU: GPUSimulator not ready, %d particles waiting"), Particles.Num());
		return;
	}

	const int32 UploadCount = Particles.Num();

	// Atomic ID 할당: 저장된 ID 무시하고 새로 할당 (멀티모듈 충돌 방지 + overflow 리셋)
	const int32 StartID = GPUSim->AllocateParticleIDs(UploadCount);
	for (int32 i = 0; i < UploadCount; ++i)
	{
		Particles[i].ParticleID = StartID + i;
		Particles[i].SourceID = CachedSourceID;
	}

	// CPU에서 GPU로 업로드 (bAppend=true: 배칭 환경에서 다른 컴포넌트 파티클 보존)
	GPUSim->UploadParticles(Particles, /*bAppend=*/true);

	// 모든 append 후 GPU 버퍼 생성/갱신 + SpawnManager 상태 리셋
	GPUSim->FinalizeUpload();

	// GPU에 올렸으니 CPU 배열 정리 (중복 업로드 방지 + 메모리 절약)
	Particles.Empty();

	UE_LOG(LogTemp, Log, TEXT("UploadCPUParticlesToGPU: Uploaded %d particles (SourceID=%d, IDs=%d~%d) to GPU"),
		UploadCount, CachedSourceID, StartID, StartID + UploadCount - 1);
}

void UKawaiiFluidSimulationModule::BeginDestroy()
{
#if WITH_EDITOR
	// Unbind from preset property changes
	UnbindFromPresetPropertyChanged();

	// Unbind from objects replaced event
	UnbindFromObjectsReplaced();

	// Unbind from PreBeginPIE
	if (PreBeginPIEHandle.IsValid())
	{
		FEditorDelegates::PreBeginPIE.Remove(PreBeginPIEHandle);
		PreBeginPIEHandle.Reset();
	}
#endif

	// Unbind from volume destroyed event
	UnbindFromVolumeDestroyedEvent();

	Super::BeginDestroy();
}

#if WITH_EDITOR
void UKawaiiFluidSimulationModule::OnPreBeginPIE(bool bIsSimulating)
{
	// PIE 시작 전 GPU 파티클을 CPU로 동기화
	// 이 데이터는 PostDuplicate에서 자동으로 딥카피되어 PIE World로 전달됨
	SyncGPUParticlesToCPU();


	UE_LOG(LogTemp, Log, TEXT("OnPreBeginPIE: Synced %d particles for PIE transfer"), Particles.Num());
}
#endif

#if WITH_EDITOR
void UKawaiiFluidSimulationModule::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Preset 변경 시
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, Preset))
	{
		// Unbind from old preset's delegate, bind to new preset
		UnbindFromPresetPropertyChanged();

			if (Preset)
		{
			// SpatialHash 재구성
			if (SpatialHash.IsValid())
			{
				SpatialHash = MakeShared<FSpatialHash>(Preset->SmoothingRadius);
			}
			// Subscribe to preset property changes
			BindToPresetPropertyChanged();
		}
		// Update CellSize and bounds (handles both internal and external volume cases)
		UpdateVolumeInfoDisplay();
	}
	// TargetSimulationVolume 변경 시 정보 업데이트
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, TargetSimulationVolume))
	{
		// Unbind from previous volume's OnDestroyed event
		UnbindFromVolumeDestroyedEvent();

		// Unregister from previous volume
		if (UKawaiiFluidVolumeComponent* PrevVolume = PreviousRegisteredVolume.Get())
		{
			PrevVolume->UnregisterModule(this);
		}

		// Register with new volume and update tracking
		if (TargetSimulationVolume)
		{
			if (UKawaiiFluidVolumeComponent* NewVolume = TargetSimulationVolume->GetVolumeComponent())
			{
				NewVolume->RegisterModule(this);
				PreviousRegisteredVolume = NewVolume;
			}
			// Bind to new volume's OnDestroyed event
			BindToVolumeDestroyedEvent();
		}
		else
		{
			PreviousRegisteredVolume = nullptr;
		}

		UpdateVolumeInfoDisplay();
	}
	// GridResolutionPreset 변경 시 (Internal Volume용)
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, GridResolutionPreset))
	{
		// Only update if not using external volume
		if (!TargetSimulationVolume)
		{
			UpdateVolumeInfoDisplay();
		}
	}
	// bUniformSize 체크박스 변경 시 - Sync values between uniform and non-uniform
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, bUniformSize))
	{
		if (!TargetSimulationVolume)
		{
			if (bUniformSize)
			{
				// When switching to uniform, use the max axis of VolumeSize
				UniformVolumeSize = FMath::Max3(VolumeSize.X, VolumeSize.Y, VolumeSize.Z);
			}
			else
			{
				// When switching to non-uniform, copy uniform to all axes
				VolumeSize = FVector(UniformVolumeSize);
			}
			RecalculateVolumeBounds();
		}
	}
	// UniformVolumeSize 변경 시 - Clamp and recalculate
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, UniformVolumeSize))
	{
		if (!TargetSimulationVolume)
		{
			// Clamp to max supported (Large preset max * 2 for full size)
			const float EffectiveCellSize = FMath::Max(CellSize, 1.0f);
			const float MaxFullSize = GridResolutionPresetHelper::GetMaxExtentForPreset(EGridResolutionPreset::Large, EffectiveCellSize) * 2.0f;
			UniformVolumeSize = FMath::Clamp(UniformVolumeSize, 10.0f, MaxFullSize);
			RecalculateVolumeBounds();
		}
	}
	// VolumeSize 변경 시 - Clamp and recalculate
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, VolumeSize))
	{
		if (!TargetSimulationVolume)
		{
			// Clamp each axis to max supported
			const float EffectiveCellSize = FMath::Max(CellSize, 1.0f);
			const float MaxFullSize = GridResolutionPresetHelper::GetMaxExtentForPreset(EGridResolutionPreset::Large, EffectiveCellSize) * 2.0f;
			VolumeSize.X = FMath::Clamp(VolumeSize.X, 10.0f, MaxFullSize);
			VolumeSize.Y = FMath::Clamp(VolumeSize.Y, 10.0f, MaxFullSize);
			VolumeSize.Z = FMath::Clamp(VolumeSize.Z, 10.0f, MaxFullSize);
			RecalculateVolumeBounds();
		}
	}
	// VolumeRotation 변경 시 - Recalculate bounds (OBB → AABB expansion)
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, VolumeRotation))
	{
		if (!TargetSimulationVolume)
		{
			RecalculateVolumeBounds();
		}
	}
	// CellSize 변경 시 - Recalculate volume size to maintain Medium preset
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UKawaiiFluidSimulationModule, CellSize))
	{
		if (!TargetSimulationVolume)
		{
			// Recalculate volume size based on Medium Z-Order preset
			const float MediumGridResolution = static_cast<float>(GridResolutionPresetHelper::GetGridResolution(EGridResolutionPreset::Medium));
			const float NewDefaultSize = MediumGridResolution * CellSize;
			UniformVolumeSize = NewDefaultSize;
			VolumeSize = FVector(NewDefaultSize);
			RecalculateVolumeBounds();
		}
	}
}
#endif

void UKawaiiFluidSimulationModule::Initialize(UKawaiiFluidPresetDataAsset* InPreset)
{
	if (bIsInitialized)
	{
		return;
	}

	Preset = InPreset;

	// SpatialHash 초기화 (Independent 모드용)
	float SpatialHashCellSize = 20.0f;
	if (Preset)
	{
		SpatialHashCellSize = Preset->SmoothingRadius;
		// Always sync CellSize to Preset's SmoothingRadius for optimal SPH neighbor search
		if (Preset->SmoothingRadius > 0.0f)
		{
			CellSize = Preset->SmoothingRadius;
		}
	}
	InitializeSpatialHash(SpatialHashCellSize);

	// Create owned volume component for internal bounds
	if (!OwnedVolumeComponent)
	{
		OwnedVolumeComponent = NewObject<UKawaiiFluidVolumeComponent>(this, NAME_None, RF_Transient);
		OwnedVolumeComponent->CellSize = CellSize;
		OwnedVolumeComponent->bShowBoundsInEditor = false;  // Module controls its own visualization
		OwnedVolumeComponent->bShowBoundsAtRuntime = false;
	}

	// Update volume bounds
	RecalculateVolumeBounds();

	bIsInitialized = true;

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidSimulationModule initialized"));
}

void UKawaiiFluidSimulationModule::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	ClearAllParticles();
	Particles.Empty();
	Colliders.Empty();
	SpatialHash.Reset();
	Preset = nullptr;
	OwnedVolumeComponent = nullptr;
	TargetSimulationVolume = nullptr;

	bIsInitialized = false;

	UE_LOG(LogTemp, Log, TEXT("UKawaiiFluidSimulationModule shutdown"));
}

void UKawaiiFluidSimulationModule::SetPreset(UKawaiiFluidPresetDataAsset* InPreset)
{
	Preset = InPreset;

	// SpatialHash 재구성
	if (Preset && SpatialHash.IsValid())
	{
		SpatialHash = MakeShared<FSpatialHash>(Preset->SmoothingRadius);
	}
}

void UKawaiiFluidSimulationModule::InitializeSpatialHash(float InCellSize)
{
	SpatialHash = MakeShared<FSpatialHash>(InCellSize);
}

AActor* UKawaiiFluidSimulationModule::GetOwnerActor() const
{
	if (UActorComponent* OwnerComp = Cast<UActorComponent>(GetOuter()))
	{
		return OwnerComp->GetOwner();
	}
	return nullptr;
}

FKawaiiFluidSimulationParams UKawaiiFluidSimulationModule::BuildSimulationParams() const
{
	FKawaiiFluidSimulationParams Params;

	// 외력
	Params.ExternalForce = AccumulatedExternalForce;

	// 콜라이더 / 상호작용 컴포넌트
	Params.Colliders = Colliders;

	// Preset에서 콜리전 설정 가져오기
	if (Preset)
	{
		Params.CollisionChannel = Preset->CollisionChannel;
		Params.ParticleRadius = GetParticleRadius();  // Use getter to respect override
	}

	// Context - Module에서 직접 접근 (Outer 체인 활용)
	Params.World = GetWorld();
	Params.IgnoreActor = GetOwnerActor();
	Params.bUseWorldCollision = bUseWorldCollision;

	// Get owner component for simulation origin and static boundary settings
	if (UKawaiiFluidComponent* OwnerComp = Cast<UKawaiiFluidComponent>(GetOuter()))
	{
		// Set simulation origin for bounds offset (preset bounds are relative to component)
		Params.SimulationOrigin = OwnerComp->GetComponentLocation();

		// Static boundary particles (Akinci 2012) - density contribution from walls/floors
		Params.bEnableStaticBoundaryParticles = OwnerComp->bEnableStaticBoundaryParticles;
		Params.StaticBoundaryParticleSpacing = OwnerComp->StaticBoundaryParticleSpacing;
	}
	else if (UKawaiiFluidVolumeComponent* VolumeComp = GetTargetVolumeComponent())
	{
		// Volume-based simulation: use VolumeComponent's static boundary settings
		Params.SimulationOrigin = VolumeComp->GetComponentLocation();

		// Static boundary particles (Akinci 2012) - density contribution from walls/floors
		// Default is false due to known issue with particles flying around chaotically
		Params.bEnableStaticBoundaryParticles = VolumeComp->IsStaticBoundaryParticlesEnabled();
		Params.StaticBoundaryParticleSpacing = VolumeComp->GetStaticBoundaryParticleSpacing();
	}
	else
	{
		// Fallback: disable static boundary particles by default
		Params.bEnableStaticBoundaryParticles = false;
		Params.StaticBoundaryParticleSpacing = 5.0f;
	}

	// Unified Simulation Volume for containment collision (always enabled)
	// Check if using external volume
	if (UKawaiiFluidVolumeComponent* ExternalVolume = GetTargetVolumeComponent())
	{
		if (IsUsingExternalVolume())
		{
			// Use external volume's location and user-defined size for containment
			Params.BoundsCenter = ExternalVolume->GetComponentLocation();
			Params.BoundsExtent = ExternalVolume->GetVolumeHalfExtent();
			Params.BoundsRotation = FQuat::Identity;  // External volumes are axis-aligned

			// AABB is the same as OBB for axis-aligned volumes
			Params.WorldBounds = FBox(
				Params.BoundsCenter - Params.BoundsExtent,
				Params.BoundsCenter + Params.BoundsExtent
			);

			// Use external volume's collision parameters
			Params.BoundsRestitution = ExternalVolume->GetWallBounce();
			Params.BoundsFriction = ExternalVolume->GetWallFriction();
		}
		else
		{
			// Internal volume - use module's own settings
			Params.BoundsCenter = VolumeCenter;
			Params.BoundsExtent = GridResolutionPresetHelper::ClampExtentToMaxSupported(GetVolumeHalfExtent(), CellSize);
			Params.BoundsRotation = VolumeRotationQuat;

			// Compute AABB from OBB for rotated internal volumes
			FVector RotatedExtents[8];
			for (int32 i = 0; i < 8; ++i)
			{
				FVector Corner(
					(i & 1) ? Params.BoundsExtent.X : -Params.BoundsExtent.X,
					(i & 2) ? Params.BoundsExtent.Y : -Params.BoundsExtent.Y,
					(i & 4) ? Params.BoundsExtent.Z : -Params.BoundsExtent.Z
				);
				RotatedExtents[i] = VolumeRotationQuat.RotateVector(Corner);
			}

			FVector AABBMin = RotatedExtents[0];
			FVector AABBMax = RotatedExtents[0];
			for (int32 i = 1; i < 8; ++i)
			{
				AABBMin = AABBMin.ComponentMin(RotatedExtents[i]);
				AABBMax = AABBMax.ComponentMax(RotatedExtents[i]);
			}

			Params.WorldBounds = FBox(VolumeCenter + AABBMin, VolumeCenter + AABBMax);
			Params.BoundsRestitution = Preset ? Preset->Bounciness : 0.0f;
			Params.BoundsFriction = Preset ? Preset->Friction : 0.5f;
		}
	}
	else
	{
		// Fallback: use internal volume settings
		Params.BoundsCenter = VolumeCenter;
		Params.BoundsExtent = GridResolutionPresetHelper::ClampExtentToMaxSupported(GetVolumeHalfExtent(), CellSize);
		Params.BoundsRotation = VolumeRotationQuat;

		// Compute AABB from OBB
		FVector RotatedExtents[8];
		for (int32 i = 0; i < 8; ++i)
		{
			FVector Corner(
				(i & 1) ? Params.BoundsExtent.X : -Params.BoundsExtent.X,
				(i & 2) ? Params.BoundsExtent.Y : -Params.BoundsExtent.Y,
				(i & 4) ? Params.BoundsExtent.Z : -Params.BoundsExtent.Z
			);
			RotatedExtents[i] = VolumeRotationQuat.RotateVector(Corner);
		}

		FVector AABBMin = RotatedExtents[0];
		FVector AABBMax = RotatedExtents[0];
		for (int32 i = 1; i < 8; ++i)
		{
			AABBMin = AABBMin.ComponentMin(RotatedExtents[i]);
			AABBMax = AABBMax.ComponentMax(RotatedExtents[i]);
		}

		Params.WorldBounds = FBox(VolumeCenter + AABBMin, VolumeCenter + AABBMax);
		Params.BoundsRestitution = Preset ? Preset->Bounciness : 0.0f;
		Params.BoundsFriction = Preset ? Preset->Friction : 0.5f;
	}

	// Event Settings
	Params.bEnableCollisionEvents = bEnableCollisionEvents;
	Params.MinVelocityForEvent = MinVelocityForEvent;
	Params.MaxEventsPerFrame = MaxEventsPerFrame;
	Params.EventCooldownPerParticle = EventCooldownPerParticle;

	if (bEnableCollisionEvents)
	{
		// 쿨다운 추적용 맵 연결 (const_cast 필요 - mutable 대안)
		Params.ParticleLastEventTimePtr = const_cast<TMap<int32, float>*>(&ParticleLastEventTime);

		// 현재 게임 시간
		if (UWorld* World = GetWorld())
		{
			Params.CurrentGameTime = World->GetTimeSeconds();
		}

		// 콜백 바인딩
		if (OnCollisionEventCallback.IsBound())
		{
			Params.OnCollisionEvent = OnCollisionEventCallback;
		}

		// SourceID 필터링용 (자기 Component에서 스폰한 파티클만 콜백)
		Params.SourceID = CachedSourceID;
	}

	return Params;
}

void UKawaiiFluidSimulationModule::ProcessCollisionFeedback(
	const TMap<int32, UFluidInteractionComponent*>& OwnerIDToIC,
	const TArray<FKawaiiFluidCollisionEvent>& CPUFeedbackBuffer)
{
	// 콜백이 없거나 충돌 이벤트 비활성화면 스킵
	if (!OnCollisionEventCallback.IsBound() || !bEnableCollisionEvents)
	{
		return;
	}

	// SourceComponent 캐시
	UKawaiiFluidComponent* OwnerComponent = GetTypedOuter<UKawaiiFluidComponent>();
	const float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	int32 EventCount = 0;

	//========================================
	// GPU 피드백 처리
	//========================================
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (bGPUSimulationActive && GPUSim)
	{
		TArray<FGPUCollisionFeedback> GPUFeedbacks;
		int32 GPUFeedbackCount = 0;
		GPUSim->GetAllCollisionFeedback(GPUFeedbacks, GPUFeedbackCount);

		for (int32 i = 0; i < GPUFeedbackCount && EventCount < MaxEventsPerFrame; ++i)
		{
			const FGPUCollisionFeedback& Feedback = GPUFeedbacks[i];

			// SourceID 필터링
			if (CachedSourceID >= 0 && Feedback.ParticleSourceID != CachedSourceID)
			{
				continue;
			}

			// 속도 체크
			const float HitSpeed = FVector3f(Feedback.ParticleVelocity).Length();
			if (HitSpeed < MinVelocityForEvent)
			{
				continue;
			}

			// 쿨다운 체크
			if (const float* LastTime = ParticleLastEventTime.Find(Feedback.ParticleIndex))
			{
				if (CurrentTime - *LastTime < EventCooldownPerParticle)
				{
					continue;
				}
			}

			// 쿨다운 업데이트
			ParticleLastEventTime.Add(Feedback.ParticleIndex, CurrentTime);

			// 이벤트 생성
			FKawaiiFluidCollisionEvent Event;
			Event.ParticleIndex = Feedback.ParticleIndex;
			Event.SourceID = Feedback.ParticleSourceID;
			Event.ColliderOwnerID = Feedback.ColliderOwnerID;
			Event.BoneIndex = Feedback.BoneIndex;
			Event.HitLocation = FVector(Feedback.ImpactNormal * (-Feedback.Penetration));
			Event.HitNormal = FVector(Feedback.ImpactNormal);
			Event.HitSpeed = HitSpeed;
			Event.SourceComponent = OwnerComponent;

			// IC 조회 (O(1))
			if (const UFluidInteractionComponent* const* FoundIC = OwnerIDToIC.Find(Feedback.ColliderOwnerID))
			{
				Event.HitInteractionComponent = const_cast<UFluidInteractionComponent*>(*FoundIC);
				Event.HitActor = (*FoundIC)->GetOwner();
			}

			OnCollisionEventCallback.Execute(Event);
			++EventCount;
		}
	}

	//========================================
	// CPU 피드백 처리 (Subsystem 버퍼에서 SourceID로 필터링)
	//========================================
	for (const FKawaiiFluidCollisionEvent& BufferEvent : CPUFeedbackBuffer)
	{
		if (EventCount >= MaxEventsPerFrame)
		{
			break;
		}

		// SourceID 필터링 - 이 Module의 파티클만 처리
		if (CachedSourceID >= 0 && BufferEvent.SourceID != CachedSourceID)
		{
			continue;
		}

		// 쿨다운 체크
		if (const float* LastTime = ParticleLastEventTime.Find(BufferEvent.ParticleIndex))
		{
			if (CurrentTime - *LastTime < EventCooldownPerParticle)
			{
				continue;
			}
		}

		// 쿨다운 업데이트
		ParticleLastEventTime.Add(BufferEvent.ParticleIndex, CurrentTime);

		// 이벤트 복사 및 추가 정보 설정
		FKawaiiFluidCollisionEvent Event = BufferEvent;
		Event.SourceComponent = OwnerComponent;

		// IC 조회 (O(1)) - CPU 버퍼에는 IC가 없을 수 있음
		if (!Event.HitInteractionComponent && Event.ColliderOwnerID >= 0)
		{
			if (const UFluidInteractionComponent* const* FoundIC = OwnerIDToIC.Find(Event.ColliderOwnerID))
			{
				Event.HitInteractionComponent = const_cast<UFluidInteractionComponent*>(*FoundIC);
				if (!Event.HitActor)
				{
					Event.HitActor = (*FoundIC)->GetOwner();
				}
			}
		}

		OnCollisionEventCallback.Execute(Event);
		++EventCount;
	}
}

int32 UKawaiiFluidSimulationModule::SpawnParticle(FVector Position, FVector Velocity)
{
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (!GPUSim)
	{
		return -1;
	}

	const float Mass = Preset ? Preset->ParticleMass : 1.0f;
	const float Radius = Preset ? Preset->ParticleRadius : 5.0f;

	FGPUSpawnRequest Request;
	Request.Position = FVector3f(Position);
	Request.Velocity = FVector3f(Velocity);
	Request.Mass = Mass;
	Request.Radius = Radius;
	Request.SourceID = CachedSourceID;

	TArray<FGPUSpawnRequest> Requests;
	Requests.Add(Request);
	GPUSim->AddSpawnRequests(Requests);

	return -1;  // GPU assigns ID asynchronously
}

void UKawaiiFluidSimulationModule::SpawnParticles(FVector Location, int32 Count, float SpawnRadius)
{
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (!GPUSim)
	{
		return;
	}

	const float Mass = Preset ? Preset->ParticleMass : 1.0f;
	const float Radius = Preset ? Preset->ParticleRadius : 5.0f;

	TArray<FGPUSpawnRequest> SpawnRequests;
	SpawnRequests.Reserve(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		FVector RandomOffset = FMath::VRand() * FMath::FRandRange(0.0f, SpawnRadius);
		FVector SpawnPos = Location + RandomOffset;

		FGPUSpawnRequest Request;
		Request.Position = FVector3f(SpawnPos);
		Request.Velocity = FVector3f::ZeroVector;
		Request.Mass = Mass;
		Request.Radius = Radius;
		Request.SourceID = CachedSourceID;
		SpawnRequests.Add(Request);
	}

	GPUSim->AddSpawnRequests(SpawnRequests);
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesSphere(FVector Center, float Radius, float Spacing,
                                                         bool bJitter, float JitterAmount, FVector Velocity,
                                                         FRotator Rotation)
{
	if (Spacing <= 0.0f || Radius <= 0.0f)
	{
		return 0;
	}

	// 구체를 포함하는 박스 범위 계산
	const int32 GridSize = FMath::CeilToInt(Radius / Spacing);
	const float RadiusSq = Radius * Radius;
	const float HalfSpacing = Spacing * 0.5f;
	const float JitterRange = Spacing * JitterAmount;

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용 (구형은 위치 회전 불필요)
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;

	// 예상 파티클 수 계산 (구 부피 / 파티클 부피)
	const float EstimatedCount = (4.0f / 3.0f * PI * Radius * Radius * Radius) / (Spacing * Spacing * Spacing);

	// GPU mode: batch spawn requests for efficiency
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (bGPUSimulationActive && GPUSim)
	{
		const float Mass = Preset ? Preset->ParticleMass : 1.0f;
		const float ParticleRadius = Preset ? Preset->ParticleRadius : 5.0f;

		TArray<FGPUSpawnRequest> SpawnRequests;
		SpawnRequests.Reserve(FMath::CeilToInt(EstimatedCount));

		for (int32 x = -GridSize; x <= GridSize; ++x)
		{
			for (int32 y = -GridSize; y <= GridSize; ++y)
			{
				for (int32 z = -GridSize; z <= GridSize; ++z)
				{
					FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

					if (LocalPos.SizeSquared() <= RadiusSq)
					{
						FVector SpawnPos = Center + LocalPos;

						if (bJitter && JitterRange > 0.0f)
						{
							SpawnPos += FVector(
								FMath::FRandRange(-JitterRange, JitterRange),
								FMath::FRandRange(-JitterRange, JitterRange),
								FMath::FRandRange(-JitterRange, JitterRange)
							);
						}

						FGPUSpawnRequest Request;
						Request.Position = FVector3f(SpawnPos);
						Request.Velocity = FVector3f(WorldVelocity);
						Request.Mass = Mass;
						Request.Radius = ParticleRadius;
						Request.SourceID = CachedSourceID;  // Propagate source identification
						SpawnRequests.Add(Request);
						++SpawnedCount;
					}
				}
			}
		}

		if (SpawnRequests.Num() > 0)
		{
			GPUSim->AddSpawnRequests(SpawnRequests);
		}
		return SpawnedCount;
	}

	return 0;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesBox(FVector Center, FVector Extent, float Spacing,
                                                      bool bJitter, float JitterAmount, FVector Velocity,
                                                      FRotator Rotation)
{
	if (Spacing <= 0.0f)
	{
		return 0;
	}

	// 각 축별 격자 개수 계산
	const int32 CountX = FMath::Max(1, FMath::CeilToInt(Extent.X * 2.0f / Spacing));
	const int32 CountY = FMath::Max(1, FMath::CeilToInt(Extent.Y * 2.0f / Spacing));
	const int32 CountZ = FMath::Max(1, FMath::CeilToInt(Extent.Z * 2.0f / Spacing));

	const float JitterRange = Spacing * JitterAmount;
	const FVector LocalStartOffset = -Extent + FVector(Spacing * 0.5f);

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	const int32 TotalCount = CountX * CountY * CountZ;
	Particles.Reserve(Particles.Num() + TotalCount);

	for (int32 x = 0; x < CountX; ++x)
	{
		for (int32 y = 0; y < CountY; ++y)
		{
			for (int32 z = 0; z < CountZ; ++z)
			{
				// 로컬 위치 계산
				FVector LocalPos = LocalStartOffset + FVector(x * Spacing, y * Spacing, z * Spacing);

				// Jitter 적용 (로컬 공간에서)
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				// 회전 적용 후 월드 위치 계산
				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);

				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesCylinder(FVector Center, float Radius, float HalfHeight, float Spacing,
                                                           bool bJitter, float JitterAmount, FVector Velocity,
                                                           FRotator Rotation)
{
	if (Spacing <= 0.0f || Radius <= 0.0f || HalfHeight <= 0.0f)
	{
		return 0;
	}

	// 원기둥을 포함하는 박스 범위 계산
	const int32 GridSizeXY = FMath::CeilToInt(Radius / Spacing);
	const int32 GridSizeZ = FMath::CeilToInt(HalfHeight / Spacing);
	const float RadiusSq = Radius * Radius;
	const float JitterRange = Spacing * JitterAmount;

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;

	// 예상 파티클 수 계산 (원기둥 부피 / 파티클 부피)
	const float EstimatedCount = (PI * Radius * Radius * HalfHeight * 2.0f) / (Spacing * Spacing * Spacing);
	Particles.Reserve(Particles.Num() + FMath::CeilToInt(EstimatedCount));

	// 격자 순회
	for (int32 x = -GridSizeXY; x <= GridSizeXY; ++x)
	{
		for (int32 y = -GridSizeXY; y <= GridSizeXY; ++y)
		{
			// XY 평면에서 원 내부인지 확인
			const float DistSqXY = x * x * Spacing * Spacing + y * y * Spacing * Spacing;
			if (DistSqXY > RadiusSq)
			{
				continue;
			}

			for (int32 z = -GridSizeZ; z <= GridSizeZ; ++z)
			{
				FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

				// Jitter 적용 (로컬 공간에서)
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				// 회전 적용 후 월드 위치 계산
				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);

				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

//=============================================================================
// Hexagonal Close Packing Spawn Functions
//=============================================================================

int32 UKawaiiFluidSimulationModule::SpawnParticlesBoxHexagonal(FVector Center, FVector Extent, float Spacing,
                                                                bool bJitter, float JitterAmount, FVector Velocity,
                                                                FRotator Rotation)
{
	if (Spacing <= 0.0f)
	{
		return 0;
	}

	// HCP density compensation: HCP is ~1.42x denser than cubic for the same spacing
	// To achieve similar number density, multiply spacing by (1/0.707)^(1/3) ≈ 1.122
	const float HCPCompensation = 1.122f;
	const float AdjustedSpacing = Spacing * HCPCompensation;

	// Hexagonal Close Packing (HCP) constants
	// XY plane: hexagonal grid with row offset
	// Z layers: alternating offset for close packing
	const float RowSpacingY = AdjustedSpacing * 0.866025f;  // sqrt(3)/2
	const float LayerSpacingZ = AdjustedSpacing * 0.816497f;  // sqrt(2/3)
	const float JitterRange = AdjustedSpacing * JitterAmount;

	// Calculate grid counts
	const int32 CountX = FMath::Max(1, FMath::CeilToInt(Extent.X * 2.0f / AdjustedSpacing));
	const int32 CountY = FMath::Max(1, FMath::CeilToInt(Extent.Y * 2.0f / RowSpacingY));
	const int32 CountZ = FMath::Max(1, FMath::CeilToInt(Extent.Z * 2.0f / LayerSpacingZ));

	const FQuat RotationQuat = Rotation.Quaternion();
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	const int32 EstimatedTotal = CountX * CountY * CountZ;
	Particles.Reserve(Particles.Num() + EstimatedTotal);

	// Start position (bottom-left-back corner)
	const FVector LocalStart(-Extent.X + AdjustedSpacing * 0.5f, -Extent.Y + RowSpacingY * 0.5f, -Extent.Z + LayerSpacingZ * 0.5f);

	for (int32 z = 0; z < CountZ; ++z)
	{
		// Z layer offset for HCP (ABC stacking pattern)
		const float ZLayerOffsetX = (z % 3 == 1) ? AdjustedSpacing * 0.5f : ((z % 3 == 2) ? AdjustedSpacing * 0.25f : 0.0f);
		const float ZLayerOffsetY = (z % 3 == 1) ? RowSpacingY / 3.0f : ((z % 3 == 2) ? RowSpacingY * 2.0f / 3.0f : 0.0f);

		for (int32 y = 0; y < CountY; ++y)
		{
			// Row offset for hexagonal pattern in XY plane
			const float RowOffsetX = (y % 2 == 1) ? AdjustedSpacing * 0.5f : 0.0f;

			for (int32 x = 0; x < CountX; ++x)
			{
				FVector LocalPos(
					LocalStart.X + x * AdjustedSpacing + RowOffsetX + ZLayerOffsetX,
					LocalStart.Y + y * RowSpacingY + ZLayerOffsetY,
					LocalStart.Z + z * LayerSpacingZ
				);

				// Check bounds
				if (FMath::Abs(LocalPos.X) > Extent.X ||
				    FMath::Abs(LocalPos.Y) > Extent.Y ||
				    FMath::Abs(LocalPos.Z) > Extent.Z)
				{
					continue;
				}

				// Apply jitter
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);
				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesSphereHexagonal(FVector Center, float Radius, float Spacing,
                                                                   bool bJitter, float JitterAmount, FVector Velocity,
                                                                   FRotator Rotation)
{
	if (Spacing <= 0.0f || Radius <= 0.0f)
	{
		return 0;
	}

	// HCP density compensation: HCP is ~1.42x denser than cubic for the same spacing
	const float HCPCompensation = 1.122f;
	const float AdjustedSpacing = Spacing * HCPCompensation;

	// Use Box Hexagonal spawn and filter by sphere radius
	const float RowSpacingY = AdjustedSpacing * 0.866025f;
	const float LayerSpacingZ = AdjustedSpacing * 0.816497f;
	const float JitterRange = AdjustedSpacing * JitterAmount;
	const float RadiusSq = Radius * Radius;

	const int32 GridSize = FMath::CeilToInt(Radius / AdjustedSpacing) + 1;
	const int32 GridSizeY = FMath::CeilToInt(Radius / RowSpacingY) + 1;
	const int32 GridSizeZ = FMath::CeilToInt(Radius / LayerSpacingZ) + 1;

	const FQuat RotationQuat = Rotation.Quaternion();
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	const float EstimatedCount = (4.0f / 3.0f) * PI * Radius * Radius * Radius / (AdjustedSpacing * AdjustedSpacing * AdjustedSpacing);
	Particles.Reserve(Particles.Num() + FMath::CeilToInt(EstimatedCount));

	for (int32 z = -GridSizeZ; z <= GridSizeZ; ++z)
	{
		const float ZLayerOffsetX = (((z + GridSizeZ) % 3) == 1) ? AdjustedSpacing * 0.5f : ((((z + GridSizeZ) % 3) == 2) ? AdjustedSpacing * 0.25f : 0.0f);
		const float ZLayerOffsetY = (((z + GridSizeZ) % 3) == 1) ? RowSpacingY / 3.0f : ((((z + GridSizeZ) % 3) == 2) ? RowSpacingY * 2.0f / 3.0f : 0.0f);

		for (int32 y = -GridSizeY; y <= GridSizeY; ++y)
		{
			const float RowOffsetX = (((y + GridSizeY) % 2) == 1) ? AdjustedSpacing * 0.5f : 0.0f;

			for (int32 x = -GridSize; x <= GridSize; ++x)
			{
				FVector LocalPos(
					x * AdjustedSpacing + RowOffsetX + ZLayerOffsetX,
					y * RowSpacingY + ZLayerOffsetY,
					z * LayerSpacingZ
				);

				// Check sphere bounds
				if (LocalPos.SizeSquared() > RadiusSq)
				{
					continue;
				}

				// Apply jitter
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);
				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesCylinderHexagonal(FVector Center, float Radius, float HalfHeight, float Spacing,
                                                                     bool bJitter, float JitterAmount, FVector Velocity,
                                                                     FRotator Rotation)
{
	if (Spacing <= 0.0f || Radius <= 0.0f || HalfHeight <= 0.0f)
	{
		return 0;
	}

	// HCP density compensation: HCP is ~1.42x denser than cubic for the same spacing
	const float HCPCompensation = 1.122f;
	const float AdjustedSpacing = Spacing * HCPCompensation;

	const float RowSpacingY = AdjustedSpacing * 0.866025f;
	const float LayerSpacingZ = AdjustedSpacing * 0.816497f;
	const float JitterRange = AdjustedSpacing * JitterAmount;
	const float RadiusSq = Radius * Radius;

	const int32 GridSizeXY = FMath::CeilToInt(Radius / AdjustedSpacing) + 1;
	const int32 GridSizeY = FMath::CeilToInt(Radius / RowSpacingY) + 1;
	const int32 GridSizeZ = FMath::CeilToInt(HalfHeight / LayerSpacingZ);

	const FQuat RotationQuat = Rotation.Quaternion();
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	const float EstimatedCount = PI * Radius * Radius * HalfHeight * 2.0f / (AdjustedSpacing * AdjustedSpacing * AdjustedSpacing);
	Particles.Reserve(Particles.Num() + FMath::CeilToInt(EstimatedCount));

	for (int32 z = -GridSizeZ; z <= GridSizeZ; ++z)
	{
		const float ZLayerOffsetX = (((z + GridSizeZ) % 3) == 1) ? AdjustedSpacing * 0.5f : ((((z + GridSizeZ) % 3) == 2) ? AdjustedSpacing * 0.25f : 0.0f);
		const float ZLayerOffsetY = (((z + GridSizeZ) % 3) == 1) ? RowSpacingY / 3.0f : ((((z + GridSizeZ) % 3) == 2) ? RowSpacingY * 2.0f / 3.0f : 0.0f);

		for (int32 y = -GridSizeY; y <= GridSizeY; ++y)
		{
			const float RowOffsetX = (((y + GridSizeY) % 2) == 1) ? AdjustedSpacing * 0.5f : 0.0f;

			for (int32 x = -GridSizeXY; x <= GridSizeXY; ++x)
			{
				FVector LocalPos(
					x * AdjustedSpacing + RowOffsetX + ZLayerOffsetX,
					y * RowSpacingY + ZLayerOffsetY,
					z * LayerSpacingZ
				);

				// Check cylinder XY bounds
				const float DistSqXY = LocalPos.X * LocalPos.X + LocalPos.Y * LocalPos.Y;
				if (DistSqXY > RadiusSq)
				{
					continue;
				}

				// Check Z bounds
				if (FMath::Abs(LocalPos.Z) > HalfHeight)
				{
					continue;
				}

				// Apply jitter
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);
				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticleDirectional(FVector Position, FVector Direction, float Speed,
                                                             float Radius, float ConeAngle)
{
	// 방향 정규화
	FVector Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = FVector(0, 0, -1);  // 기본: 아래 방향
	}

	FVector SpawnPos = Position;
	FVector SpawnVel = Dir * Speed;

	// 스트림 반경 적용 (위치 분산)
	if (Radius > 0.0f)
	{
		// 방향에 수직인 평면에서 랜덤 오프셋
		FVector Right, Up;
		Dir.FindBestAxisVectors(Right, Up);

		const float RandomAngle = FMath::FRandRange(0.0f, 2.0f * PI);
		const float RandomRadius = FMath::FRandRange(0.0f, Radius);

		SpawnPos += Right * FMath::Cos(RandomAngle) * RandomRadius;
		SpawnPos += Up * FMath::Sin(RandomAngle) * RandomRadius;
	}

	// 원뿔 분사각 적용 (속도 방향 분산)
	if (ConeAngle > 0.0f)
	{
		// 분사각 범위 내에서 랜덤 방향
		const float HalfAngleRad = FMath::DegreesToRadians(ConeAngle * 0.5f);
		const float RandomPhi = FMath::FRandRange(0.0f, 2.0f * PI);
		const float RandomTheta = FMath::FRandRange(0.0f, HalfAngleRad);

		// 로컬 좌표계에서 랜덤 방향 생성
		FVector Right, Up;
		Dir.FindBestAxisVectors(Right, Up);

		const float SinTheta = FMath::Sin(RandomTheta);
		FVector RandomDir = Dir * FMath::Cos(RandomTheta)
		                  + Right * SinTheta * FMath::Cos(RandomPhi)
		                  + Up * SinTheta * FMath::Sin(RandomPhi);

		SpawnVel = RandomDir.GetSafeNormal() * Speed;
	}

	return SpawnParticle(SpawnPos, SpawnVel);
}

int32 UKawaiiFluidSimulationModule::SpawnParticleDirectionalHexLayer(FVector Position, FVector Direction, float Speed,
                                                                      float Radius, float Spacing, float Jitter)
{
	TArray<FGPUSpawnRequest> BatchRequests;
	int32 SpawnedCount = SpawnParticleDirectionalHexLayerBatch(Position, Direction, Speed, Radius, Spacing, Jitter, BatchRequests);

	// Send batch requests
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (BatchRequests.Num() > 0 && GPUSim)
	{
		GPUSim->AddSpawnRequests(BatchRequests);
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticleDirectionalHexLayerBatch(FVector Position, FVector Direction, float Speed,
                                                                          float Radius, float Spacing, float Jitter,
                                                                          TArray<FGPUSpawnRequest>& OutBatch)
{
	// 방향 정규화
	FVector Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = FVector(0, 0, -1);  // 기본: 아래 방향
	}

	// 자동 간격 계산 (SmoothingRadius * 0.5)
	if (Spacing <= 0.0f)
	{
		Spacing = Preset ? Preset->SmoothingRadius * 0.5f : 10.0f;
	}

	// Jitter 범위 제한 (0 ~ 0.5)
	Jitter = FMath::Clamp(Jitter, 0.0f, 0.5f);
	const float MaxJitterOffset = Spacing * Jitter;
	const bool bApplyJitter = Jitter > KINDA_SMALL_NUMBER;

	// 방향에 수직인 로컬 좌표계 생성
	FVector Right, Up;
	Dir.FindBestAxisVectors(Right, Up);

	// Hexagonal Packing 상수
	const float RowSpacing = Spacing * FMath::Sqrt(3.0f) * 0.5f;  // ~0.866 * Spacing
	const float RadiusSq = Radius * Radius;

	// 행 개수 계산
	const int32 NumRows = FMath::CeilToInt(Radius / RowSpacing) * 2 + 1;
	const int32 HalfRows = NumRows / 2;

	int32 SpawnedCount = 0;
	const FVector SpawnVel = Dir * Speed;

	const float ParticleMass = Preset ? Preset->ParticleMass : 1.0f;
	const float ParticleRadius = Preset ? Preset->ParticleRadius : 5.0f;

	// Hexagonal grid 순회
	for (int32 RowIdx = -HalfRows; RowIdx <= HalfRows; ++RowIdx)
	{
		const float LocalY = RowIdx * RowSpacing;
		const float LocalYSq = LocalY * LocalY;

		// 이 행에서 원 내부의 최대 X 범위 계산
		if (LocalYSq > RadiusSq)
		{
			continue;  // 원 밖의 행
		}

		const float MaxX = FMath::Sqrt(RadiusSq - LocalYSq);

		// 홀수 행은 X 오프셋 적용 (Hexagonal Packing)
		const float XOffset = (FMath::Abs(RowIdx) % 2 != 0) ? Spacing * 0.5f : 0.0f;

		// X 시작점 계산 (중심 기준 대칭)
		const int32 NumCols = FMath::FloorToInt(MaxX / Spacing);

		for (int32 ColIdx = -NumCols; ColIdx <= NumCols; ++ColIdx)
		{
			float LocalX = ColIdx * Spacing + XOffset;
			float LocalYFinal = LocalY;

			// Jitter 적용: 랜덤 오프셋 추가
			if (bApplyJitter)
			{
				LocalX += FMath::FRandRange(-MaxJitterOffset, MaxJitterOffset);
				LocalYFinal += FMath::FRandRange(-MaxJitterOffset, MaxJitterOffset);
			}

			// 원 내부 체크 (jitter 후에도 원 내부에 있는지 확인)
			if (LocalX * LocalX + LocalYFinal * LocalYFinal <= RadiusSq)
			{
				FVector SpawnPos = Position + Right * LocalX + Up * LocalYFinal;
				
				// Create request and add to batch
				FGPUSpawnRequest Request;
				Request.Position = FVector3f(SpawnPos);
				Request.Velocity = FVector3f(SpawnVel);
				Request.Mass = ParticleMass;
				Request.Radius = ParticleRadius;
				Request.SourceID = CachedSourceID;

				OutBatch.Add(Request);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

void UKawaiiFluidSimulationModule::ClearAllParticles()
{
	Particles.Empty();

	// GPU 파티클 클리어 (이 Module의 SourceID 파티클만)
	// TWeakPtr::Pin()으로 안전하게 접근 - GPUSimulator가 이미 파괴되었으면 nullptr 반환
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (bGPUSimulationActive && GPUSim)
	{
		// 캐시된 SourceID별 ParticleIDs 참조 (복사 없음, O(1) lookup)
		const TArray<int32>* MyParticleIDsPtr = GPUSim->GetParticleIDsBySourceID(CachedSourceID);
		if (MyParticleIDsPtr && MyParticleIDsPtr->Num() > 0)
		{
			// 내 파티클들 Despawn 요청 (CleanupCompletedRequests는 Readback 시 호출됨)
			GPUSim->AddDespawnByIDRequests(*MyParticleIDsPtr);
			UE_LOG(LogTemp, Log, TEXT("ClearAllParticles: SourceID=%d, Clearing %d particles"),
				CachedSourceID, MyParticleIDsPtr->Num());
		}
	}
}

int32 UKawaiiFluidSimulationModule::RemoveOldestParticles(int32 Count)
{
	if (Count <= 0)
	{
		return 0;
	}

	// GPU 모드
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (bGPUSimulationActive && GPUSim)
	{
		// 캐시된 SourceID별 ParticleIDs 참조 (이미 ParticleID로 정렬됨)
		const TArray<int32>* MyParticleIDsPtr = GPUSim->GetParticleIDsBySourceID(CachedSourceID);
		if (!MyParticleIDsPtr || MyParticleIDsPtr->Num() == 0)
		{
			return 0;
		}

		const int32 MyCount = MyParticleIDsPtr->Num();
		const int32 RemoveCount = FMath::Min(Count, MyCount);

		// GPU에서 ParticleID로 정렬된 상태로 readback됨 - 앞쪽 N개가 가장 오래된 파티클 O(1)
		TArray<int32> IDsToRemove;
		IDsToRemove.SetNumUninitialized(RemoveCount);
		FMemory::Memcpy(IDsToRemove.GetData(), MyParticleIDsPtr->GetData(), RemoveCount * sizeof(int32));

		GPUSim->AddDespawnByIDRequests(IDsToRemove);

		UE_LOG(LogTemp, Log, TEXT("RemoveOldestParticles: SourceID=%d, Removing %d (IDs: %d~%d), Total=%d"),
			CachedSourceID, RemoveCount, IDsToRemove[0], IDsToRemove.Last(), MyCount);

		return RemoveCount;
	}

	return 0;
}

int32 UKawaiiFluidSimulationModule::RemoveOldestParticlesForSource(int32 SourceID, int32 Count)
{
	if (Count <= 0 || SourceID < 0)
	{
		return 0;
	}

	// GPU 모드
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (bGPUSimulationActive && GPUSim)
	{
		// 지정된 SourceID의 ParticleIDs 참조 (이미 ParticleID로 정렬됨)
		const TArray<int32>* ParticleIDsPtr = GPUSim->GetParticleIDsBySourceID(SourceID);
		if (!ParticleIDsPtr || ParticleIDsPtr->Num() == 0)
		{
			return 0;
		}

		const int32 SourceCount = ParticleIDsPtr->Num();
		const int32 RemoveCount = FMath::Min(Count, SourceCount);

		// GPU에서 ParticleID로 정렬된 상태로 readback됨 - 앞쪽 N개가 가장 오래된 파티클 O(1)
		TArray<int32> IDsToRemove;
		IDsToRemove.SetNumUninitialized(RemoveCount);
		FMemory::Memcpy(IDsToRemove.GetData(), ParticleIDsPtr->GetData(), RemoveCount * sizeof(int32));

		GPUSim->AddDespawnByIDRequests(IDsToRemove);

		UE_LOG(LogTemp, Log, TEXT("RemoveOldestParticlesForSource: SourceID=%d, Removing %d (IDs: %d~%d), Total=%d"),
			SourceID, RemoveCount, IDsToRemove[0], IDsToRemove.Last(), SourceCount);

		return RemoveCount;
	}

	return 0;
}

TArray<FVector> UKawaiiFluidSimulationModule::GetParticlePositions() const
{
	TArray<FVector> Positions;
	Positions.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Positions.Add(Particle.Position);
	}

	return Positions;
}

TArray<FVector> UKawaiiFluidSimulationModule::GetParticleVelocities() const
{
	TArray<FVector> Velocities;
	Velocities.Reserve(Particles.Num());

	for (const FFluidParticle& Particle : Particles)
	{
		Velocities.Add(Particle.Velocity);
	}

	return Velocities;
}

void UKawaiiFluidSimulationModule::ApplyExternalForce(FVector Force)
{
	AccumulatedExternalForce += Force;
}

void UKawaiiFluidSimulationModule::ApplyForceToParticle(int32 ParticleIndex, FVector Force)
{
	if (Particles.IsValidIndex(ParticleIndex))
	{
		Particles[ParticleIndex].Velocity += Force;
	}
}

void UKawaiiFluidSimulationModule::RegisterCollider(UFluidCollider* Collider)
{
	if (Collider && !Colliders.Contains(Collider))
	{
		Colliders.Add(Collider);
	}
}

void UKawaiiFluidSimulationModule::UnregisterCollider(UFluidCollider* Collider)
{
	Colliders.Remove(Collider);
}

TArray<int32> UKawaiiFluidSimulationModule::GetParticlesInRadius(FVector Location, float Radius) const
{
	TArray<int32> Result;
	const float RadiusSq = Radius * Radius;

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		const float DistSq = FVector::DistSquared(Particles[i].Position, Location);
		if (DistSq <= RadiusSq)
		{
			Result.Add(i);
		}
	}

	return Result;
}

TArray<int32> UKawaiiFluidSimulationModule::GetParticlesInBox(FVector Center, FVector Extent) const
{
	TArray<int32> Result;
	const FBox Box(Center - Extent, Center + Extent);

	for (int32 i = 0; i < Particles.Num(); ++i)
	{
		if (Box.IsInside(Particles[i].Position))
		{
			Result.Add(i);
		}
	}

	return Result;
}

bool UKawaiiFluidSimulationModule::GetParticleInfo(int32 ParticleIndex, FVector& OutPosition, FVector& OutVelocity, float& OutDensity) const
{
	if (!Particles.IsValidIndex(ParticleIndex))
	{
		return false;
	}

	const FFluidParticle& Particle = Particles[ParticleIndex];
	OutPosition = Particle.Position;
	OutVelocity = Particle.Velocity;
	OutDensity = Particle.Density;

	return true;
}

//========================================
// IKawaiiFluidDataProvider Interface
//========================================

float UKawaiiFluidSimulationModule::GetParticleRadius() const
{
	if (Preset)
	{
		return Preset->ParticleRadius;
	}

	return 10.0f; // 기본값
}

FString UKawaiiFluidSimulationModule::GetDebugName() const
{
	AActor* Owner = GetOwnerActor();
	return FString::Printf(TEXT("SimulationModule_%s"),
		Owner ? *Owner->GetName() : TEXT("NoOwner"));
}

//========================================
// GPU Buffer Access (Phase 2)
//========================================

bool UKawaiiFluidSimulationModule::IsGPUSimulationActive() const
{
	return bGPUSimulationActive && WeakGPUSimulator.IsValid();
}

int32 UKawaiiFluidSimulationModule::GetGPUParticleCount() const
{
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (GPUSim && GPUSim->IsReady())
	{
		return GPUSim->GetParticleCount();
	}
	return 0;
}

int32 UKawaiiFluidSimulationModule::GetParticleCountForSource(int32 SourceID) const
{
	TSharedPtr<FGPUFluidSimulator> GPUSim = WeakGPUSimulator.Pin();
	if (GPUSim && GPUSim->IsReady())
	{
		FGPUSpawnManager* SpawnManager = GPUSim->GetSpawnManager();
		if (SpawnManager)
		{
			return SpawnManager->GetParticleCountForSource(SourceID);
		}
	}
	return -1;
}

//========================================
// 명시적 개수 지정 스폰 함수
//========================================

int32 UKawaiiFluidSimulationModule::SpawnParticlesSphereByCount(FVector Center, float Radius, int32 Count,
                                                                bool bJitter, float JitterAmount, FVector Velocity,
                                                                FRotator Rotation)
{
	if (Count <= 0 || Radius <= 0.0f)
	{
		return 0;
	}

	// 개수에서 간격 역계산: 구 부피 = (4/3)πr³, 파티클당 부피 = spacing³
	// Count = Volume / spacing³ → spacing = (Volume / Count)^(1/3)
	const float Volume = (4.0f / 3.0f) * PI * Radius * Radius * Radius;
	const float Spacing = FMath::Pow(Volume / Count, 1.0f / 3.0f);

	// 실제 격자 기반으로 스폰 (Spacing 기반 함수와 동일한 결과를 위해)
	const int32 GridSize = FMath::CeilToInt(Radius / Spacing);
	const float RadiusSq = Radius * Radius;
	const float JitterRange = Spacing * JitterAmount;

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용 (구형은 위치 회전 불필요)
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	Particles.Reserve(Particles.Num() + Count);

	// 격자 순회
	for (int32 x = -GridSize; x <= GridSize && SpawnedCount < Count; ++x)
	{
		for (int32 y = -GridSize; y <= GridSize && SpawnedCount < Count; ++y)
		{
			for (int32 z = -GridSize; z <= GridSize && SpawnedCount < Count; ++z)
			{
				FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

				// 구 내부인지 확인
				if (LocalPos.SizeSquared() <= RadiusSq)
				{
					FVector SpawnPos = Center + LocalPos;

					// Jitter 적용
					if (bJitter && JitterRange > 0.0f)
					{
						SpawnPos += FVector(
							FMath::FRandRange(-JitterRange, JitterRange),
							FMath::FRandRange(-JitterRange, JitterRange),
							FMath::FRandRange(-JitterRange, JitterRange)
						);
					}

					SpawnParticle(SpawnPos, WorldVelocity);
					++SpawnedCount;
				}
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesBoxByCount(FVector Center, FVector Extent, int32 Count,
                                                             bool bJitter, float JitterAmount, FVector Velocity,
                                                             FRotator Rotation)
{
	if (Count <= 0)
	{
		return 0;
	}

	// 개수에서 간격 역계산: 박스 부피 = 8 * Extent.X * Extent.Y * Extent.Z
	const float Volume = 8.0f * Extent.X * Extent.Y * Extent.Z;

	if (Volume <= 0.0f)
	{
		return 0;
	}

	// 균등 분포를 위해 각 축의 비율 유지
	// n = nx * ny * nz, 비율 유지하면 nx:ny:nz = Ex:Ey:Ez
	// spacing = (Volume / Count)^(1/3)
	const float Spacing = FMath::Pow(Volume / Count, 1.0f / 3.0f);

	const int32 CountX = FMath::Max(1, FMath::RoundToInt(Extent.X * 2.0f / Spacing));
	const int32 CountY = FMath::Max(1, FMath::RoundToInt(Extent.Y * 2.0f / Spacing));
	const int32 CountZ = FMath::Max(1, FMath::RoundToInt(Extent.Z * 2.0f / Spacing));

	const float JitterRange = Spacing * JitterAmount;
	const FVector LocalStartOffset = -Extent + FVector(Spacing * 0.5f);

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	Particles.Reserve(Particles.Num() + Count);

	for (int32 x = 0; x < CountX && SpawnedCount < Count; ++x)
	{
		for (int32 y = 0; y < CountY && SpawnedCount < Count; ++y)
		{
			for (int32 z = 0; z < CountZ && SpawnedCount < Count; ++z)
			{
				// 로컬 위치 계산
				FVector LocalPos = LocalStartOffset + FVector(x * Spacing, y * Spacing, z * Spacing);

				// Jitter 적용 (로컬 공간에서)
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				// 회전 적용 후 월드 위치 계산
				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);

				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

int32 UKawaiiFluidSimulationModule::SpawnParticlesCylinderByCount(FVector Center, float Radius, float HalfHeight, int32 Count,
                                                                   bool bJitter, float JitterAmount, FVector Velocity,
                                                                   FRotator Rotation)
{
	if (Count <= 0 || Radius <= 0.0f || HalfHeight <= 0.0f)
	{
		return 0;
	}

	// 개수에서 간격 역계산: 원기둥 부피 = π * r² * 2h
	const float Volume = PI * Radius * Radius * HalfHeight * 2.0f;
	const float Spacing = FMath::Pow(Volume / Count, 1.0f / 3.0f);

	const int32 GridSizeXY = FMath::CeilToInt(Radius / Spacing);
	const int32 GridSizeZ = FMath::CeilToInt(HalfHeight / Spacing);
	const float RadiusSq = Radius * Radius;
	const float JitterRange = Spacing * JitterAmount;

	// FRotator -> FQuat 변환
	const FQuat RotationQuat = Rotation.Quaternion();

	// 속도에 회전 적용
	const FVector WorldVelocity = RotationQuat.RotateVector(Velocity);

	int32 SpawnedCount = 0;
	Particles.Reserve(Particles.Num() + Count);

	// 격자 순회
	for (int32 x = -GridSizeXY; x <= GridSizeXY && SpawnedCount < Count; ++x)
	{
		for (int32 y = -GridSizeXY; y <= GridSizeXY && SpawnedCount < Count; ++y)
		{
			// XY 평면에서 원 내부인지 확인
			const float DistSqXY = x * x * Spacing * Spacing + y * y * Spacing * Spacing;
			if (DistSqXY > RadiusSq)
			{
				continue;
			}

			for (int32 z = -GridSizeZ; z <= GridSizeZ && SpawnedCount < Count; ++z)
			{
				FVector LocalPos(x * Spacing, y * Spacing, z * Spacing);

				// Jitter 적용 (로컬 공간에서)
				if (bJitter && JitterRange > 0.0f)
				{
					LocalPos += FVector(
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange),
						FMath::FRandRange(-JitterRange, JitterRange)
					);
				}

				// 회전 적용 후 월드 위치 계산
				const FVector WorldPos = Center + RotationQuat.RotateVector(LocalPos);

				SpawnParticle(WorldPos, WorldVelocity);
				++SpawnedCount;
			}
		}
	}

	return SpawnedCount;
}

//========================================
// Simulation Bounds API
//========================================

void UKawaiiFluidSimulationModule::SetSimulationVolume(const FVector& Size, const FRotator& Rotation, float Bounce, float Friction)
{
	// Size is full size, convert to half-extent for internal storage
	const FVector HalfExtent = Size * 0.5f;

	// Clamp half-extent to maximum supported by Large preset
	const float EffectiveCellSize = FMath::Max(CellSize, 1.0f);
	const FVector ClampedHalfExtent = GridResolutionPresetHelper::ClampExtentToMaxSupported(HalfExtent, EffectiveCellSize);
	const FVector ClampedFullSize = ClampedHalfExtent * 2.0f;

	// Update the appropriate size property based on current bUniformSize setting
	// Note: bUniformSize is user-controlled from editor, don't modify it here
	if (bUniformSize)
	{
		UniformVolumeSize = FMath::Max3(ClampedFullSize.X, ClampedFullSize.Y, ClampedFullSize.Z);
	}
	else
	{
		VolumeSize = ClampedFullSize;
	}

	VolumeRotation = Rotation;
	// Note: Bounce/Friction parameters are ignored. Use Preset's Restitution/Friction instead.

	// Recalculate bounds (auto-selects Z-Order preset)
	RecalculateVolumeBounds();
}

// Legacy API - deprecated
void UKawaiiFluidSimulationModule::SetContainment(bool bEnabled, const FVector& Center, const FVector& Extent,
                                                   const FQuat& Rotation, float Restitution, float Friction)
{
	// Map to new unified API (ignoring bEnabled and Center since containment is always enabled)
	// Extent is half-extent in legacy API, convert to full size
	const float EffectiveCellSize = FMath::Max(CellSize, 1.0f);
	const FVector ClampedHalfExtent = GridResolutionPresetHelper::ClampExtentToMaxSupported(Extent, EffectiveCellSize);
	const FVector ClampedFullSize = ClampedHalfExtent * 2.0f;

	// Update the appropriate size property based on current bUniformSize setting
	// Note: bUniformSize is user-controlled from editor, don't modify it here
	if (bUniformSize)
	{
		UniformVolumeSize = FMath::Max3(ClampedFullSize.X, ClampedFullSize.Y, ClampedFullSize.Z);
	}
	else
	{
		VolumeSize = ClampedFullSize;
	}

	VolumeRotation = Rotation.Rotator();
	VolumeCenter = Center;  // Set center directly
	VolumeRotationQuat = Rotation;
	// Note: Restitution/Friction parameters are ignored. Use Preset's Restitution/Friction instead.
}

void UKawaiiFluidSimulationModule::ResolveVolumeBoundaryCollisions()
{
	// Determine bounds parameters based on whether using external or internal volume
	FVector EffectiveCenter;
	FVector EffectiveHalfExtent;
	FQuat EffectiveRotation;
	float EffectiveBounce;
	float EffectiveFriction;

	// Get bounce/friction from Preset
	const float PresetBounce = Preset ? Preset->Bounciness : 0.0f;
	const float PresetFriction = Preset ? Preset->Friction : 0.5f;

	if (UKawaiiFluidVolumeComponent* ExternalVolume = GetTargetVolumeComponent())
	{
		if (IsUsingExternalVolume())
		{
			// Use external volume's parameters
			EffectiveCenter = ExternalVolume->GetComponentLocation();
			EffectiveHalfExtent = ExternalVolume->GetVolumeHalfExtent();
			EffectiveRotation = FQuat::Identity;  // External volumes are axis-aligned
			EffectiveBounce = ExternalVolume->GetWallBounce();
			EffectiveFriction = ExternalVolume->GetWallFriction();
		}
		else
		{
			// Use internal volume parameters
			EffectiveCenter = VolumeCenter;
			EffectiveHalfExtent = GridResolutionPresetHelper::ClampExtentToMaxSupported(GetVolumeHalfExtent(), CellSize);
			EffectiveRotation = VolumeRotationQuat;
			EffectiveBounce = PresetBounce;
			EffectiveFriction = PresetFriction;
		}
	}
	else
	{
		// Fallback to internal parameters
		EffectiveCenter = VolumeCenter;
		EffectiveHalfExtent = GridResolutionPresetHelper::ClampExtentToMaxSupported(GetVolumeHalfExtent(), CellSize);
		EffectiveRotation = VolumeRotationQuat;
		EffectiveBounce = PresetBounce;
		EffectiveFriction = PresetFriction;
	}

	// OBB (Oriented Bounding Box) 충돌 처리
	// 파티클을 로컬 공간으로 변환하여 AABB 충돌 체크 후 다시 월드로 변환
	const FQuat InverseRotation = EffectiveRotation.Inverse();
	const FVector LocalBoxMin = -EffectiveHalfExtent;
	const FVector LocalBoxMax = EffectiveHalfExtent;

	for (FFluidParticle& P : Particles)
	{
		// 월드 -> 로컬 변환
		FVector LocalPos = InverseRotation.RotateVector(P.PredictedPosition - EffectiveCenter);
		FVector LocalVel = InverseRotation.RotateVector(P.Velocity);
		bool bCollided = false;

		// 로컬 공간에서 AABB 충돌 체크
		// X축 체크
		if (LocalPos.X < LocalBoxMin.X)
		{
			LocalPos.X = LocalBoxMin.X;
			if (LocalVel.X < 0.0f)
			{
				LocalVel.X = -LocalVel.X * EffectiveBounce;
			}
			LocalVel.Y *= (1.0f - EffectiveFriction);
			LocalVel.Z *= (1.0f - EffectiveFriction);
			bCollided = true;
		}
		else if (LocalPos.X > LocalBoxMax.X)
		{
			LocalPos.X = LocalBoxMax.X;
			if (LocalVel.X > 0.0f)
			{
				LocalVel.X = -LocalVel.X * EffectiveBounce;
			}
			LocalVel.Y *= (1.0f - EffectiveFriction);
			LocalVel.Z *= (1.0f - EffectiveFriction);
			bCollided = true;
		}

		// Y축 체크
		if (LocalPos.Y < LocalBoxMin.Y)
		{
			LocalPos.Y = LocalBoxMin.Y;
			if (LocalVel.Y < 0.0f)
			{
				LocalVel.Y = -LocalVel.Y * EffectiveBounce;
			}
			LocalVel.X *= (1.0f - EffectiveFriction);
			LocalVel.Z *= (1.0f - EffectiveFriction);
			bCollided = true;
		}
		else if (LocalPos.Y > LocalBoxMax.Y)
		{
			LocalPos.Y = LocalBoxMax.Y;
			if (LocalVel.Y > 0.0f)
			{
				LocalVel.Y = -LocalVel.Y * EffectiveBounce;
			}
			LocalVel.X *= (1.0f - EffectiveFriction);
			LocalVel.Z *= (1.0f - EffectiveFriction);
			bCollided = true;
		}

		// Z축 체크
		if (LocalPos.Z < LocalBoxMin.Z)
		{
			LocalPos.Z = LocalBoxMin.Z;
			if (LocalVel.Z < 0.0f)
			{
				LocalVel.Z = -LocalVel.Z * EffectiveBounce;
			}
			LocalVel.X *= (1.0f - EffectiveFriction);
			LocalVel.Y *= (1.0f - EffectiveFriction);
			bCollided = true;
		}
		else if (LocalPos.Z > LocalBoxMax.Z)
		{
			LocalPos.Z = LocalBoxMax.Z;
			if (LocalVel.Z > 0.0f)
			{
				LocalVel.Z = -LocalVel.Z * EffectiveBounce;
			}
			LocalVel.X *= (1.0f - EffectiveFriction);
			LocalVel.Y *= (1.0f - EffectiveFriction);
			bCollided = true;
		}

		// 로컬 -> 월드 변환하여 Position/Velocity 업데이트
		if (bCollided)
		{
			P.PredictedPosition = EffectiveCenter + EffectiveRotation.RotateVector(LocalPos);
			P.Position = P.PredictedPosition;
			P.Velocity = EffectiveRotation.RotateVector(LocalVel);
		}
	}
}

// Legacy - redirects to new function
void UKawaiiFluidSimulationModule::ResolveContainmentCollisions()
{
	ResolveVolumeBoundaryCollisions();
}

//========================================
// Source Identification
//========================================

void UKawaiiFluidSimulationModule::SetSourceID(int32 InSourceID)
{
	CachedSourceID = InSourceID;
	UE_LOG(LogTemp, Log, TEXT("SimulationModule::SetSourceID = %d"), CachedSourceID);
}

//========================================
// Simulation Volume
//========================================

UKawaiiFluidVolumeComponent* UKawaiiFluidSimulationModule::GetTargetVolumeComponent() const
{
	// If external volume actor is set, use its component
	if (TargetSimulationVolume)
	{
		return TargetSimulationVolume->GetVolumeComponent();
	}

	// Otherwise return internal owned volume component
	return OwnedVolumeComponent;
}

void UKawaiiFluidSimulationModule::SetTargetSimulationVolume(AKawaiiFluidVolume* NewSimulationVolume)
{
	if (TargetSimulationVolume == NewSimulationVolume)
	{
		return;
	}

	// Unbind from old volume's OnDestroyed event
	UnbindFromVolumeDestroyedEvent();

	// Unregister from old volume
	if (TargetSimulationVolume)
	{
		if (UKawaiiFluidVolumeComponent* OldVolume = TargetSimulationVolume->GetVolumeComponent())
		{
			OldVolume->UnregisterModule(this);
		}
	}

	TargetSimulationVolume = NewSimulationVolume;

	// Register to new volume and update tracking
	if (TargetSimulationVolume)
	{
		if (UKawaiiFluidVolumeComponent* NewVolume = TargetSimulationVolume->GetVolumeComponent())
		{
			NewVolume->RegisterModule(this);
			PreviousRegisteredVolume = NewVolume;
		}
		// Bind to new volume's OnDestroyed event
		BindToVolumeDestroyedEvent();
	}
	else
	{
		PreviousRegisteredVolume = nullptr;
	}

	// Update volume info display (handles CellSize derivation)
	UpdateVolumeInfoDisplay();
}

void UKawaiiFluidSimulationModule::RecalculateVolumeBounds()
{
	// Ensure valid CellSize
	CellSize = FMath::Max(CellSize, 1.0f);

	// Get the maximum half-extent supported by Large preset
	const float LargeMaxHalfExtent = GridResolutionPresetHelper::GetMaxExtentForPreset(EGridResolutionPreset::Large, CellSize);

	// Get current half-extent from VolumeSize (full size / 2)
	const FVector OriginalHalfExtent = GetVolumeHalfExtent();

	// First pass: Clamp half-extent to Large max (without rotation)
	FVector WorkingHalfExtent = GridResolutionPresetHelper::ClampExtentToMaxSupported(OriginalHalfExtent, CellSize);

	// Compute rotation quaternion
	VolumeRotationQuat = VolumeRotation.Quaternion();

	// Calculate the AABB extent for the rotated OBB
	FVector EffectiveHalfExtent = WorkingHalfExtent;

	// Helper lambda to compute AABB half-extent from OBB half-extent and rotation
	auto ComputeRotatedAABBHalfExtent = [this](const FVector& OBBHalfExtent) -> FVector
	{
		if (VolumeRotationQuat.Equals(FQuat::Identity))
		{
			return OBBHalfExtent;
		}

		FVector RotatedCorners[8];
		for (int32 i = 0; i < 8; ++i)
		{
			FVector Corner(
				(i & 1) ? OBBHalfExtent.X : -OBBHalfExtent.X,
				(i & 2) ? OBBHalfExtent.Y : -OBBHalfExtent.Y,
				(i & 4) ? OBBHalfExtent.Z : -OBBHalfExtent.Z
			);
			RotatedCorners[i] = VolumeRotationQuat.RotateVector(Corner);
		}

		FVector AABBMin = RotatedCorners[0];
		FVector AABBMax = RotatedCorners[0];
		for (int32 i = 1; i < 8; ++i)
		{
			AABBMin = AABBMin.ComponentMin(RotatedCorners[i]);
			AABBMax = AABBMax.ComponentMax(RotatedCorners[i]);
		}

		return FVector(
			FMath::Max(FMath::Abs(AABBMin.X), FMath::Abs(AABBMax.X)),
			FMath::Max(FMath::Abs(AABBMin.Y), FMath::Abs(AABBMax.Y)),
			FMath::Max(FMath::Abs(AABBMin.Z), FMath::Abs(AABBMax.Z))
		);
	};

	if (!VolumeRotationQuat.Equals(FQuat::Identity))
	{
		// Compute rotated AABB
		EffectiveHalfExtent = ComputeRotatedAABBHalfExtent(WorkingHalfExtent);

		// Check if rotated AABB exceeds Large preset limits
		const float MaxAABBHalfExtent = FMath::Max3(EffectiveHalfExtent.X, EffectiveHalfExtent.Y, EffectiveHalfExtent.Z);
		if (MaxAABBHalfExtent > LargeMaxHalfExtent)
		{
			// Scale down the original extent proportionally so rotated AABB fits within Large
			const float ScaleFactor = LargeMaxHalfExtent / MaxAABBHalfExtent;
			WorkingHalfExtent = WorkingHalfExtent * ScaleFactor;

			// Recompute rotated AABB with scaled extent
			EffectiveHalfExtent = ComputeRotatedAABBHalfExtent(WorkingHalfExtent);
		}
	}

	// Apply final extent if different from original (update VolumeSize/UniformVolumeSize)
	if (!WorkingHalfExtent.Equals(OriginalHalfExtent, 0.01f))
	{
		const bool bWasRotated = !VolumeRotationQuat.Equals(FQuat::Identity);
		const FVector OriginalSize = OriginalHalfExtent * 2.0f;
		const FVector NewSize = WorkingHalfExtent * 2.0f;

		if (bWasRotated)
		{
			const float RotatedAABB = FMath::Max3(ComputeRotatedAABBHalfExtent(OriginalHalfExtent).X,
			                                       ComputeRotatedAABBHalfExtent(OriginalHalfExtent).Y,
			                                       ComputeRotatedAABBHalfExtent(OriginalHalfExtent).Z);
			UE_LOG(LogTemp, Warning, TEXT("VolumeSize adjusted: Rotated AABB (%.1f cm) exceeds limit (%.1f cm). Size scaled from (%s) to (%s)"),
				RotatedAABB * 2.0f, LargeMaxHalfExtent * 2.0f, *OriginalSize.ToString(), *NewSize.ToString());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("VolumeSize exceeds limit (%.1f cm per axis). Clamped from (%s) to (%s)"),
				LargeMaxHalfExtent * 2.0f, *OriginalSize.ToString(), *NewSize.ToString());
		}

		// Update the stored size values
		VolumeSize = NewSize;
		if (bUniformSize)
		{
			UniformVolumeSize = FMath::Max3(NewSize.X, NewSize.Y, NewSize.Z);
		}
	}

	// Auto-select the smallest Z-Order preset that can contain the volume
	GridResolutionPreset = GridResolutionPresetHelper::SelectPresetForExtent(EffectiveHalfExtent, CellSize);

	// Update grid parameters from auto-selected preset
	GridAxisBits = GridResolutionPresetHelper::GetAxisBits(GridResolutionPreset);
	GridResolution = GridResolutionPresetHelper::GetGridResolution(GridResolutionPreset);
	MaxCells = GridResolutionPresetHelper::GetMaxCells(GridResolutionPreset);

	// Calculate Z-Order bounds extent from grid resolution and cell size
	BoundsExtent = static_cast<float>(GridResolution) * CellSize;

	// Get owner location for bounds center
	FVector OwnerLocation = FVector::ZeroVector;
	if (AActor* Owner = GetOwnerActor())
	{
		OwnerLocation = Owner->GetActorLocation();
	}

	// Update volume center
	VolumeCenter = OwnerLocation;

	// Calculate world bounds (centered on owner) - this is the Z-Order space bounds
	const float HalfExtent = BoundsExtent * 0.5f;
	WorldBoundsMin = OwnerLocation - FVector(HalfExtent, HalfExtent, HalfExtent);
	WorldBoundsMax = OwnerLocation + FVector(HalfExtent, HalfExtent, HalfExtent);

	// Update owned volume component if exists
	// Sync all bounds since OwnedVolumeComponent doesn't have a proper transform
	if (OwnedVolumeComponent)
	{
		OwnedVolumeComponent->GridResolutionPreset = GridResolutionPreset;
		OwnedVolumeComponent->GridAxisBits = GridAxisBits;
		OwnedVolumeComponent->GridResolution = GridResolution;
		OwnedVolumeComponent->MaxCells = MaxCells;
		OwnedVolumeComponent->CellSize = CellSize;
		OwnedVolumeComponent->BoundsExtent = BoundsExtent;
		OwnedVolumeComponent->WorldBoundsMin = WorldBoundsMin;
		OwnedVolumeComponent->WorldBoundsMax = WorldBoundsMax;
	}
}

void UKawaiiFluidSimulationModule::UpdateVolumeInfoDisplay()
{
	if (TargetSimulationVolume)
	{
		// Read info from external volume
		if (UKawaiiFluidVolumeComponent* ExternalVolume = TargetSimulationVolume->GetVolumeComponent())
		{
			// Copy all info from external volume (preset is controlled by external)
			GridResolutionPreset = ExternalVolume->GridResolutionPreset;
			GridAxisBits = ExternalVolume->GridAxisBits;
			GridResolution = ExternalVolume->GridResolution;
			MaxCells = ExternalVolume->MaxCells;
			BoundsExtent = ExternalVolume->BoundsExtent;
			WorldBoundsMin = ExternalVolume->GetWorldBoundsMin();
			WorldBoundsMax = ExternalVolume->GetWorldBoundsMax();
			CellSize = ExternalVolume->CellSize;

			// Note: Bounce/Friction are now obtained from Preset directly (no longer copied from volume)

			// Copy size parameters for consistency (read-only display when using external)
			bUniformSize = ExternalVolume->bUniformSize;
			UniformVolumeSize = ExternalVolume->UniformVolumeSize;
			VolumeSize = ExternalVolume->VolumeSize;
		}
	}
	else
	{
		// Derive CellSize from Preset's SmoothingRadius
		if (Preset && Preset->SmoothingRadius > 0.0f)
		{
			CellSize = Preset->SmoothingRadius;

			// Always recalculate volume size based on Medium preset
			// This ensures the default grid preset is Medium for the given CellSize
			const float MediumGridResolution = static_cast<float>(GridResolutionPresetHelper::GetGridResolution(EGridResolutionPreset::Medium));
			const float NewDefaultSize = MediumGridResolution * CellSize;
			UniformVolumeSize = NewDefaultSize;
			VolumeSize = FVector(NewDefaultSize);
		}
		// Calculate bounds from internal settings (uses GridResolutionPreset)
		RecalculateVolumeBounds();
	}
}

void UKawaiiFluidSimulationModule::OnTargetVolumeDestroyed(AActor* DestroyedActor)
{
	// The TargetSimulationVolume actor was deleted
	if (DestroyedActor == TargetSimulationVolume)
	{
		// Unregister from the volume component before it's destroyed
		if (UKawaiiFluidVolumeComponent* VolumeComp = PreviousRegisteredVolume.Get())
		{
			VolumeComp->UnregisterModule(this);
		}

		// Clear references (don't unbind - the actor is being destroyed)
		TargetSimulationVolume = nullptr;
		PreviousRegisteredVolume = nullptr;
		bBoundToVolumeDestroyed = false;

		// Update display - derives CellSize from Preset
		UpdateVolumeInfoDisplay();
	}
}

void UKawaiiFluidSimulationModule::BindToVolumeDestroyedEvent()
{
	// First unbind any existing binding
	UnbindFromVolumeDestroyedEvent();

	// Bind to the new volume's OnDestroyed
	if (TargetSimulationVolume && IsValid(TargetSimulationVolume))
	{
		TargetSimulationVolume->OnDestroyed.AddDynamic(this, &UKawaiiFluidSimulationModule::OnTargetVolumeDestroyed);
		bBoundToVolumeDestroyed = true;
	}
}

void UKawaiiFluidSimulationModule::UnbindFromVolumeDestroyedEvent()
{
	if (bBoundToVolumeDestroyed)
	{
		// Need to check if the actor is still valid before unbinding
		if (TargetSimulationVolume && IsValid(TargetSimulationVolume))
		{
			TargetSimulationVolume->OnDestroyed.RemoveDynamic(this, &UKawaiiFluidSimulationModule::OnTargetVolumeDestroyed);
		}
		bBoundToVolumeDestroyed = false;
	}
}

void UKawaiiFluidSimulationModule::OnPresetChangedExternal(UKawaiiFluidPresetDataAsset* NewPreset)
{
#if WITH_EDITOR
	// Unbind from old preset's delegate
	UnbindFromPresetPropertyChanged();
#endif

	// Update preset reference
	Preset = NewPreset;

	// Update SpatialHash if it exists
	if (SpatialHash.IsValid() && Preset)
	{
		SpatialHash = MakeShared<FSpatialHash>(Preset->SmoothingRadius);
	}

#if WITH_EDITOR
	// Bind to new preset's property changed delegate
	if (Preset)
	{
		BindToPresetPropertyChanged();
	}
#endif

	// Update CellSize and bounds display
	UpdateVolumeInfoDisplay();
}

#if WITH_EDITOR
void UKawaiiFluidSimulationModule::OnPresetPropertyChanged(UKawaiiFluidPresetDataAsset* ChangedPreset)
{
	// Ensure this is our preset
	if (ChangedPreset != Preset)
	{
		return;
	}

	// Mark preset as dirty for runtime rebuild

	// Update SpatialHash if it exists
	if (SpatialHash.IsValid() && Preset)
	{
		SpatialHash = MakeShared<FSpatialHash>(Preset->SmoothingRadius);
	}

	// Update CellSize and bounds display
	UpdateVolumeInfoDisplay();
}

void UKawaiiFluidSimulationModule::BindToPresetPropertyChanged()
{
	// First unbind any existing binding
	UnbindFromPresetPropertyChanged();

	// Bind to the preset's OnPropertyChanged delegate
	if (Preset)
	{
		PresetPropertyChangedHandle = Preset->OnPropertyChanged.AddUObject(
			this, &UKawaiiFluidSimulationModule::OnPresetPropertyChanged);
	}
}

void UKawaiiFluidSimulationModule::UnbindFromPresetPropertyChanged()
{
	if (PresetPropertyChangedHandle.IsValid())
	{
		if (Preset)
		{
			Preset->OnPropertyChanged.Remove(PresetPropertyChangedHandle);
		}
		PresetPropertyChangedHandle.Reset();
	}
}

void UKawaiiFluidSimulationModule::OnObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap)
{
	// Check if our Preset was replaced (e.g., via asset reload)
	if (Preset)
	{
		if (UObject* const* NewPresetPtr = ReplacementMap.Find(Preset))
		{
			UKawaiiFluidPresetDataAsset* NewPreset = Cast<UKawaiiFluidPresetDataAsset>(*NewPresetPtr);

			UE_LOG(LogTemp, Log, TEXT("SimulationModule: Preset replaced via reload (Old=%p, New=%p)"),
				Preset.Get(), NewPreset);

			// Unbind from old preset
			UnbindFromPresetPropertyChanged();

			// Update preset reference
			Preset = NewPreset;
		
			// Update SpatialHash if it exists
			if (SpatialHash.IsValid() && Preset)
			{
				SpatialHash = MakeShared<FSpatialHash>(Preset->SmoothingRadius);
			}

			// Bind to new preset's property changed delegate
			if (Preset)
			{
				BindToPresetPropertyChanged();
			}

			// Update CellSize and bounds display
			UpdateVolumeInfoDisplay();
		}
	}
}

void UKawaiiFluidSimulationModule::BindToObjectsReplaced()
{
	// First unbind any existing binding
	UnbindFromObjectsReplaced();

	// Bind to the objects replaced delegate
	ObjectsReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddUObject(
		this, &UKawaiiFluidSimulationModule::OnObjectsReplaced);
}

void UKawaiiFluidSimulationModule::UnbindFromObjectsReplaced()
{
	if (ObjectsReplacedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectsReplaced.Remove(ObjectsReplacedHandle);
		ObjectsReplacedHandle.Reset();
	}
}
#endif
