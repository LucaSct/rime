// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math/transform.hpp"

// The pose to DRAW this frame, when that differs from the pose the simulation last computed.
//
// WHY THIS EXISTS. A fixed simulation tick and a free-running render frame do not line up
// (ADR-0023 §1): the renderer usually sits somewhere BETWEEN two ticks, and drawing the newest tick
// regardless is what makes a 60 Hz sim judder on a 144 Hz display. The fix is to draw a blend of
// the previous and current states — but the blend is a function of the frame's interpolation
// `alpha`, which is wall-clock-derived and therefore not something simulation state may ever
// contain. So the blend cannot live in `WorldTransform`: that is the SIMULATED pose, recomputed
// every tick by propagate_transforms and read by physics and gameplay. It gets its own component,
// and the split between the two is the whole point.
//
// WHY IT LIVES IN `ecs`. The producer and the consumer are in modules that cannot see each other.
// `rime::render` links rhi/ecs/assets; `rime::replication` links ecs/net; neither depends on the
// other, and neither should — a renderer that knew about netcode would be a layering mistake we
// would pay for at every future backend. `ecs` is the one module both already stand on, so a
// component here is how they agree on a pose without ever meeting. Any future producer of smoothed
// motion (client-side prediction, an animation blend, a camera shake pass) uses the same seam.
//
// UNREFLECTED, DELIBERATELY — the same reasoning as WorldTransform (see transform.hpp): derived,
// local, presentation-only state. It is never persisted to a scene, never rides a snapshot, never
// enters world_content_hash, and never crosses the wire. On the network side that last point is
// load-bearing rather than incidental: an unreflected component stays out of
// ecs::component_schema_hash, which is what lets a CLIENT carry this component while the server
// does not and still pass the handshake.
//
// THE CONTRACT, in both directions:
//   - `WorldTransform` is always the simulated truth. Physics, gameplay and anything else inside
//     the tick read that and only that.
//   - `RenderTransform`, WHERE PRESENT, is what to actually draw. It is absent on almost every
//     entity — nothing in `ecs` itself ever adds it — so a renderer treats it as an override and
//     falls back to WorldTransform, which costs an entity that never blends exactly nothing.
//   - It is written on the RENDER path and read on the RENDER path. Nothing in PreSim, Schedule,
//     PostSim or Publish may touch it. That is what keeps a wall-clock-derived value out of the
//     deterministic tick; see docs/design/replication.md.
namespace rime::ecs {

struct RenderTransform {
    core::Transform value;
};

} // namespace rime::ecs
