// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "NiagaraDI/NiagaraDataInterfaceKawaiiFluid.h"
#include "Core/KawaiiRenderParticle.h"
#include "Components/KawaiiFluidComponent.h"
#include "Modules/KawaiiFluidSimulationModule.h"
#include "NiagaraShader.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraCompileHashVisitor.h"
#include "NiagaraTypes.h"
#include "ShaderParameterUtils.h"
#include "RHICommandList.h"
#include "Engine/World.h"

//========================================
// Function name definitions
//========================================

const FName UNiagaraDataInterfaceKawaiiFluid::GetParticleCountName(TEXT("GetParticleCount"));
const FName UNiagaraDataInterfaceKawaiiFluid::GetParticlePositionName(TEXT("GetParticlePosition"));
const FName UNiagaraDataInterfaceKawaiiFluid::GetParticleVelocityName(TEXT("GetParticleVelocity"));
const FName UNiagaraDataInterfaceKawaiiFluid::GetParticleRadiusName(TEXT("GetParticleRadius"));

//========================================
// Constructor
//========================================

UNiagaraDataInterfaceKawaiiFluid::UNiagaraDataInterfaceKawaiiFluid(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, bAutoUpdate(true)
	, UpdateInterval(0.0f)
{
}

//========================================
// Niagara Type Registry registration (required!)
//========================================

void UNiagaraDataInterfaceKawaiiFluid::PostInitProperties()
{
	Super::PostInitProperties();

	// Register to Type Registry only when CDO (Class Default Object)
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		// AllowAnyVariable: Can be used as variable type
		// AllowParameter: Can be added as User Parameter
		ENiagaraTypeRegistryFlags Flags = 
			ENiagaraTypeRegistryFlags::AllowAnyVariable | 
			ENiagaraTypeRegistryFlags::AllowParameter;
		
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), Flags);
		
		UE_LOG(LogTemp, Warning, TEXT("✅ UNiagaraDataInterfaceKawaiiFluid registered with Niagara Type Registry"));
	}
}

//========================================
// UPROPERTY synchronization
//========================================

bool UNiagaraDataInterfaceKawaiiFluid::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination))
	{
		return false;
	}

	UNiagaraDataInterfaceKawaiiFluid* DestTyped = CastChecked<UNiagaraDataInterfaceKawaiiFluid>(Destination);
	DestTyped->SourceFluidActor = SourceFluidActor;
	DestTyped->bAutoUpdate = bAutoUpdate;
	DestTyped->UpdateInterval = UpdateInterval;

	return true;
}

//========================================
// Function signature registration
//========================================

#if WITH_EDITORONLY_DATA
void UNiagaraDataInterfaceKawaiiFluid::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const
{
	// 1. GetParticleCount
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetParticleCountName;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("KawaiiFluid")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count")));
		Sig.SetDescription(NSLOCTEXT("Niagara", "KawaiiFluid_GetParticleCount", "Returns the total number of fluid particles"));
		OutFunctions.Add(Sig);
	}

	// 2. GetParticlePosition
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetParticlePositionName;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("KawaiiFluid")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position")));
		Sig.SetDescription(NSLOCTEXT("Niagara", "KawaiiFluid_GetParticlePosition", "Returns position of particle at given index"));
		OutFunctions.Add(Sig);
	}

	// 3. GetParticleVelocity
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetParticleVelocityName;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("KawaiiFluid")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Velocity")));
		Sig.SetDescription(NSLOCTEXT("Niagara", "KawaiiFluid_GetParticleVelocity", "Returns velocity of particle at given index"));
		OutFunctions.Add(Sig);
	}

	// 4. GetParticleRadius
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetParticleRadiusName;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("KawaiiFluid")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Radius")));
		Sig.SetDescription(NSLOCTEXT("Niagara", "KawaiiFluid_GetParticleRadius", "Returns rendering radius for fluid particles"));
		OutFunctions.Add(Sig);
	}
}
#endif

//========================================
// VM function binding
//========================================

