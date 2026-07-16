// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/core/containers/handle.hpp"

// The DestructionWorld handle types, factored into their own tiny header so the event payloads
// (events.hpp) can name an InstanceId without pulling in the whole world seam — and so the world
// seam can, in turn, include events.hpp for its events() return type without a circular include.
// (The physics module keeps events.hpp standalone for the same reason; this mirrors it.)
namespace rime::destruction {

// A registered fracture pattern's id — one per distinct destructible asset, the shape economy
// (register once, spawn many). Append-only in v1 (unregister is m8.5), so the generation stays 0.
struct PatternTag {};

using PatternId = core::Handle<PatternTag>;

// A standing instance's id. Append-only in v1 (despawn is m8.5).
struct InstanceTag {};

using InstanceId = core::Handle<InstanceTag>;

// "No such part" — returned by part_from_child for an out-of-range child index. Real part ids are
// cook order, always < the pattern's part count.
inline constexpr std::uint32_t kInvalidPartIndex = 0xFFFFFFFFu;

} // namespace rime::destruction
