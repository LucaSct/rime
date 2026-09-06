// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>

#include "rime/ground/surface.hpp"
#include "rime/render/material.hpp"

namespace rime::physics {
class PhysicsWorld;
}

namespace rime::render {
class MeshRegistry;
}

// Standing a `GroundSurface` up, in the two halves every bridge in this engine is split into: the
// GPU-free one that a headless server also runs, and the one that needs a device.
namespace rime::ground {

struct BindStats {
    std::size_t bound = 0;
    // A surface on a SCALED entity is refused rather than guessed at, and counted rather than
    // silently skipped. A physics body cannot be scaled, so honouring the transform would need the
    // scale folded into the shape — at which point the collider stops being derived from the
    // authored numbers and the module's whole promise is gone. Guardrail 5: every skip path gets a
    // counter, because a proof that cannot see what it skipped reads as passing.
    std::size_t scaled_refused = 0;
};

// Create a static body for every `GroundSurface` that does not already have a live `GroundBody`,
// from `derive_collider`, at the entity's `WorldTransform` (its `LocalTransform` if it has none —
// destruction's rule, for the same reason: a ground may be parented).
//
// PER PEER. Level geometry is stood up independently by the server and by each client rather than
// replicated — that is what lets a client predict standing on something without waiting to be told
// the floor exists — so this runs once on each, and the derivation being pure is what makes the two
// agree. Idempotent: an entity with a live `GroundBody` is skipped, so re-running costs one query.
[[nodiscard]] BindStats bind_ground(ecs::World& world, physics::PhysicsWorld& physics);

// Upload the drawn surface and give it a look. Split from `bind_ground` because it needs a device,
// exactly as `upload_prop_meshes` is split from `build_palette` — a headless peer binds and never
// applies.
[[nodiscard]] std::size_t
apply_ground(ecs::World& world, render::MeshRegistry& meshes, render::MaterialId material);

} // namespace rime::ground
