// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::ecs — the data-oriented world. Include this to pull in the whole ECS
// public surface; include the individual headers when you only need one. See ADR-0018 for the
// storage model and docs/design/ecs.md for the design.
//
// As of M4.2a: entities (generational ids), the entity directory, the component-type registry, and
// the archetype **storage primitives** — the ChunkPool (allocator-backed 16 KiB blocks), the
// per-signature chunk layout, and the Chunk SoA row store. The World-level add/remove/get that ties
// storage to entities lands in M4.2b, queries + iteration in M4.3, parallel systems in M4.4.

#include "rime/ecs/chunk.hpp"
#include "rime/ecs/chunk_pool.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/entity_directory.hpp"
#include "rime/ecs/signature.hpp"
#include "rime/ecs/world.hpp"
