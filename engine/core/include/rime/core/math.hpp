// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::core math: scalar helpers, the SIMD seam, vectors, matrices,
// quaternions, and TRS transforms. Include this for everything, or the individual headers under
// math/ for finer-grained dependencies. Conventions: docs/adr/0004 (+ 0005 for rotations);
// derivations: docs/math/vectors-matrices.md and docs/math/quaternions-transforms.md.
#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/scalar.hpp"
#include "rime/core/math/simd.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
