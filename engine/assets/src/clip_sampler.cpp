// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <algorithm>
#include <cmath>
#include <vector>

#include "rime/assets/clip_asset.hpp"
#include "rime/core/math/mat.hpp"

// CPU animation sampling (M6.7, AN0). See clip_asset.hpp for the shape and docs/math/skinning.md
// for the math. Everything here is deterministic and device-free.
namespace rime::assets {
namespace {

using core::Mat4;
using core::Quat;
using core::Transform;
using core::Vec3;

// Map a raw sample time into the clip's range. Clamp holds the endpoints; Loop wraps modulo the
// duration (and folds a negative time back in, since std::fmod keeps the sign of the dividend).
float map_time(float t, float duration, TimePolicy policy) noexcept {
    if (duration <= 0.0f) {
        return 0.0f;
    }
    if (policy == TimePolicy::Loop) {
        float wrapped = std::fmod(t, duration);
        if (wrapped < 0.0f) {
            wrapped += duration;
        }
        return wrapped;
    }
    return std::clamp(t, 0.0f, duration);
}

// The keyframe interval bracketing time t: indices (i0, i1) and the fraction `alpha` in [0,1] from
// i0 to i1. Outside the track we hold the nearest endpoint (i0 == i1, alpha 0) — no extrapolation.
// `times` is strictly increasing and non-empty (the caller checks the empty-channel case first).
struct KeySpan {
    std::size_t i0;
    std::size_t i1;
    float alpha;
};

KeySpan bracket(const std::vector<float>& times, float t) noexcept {
    const std::size_t n = times.size();
    // upper_bound → first key strictly greater than t; a binary search, so long tracks stay cheap.
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    if (it == times.begin()) {
        return {0, 0, 0.0f}; // at or before the first key
    }
    if (it == times.end()) {
        return {n - 1, n - 1, 0.0f}; // at or after the last key
    }
    const std::size_t i1 = static_cast<std::size_t>(it - times.begin());
    const std::size_t i0 = i1 - 1;
    const float span = times[i1] - times[i0];
    const float alpha = span > 0.0f ? (t - times[i0]) / span : 0.0f;
    return {i0, i1, alpha};
}

Vec3 lerp(Vec3 a, Vec3 b, float t) noexcept {
    return a + (b - a) * t;
}

Vec3 sample_vec3(const Channel<Vec3>& ch, float t) noexcept {
    const KeySpan k = bracket(ch.times, t);
    if (ch.interp == Interpolation::Step || k.i0 == k.i1) {
        return ch.values[k.i0];
    }
    return lerp(ch.values[k.i0], ch.values[k.i1], k.alpha);
}

Quat sample_quat(const Channel<Quat>& ch, float t) noexcept {
    const KeySpan k = bracket(ch.times, t);
    if (ch.interp == Interpolation::Step || k.i0 == k.i1) {
        return ch.values[k.i0];
    }
    // Normalized lerp along the SHORTEST arc. A quaternion and its negation are the same rotation
    // (the SU(2)→SO(3) double cover), so if the two keys are on opposite hemispheres (dot < 0) we
    // flip one first — otherwise the blend would swing the long way round (and, for exact
    // opposites, pass through zero). nlerp (not slerp) is the AN0 choice: cheap, and its slight
    // non-constant angular velocity is imperceptible between typical keyframes; slerp is the
    // quality seam AN1 can opt into.
    Quat a = ch.values[k.i0];
    Quat b = ch.values[k.i1];
    if (core::dot(a, b) < 0.0f) {
        b = -b;
    }
    return core::normalize(a * (1.0f - k.alpha) + b * k.alpha);
}

} // namespace

Transform sample_joint_local(const JointAnimation& anim, const Transform& bind, float t) noexcept {
    // Each silent channel falls back to the bind-local value, so a clip only overrides what it
    // moves.
    Transform out;
    out.translation =
        anim.translation.empty() ? bind.translation : sample_vec3(anim.translation, t);
    out.rotation = anim.rotation.empty() ? bind.rotation : sample_quat(anim.rotation, t);
    out.scale = anim.scale.empty() ? bind.scale : sample_vec3(anim.scale, t);
    return out;
}

std::size_t sample_clip(const Clip& clip,
                        const Skeleton& skeleton,
                        float t,
                        TimePolicy policy,
                        std::span<Mat4> out) noexcept {
    const std::size_t n = skeleton.joint_count();
    if (n == 0 || clip.joint_count() != n || out.size() < n) {
        return 0; // shape mismatch — a clean status, not a throw (frame-code discipline)
    }
    const float mt = map_time(t, clip.duration, policy);

    // World placements built parent-first. The topological joint order (a parent's index precedes
    // its children's) is what lets this be one forward pass — a child reads its already-computed
    // parent. We compose in MATRIX space (not TRS-Transform space) because a parent's non-uniform
    // scale shears a child in a way a single T·R·S cannot represent; matrices carry that correctly,
    // and it is exactly what GPU skinning (AN1) will do. (The scratch allocation is AN0-acceptable;
    // AN1's per-frame path takes caller-provided scratch.)
    std::vector<Mat4> world(n);
    for (std::size_t j = 0; j < n; ++j) {
        const Joint& joint = skeleton.joints[j];
        const Mat4 local_mat =
            core::to_matrix(sample_joint_local(clip.joints[j], joint.local_bind, mt));
        world[j] = joint.parent == Joint::kNoParent
                       ? local_mat
                       : world[static_cast<std::size_t>(joint.parent)] * local_mat;
        // The palette entry: model space → (bind-local via inverse bind) → animated world.
        out[j] = world[j] * joint.inverse_bind;
    }
    return n;
}

} // namespace rime::assets
