// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Particle structure for rendering
 */
struct KAWAIIFLUIDRUNTIME_API FKawaiiRenderParticle
{
public:
	FVector3f Position;
	FVector3f Velocity;
	float Radius;
	float Padding; // Padding for 16-byte alignment

	FKawaiiRenderParticle()
		: Position(FVector3f::ZeroVector)
		, Velocity(FVector3f::ZeroVector)
		, Radius(1.0f)
		, Padding(0.0f)
	{
	}
};

// Verify 32-byte size
static_assert(sizeof(FKawaiiRenderParticle) == 32, "FKawaiiRenderParticle size is not 32 bytes.");

// Verify offset
static_assert(STRUCT_OFFSET(FKawaiiRenderParticle, Radius) == 24, "Radius offset is incorrect.");
