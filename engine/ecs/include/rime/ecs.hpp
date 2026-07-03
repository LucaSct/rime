// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::ecs — the data-oriented world. Include this to pull in the whole ECS
// public surface; include the individual headers when you only need one. See ADR-0018 for the
// storage model and docs/design/ecs.md for the design.
//
// As of M4.3: entities (generational ids), the entity directory, the component-type registry, the
// archetype storage (ChunkPool + per-signature ChunkLayout + Chunk SoA rows + Archetype), the World
// that ties them together (spawn, add/remove component = archetype move, get/has), and Query<Ts...>
// for column-wise iteration over the entities that have a given set of components. Parallel systems
// over queries land in M4.4.

#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/chunk_pool.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/entity_directory.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/signature.hpp"
#include "rime/ecs/world.hpp"
