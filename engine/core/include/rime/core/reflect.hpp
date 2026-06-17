// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::core reflection: the type-descriptor core (TypeInfo + the RIME_REFLECT
// registration macros) and the generic, reflection-driven serializer / debug dumper built on it.
// Register a struct once with RIME_REFLECT and it serializes and prints for free. Include this, or
// the individual headers under reflect/. Design: docs/design/reflection.md.
#include "rime/core/reflect/serialize.hpp"
#include "rime/core/reflect/type_info.hpp"
