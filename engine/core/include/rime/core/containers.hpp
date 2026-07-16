// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::core containers: generational handles and the slot map they index,
// plus the double-buffered event channel producers publish their per-tick events through. These are
// the cache-friendly, handle-based building blocks the data-oriented systems (ECS, asset tables,
// GPU-object pools, the destruction event fan-out) sit on. Include this, or the individual headers
// under containers/ for finer-grained dependencies.
#include "rime/core/containers/event_channel.hpp"
#include "rime/core/containers/handle.hpp"
#include "rime/core/containers/slot_map.hpp"
