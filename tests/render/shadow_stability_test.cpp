// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Numerical stability of the cascaded-shadow fit (compute_cascades), GPU-free.
//
// The shimmer property is: a FIXED world point lands on the SAME sub-texel position of a cascade's
// shadow map no matter where the camera has walked or turned, so the shadow's texel grid is pinned
// to the world. We test exactly that, on the quantity a pixel sees — the fractional part of the
// point's shadow-map texel coordinate — and NOT by comparing matrices: two matrices that differ by
// 1e-3 can place a texel centre half a texel apart, and a snapped fit may legitimately jump by
// whole texels (which changes the matrix a lot and the phase not at all).
//
// The oracle is deliberately not the production formula. It never looks at the fit's radius or
// centre; it only projects a world point through the returned matrix and asks "is the phase
// constant?". The sensitivity controls then break the fit three different ways (centre not snapped,
// radius jittering, light basis wobbling) and require the same oracle to FAIL — a proof that cannot
// fail on a known-bad input proves nothing (see the project's near-null-proof rule).

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/lighting/shadows.hpp"

using namespace rime;
using namespace rime::render;

namespace {

constexpr float kFovY = 0.87266f;
constexpr float kAspect = 16.0f / 9.0f;
constexpr float kNear = 0.1f;
constexpr float kFar = 300.0f;

// A walking-and-turning camera: the pose sequence a player produces, not a synthetic grid.
core::Mat4 pose_view(int i, core::Vec3 copy) {
    const float fi = static_cast<float>(i);
    const core::Vec3 eye{76.0f + 0.007f * fi + copy.x, 1.5f, 2.0f + 0.003f * fi + copy.z};
    const float yaw = 0.003f * fi;
    const float pitch = 0.03f * std::sin(0.07f * fi);
    const core::Vec3 fwd{
        -std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    return core::look_at(eye, eye + fwd, {0.0f, 1.0f, 0.0f});
}

LightingSettings make_settings(std::uint32_t cascades, std::uint32_t res) {
    LightingSettings s;
    s.cascade_count = cascades;
    s.shadow_map_resolution = res;
    s.cascade_split_lambda = 0.5f;
    return s;
}

CascadeInputs make_inputs(const core::Mat4& view, core::Vec3 light) {
    CascadeInputs in;
    in.camera_view = view;
    in.fov_y = kFovY;
    in.aspect = kAspect;
    in.z_near = kNear;
    in.z_far = kFar;
    in.light_dir = light;
    return in;
}

// Double-precision clip-space xyz of a world point through a float matrix (w is 1 for ortho).
std::array<double, 3> project_d(const core::Mat4& m, const std::array<double, 3>& p) {
    std::array<double, 3> o{};
    for (int r = 0; r < 3; ++r) {
        o[r] = static_cast<double>(m.at(r, 0)) * p[0] + static_cast<double>(m.at(r, 1)) * p[1] +
               static_cast<double>(m.at(r, 2)) * p[2] + static_cast<double>(m.at(r, 3));
    }
    return o;
}

double circular_diff(double a, double b) {
    double d = std::fabs(a - b);
    d -= std::floor(d); // wrap into [0,1)
    return std::min(d, 1.0 - d);
}

struct Stability {
    double phase_texels = 0.0; // worst sub-texel drift of the fixed point, in texels, minus budget
    double scale_rel = 0.0;    // worst relative change of the XY linear part
};

using Perturb = std::function<void(CascadeFit&, int pose)>;

// Walk `poses` camera poses and measure, per cascade, how much the fixed point's sub-texel phase
// and the XY texel scale move relative to pose 0. The phase error is reported minus an honest
// float-storage budget: the matrix translation is stored in a float, so a point 1000 units from
// the origin cannot be placed better than ~1000*eps. Everything above that budget is the fit's.
Stability measure(std::uint32_t cascades,
                  std::uint32_t res,
                  core::Vec3 copy,
                  core::Vec3 light,
                  int poses,
                  const Perturb& perturb) {
    const std::array<double, 3> p{76.0 + copy.x, 0.0, -30.0 + copy.z};
    const LightingSettings s = make_settings(cascades, res);
    Stability out;
    std::vector<std::array<double, 2>> ref_phase(cascades);
    std::vector<std::array<double, 4>> ref_lin(cascades);
    for (int i = 0; i < poses; ++i) {
        CascadeFit fit = compute_cascades(make_inputs(pose_view(i, copy), light), s);
        if (perturb) {
            perturb(fit, i);
        }
        for (std::uint32_t c = 0; c < cascades; ++c) {
            const core::Mat4& m = fit.view_proj[c];
            const auto q = project_d(m, p);
            const double half = 0.5 * res;
            const std::array<double, 2> phase{q[0] * half + half, q[1] * half + half};
            // |sum of the terms| bounds the float rounding of the evaluation and of the stored
            // translation; scale it to texels.
            double budget = 0.0;
            for (int r = 0; r < 2; ++r) {
                double sum = std::fabs(static_cast<double>(m.at(r, 3)));
                for (int k = 0; k < 3; ++k) {
                    sum += std::fabs(static_cast<double>(m.at(r, k)) * p[k]);
                }
                budget = std::max(budget, sum * half * 1.2e-7 * 2.0);
            }
            const std::array<double, 4> lin{m.at(0, 0), m.at(0, 2), m.at(1, 0), m.at(1, 2)};
            if (i == 0) {
                ref_phase[c] = phase;
                ref_lin[c] = lin;
                continue;
            }
            for (int a = 0; a < 2; ++a) {
                out.phase_texels =
                    std::max(out.phase_texels, circular_diff(phase[a], ref_phase[c][a]) - budget);
            }
            for (int a = 0; a < 4; ++a) {
                const double denom = std::max(std::fabs(ref_lin[c][a]), 1e-12);
                out.scale_rel = std::max(out.scale_rel, std::fabs(lin[a] - ref_lin[c][a]) / denom);
            }
        }
    }
    return out;
}

constexpr double kPhaseTol = 0.002; // texels, after the float-storage budget
constexpr double kScaleTol = 2e-6;  // relative

const core::Vec3 kLights[] = {
    {0.4f, -1.0f, 0.3f},   // the sample's oblique sun
    {0.02f, -1.0f, 0.01f}, // nearly vertical (takes the +Z up-vector branch)
    {0.9f, -0.3f, 0.2f},   // low, raking sun
};
const core::Vec3 kCopies[] = {{0, 0, 0}, {100, 0, -100}, {-1000, 0, 1000}};

} // namespace

TEST_CASE(
    "shadow stability: a fixed world point keeps its sub-texel phase while walking and turning") {
    for (const std::uint32_t res : {1024u, 2048u}) {
        for (const core::Vec3& copy : kCopies) {
            for (const core::Vec3& light : kLights) {
                const Stability st = measure(3, res, copy, light, 300, nullptr);
                CAPTURE(res);
                CAPTURE(copy.x);
                CAPTURE(light.x);
                CHECK(st.phase_texels < kPhaseTol);
                CHECK(st.scale_rel < kScaleTol);
            }
        }
    }
}

TEST_CASE("shadow stability: texel scale does not depend on where the camera is") {
    // Fixed FOV / splits / light => the XY linear part (texels per world unit and its direction)
    // is a constant of the configuration. Checked at count 1, 3 and 4.
    for (const std::uint32_t n : {1u, 3u, 4u}) {
        const Stability st = measure(n, 1024, {1000, 0, -1000}, kLights[0], 200, nullptr);
        CAPTURE(n);
        CHECK(st.scale_rel < kScaleTol);
    }
}

TEST_CASE("shadow stability: the guarded map still contains the whole rotated, translated slice") {
    const float tan_half = std::tan(kFovY * 0.5f);
    for (const std::uint32_t n : {1u, 3u, 4u}) {
        for (const std::uint32_t res : {64u, 1024u}) {
            // Independent restatement of the slice boundaries (practical split, lambda 0.5).
            std::array<double, 5> sp{};
            sp[0] = kNear;
            for (std::uint32_t c = 1; c <= n; ++c) {
                const double t = static_cast<double>(c) / n;
                sp[c] =
                    0.5 * (kNear * std::pow(kFar / kNear, t)) + 0.5 * (kNear + (kFar - kNear) * t);
            }
            double worst = 0.0; // max |NDC| over every slice corner
            for (int i = 0; i < 200; i += 7) {
                const core::Mat4 view = pose_view(i, {1000, 0, -1000});
                const core::Mat4 inv = core::inverse(view);
                const CascadeFit fit =
                    compute_cascades(make_inputs(view, kLights[0]), make_settings(n, res));
                for (std::uint32_t c = 0; c < n; ++c) {
                    for (int zi = 0; zi < 2; ++zi) {
                        const double d = sp[c + zi];
                        for (int sx = -1; sx <= 1; sx += 2) {
                            for (int sy = -1; sy <= 1; sy += 2) {
                                const core::Vec3 cam{
                                    static_cast<float>(sx * d * tan_half * kAspect),
                                    static_cast<float>(sy * d * tan_half),
                                    static_cast<float>(-d)};
                                const core::Vec3 w = core::transform_point(inv, cam);
                                const auto q = project_d(fit.view_proj[c],
                                                         {double(w.x), double(w.y), double(w.z)});
                                worst = std::max({worst, std::fabs(q[0]), std::fabs(q[1])});
                                CHECK(q[2] > -1e-3);
                                CHECK(q[2] < 1.0 + 1e-3);
                            }
                        }
                    }
                }
            }
            CAPTURE(n);
            CAPTURE(res);
            // 1e-3 of NDC: float round-off of a corner ~1000 units from the origin.
            CHECK(worst <= 1.0 + 2e-3);
        }
    }
}

TEST_CASE("shadow stability: cascades are ordered near to far, finite, and tolerate tiny maps") {
    for (const std::uint32_t n : {1u, 3u, 4u}) {
        for (const std::uint32_t res : {0u, 1u, 2u, 4u, 1024u}) {
            const CascadeFit fit =
                compute_cascades(make_inputs(pose_view(0, {}), kLights[0]), make_settings(n, res));
            CAPTURE(n);
            CAPTURE(res);
            REQUIRE(fit.count == n);
            float prev_texel_world = 0.0f;
            for (std::uint32_t c = 0; c < n; ++c) {
                for (const float e : fit.view_proj[c].m) {
                    CHECK(std::isfinite(e));
                }
                // World units per NDC unit along the light X axis = 1/|row 0|; larger = coarser.
                const float rx = std::hypot(fit.view_proj[c].at(0, 0),
                                            fit.view_proj[c].at(0, 1),
                                            fit.view_proj[c].at(0, 2));
                CHECK(rx > 0.0f);
                const float world_per_ndc = 1.0f / rx;
                CHECK(world_per_ndc > prev_texel_world);
                prev_texel_world = world_per_ndc;
            }
        }
    }
}

// ---- sensitivity controls -------------------------------------------------------------------
// Each breaks a fit in a way the real bug (or a naive fix) would, then demands the oracle notice.

TEST_CASE("shadow stability: the oracle fails on an unsnapped centre (control)") {
    // Slide the map by a pose-dependent sub-texel amount, as an unsnapped centre would.
    const Perturb unsnapped = [](CascadeFit& f, int pose) {
        for (std::uint32_t c = 0; c < f.count; ++c) {
            f.view_proj[c].at(0, 3) += 2.0f / 1024.0f * 0.5f * std::sin(0.9f * pose);
        }
    };
    const Stability st = measure(3, 1024, {}, kLights[0], 300, unsnapped);
    CHECK(st.phase_texels > 0.1);
}

TEST_CASE("shadow stability: the oracle fails on a jittering radius (control)") {
    // 1e-3 relative radius wobble — the order of the old fit's round-off at world coordinates.
    const Perturb radius = [](CascadeFit& f, int pose) {
        const float k = 1.0f + 1e-3f * std::sin(1.3f * pose);
        for (std::uint32_t c = 0; c < f.count; ++c) {
            for (int col = 0; col < 4; ++col) {
                f.view_proj[c].at(0, col) *= k;
                f.view_proj[c].at(1, col) *= k;
            }
        }
    };
    const Stability st = measure(3, 1024, {}, kLights[0], 300, radius);
    CHECK(st.scale_rel > 100 * kScaleTol);
}

TEST_CASE("shadow stability: the oracle fails on a wobbling light basis (control)") {
    // Rotate the map's XY by up to 1e-3 rad, as a basis re-derived from translated floats would.
    const Perturb rot = [](CascadeFit& f, int pose) {
        const float a = 1e-3f * std::sin(0.7f * pose);
        const float cs = std::cos(a);
        const float sn = std::sin(a);
        for (std::uint32_t c = 0; c < f.count; ++c) {
            for (int col = 0; col < 4; ++col) {
                const float x = f.view_proj[c].at(0, col);
                const float y = f.view_proj[c].at(1, col);
                f.view_proj[c].at(0, col) = cs * x - sn * y;
                f.view_proj[c].at(1, col) = sn * x + cs * y;
            }
        }
    };
    const Stability st = measure(3, 1024, {}, kLights[0], 300, rot);
    CHECK(st.scale_rel > 100 * kScaleTol);
}

// ---- shadow_max_distance (coverage decoupled from the camera's raster far) -------------------

namespace {

CascadeFit fit_with(float camera_far, float cap, std::uint32_t n, int pose = 0) {
    CascadeInputs in = make_inputs(pose_view(pose, {}), kLights[0]);
    in.z_far = camera_far;
    LightingSettings s = make_settings(n, 2048);
    s.shadow_max_distance = cap;
    return compute_cascades(in, s);
}

bool same_fit(const CascadeFit& a, const CascadeFit& b) {
    if (a.count != b.count) {
        return false;
    }
    for (std::uint32_t c = 0; c < a.count; ++c) {
        if (std::memcmp(a.view_proj[c].m, b.view_proj[c].m, sizeof(a.view_proj[c].m)) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("shadow distance: unset or unusable caps leave the camera range untouched") {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const std::uint32_t n : {1u, 3u, 4u}) {
        const CascadeFit base = fit_with(3072.0f, 0.0f, n);
        // Bad caps: negative, NaN, infinite, at and below near.
        for (const float bad : {-5.0f, nan, inf, kNear, 0.05f}) {
            CAPTURE(n);
            CAPTURE(bad);
            CHECK(same_fit(base, fit_with(3072.0f, bad, n)));
        }
        // A cap at or beyond the camera's far shortens nothing.
        CHECK(same_fit(base, fit_with(3072.0f, 3072.0f, n)));
        CHECK(same_fit(base, fit_with(3072.0f, 9000.0f, n)));
    }
}

TEST_CASE("shadow distance: a finite cap fits exactly like a camera whose far is that cap") {
    for (const std::uint32_t n : {1u, 3u, 4u}) {
        const CascadeFit direct = fit_with(128.0f, 0.0f, n);
        for (const float camera_far : {3072.0f, 4096.0f}) {
            CAPTURE(n);
            CAPTURE(camera_far);
            // Same matrices => same splits, so camera far no longer moves the fit.
            CHECK(same_fit(direct, fit_with(camera_far, 128.0f, n)));
        }
        // Not vacuous: the uncapped camera far really does give a different fit.
        CHECK_FALSE(same_fit(direct, fit_with(3072.0f, 0.0f, n)));
        CHECK_FALSE(same_fit(fit_with(3072.0f, 0.0f, n), fit_with(4096.0f, 0.0f, n)));
    }
}

TEST_CASE("shadow distance: capped splits start at near, end at the cap, and cover that frustum") {
    const float tan_half = std::tan(kFovY * 0.5f);
    constexpr float kCap = 128.0f;
    for (const std::uint32_t n : {1u, 3u, 4u}) {
        // Independent restatement of the practical split with far = cap.
        std::array<double, 5> sp{};
        sp[0] = kNear;
        for (std::uint32_t c = 1; c <= n; ++c) {
            const double t = static_cast<double>(c) / n;
            sp[c] = 0.5 * (kNear * std::pow(kCap / kNear, t)) + 0.5 * (kNear + (kCap - kNear) * t);
        }
        CHECK(sp[n] == doctest::Approx(kCap));
        double worst = 0.0;
        for (int i = 0; i < 200; i += 13) {
            const core::Mat4 inv = core::inverse(pose_view(i, {}));
            const CascadeFit fit = fit_with(3072.0f, kCap, n, i);
            for (std::uint32_t c = 0; c < n; ++c) {
                for (int zi = 0; zi < 2; ++zi) {
                    const double d = sp[c + zi];
                    for (int sx = -1; sx <= 1; sx += 2) {
                        for (int sy = -1; sy <= 1; sy += 2) {
                            const core::Vec3 cam{static_cast<float>(sx * d * tan_half * kAspect),
                                                 static_cast<float>(sy * d * tan_half),
                                                 static_cast<float>(-d)};
                            const core::Vec3 w = core::transform_point(inv, cam);
                            const auto q = project_d(fit.view_proj[c],
                                                     {double(w.x), double(w.y), double(w.z)});
                            worst = std::max({worst, std::fabs(q[0]), std::fabs(q[1])});
                            CHECK(q[2] > -1e-3);
                            CHECK(q[2] < 1.0 + 1e-3);
                        }
                    }
                }
            }
        }
        CAPTURE(n);
        CHECK(worst <= 1.0 + 2e-3);
    }
}