void UNiagaraDataInterfaceKawaiiFluid::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, 
                                                               void* InstanceData, 
                                                               FVMExternalFunction& OutFunc)
{
	if (BindingInfo.Name == GetParticleCountName)
	{
		OutFunc = FVMExternalFunction::CreateUObject(this, &UNiagaraDataInterfaceKawaiiFluid::VMGetParticleCount);
	}
	else if (BindingInfo.Name == GetParticlePositionName)
	{
		OutFunc = FVMExternalFunction::CreateUObject(this, &UNiagaraDataInterfaceKawaiiFluid::VMGetParticlePosition);
	}
	else if (BindingInfo.Name == GetParticleVelocityName)
	{
		OutFunc = FVMExternalFunction::CreateUObject(this, &UNiagaraDataInterfaceKawaiiFluid::VMGetParticleVelocity);
	}
	else if (BindingInfo.Name == GetParticleRadiusName)
	{
		OutFunc = FVMExternalFunction::CreateUObject(this, &UNiagaraDataInterfaceKawaiiFluid::VMGetParticleRadius);
	}
}

//========================================
// Per-Instance data management
//========================================

bool UNiagaraDataInterfaceKawaiiFluid::InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	FNDIKawaiiFluid_InstanceData* InstanceData = new (PerInstanceData) FNDIKawaiiFluid_InstanceData();

	// Runtime validation: Check User Parameter connection
	if (SourceFluidActor.IsNull())
	{
		UE_LOG(LogTemp, Error, TEXT("UNiagaraDataInterfaceKawaiiFluid: SourceFluidActor is not set! Please assign an Actor in User Parameters."));
		return true; // Initialization succeeds but no data available
	}

	// Find UKawaiiFluidComponent from Actor
	if (SourceFluidActor.IsValid())
	{
		AActor* Actor = SourceFluidActor.Get();
		if (Actor)
		{
			UKawaiiFluidComponent* FluidComp = Actor->FindComponentByClass<UKawaiiFluidComponent>();
			if (FluidComp && FluidComp->GetSimulationModule())
			{
				InstanceData->SourceComponent = FluidComp;
				InstanceData->SourceModule = FluidComp->GetSimulationModule();

				// Set initial CachedParticleCount (before Tick!)
				const TArray<FFluidParticle>& Particles = FluidComp->GetSimulationModule()->GetParticles();
				InstanceData->CachedParticleCount = Particles.Num();

				UE_LOG(LogTemp, Log, TEXT("Niagara DI: Found KawaiiFluidComponent on %s (Particles: %d)"),
					*Actor->GetName(), InstanceData->CachedParticleCount);
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("Niagara DI: Actor '%s' does not have UKawaiiFluidComponent!"),
					*Actor->GetName());
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Niagara DI: SourceFluidActor is invalid (Actor deleted or not loaded)"));
		}
	}

	return true;
}

void UNiagaraDataInterfaceKawaiiFluid::DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	FNDIKawaiiFluid_InstanceData* InstanceData = static_cast<FNDIKawaiiFluid_InstanceData*>(PerInstanceData);
	InstanceData->~FNDIKawaiiFluid_InstanceData();
}

//========================================
// Per-frame update
//========================================

bool UNiagaraDataInterfaceKawaiiFluid::PerInstanceTick(void* PerInstanceData,
                                                         FNiagaraSystemInstance* SystemInstance,
                                                         float DeltaSeconds)
{
	FNDIKawaiiFluid_InstanceData* InstanceData = static_cast<FNDIKawaiiFluid_InstanceData*>(PerInstanceData);

	if (!bAutoUpdate)
	{
		return false;
	}

	// Check update interval
	InstanceData->LastUpdateTime += DeltaSeconds;
	if (UpdateInterval > 0.0f && InstanceData->LastUpdateTime < UpdateInterval)
	{
		return false;
	}
	InstanceData->LastUpdateTime = 0.0f;

	// Check module validity
	UKawaiiFluidSimulationModule* SimModule = InstanceData->SourceModule.Get();
	if (!SimModule)
	{
		return false;
	}

	// Get particle data
	const TArray<FFluidParticle>& Particles = SimModule->GetParticles();
	InstanceData->CachedParticleCount = Particles.Num();

	// BREAKPOINT: Log output only during PIE execution
	#if !UE_BUILD_SHIPPING
	static bool bFirstTick = true;
	if (bFirstTick && Particles.Num() > 0)
	{
		// Check if World is Game World (PIE, Standalone, etc.)
		UKawaiiFluidComponent* FluidComp = InstanceData->SourceComponent.Get();
		if (FluidComp)
		{
			if (UWorld* World = FluidComp->GetWorld())
			{
				if (World->IsGameWorld())
				{
					UE_LOG(LogTemp, Error, TEXT("🔴 BREAKPOINT: PerInstanceTick - CachedParticleCount=%d (PIE)"),
						InstanceData->CachedParticleCount);
					bFirstTick = false;
				}
			}
		}
	}
	#endif

	if (Particles.Num() == 0)
	{
		return false;
	}

	return true;
}

