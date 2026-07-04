// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Umbrella header for rime::ecs — the data-oriented world. Include this to pull in the whole ECS
// public surface; include the individual headers when you only need one. See ADR-0018 for the
// storage model and docs/design/ecs.md for the design.
//
// As of M4.5: entities (generational ids), the entity directory, the component-type registry, the
// archetype storage (ChunkPool + per-signature ChunkLayout + Chunk SoA rows + Archetype), the World
// that ties them together (spawn, add/remove component = archetype move, get/has), Query<Ts...> for
// column-wise iteration over the entities that have a given set of components, Query::par_for_each
// to run that iteration across all cores on the job system (one chunk per task), the System +
// Schedule scheduler that batches systems into parallel phases from their declared read/write
// access sets, the CommandBuffer that records structural edits (spawn/despawn/add/remove) for the
// schedule to apply at phase boundaries, and the transform hierarchy (LocalTransform /
// WorldTransform / Parent + propagate_transforms, world = parent.world * local). The 100k-entity
// proof sample is M4.6.

#include "rime/ecs/archetype.hpp"
#include "rime/ecs/chunk.hpp"
#include "rime/ecs/chunk_pool.hpp"
#include "rime/ecs/command_buffer.hpp"
#include "rime/ecs/component.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/ecs/entity_directory.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/schedule.hpp"
#include "rime/ecs/signature.hpp"
#include "rime/ecs/system.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
