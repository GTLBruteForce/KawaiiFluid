// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Physics/SPHKernels.h"

namespace SPHKernels
{
	// Constant for converting Unreal units (cm) to meters (m)
	constexpr float CM_TO_M = 0.01f;

	//========================================
	// Poly6 Kernel
	//========================================

	float Poly6Coefficient(float h)
	{
		// 315 / (64 * π * h^9)
		return 315.0f / (64.0f * PI * FMath::Pow(h, 9.0f));
	}

	float Poly6(float r, float h)
	{
		if (r < 0.0f || r > h)
		{
			return 0.0f;
		}

		// Convert cm -> m
		float r_m = r * CM_TO_M;
		float h_m = h * CM_TO_M;

		float h2 = h_m * h_m;
		float r2 = r_m * r_m;
		float diff = h2 - r2;

		return Poly6Coefficient(h_m) * diff * diff * diff;
	}

	float Poly6(const FVector& r, float h)
	{
		return Poly6(r.Size(), h);
	}

	//========================================
	// Spiky Kernel Gradient
	//========================================

	float SpikyGradientCoefficient(float h)
	{
		// -45 / (π * h^6)
		return -45.0f / (PI * FMath::Pow(h, 6.0f));
	}

	FVector SpikyGradient(const FVector& r, float h)
	{
		float rLen = r.Size();

		if (rLen <= 0.0f || rLen > h)
		{
			return FVector::ZeroVector;
		}

		// Convert cm -> m
		float rLen_m = rLen * CM_TO_M;
		float h_m = h * CM_TO_M;

		float diff = h_m - rLen_m;
		float coeff = SpikyGradientCoefficient(h_m) * diff * diff;

		// r̂ (unit vector) - direction unchanged
		FVector rNorm = r / rLen;

		// Convert result to cm units (gradient = 1/m^4, used for position correction so convert to cm)
		// Gradient magnitude is in 1/m units, so multiply by 0.01 to convert to cm
		return coeff * rNorm * CM_TO_M;
	}

	//========================================
	// Viscosity Kernel Laplacian
	//========================================

	float ViscosityLaplacianCoefficient(float h)
	{
		// 45 / (π * h^6)
		return 45.0f / (PI * FMath::Pow(h, 6.0f));
	}

	float ViscosityLaplacian(float r, float h)
	{
		if (r < 0.0f || r > h)
		{
			return 0.0f;
		}

		// Convert cm -> m
		float r_m = r * CM_TO_M;
		float h_m = h * CM_TO_M;

		return ViscosityLaplacianCoefficient(h_m) * (h_m - r_m);
	}

	//========================================
	// Adhesion Kernel (Akinci 2013)
	//========================================

	float Adhesion(float r, float h)
	{
		// Valid range: 0.5h < r < h
		if (r < 0.5f * h || r > h)
		{
			return 0.0f;
		}

		// Convert cm -> m
		float r_m = r * CM_TO_M;
		float h_m = h * CM_TO_M;

		// Formula: 0.007 / h^3.25 * (-4r²/h + 6r - 2h)^0.25
		float coeff = 0.007f / FMath::Pow(h_m, 3.25f);
		float inner = -4.0f * r_m * r_m / h_m + 6.0f * r_m - 2.0f * h_m;

		if (inner <= 0.0f)
		{
			return 0.0f;
		}

		return coeff * FMath::Pow(inner, 0.25f);
	}

	//========================================
	// Cohesion Kernel (Akinci 2013)
	//========================================

	float Cohesion(float r, float h)
	{
		if (r < 0.0f || r > h)
		{
			return 0.0f;
		}

		// Convert cm -> m
		float r_m = r * CM_TO_M;
		float h_m = h * CM_TO_M;

		float coeff = 32.0f / (PI * FMath::Pow(h_m, 9.0f));
		float h2 = h_m * 0.5f;

		if (r_m <= h2)
		{
			// Range: 0 < r <= h/2
			float diff1 = h_m - r_m;
			float diff2 = diff1 * diff1 * diff1;
			float r3 = r_m * r_m * r_m;
			return coeff * 2.0f * diff2 * r3 - (FMath::Pow(h_m, 6.0f) / 64.0f);
		}
		else
		{
			// Range: h/2 < r <= h
			float diff = h_m - r_m;
			return coeff * diff * diff * diff * r_m * r_m * r_m;
		}
	}

	//========================================
	// Precompute Coefficients
	//========================================

	void FKernelCoefficients::Precompute(float SmoothingRadius)
	{
		// Convert cm -> m
		h = SmoothingRadius * CM_TO_M;
		h2 = h * h;
		h6 = h2 * h2 * h2;
		h9 = h6 * h2 * h;

		Poly6Coeff = 315.0f / (64.0f * PI * h9);
		SpikyGradCoeff = -45.0f / (PI * h6);
		ViscosityLapCoeff = 45.0f / (PI * h6);
	}
}
