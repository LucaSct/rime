// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the M6.7 CPU animation sampler (AN0). A 2-bone skeleton whose bind pose and
// inverse-bind matrices are chosen so the palette is hand-computable: root A at the origin, child B
// two units along +X, both at rest. sample_clip is checked at t = 0 / half / end against paper
// values — exact for STEP and translation LINEAR, to tolerance for quaternion nlerp. Deliberately
// GPU-free (docs/math/skinning.md derives the equation). The neighborhood case uses q and −q — the
// same rotation — to catch the quaternion double-cover bug a naive lerp hits.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>

#include "rime/assets/clip_asset.hpp"
#include "rime/assets/skeleton_asset.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"

using namespace rime::assets;
using rime::core::Mat4;
using rime::core::Quat;
using rime::core::quat_from_axis_angle;
using rime::core::Transform;
using rime::core::Vec3;
using rime::core::Vec4;

namespace {

constexpr float kHalfPi = 1.57079633f;

// Root A at the origin (identity bind, identity inverse-bind); child B two units along +X. B's
// inverse-bind is the inverse of its bind-world placement, so the bind-pose palette is exactly
// identity — the invariant the first test pins.
Skeleton two_bone_skeleton() {
    Skeleton s;
    Joint a;
    a.parent = Joint::kNoParent;
    a.name_hash = 0xAu;
    // a.local_bind and a.inverse_bind default to identity.

    Joint b;
    b.parent = 0;
    b.name_hash = 0xBu;
    Transform b_local;
    b_local.translation = Vec3{2.0f, 0.0f, 0.0f};
    b.local_bind = b_local;
    // A is identity, so B's bind world = to_matrix(b_local); its inverse is
    // to_matrix(inverse(b_local)).
    b.inverse_bind = rime::core::to_matrix(rime::core::inverse(b_local));

    s.joints = {a, b};
    return s;
}

// A clip with `n` silent joints (every channel empty) → sampling reproduces the bind pose.
Clip static_clip(std::size_t n, float duration) {
    Clip c;
    c.duration = duration;
    c.joints.resize(n);
    return c;
}

Vec3 translation_of(const Mat4& m) {
    return {m.m[12], m.m[13], m.m[14]}; // column-major: the 4th column is the translation
}

} // namespace

TEST_CASE("sample_clip: a static clip yields the identity palette (bind pose invariant)") {
    const Skeleton sk = two_bone_skeleton();
    const Clip clip = static_clip(2, 1.0f);
    std::array<Mat4, 2> pal{};
    CHECK(sample_clip(clip, sk, 0.3f, TimePolicy::Clamp, pal) == 2);
    const Mat4 identity{};
    for (const Mat4& m : pal) {
        for (int i = 0; i < 16; ++i) {
            CHECK(m.m[i] == doctest::Approx(identity.m[i]));
        }
    }
}

TEST_CASE("sample_clip: LINEAR translation lerps exactly and propagates down the tree") {
    const Skeleton sk = two_bone_skeleton();
    Clip clip;
    clip.duration = 1.0f;
    clip.joints.resize(2);
    clip.joints[0].translation.interp = Interpolation::Linear;
    clip.joints[0].translation.times = {0.0f, 1.0f};
    clip.joints[0].translation.values = {Vec3{0, 0, 0}, Vec3{6, 0, 0}};

    std::array<Mat4, 2> pal{};
    REQUIRE(sample_clip(clip, sk, 0.5f, TimePolicy::Clamp, pal) == 2);
    // A moves +3 at t=0.5; B rigidly follows, so relative to each one's bind both palettes are
    // translate(+3, 0, 0) — the tree compose + inverse bind working together.
    const Vec3 ta = translation_of(pal[0]);
    CHECK(ta.x == doctest::Approx(3.0f));
    CHECK(std::abs(ta.y) < 1e-5f);
    CHECK(std::abs(ta.z) < 1e-5f);
    const Vec3 tb = translation_of(pal[1]);
    CHECK(tb.x == doctest::Approx(3.0f));
    CHECK(std::abs(tb.y) < 1e-5f);
}

