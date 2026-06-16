// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::core memory: the allocator interface (plus make_in/destroy_in and
// alignment helpers) and the concrete allocators. Include this for all of them, or the
// individual headers under memory/ for finer-grained dependencies.
#include "rime/core/memory/allocator.hpp"
#include "rime/core/memory/arena.hpp"
#include "rime/core/memory/memory_tracker.hpp"
#include "rime/core/memory/pool.hpp"
#include "rime/core/memory/stack_allocator.hpp"
