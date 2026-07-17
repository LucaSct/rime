// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/core/reflect/type_info.hpp"

// Reflection registrations for the math value types — Vec3, Quat, and the TRS Transform they
// compose into. Split out of the type headers on purpose: vec.hpp / quat.hpp are included almost
// everywhere, and most of that code neither serializes nor inspects a vector. A consumer that DOES
// — the M9 scene format, the editor's property inspectors, a debug dumper — includes this one
// header and gets nested-struct reflection for a whole `Transform` for free (its
// `translation`/`rotation`/`scale` become FieldType::Struct fields that recurse into x/y/z/w).
// Registering them in one place also gives each math type a single, stable `type_hash`, instead of
// every consumer risking a subtly different registration. Design: docs/design/reflection.md,
// docs/design/scene-format.md.
//
// Only the types a reflected component actually stores today are here (Transform needs Vec3 +
// Quat). Vec2/Vec4 join the moment a reflected struct first carries one — reflect them right above
// Transform and keep the "nested type registered before its container" order the macros require.

RIME_REFLECT_BEGIN(rime::core::Vec3)
RIME_REFLECT_FIELD(x)
RIME_REFLECT_FIELD(y)
RIME_REFLECT_FIELD(z)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::core::Quat)
RIME_REFLECT_FIELD(x)
RIME_REFLECT_FIELD(y)
RIME_REFLECT_FIELD(z)
RIME_REFLECT_FIELD(w)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::core::Transform)
RIME_REFLECT_FIELD(translation)
RIME_REFLECT_FIELD(rotation)
RIME_REFLECT_FIELD(scale)
RIME_REFLECT_END()