TEST_CASE("sample_clip: STEP holds the previous keyframe") {
    const Skeleton sk = two_bone_skeleton();
    Clip clip;
    clip.duration = 1.0f;
    clip.joints.resize(2);
    auto& rot = clip.joints[0].rotation;
    rot.interp = Interpolation::Step;
    rot.times = {0.0f, 0.5f, 1.0f};
    rot.values = {rime::core::quat_identity(),
                  quat_from_axis_angle(Vec3{0, 0, 1}, kHalfPi),
                  quat_from_axis_angle(Vec3{0, 0, 1}, 2.0f * kHalfPi)};

    std::array<Mat4, 2> pal{};
    // t=0.3 holds key0 (identity): (1,0,0) is unrotated.
    REQUIRE(sample_clip(clip, sk, 0.3f, TimePolicy::Clamp, pal) == 2);
    const Vec4 x0 = pal[0] * Vec4{1, 0, 0, 1};
    CHECK(x0.x == doctest::Approx(1.0f));
    CHECK(std::abs(x0.y) < 1e-4f);
    // t=0.7 holds key1 (90° about +Z): (1,0,0) → (0,1,0), NOT the key2 180° pose.
    REQUIRE(sample_clip(clip, sk, 0.7f, TimePolicy::Clamp, pal) == 2);
    const Vec4 x1 = pal[0] * Vec4{1, 0, 0, 1};
    CHECK(std::abs(x1.x) < 1e-4f);
    CHECK(x1.y == doctest::Approx(1.0f));
}

TEST_CASE("sample_clip: nlerp takes the shortest arc across the quaternion double cover") {
    const Skeleton sk = two_bone_skeleton();
    const Quat q = quat_from_axis_angle(Vec3{0, 0, 1}, kHalfPi); // 90° about +Z
    Clip clip;
    clip.duration = 1.0f;
    clip.joints.resize(2);
    auto& rot = clip.joints[0].rotation;
    rot.interp = Interpolation::Linear;
    rot.times = {0.0f, 1.0f};
    rot.values = {q, -q}; // −q is the SAME rotation; a naive lerp midpoint is 0 → identity (wrong)

    std::array<Mat4, 2> pal{};
    REQUIRE(sample_clip(clip, sk, 0.5f, TimePolicy::Clamp, pal) == 2);
    // Neighborhood fix keeps the 90° rotation: (1,0,0) → (0,1,0). Without it, the pose collapses to
    // identity and (1,0,0) would stay put.
    const Vec4 xr = pal[0] * Vec4{1, 0, 0, 1};
    CHECK(std::abs(xr.x) < 0.01f);
    CHECK(xr.y == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("sample_clip: Loop wraps the time, Clamp holds the endpoint") {
    const Skeleton sk = two_bone_skeleton();
    Clip clip;
    clip.duration = 1.0f;
    clip.joints.resize(2);
    clip.joints[0].translation.interp = Interpolation::Linear;
    clip.joints[0].translation.times = {0.0f, 1.0f};
    clip.joints[0].translation.values = {Vec3{0, 0, 0}, Vec3{4, 0, 0}};

    std::array<Mat4, 2> at_half{}, past_end_loop{}, past_end_clamp{};
    REQUIRE(sample_clip(clip, sk, 0.5f, TimePolicy::Loop, at_half) == 2); // → 2.0
    REQUIRE(sample_clip(clip, sk, 1.5f, TimePolicy::Loop, past_end_loop) ==
            2); // wraps to 0.5 → 2.0
    REQUIRE(sample_clip(clip, sk, 1.5f, TimePolicy::Clamp, past_end_clamp) ==
            2); // clamps to 1.0 → 4.0
    CHECK(translation_of(at_half[0]).x == doctest::Approx(2.0f));
    CHECK(translation_of(past_end_loop[0]).x == doctest::Approx(2.0f));
    CHECK(translation_of(past_end_clamp[0]).x == doctest::Approx(4.0f));
}

TEST_CASE("sample_clip: a shape mismatch returns 0") {
    const Skeleton sk = two_bone_skeleton();
    const Clip clip = static_clip(2, 1.0f);
    std::array<Mat4, 1> too_small{};
    CHECK(sample_clip(clip, sk, 0.0f, TimePolicy::Clamp, too_small) == 0); // output too small
    const Clip wrong_joint_count = static_clip(3, 1.0f);
    std::array<Mat4, 2> pal{};
    CHECK(sample_clip(wrong_joint_count, sk, 0.0f, TimePolicy::Clamp, pal) == 0);
}

TEST_CASE("skeleton: name lookup and topological-order validation") {
    Skeleton sk = two_bone_skeleton();
    CHECK(sk.find(0xAu) == 0);
    CHECK(sk.find(0xBu) == 1);
    CHECK(sk.find(0xCu) == Joint::kNoParent);
    CHECK(sk.is_topologically_ordered());
    sk.joints[0].parent = 1; // a root that claims a parent appearing after it — not a valid order
    CHECK_FALSE(sk.is_topologically_ordered());
}
