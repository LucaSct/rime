// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "rime/assets/skeleton_asset.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"

// An animation clip and the CPU sampler that evaluates it (M6.7, AN0). A clip stores, per joint,
// keyframed translation / rotation / scale tracks; `sample_clip` evaluates them at a time t into a
// skinning palette (one Mat4 per joint). This is pure CPU and GPU-free by design — correctness
// first; per-frame GPU palette skinning is AN1 (M7), which reuses this exact palette as its input.
//
// The cooked layout is deliberately **columnar per joint** (separate times[] / values[] arrays),
// shaped for the sampler's inner loop, NOT for glTF's per-accessor channel/sampler form — designing
// the runtime format around the source would be the "glTF-shaped" trap the asset model warns
// against.
namespace rime::assets {

// How a track holds its value between keyframes (matching glTF's two non-cubic modes).
enum class Interpolation : std::uint8_t {
    Step = 0,   // hold the previous keyframe's value (glTF STEP) — discrete/mechanical motion
    Linear = 1, // blend between neighbours (glTF LINEAR): lerp for vectors, nlerp for quaternions
};

// One keyframed track for one joint: strictly-increasing `times` and the parallel `values`. An
// empty track means "this joint does not animate this component" — the sampler falls back to the
// skeleton's bind-local value, so a clip only pays for the channels it actually moves.
template <class T> struct Channel {
    Interpolation interp = Interpolation::Linear;
    std::vector<float> times;
    std::vector<T> values;

    [[nodiscard]] bool empty() const noexcept { return times.empty(); }
};

// One joint's animation: separate T / R / S tracks. The index into Clip::joints matches the
// skeleton joint index, so no name lookup happens at sample time.
struct JointAnimation {
    Channel<core::Vec3> translation;
    Channel<core::Quat> rotation;
    Channel<core::Vec3> scale;
};

// A clip: per-joint TRS tracks plus a duration (seconds). Looping vs clamping is a *sample-time*
// policy, not baked into the data — the same clip can play either way.
struct Clip {
    float duration = 0.0f;
    std::vector<JointAnimation> joints; // one entry per skeleton joint, by index

    [[nodiscard]] std::size_t joint_count() const noexcept { return joints.size(); }
};

// How a sample time outside [0, duration] is treated.
enum class TimePolicy : std::uint8_t {
    Clamp = 0, // hold the endpoints (a one-shot clip that stops on its last pose)
    Loop = 1,  // wrap modulo duration (a cycle) — t and t + duration sample identically
};

// Sample one joint's *local* pose at time t, falling back to `bind` for any silent channel. Exposed
// for tests/tooling; `sample_clip` calls it per joint. `t` is assumed already mapped into range.
[[nodiscard]] core::Transform
sample_joint_local(const JointAnimation& anim, const core::Transform& bind, float t) noexcept;

// Sample a clip at time `t` into a skinning palette: `out[j] = world_j · inverseBind_j`, where
// `world_j` is joint j's animated local pose composed down the tree (parent-first, which the
// topological joint order makes a single forward pass). `t` is mapped into range per `policy`
// first. `out` must hold at least `skeleton.joint_count()` matrices; returns the number written, or
// 0 on a shape mismatch (clip and skeleton disagree on joint count, or `out` is too small) — a
// status, not a throw (frame-code discipline). Derivation: docs/math/skinning.md.
[[nodiscard]] std::size_t sample_clip(const Clip& clip,
                                      const Skeleton& skeleton,
                                      float t,
                                      TimePolicy policy,
                                      std::span<core::Mat4> out) noexcept;

} // namespace rime::assets
