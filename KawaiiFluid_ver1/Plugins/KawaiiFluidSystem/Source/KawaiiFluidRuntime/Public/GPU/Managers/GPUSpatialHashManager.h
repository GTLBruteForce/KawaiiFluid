// Copyright KawaiiFluid Team. All Rights Reserved.
//
// DEPRECATED: This header is kept for backward compatibility only.
// The class FGPUSpatialHashManager has been renamed to FGPUZOrderSortManager
// to accurately reflect its implementation (Z-Order Morton Code sorting,
// not traditional spatial hashing).
//
// New code should include "GPU/Managers/GPUZOrderSortManager.h" instead.
//
// The 'using' alias in GPUZOrderSortManager.h provides backward compatibility,
// so existing code using FGPUSpatialHashManager will continue to work.

#pragma once

// Redirect to the correctly named header
#include "GPU/Managers/GPUZOrderSortManager.h"
