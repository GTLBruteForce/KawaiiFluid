// Copyright KawaiiFluid Team. All Rights Reserved.

#include "Rendering/KawaiiFluidRendererSettings.h"
#include "UObject/ConstructorHelpers.h"

FKawaiiFluidISMRendererSettings::FKawaiiFluidISMRendererSettings()
	: bEnabled(true)
	, ParticleScale(1.0f)
{
	// Set default mesh
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMeshFinder(TEXT("/Engine/BasicShapes/Sphere"));
	if (SphereMeshFinder.Succeeded())
	{
		ParticleMesh = SphereMeshFinder.Object;
	}

	// Set default material
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	if (BasicMaterialFinder.Succeeded())
	{
		ParticleMaterial = BasicMaterialFinder.Object;
	}
}
