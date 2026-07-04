// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/entity.hpp"

// The transform hierarchy: parent/child placement composed into world space. Three components and
// one pass. An entity's LocalTransform is its placement RELATIVE to its parent; its WorldTransform
// is the absolute placement the renderer / physics consume; Parent names the entity it hangs off
// (absent, or kNullEntity, = a root). The pass computes, for every entity that has both a
// LocalTransform and a WorldTransform,
//     world = parent.world * local     (a child — core::Transform's compose operator does the work)
//     world = local                    (a root)
// The one subtlety is ORDER: a child must compose against its parent's ALREADY-updated world, so
// the pass walks the hierarchy depth by depth (roots first) and updates each level in parallel on
// the job system — every parent at a shallower level has finished before its children start. See
// docs/math/transform-hierarchy.md for the composition math and the ordering argument.
namespace rime::ecs {

class World;

// Placement relative to the parent (or to world space, for a root) — the authored/edited transform.
struct LocalTransform {
    core::Transform value;
};

// Absolute world-space placement — LocalTransform composed down the parent chain. Written by
// propagate_transforms; read by everything downstream (render, physics).
struct WorldTransform {
    core::Transform value;
};

// The parent this entity hangs off. kNullEntity — or having no Parent component at all — means
// root.
struct Parent {
    Entity value = kNullEntity;
};

// Recompute WorldTransform for every entity that has {LocalTransform, WorldTransform}, composing
// down the parent chain (world = parent.world * local; roots: world = local). Processes the
// hierarchy depth by depth so a child always composes against its parent's updated world, and
// updates each level in parallel on `jobs`. A flat scene (no Parent components in play) takes a
// fully-parallel fast path.
void propagate_transforms(World& world, core::JobSystem& jobs);

} // namespace rime::ecs
