// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/scalar.hpp"
#include "rime/core/math/vec.hpp"

// A turntable / orbit camera for the ICEM viewer. The camera looks at a `target` point and orbits it
// on a sphere — yaw spins about the world-up axis, pitch raises/lowers the eye — and it can dolly
// (zoom) along the view ray and pan the target in the view plane. This is the canonical "inspect a
// part" camera every CAD/DCC tool uses: unlike a free-fly camera it keeps the model's up-axis fixed,
// and unlike an arcball it never rolls, so an engineering part is never left tilted.
//
// It is *pure* rime::core math — no platform or RHI dependency — so the application feeds it abstract
// input deltas (orbit/pan/zoom) and the camera is fully unit-testable with no window or GPU. The
// spherical-coordinate placement and the fit-to-sphere derivation are written out in
// docs/math/orbit-camera.md. (A candidate to promote into engine/render once M5 builds that module.)
namespace rime::viewer {

using core::Mat4;
using core::Vec3;

struct OrbitCamera {
    // ── Orbit state ──────────────────────────────────────────────────────────────────────────
    Vec3 target{0.0f, 0.0f, 0.0f};   // the point we orbit and look at (world space)
    float distance = 5.0f;           // eye-to-target distance — the orbit radius
    float yaw = 0.0f;                // azimuth about world_up [rad]
    float pitch = 0.0f;              // elevation above the horizon plane [rad], clamped off the poles
    Vec3 world_up{0.0f, 1.0f, 0.0f}; // the axis yaw spins about (for ICEM's z-up parts set {0,0,1})

    // ── Lens ─────────────────────────────────────────────────────────────────────────────────
    float fov_y = core::radians(50.0f); // vertical field of view [rad]
    float z_near = 0.01f;
    float z_far = 1000.0f;

    // ── Limits / sensitivities (tunable by the app) ──────────────────────────────────────────
    float min_distance = 1e-3f;
    float max_distance = 1e6f;
    // Keep pitch a hair off ±90°: at the exact pole the view ray is parallel to world_up and look_at
    // is undefined (its basis collapses). This is the standard turntable singularity guard.
    float pitch_limit = core::kHalfPi - 0.01f;

    // ── Input verbs (the app maps mouse/scroll to these) ─────────────────────────────────────
    // Orbit by angle deltas [rad]; pitch is clamped to keep the camera off the poles.
    void orbit(float d_yaw, float d_pitch) noexcept {
        yaw += d_yaw;
        pitch = std::clamp(pitch + d_pitch, -pitch_limit, pitch_limit);
    }

    // Zoom multiplicatively: each positive `steps` moves the eye 10% closer, each negative 10% farther.
    // Multiplicative (not additive) zoom feels uniform at every scale — the same notch covers the same
    // *fraction* of the distance whether you are near or far.
    void zoom(float steps) noexcept {
        distance = std::clamp(distance * std::pow(0.9f, steps), min_distance, max_distance);
    }

    // Pan the target in the camera's view plane. Deltas are in the screen's right/up directions; we
    // scale by `distance` so a drag covers a consistent fraction of the view regardless of zoom.
    void pan(float d_right, float d_up, float speed = 1.0f) noexcept {
        const Basis b = view_basis();
        target += (b.right * (-d_right) + b.up * d_up) * (speed * distance);
    }

    // Frame a bounding sphere with a square viewport (fits the vertical field of view). For a
    // non-square viewport, prefer the aspect-aware overload so the part is not clipped left/right.
    void frame(Vec3 center, float radius) noexcept { frame(center, radius, 1.0f); }

    // Aspect-aware framing: center the target on the sphere and back off until it fits whichever of
    // the horizontal/vertical fields of view is tighter (aspect = width / height). The half-angle θ
    // that just contains a sphere of radius r at distance d satisfies sin θ = r / d, so d = r / sin θ;
    // a small margin leaves breathing room. Derivation: docs/math/orbit-camera.md.
    void frame(Vec3 center, float radius, float aspect, float margin = 1.1f) noexcept {
        target = center;
        const float half_y = 0.5f * fov_y;
        const float half_x = std::atan(std::tan(half_y) * aspect); // horizontal half-fov
        const float theta = std::min(half_y, half_x);
        distance = std::clamp(radius / std::sin(theta) * margin, min_distance, max_distance);
    }

    // ── Derived geometry ─────────────────────────────────────────────────────────────────────
    // Offset from target to eye: a point on the orbit sphere in spherical coordinates, expressed in a
    // basis built from world_up so both y-up and z-up work without special-casing (docs/math).
    [[nodiscard]] Vec3 eye() const noexcept {
        const Basis b = orbit_basis();
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const Vec3 offset = ((b.right * sy + b.forward * cy) * cp + b.up * sp) * distance;
        return target + offset;
    }

    [[nodiscard]] Vec3 forward() const noexcept { return core::normalize(target - eye()); }
    [[nodiscard]] Vec3 right() const noexcept {
        return core::normalize(core::cross(forward(), core::normalize(world_up)));
    }
    [[nodiscard]] Vec3 up() const noexcept { return core::cross(right(), forward()); }

    [[nodiscard]] Mat4 view() const noexcept {
        return core::look_at(eye(), target, core::normalize(world_up));
    }
    [[nodiscard]] Mat4 proj(float aspect) const noexcept {
        return core::perspective(fov_y, aspect, z_near, z_far);
    }
    // Clip-from-world: clip = proj * view * world (column-vector convention).
    [[nodiscard]] Mat4 view_proj(float aspect) const noexcept { return proj(aspect) * view(); }

private:
    struct Basis {
        Vec3 right;
        Vec3 forward;
        Vec3 up;
    };

    // An orthonormal basis whose `up` is world_up and whose right/forward span the horizon plane. The
    // seed axis (a world axis not parallel to up) only fixes which direction yaw = 0 faces, so y-up
    // gives the familiar "yaw 0 looks down −Z" and z-up gives "yaw 0 looks down −X".
    [[nodiscard]] Basis orbit_basis() const noexcept {
        const Vec3 up = core::normalize(world_up);
        const Vec3 seed = (std::fabs(up.y) < 0.99f) ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 forward = core::normalize(core::cross(seed, up));
        const Vec3 right = core::normalize(core::cross(up, forward));
        return {right, forward, up};
    }

    // The camera-relative basis (screen right/up + view forward) used for panning.
    [[nodiscard]] Basis view_basis() const noexcept {
        const Vec3 f = forward();
        const Vec3 r = core::normalize(core::cross(f, core::normalize(world_up)));
        const Vec3 u = core::cross(r, f);
        return {r, f, u};
    }
};

} // namespace rime::viewer
