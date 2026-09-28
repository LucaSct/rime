// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

namespace rime::core {
class JobSystem;
}

namespace rime::ecs {
class World;
}

namespace rime::scene {

// The "load then derive" half of a reflection-driven world reconstruction. A `.rscene` load (or a
// play-session restore) only ever carries LocalTransform — WorldTransform is DERIVED state and
// deliberately unreflected, so it cannot ride a snapshot, and `ecs::propagate_transforms` only
// recomposes entities that already carry BOTH components. So after a load nothing has a world pose
// unless someone adds the missing component first. This is the shared answer: give every
// LocalTransform holder that lacks one a default WorldTransform, then compose the (possibly
// parented) local chain into world space. See derive_transforms.cpp for why the add happens after
// the entities are collected.
void derive_world_transforms(ecs::World& world, core::JobSystem& jobs);

} // namespace rime::scene