//========================================
// GPU buffer update (render thread)
//========================================

void UNiagaraDataInterfaceKawaiiFluid::UpdateGPUBuffers_RenderThread(FNDIKawaiiFluid_InstanceData* InstanceData,
                                                                       const TArray<FFluidParticle>& Particles,
                                                                       float Radius)
{
	int32 ParticleCount = Particles.Num();

	// Convert FFluidParticle to FKawaiiRenderParticle
	TArray<FKawaiiRenderParticle> RenderParticles;
	RenderParticles.Reserve(ParticleCount);

	for (const FFluidParticle& Particle : Particles)
	{
		FKawaiiRenderParticle RenderParticle;
		RenderParticle.Position = (FVector3f)Particle.Position;
		RenderParticle.Velocity = (FVector3f)Particle.Velocity;
		RenderParticle.Radius = Radius;
		RenderParticle.Padding = 0.0f;
		RenderParticles.Add(RenderParticle);
	}

	// Send to render thread
	ENQUEUE_RENDER_COMMAND(UpdateKawaiiFluidBuffers)(
		[InstanceData, RenderParticles, ParticleCount](FRHICommandListImmediate& RHICmdList)
		{
			// Check if buffer reallocation is needed
			if (InstanceData->BufferCapacity < ParticleCount)
			{
				int32 NewCapacity = FMath::Max(ParticleCount, 1024);
				InstanceData->BufferCapacity = NewCapacity;

				// Create Particle Buffer (FKawaiiRenderParticle size = 32 bytes)
				// UE 5.7 API: Use FRHIBufferCreateDesc (builder pattern)
				FRHIBufferCreateDesc BufferDesc;
				BufferDesc.Size = NewCapacity * sizeof(FKawaiiRenderParticle);
				BufferDesc.Usage = BUF_ShaderResource | BUF_Dynamic;
				BufferDesc.DebugName = TEXT("KawaiiFluid_Particles");

				InstanceData->ParticleBuffer = RHICmdList.CreateBuffer(BufferDesc);

				// Create SRV (UE 5.7 API: Use FRHIViewDesc)
				InstanceData->ParticleSRV = RHICmdList.CreateShaderResourceView(
					InstanceData->ParticleBuffer,
					FRHIViewDesc::CreateBufferSRV()
						.SetType(FRHIViewDesc::EBufferType::Typed)
						.SetFormat(PF_R32_FLOAT)
				);
			}

			// Direct copy of FKawaiiRenderParticle
			void* Data = RHICmdList.LockBuffer(InstanceData->ParticleBuffer, 0,
			                                    ParticleCount * sizeof(FKawaiiRenderParticle),
			                                    RLM_WriteOnly);
			FMemory::Memcpy(Data, RenderParticles.GetData(),
			                 ParticleCount * sizeof(FKawaiiRenderParticle));
			RHICmdList.UnlockBuffer(InstanceData->ParticleBuffer);
		}
	);
}

//========================================
// VM function implementations (for CPU simulation)
//========================================

