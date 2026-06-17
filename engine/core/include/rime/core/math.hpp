// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::core math: scalar helpers, the SIMD seam, vectors, and matrices.
// (Quaternions and transforms arrive in M1.4.) Include this for everything, or the individual
// headers under math/ for finer-grained dependencies. Conventions: docs/adr/0004; derivation:
// docs/math/vectors-matrices.md.
#include "rime/core/math/mat.hpp"
#include "rime/core/math/scalar.hpp"
#include "rime/core/math/simd.hpp"
#include "rime/core/math/vec.hpp"
