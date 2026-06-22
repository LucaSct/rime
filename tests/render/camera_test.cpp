// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the orbit-camera brick (A2). The camera is pure rime::core math, so its whole contract is
// checked headless here — no window, no GPU: orbit angles place the eye on the sphere, the pole guard
// clamps pitch, zoom/pan behave, the projection centers the target and orders depth correctly, the
// fit-to-sphere framing keeps a part on screen, and a z-up world (ICEM's convention) also works.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>

#include "camera.hpp" // samples/03-icem-viewer, on the test's include path (see CMakeLists.txt)

using rime::core::Vec3;
using rime::core::Vec4;
using rime::viewer::OrbitCamera;

namespace {

// Project a world point to normalized device coordinates (clip ÷ w). Vulkan NDC: x,y ∈ [-1,1] on
// screen, z ∈ [0,1] from near to far.
Vec3 to_ndc(const OrbitCamera& cam, float aspect, Vec3 p) {
    const Vec4 clip = cam.view_proj(aspect) * Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
}

} // namespace

TEST_CASE("orbit camera: default front view places the eye on +Z") {
    OrbitCamera cam;
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.distance = 5.0f; // yaw = pitch = 0

    const Vec3 e = cam.eye();
    CHECK(e.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(e.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(e.z == doctest::Approx(5.0f).epsilon(0.001));

    const Vec3 f = cam.forward(); // looks toward the target, i.e. down −Z
    CHECK(f.z == doctest::Approx(-1.0f).epsilon(0.001));
}

TEST_CASE("orbit camera: yawing 90 degrees swings the eye onto +X") {
    OrbitCamera cam;
    cam.distance = 5.0f;
    cam.orbit(rime::core::kHalfPi, 0.0f);

    const Vec3 e = cam.eye();
    CHECK(e.x == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(e.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(e.z == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("orbit camera: pitch is clamped off the pole and stays finite") {
    OrbitCamera cam;
    cam.distance = 5.0f;
    cam.orbit(0.0f, 10.0f); // way past vertical

    CHECK(cam.pitch == doctest::Approx(cam.pitch_limit));

    const Vec3 e = cam.eye();
    CHECK(std::isfinite(e.x));
    CHECK(std::isfinite(e.y));
    CHECK(std::isfinite(e.z));
    CHECK(e.y > 4.0f); // looking almost straight down: the eye is high above the target

    // The view matrix must remain well-defined (no NaNs from a degenerate look_at basis).
    const rime::core::Mat4 v = cam.view();
    for (float m : v.m) CHECK(std::isfinite(m));
}

TEST_CASE("orbit camera: zoom is multiplicative and reversible") {
    OrbitCamera cam;
    cam.distance = 5.0f;

    cam.zoom(1.0f); // one notch closer → 10% nearer
    CHECK(cam.distance == doctest::Approx(4.5f));

    cam.zoom(-1.0f); // back out
    CHECK(cam.distance == doctest::Approx(5.0f));

    // Distance never collapses to zero or runs away.
    cam.zoom(1000.0f);
    CHECK(cam.distance >= cam.min_distance);
}

TEST_CASE("orbit camera: panning moves the target in the view plane only") {
    OrbitCamera cam;
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.distance = 5.0f; // front view: forward −Z, right +X, up +Y

    const float yaw0 = cam.yaw, pitch0 = cam.pitch, dist0 = cam.distance;
    cam.pan(1.0f, 0.0f); // drag right by one screen-unit

    // Target slides along −X (so the model appears to move right); orientation/zoom unchanged.
    CHECK(cam.target.x == doctest::Approx(-5.0f).epsilon(0.001));
    CHECK(cam.target.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(cam.target.z == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(cam.yaw == doctest::Approx(yaw0));
    CHECK(cam.pitch == doctest::Approx(pitch0));
    CHECK(cam.distance == doctest::Approx(dist0));
}

TEST_CASE("orbit camera: projection centers the target and orders depth near→far") {
    OrbitCamera cam;
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.distance = 5.0f;
    const float aspect = 1.0f;

    // The target sits at the center of the view.
    const Vec3 c = to_ndc(cam, aspect, cam.target);
    CHECK(c.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(c.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(c.z > 0.0f);
    CHECK(c.z < 1.0f);

    // A point farther from the eye (beyond the target along the view ray) has greater NDC depth.
    const Vec3 farther = cam.target + cam.forward() * 1.0f;
    const Vec3 nf = to_ndc(cam, aspect, farther);
    CHECK(nf.z > c.z);
}

TEST_CASE("orbit camera: frame() fits a bounding sphere within the view") {
    OrbitCamera cam;
    const Vec3 center{1.0f, 2.0f, 3.0f};
    const float radius = 2.0f;
    const float aspect = 1.0f;
    cam.frame(center, radius, aspect);

    CHECK(cam.target.x == doctest::Approx(center.x));
    CHECK(cam.target.y == doctest::Approx(center.y));
    CHECK(cam.target.z == doctest::Approx(center.z));

    // A point on the sphere's silhouette (offset by the radius along the screen-up direction) projects
    // inside the viewport — the part is fully visible, not clipped.
    const Vec3 top = center + cam.up() * radius;
    const Vec3 n = to_ndc(cam, aspect, top);
    CHECK(std::fabs(n.y) < 1.0f);
    CHECK(std::fabs(n.x) < 1.0f);
}

TEST_CASE("orbit camera: a z-up world (ICEM convention) also works") {
    OrbitCamera cam;
    cam.world_up = {0.0f, 0.0f, 1.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.distance = 5.0f; // yaw = pitch = 0

    const Vec3 e = cam.eye(); // front view with z-up looks down −X
    CHECK(e.x == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(e.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(e.z == doctest::Approx(0.0f).epsilon(0.001));

    // The target still centers, and the matrices are finite.
    const Vec3 c = to_ndc(cam, 1.0f, cam.target);
    CHECK(c.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(c.y == doctest::Approx(0.0f).epsilon(0.001));
}
