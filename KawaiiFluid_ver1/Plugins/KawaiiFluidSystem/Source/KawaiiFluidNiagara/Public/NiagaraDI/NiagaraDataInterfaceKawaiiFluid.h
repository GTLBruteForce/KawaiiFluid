// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraDataInterface.h"
#include "NiagaraCommon.h"
#include "RenderResource.h"
#include "RHIResources.h"
#include "NiagaraDataInterfaceKawaiiFluid.generated.h"

class UKawaiiFluidComponent;
class UKawaiiFluidSimulationModule;
struct FKawaiiRenderParticle;
struct FFluidParticle;

/**
 * Per-Instance data structure
 * One instance is created for each Niagara system instance
 */
struct FNDIKawaiiFluid_InstanceData
{
	/** Referenced FluidComponent (weak pointer) */
	TWeakObjectPtr<UKawaiiFluidComponent> SourceComponent;

	/** SimulationModule cache (retrieved from Component) */
	TWeakObjectPtr<UKawaiiFluidSimulationModule> SourceModule;

	/** Last update time */
	float LastUpdateTime = 0.0f;

	/** Cached particle count */
	int32 CachedParticleCount = 0;

	/** GPU buffer (Position + Velocity) */
	FBufferRHIRef ParticleBuffer;
	FShaderResourceViewRHIRef ParticleSRV;

	/** Buffer capacity (minimize reallocation) */
	int32 BufferCapacity = 0;

	/** Check if buffer is valid */
	bool IsBufferValid() const
	{
		return ParticleBuffer.IsValid() && ParticleSRV.IsValid();
	}
};

/**
 * Kawaii Fluid Data Interface
 * Passes CPU-generated particle data to Niagara GPU particles
 *
 * @note Uses SimulationModule from UKawaiiFluidComponent
 */
UCLASS(EditInlineNew, Category = "KawaiiFluid", meta = (DisplayName = "Kawaii Fluid Data"))
class KAWAIIFLUIDNIAGARA_API UNiagaraDataInterfaceKawaiiFluid : public UNiagaraDataInterface
{
	GENERATED_UCLASS_BODY()

	/**
	 * Actor with FluidComponent to connect
	 * @note Must select an Actor with UKawaiiFluidComponent
	 */
	UPROPERTY(EditAnywhere, Category = "Kawaii Fluid", meta = (AllowedClasses = "/Script/Engine.Actor"))
	TSoftObjectPtr<AActor> SourceFluidActor;

	/** Enable auto update (manual call required if false) */
	UPROPERTY(EditAnywhere, Category = "Kawaii Fluid")
	bool bAutoUpdate = true;

	/** Update frequency (seconds, 0 = every frame) */
	UPROPERTY(EditAnywhere, Category = "Kawaii Fluid", meta = (ClampMin = "0.0"))
	float UpdateInterval = 0.0f;

	//========================================
	// UNiagaraDataInterface overrides
	//========================================

	/** VM function binding (for CPU simulation) */
	virtual void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, 
	                                    void* InstanceData, 
	                                    FVMExternalFunction& OutFunc) override;

	/** Check if execution target is supported */
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override { return true; }

	/** GPU simulation function registration */
#if WITH_EDITORONLY_DATA
	virtual void GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, 
	                                         FString& OutHLSL) override;
	virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, 
	                               const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, 
	                               int FunctionInstanceIndex, 
	                               FString& OutHLSL) override;
	virtual bool AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const override;
#endif

	/** Per-Instance data size */
	virtual int32 PerInstanceDataSize() const override
	{
		return sizeof(FNDIKawaiiFluid_InstanceData);
	}

	/** Per-Instance data initialization */
	virtual bool InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;

	/** Per-Instance data destruction */
	virtual void DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance) override;

	/** Per-frame update (game thread) */
	virtual bool PerInstanceTick(void* PerInstanceData, 
	                              FNiagaraSystemInstance* SystemInstance, 
	                              float DeltaSeconds) override;

	/** Check if distance field is required */
	virtual bool RequiresDistanceFieldData() const override { return false; }

	/** PreSimulate Tick required */
	virtual bool HasPreSimulateTick() const override { return true; }

	/** Copyable */
	virtual bool Equals(const UNiagaraDataInterface* Other) const override;

	/** GPU compute parameter setup */
	virtual void ProvidePerInstanceDataForRenderThread(void* DataForRenderThread, 
	                                                    void* PerInstanceData, 
	                                                    const FNiagaraSystemInstanceID& SystemInstance) override;

	//========================================
	// UObject Interface
	//========================================

	/** Niagara Type Registry registration (required!) */
	virtual void PostInitProperties() override;

	//========================================
	// UNiagaraDataInterface overrides
	//========================================

	/** Data copy (UPROPERTY synchronization) */
	virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;

	//========================================
	// VM functions (for CPU simulation)
	//========================================

	/** Get particle count */
	void VMGetParticleCount(FVectorVMExternalFunctionContext& Context);

	/** Get particle position at specific index */
	void VMGetParticlePosition(FVectorVMExternalFunctionContext& Context);

	/** Get particle velocity at specific index */
	void VMGetParticleVelocity(FVectorVMExternalFunctionContext& Context);

	/** Get particle radius */
	void VMGetParticleRadius(FVectorVMExternalFunctionContext& Context);

protected:
#if WITH_EDITORONLY_DATA
	/** Function signature registration (editor only) */
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
#endif

	//========================================
	// Internal helper functions
	//========================================

private:
	/** GPU buffer update (render thread) */
	void UpdateGPUBuffers_RenderThread(FNDIKawaiiFluid_InstanceData* InstanceData,
	                                     const TArray<FFluidParticle>& Particles,
	                                     float Radius);

	/** Function name constants */
	static const FName GetParticleCountName;
	static const FName GetParticlePositionName;
	static const FName GetParticleVelocityName;
	static const FName GetParticleRadiusName;
};