void UNiagaraDataInterfaceKawaiiFluid::VMGetParticleCount(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<FNDIKawaiiFluid_InstanceData> InstanceData(Context);
	FNDIOutputParam<int32> OutCount(Context);

	int32 Count = InstanceData->CachedParticleCount;

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutCount.SetAndAdvance(Count);
	}

	// Log output only during PIE execution (first call)
	#if !UE_BUILD_SHIPPING
	static bool bFirstCall = true;
	if (bFirstCall && Count > 0)
	{
		// Check world state (use SourceComponent from InstanceData)
		UKawaiiFluidComponent* FluidComp = InstanceData->SourceComponent.Get();
		if (FluidComp)
		{
			if (UWorld* World = FluidComp->GetWorld())
			{
				if (World->IsGameWorld())
				{
					UE_LOG(LogTemp, Warning, TEXT("🎯 VMGetParticleCount called: %d particles (PIE)"), Count);
					bFirstCall = false;
				}
			}
		}
	}
	#endif
}

void UNiagaraDataInterfaceKawaiiFluid::VMGetParticlePosition(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<FNDIKawaiiFluid_InstanceData> InstanceData(Context);
	FNDIInputParam<int32> InIndex(Context);
	FNDIOutputParam<FVector3f> OutPosition(Context);

	// Get particle data from SimulationModule
	UKawaiiFluidSimulationModule* SimModule = InstanceData->SourceModule.Get();
	if (!SimModule)
	{
		// Return zero if no module
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			InIndex.GetAndAdvance();
			OutPosition.SetAndAdvance(FVector3f::ZeroVector);
		}
		return;
	}

	const TArray<FFluidParticle>& Particles = SimModule->GetParticles();

	// Log output only during PIE execution (first call)
	#if !UE_BUILD_SHIPPING
	static bool bFirstCall = true;
	if (bFirstCall && Particles.Num() > 0)
	{
		UKawaiiFluidComponent* FluidComp = InstanceData->SourceComponent.Get();
		if (FluidComp)
		{
			if (UWorld* World = FluidComp->GetWorld())
			{
				if (World->IsGameWorld())
				{
					UE_LOG(LogTemp, Warning, TEXT("🎯 VMGetParticlePosition called: %d instances (PIE)"), Context.GetNumInstances());
					UE_LOG(LogTemp, Warning, TEXT("  → First Particle Position: (%f, %f, %f)"),
						Particles[0].Position.X, Particles[0].Position.Y, Particles[0].Position.Z);
					bFirstCall = false;
				}
			}
		}
	}
	#endif

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		int32 Index = InIndex.GetAndAdvance();
		if (Particles.IsValidIndex(Index))
		{
			OutPosition.SetAndAdvance((FVector3f)Particles[Index].Position);
		}
		else
		{
			OutPosition.SetAndAdvance(FVector3f::ZeroVector);
		}
	}
}

void UNiagaraDataInterfaceKawaiiFluid::VMGetParticleVelocity(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<FNDIKawaiiFluid_InstanceData> InstanceData(Context);
	FNDIInputParam<int32> InIndex(Context);
	FNDIOutputParam<FVector3f> OutVelocity(Context);

	// Get particle data from SimulationModule
	UKawaiiFluidSimulationModule* SimModule = InstanceData->SourceModule.Get();
	if (!SimModule)
	{
		// Return zero if no module
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			InIndex.GetAndAdvance();
			OutVelocity.SetAndAdvance(FVector3f::ZeroVector);
		}
		return;
	}

	const TArray<FFluidParticle>& Particles = SimModule->GetParticles();

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		int32 Index = InIndex.GetAndAdvance();
		if (Particles.IsValidIndex(Index))
		{
			OutVelocity.SetAndAdvance((FVector3f)Particles[Index].Velocity);
		}
		else
		{
			OutVelocity.SetAndAdvance(FVector3f::ZeroVector);
		}
	}
}

void UNiagaraDataInterfaceKawaiiFluid::VMGetParticleRadius(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<FNDIKawaiiFluid_InstanceData> InstanceData(Context);
	FNDIOutputParam<float> OutRadius(Context);

	UKawaiiFluidSimulationModule* SimModule = InstanceData->SourceModule.Get();
	float Radius = SimModule ? SimModule->GetParticleRadius() : 5.0f;

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutRadius.SetAndAdvance(Radius);
	}
}

//========================================
// Other overrides
//========================================

