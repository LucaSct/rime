// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/reflect.hpp" // core::Transform reflection — see the note at the bottom
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

// ── Reflection, deliberately in THIS header and not a separate one ──────────────────────────────
//
// core/math/reflect.hpp splits the math value types' reflection out of vec.hpp/quat.hpp on purpose,
// and that is the right call for them. It is the WRONG call for a COMPONENT, and the difference is
// one line in the ECS: `World::add_component` registers a component implicitly —
//     add_component_raw(e, register_component<T>())            (ecs/world.hpp)
// — so the first `spawn_with(LocalTransform{...})` anywhere in the program registers the type, and
// `register_component<T>` is a TEMPLATE instantiated in whichever translation unit did that. A TU
// that can see LocalTransform but not its ReflectionTraits specialization instantiates the same
// weak symbol against the PRIMARY (unreflected) template, the linker folds the two definitions
// into one and keeps either, and the binary that loses the coin toss has a LocalTransform with no
// reflection at all. Nothing diagnoses this: it is an ODR violation, so the compiler is not
// required to notice and GCC does not.
//
// Measured, 2026-10-23 (PR #348): `samples/99-the-block/play_city.cpp` spawns LocalTransform and
// did not include ecs/reflect.hpp. It links into `the-block-host` and not into the engine's own
// host, so the two hosts disagreed about the same scene — the game's host reported
// "213 component(s) skipped — this build does not register: rime::ecs::LocalTransform" and one
// fewer schema type, while the engine's host, which registers strictly LESS, read the scene fine.
// Keeping the specialization next to the declaration makes that state unreachable: a TU cannot see
// the component without it.
//
// WorldTransform is NOT reflected, and must not be: it is DERIVED state (propagate_transforms
// recomputes it from the LocalTransform chain every tick), so it is never persisted to a scene,
// never rides an editor snapshot and never enters world_content_hash — exactly like
// physics::RigidBodyHandle. Register it as a live component where a renderer needs to query it; it
// simply will not be serialized. That is what makes the editor snapshot carry exactly one editable
// placement. (It used to carry a second reason — WorldTransform is structurally identical to
// LocalTransform, and compute_type_hash folded only the FIELDS, so the two collided on one
// type_hash. That collision is gone, since the hash folds the type NAME now (ADR-0033 amendment
// A2), but the derived-state reason stands on its own.)
RIME_REFLECT_BEGIN(rime::ecs::LocalTransform)
RIME_REFLECT_FIELD(value)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::ecs::Parent)
RIME_REFLECT_FIELD(value)
RIME_REFLECT_END()