bool UNiagaraDataInterfaceKawaiiFluid::Equals(const UNiagaraDataInterface* Other) const
{
	if (!Super::Equals(Other))
	{
		return false;
	}

	const UNiagaraDataInterfaceKawaiiFluid* OtherTyped = CastChecked<const UNiagaraDataInterfaceKawaiiFluid>(Other);
	return SourceFluidActor == OtherTyped->SourceFluidActor &&
	       bAutoUpdate == OtherTyped->bAutoUpdate &&
	       FMath::IsNearlyEqual(UpdateInterval, OtherTyped->UpdateInterval);
}

void UNiagaraDataInterfaceKawaiiFluid::ProvidePerInstanceDataForRenderThread(void* DataForRenderThread, 
                                                                              void* PerInstanceData, 
                                                                              const FNiagaraSystemInstanceID& SystemInstance)
{
	// Copy instance data to render thread
	FNDIKawaiiFluid_InstanceData* SourceData = static_cast<FNDIKawaiiFluid_InstanceData*>(PerInstanceData);
	FNDIKawaiiFluid_InstanceData* DestData = new (DataForRenderThread) FNDIKawaiiFluid_InstanceData();
	
	*DestData = *SourceData;
}

//========================================
// GPU function HLSL generation (editor only)
//========================================

#if WITH_EDITORONLY_DATA

void UNiagaraDataInterfaceKawaiiFluid::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, 
                                                                    FString& OutHLSL)
{
	OutHLSL += TEXT("Buffer<float4> {ParameterName}_ParticleBuffer;\n");
	OutHLSL += TEXT("int {ParameterName}_ParticleCount;\n");
}

bool UNiagaraDataInterfaceKawaiiFluid::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, 
                                                         const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, 
                                                         int FunctionInstanceIndex, 
                                                         FString& OutHLSL)
{
	if (FunctionInfo.DefinitionName == GetParticleCountName)
	{
		OutHLSL += FString::Printf(TEXT("void %s(out int Count) { Count = {ParameterName}_ParticleCount; }\n"), 
		                            *FunctionInfo.InstanceName);
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetParticlePositionName)
	{
		// FKawaiiRenderParticle = 32 bytes = float4 × 2
		// float4[0] = Position.xyz + Velocity.x
		// float4[1] = Velocity.yz + Radius + Padding
		OutHLSL += FString::Printf(TEXT("void %s(int Index, out float3 Position) {\n"), 
		                            *FunctionInfo.InstanceName);
		OutHLSL += TEXT("    float4 Data0 = {ParameterName}_ParticleBuffer[Index * 2 + 0];\n");
		OutHLSL += TEXT("    Position = Data0.xyz;\n");
		OutHLSL += TEXT("}\n");
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetParticleVelocityName)
	{
		// Velocity is Data0.w + Data1.xy
		OutHLSL += FString::Printf(TEXT("void %s(int Index, out float3 Velocity) {\n"), 
		                            *FunctionInfo.InstanceName);
		OutHLSL += TEXT("    float4 Data0 = {ParameterName}_ParticleBuffer[Index * 2 + 0];\n");
		OutHLSL += TEXT("    float4 Data1 = {ParameterName}_ParticleBuffer[Index * 2 + 1];\n");
		OutHLSL += TEXT("    Velocity = float3(Data0.w, Data1.xy);\n");
		OutHLSL += TEXT("}\n");
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetParticleRadiusName)
	{
		// Radius is Data1.z (based on index 0)
		OutHLSL += FString::Printf(TEXT("void %s(out float Radius) {\n"), 
		                            *FunctionInfo.InstanceName);
		OutHLSL += TEXT("    float4 Data1 = {ParameterName}_ParticleBuffer[0 * 2 + 1];\n");
		OutHLSL += TEXT("    Radius = Data1.z;\n");
		OutHLSL += TEXT("}\n");
		return true;
	}

	return false;
}

bool UNiagaraDataInterfaceKawaiiFluid::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	if (!Super::AppendCompileHash(InVisitor))
	{
		return false;
	}

	// Version update (structure change)
	InVisitor->UpdatePOD(TEXT("KawaiiFluidNiagaraDI"), (int32)2);
	
	return true;
}

#endif // WITH_EDITORONLY_DATA
