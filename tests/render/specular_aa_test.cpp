// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Geometric specular anti-aliasing (ADR-0078 step 1a): `filter_specular_alpha` in brdf.glsl, called
// by pbr_forward.frag, pbr_forward_shadowed.frag and terrain.frag. Golden images are useless across
// drivers, so both proofs are STRUCTURAL -- properties the maths guarantees, checked with margins:
//
//  P1  A FLAT surface is untouched. A flat quad's shading normal is constant, so dFdx(n) = dFdy(n)
//  =
//      0, the added variance is exactly zero and the filter returns its input. The proof renders a
//      smooth metal quad under one directional light and compares EVERY pixel near the highlight
//      to the analytic GGX value for the UNFILTERED alpha, computed here in C++. This is the
//      load-bearing test: it is what stops the filter from quietly dulling every flat surface.
//
//  P2  A CURVED surface is calmer. A smooth metal sphere's highlight is far smaller than a pixel,
//  so
//      which pixels catch it flips with sub-pixel camera motion. The proof renders a set of spheres
//      at different sub-pixel phases and measures the energy of the horizontal pixel-to-pixel
//      luminance difference near the silhouette. The threshold is derived from a measurement of the
//      same scene with the filter switched off (see the numbers beside the assertion).
//
// What is NOT proven here: that the filter is correct on normal-mapped content (the sphere is
// smooth, its curvature is the only variance source), and terrain.frag is not exercised at all --
// its normal is a per-triangle face normal, so dFdx(n) is zero inside a facet (see the brick
// report).

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

namespace {

using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

constexpr double kPiD = 3.14159265358979323846;

struct V3 {
    double x, y, z;
};

V3 operator+(V3 a, V3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

V3 operator*(double s, V3 a) {
    return {s * a.x, s * a.y, s * a.z};
}

double dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

V3 normalize(V3 a) {
    const double l = std::sqrt(dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

// Cook-Torrance GGX specular for a WHITE METAL (F = 1) -- brdf.glsl's d_ggx * v_smith_ggx times the
// geometry cosine, restated in double. This is the reference the shader is compared against.
double ggx_specular_white_metal(double n_dot_h, double n_dot_v, double n_dot_l, double alpha) {
    const double a2 = alpha * alpha;
    const double t = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    const double d = a2 / (kPiD * t * t);
    const double gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const double gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    const double v = 0.5 / std::max(gv + gl, 1e-5);
    return d * v * n_dot_l;
}

// Render one frame of `world` and return the HDR target decoded to linear radiance.
HdrImage render_hdr(rhi::Device& device,
                    MeshRegistry& meshes,
                    MaterialRegistry& materials,
                    ecs::World& world,
                    std::uint32_t size) {
    SceneRenderer renderer(device, meshes, materials);
    renderer.set_ambient(0.0f, 0.0f, 0.0f); // the proofs read the SPECULAR lobe alone
    RenderGraph graph(device);
    graph.reset();
    const SceneRenderer::Output out = renderer.render(graph, world, {size, size}, true);
    REQUIRE(out.ldr.is_valid());
    graph.export_texture(out.hdr);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return decode_hdr(read_texture(device, graph.physical(out.hdr), size, size, 8), size, size);
}

// A white metal, roughness 0.05 (alpha = 0.0025), with the normal map flattened to EXACTLY +Z: the
// renderer's fallback normal texel is (128,128,255), which decodes to a normal tilted by 0.0039 in
// x and y -- larger than this material's lobe -- so normal_scale = 0 removes the tilt and makes the
// analytic reference exact.
PbrMaterialDesc smooth_metal() {
    PbrMaterialDesc m{};
    m.base_color[0] = m.base_color[1] = m.base_color[2] = 1.0f;
    m.metallic = 1.0f;
    m.roughness = 0.05f;
    m.normal_scale = 0.0f;
    return m;
}

constexpr float kRoughness = 0.05f;
constexpr double kAlpha = static_cast<double>(kRoughness) * static_cast<double>(kRoughness);

} // namespace

TEST_CASE(
    "specular aa: P1 -- a flat quad's highlight matches the UNFILTERED analytic GGX (ADR-0078)") {
    using ecs::WorldTransform;
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available -- skipping specular-AA proofs");
        return;
    }
    constexpr std::uint32_t kSize = 256;

    // ── The scene, and where the highlight lands ────────────────────────────────────────────
    // A +y-facing quad at the origin. The camera sits at (0,4,4) pitched -45 deg about x, so it
    // looks along f = (0,-s,-s), s = sqrt(1/2), straight at the origin. A mirror at the origin
    // sends that view ray out along (0,+s,-s), so the vector TOWARD the light that makes h = n at
    // the origin is l = (0,s,-s); the light TRAVELS along -l = (0,-s,+s), which is the entity's -z
    // after a -135 deg rotation about x (y' = sin(-135) = -s, z' = -cos(-135) = +s).
    MeshRegistry meshes(*device);
    const MeshId plane = meshes.add(make_plane(20.0f), "specaa-plane");
    REQUIRE(plane != kInvalidMeshId);
    MaterialRegistry materials;
    const MaterialId metal = materials.add(smooth_metal());

    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(WorldTransform{}, MeshRef{plane}, MaterialRef{metal});

    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 4.0f, 4.0f};
    cam_tf.rotation =
        core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.25f * static_cast<float>(kPiD));
    (void)world.spawn_with(WorldTransform{cam_tf}, Camera{});

    core::Transform light_tf{};
    light_tf.rotation =
        core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.75f * static_cast<float>(kPiD));
    (void)world.spawn_with(WorldTransform{light_tf}, DirectionalLight{1.0f, 1.0f, 1.0f, 1.0f});

    const HdrImage hdr = render_hdr(*device, meshes, materials, world, kSize);

    // ── The analytic image ───────────────────────────────────────────────────────────────────
    // Camera basis: right r = (1,0,0); up u = (0,s,-s) (the +y axis rotated -45 deg about x);
    // forward f = (0,-s,-s). The projection bakes Vulkan's y-down NDC, so pixel row y maps to
    // ndc_y = (y+0.5)/N*2-1 and WORLD up is -ndc_y. Pixel centres, because that is where the
    // rasteriser samples. Ray/plane: p = cam + t*dir with p.y = 0.
    const double s = std::sqrt(0.5);
    const V3 cam{0.0, 4.0, 4.0};
    const V3 right{1.0, 0.0, 0.0}, up{0.0, s, -s}, fwd{0.0, -s, -s};
    const double tan_half = std::tan(static_cast<double>(Camera{}.fov_y) * 0.5);
    const V3 l = normalize({0.0, s, -s}); // toward the light
    const V3 n{0.0, 1.0, 0.0};

    std::vector<double> expected(static_cast<std::size_t>(kSize) * kSize, 0.0);
    double expected_peak = 0.0;
    std::uint32_t pk_x = 0, pk_y = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const double ndc_x = (x + 0.5) / kSize * 2.0 - 1.0;
            const double ndc_y = (y + 0.5) / kSize * 2.0 - 1.0;
            const V3 dir = fwd + (ndc_x * tan_half) * right + (-ndc_y * tan_half) * up;
            if (dir.y >= 0.0)
                continue; // above the horizon: no plane
            const double t = -cam.y / dir.y;
            const V3 p = cam + t * dir;
            const V3 v = normalize(cam + (-1.0) * p);
            const V3 h = normalize(v + l);
            const double e = ggx_specular_white_metal(dot(n, h), dot(n, v), dot(n, l), kAlpha);
            expected[static_cast<std::size_t>(y) * kSize + x] = e;
            if (e > expected_peak) {
                expected_peak = e;
                pk_x = x;
                pk_y = y;
            }
        }
    }
    REQUIRE(expected_peak > 100.0); // the highlight really is on screen and really is hot

    // ── Compare every pixel in a window around the peak ──────────────────────────────────────
    // Restrict to pixels whose analytic value is at least a quarter of the peak: there the lobe is
    // well above float rounding of 1 - nh^2*(1-a2) (which near the peak is ~6e-6 against an ulp of
    // 6e-8, i.e. a few percent -- hence the 15 % band rather than 1 %). If the filter had added
    // even a few percent of alpha^2 here, the peak would fall by that much and fail this.
    std::uint32_t compared = 0;
    double worst_rel = 0.0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const double e = expected[static_cast<std::size_t>(y) * kSize + x];
            if (e < 0.25 * expected_peak)
                continue;
            ++compared;
            const double got = hdr.luminance(x, y);
            const double rel = std::fabs(got - e) / e;
            worst_rel = std::max(worst_rel, rel);
        }
    }
    const double got_peak = hdr.luminance(pk_x, pk_y);
    std::printf("[specular_aa P1] analytic peak %.2f at (%u,%u), rendered %.2f (ratio %.4f); %u px "
                "compared, worst relative error %.4f\n",
                expected_peak,
                pk_x,
                pk_y,
                got_peak,
                got_peak / expected_peak,
                compared,
                worst_rel);
    REQUIRE(compared >= 1);
    CHECK(got_peak == doctest::Approx(expected_peak).epsilon(0.15));
    CHECK(worst_rel < 0.15);
    // The filter can only WIDEN the lobe, i.e. LOWER the peak: the rendered peak must not exceed
    // the unfiltered analytic peak (beyond float noise). That is "alpha never decreases" as
    // measured.
    CHECK(got_peak <= expected_peak * 1.05);
}

TEST_CASE(
    "specular aa: P2 -- filtering a curved surface calms pixel-to-pixel specular (ADR-0078)") {
    using ecs::WorldTransform;
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available -- skipping specular-AA proofs");
        return;
    }
    constexpr std::uint32_t kSize = 256;

    // ── The scene ────────────────────────────────────────────────────────────────────────────
    // A 2x2 grid of unit spheres seen from z = 8 (identity camera, looking down -z), each shifted
    // by a different FRACTION of a pixel (a pixel is ~0.029 world units here). The highlight is far
    // smaller than a pixel, so one sphere's score depends on luck; four sub-pixel phases average
    // the luck out. The sun's TOWARD vector is l = (sin 100, 0, cos 100): h = (v+l)/|v+l| sits 50
    // deg from the view axis, so the highlight lands at sin 50 = 0.77 of the radius -- inside the
    // annulus [0.55 R, 0.98 R] the statistic reads. Light travels along -l = (-0.985, 0, 0.174),
    // which is the entity's -z after a +100 deg rotation about y.
    MeshRegistry meshes(*device);
    const MeshId sphere = meshes.add(make_uv_sphere(1.0f, 64, 128), "specaa-sphere");
    REQUIRE(sphere != kInvalidMeshId);
    MaterialRegistry materials;
    const MaterialId metal = materials.add(smooth_metal());

    constexpr float kPx = 0.0291f; // world size of one pixel at the sphere plane (measured below)
    const std::array<std::array<float, 2>, 4> phase = {
        {{0.0f, 0.0f}, {0.25f, 0.5f}, {0.5f, 0.75f}, {0.75f, 0.25f}}};
    const std::array<std::array<float, 2>, 4> centre = {
        {{-1.2f, 1.2f}, {1.2f, 1.2f}, {-1.2f, -1.2f}, {1.2f, -1.2f}}};

    ecs::World world;
    register_render_components(world);
    for (std::size_t i = 0; i < 4; ++i) {
        core::Transform tf{};
        tf.translation = {centre[i][0] + phase[i][0] * kPx, centre[i][1] + phase[i][1] * kPx, 0.0f};
        (void)world.spawn_with(WorldTransform{tf}, MeshRef{sphere}, MaterialRef{metal});
    }
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 0.0f, 8.0f};
    (void)world.spawn_with(WorldTransform{cam_tf}, Camera{});
    core::Transform light_tf{};
    light_tf.rotation =
        core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, static_cast<float>(100.0 * kPiD / 180.0));
    (void)world.spawn_with(WorldTransform{light_tf}, DirectionalLight{1.0f, 1.0f, 1.0f, 1.0f});

    const HdrImage hdr = render_hdr(*device, meshes, materials, world, kSize);

    const core::Mat4 view_proj =
        core::perspective(Camera{}.fov_y, 1.0f, Camera{}.z_near, Camera{}.z_far) *
        core::inverse(core::to_matrix(cam_tf));
    const float radius_px = project(view_proj, {1.0f, 0.0f, 0.0f}, kSize).x -
                            project(view_proj, {0.0f, 0.0f, 0.0f}, kSize).x;
    REQUIRE(radius_px > 20.0f);

    // ── The statistic ────────────────────────────────────────────────────────────────────────
    // Sum, over every horizontally adjacent pixel pair inside the annulus, of the squared luminance
    // difference. A highlight that hops between pixels contributes (peak)^2 per hop; a filtered one
    // spreads the same energy over several pixels and contributes far less. `energy` is the total
    // radiance in the annulus: filtering redistributes it, it must not remove most of it.
    double diff_energy = 0.0, energy = 0.0, hottest = 0.0;
    std::uint32_t pairs = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const Pixel c =
            project(view_proj,
                    {centre[i][0] + phase[i][0] * kPx, centre[i][1] + phase[i][1] * kPx, 0.0f},
                    kSize);
        const auto r_hi = static_cast<std::int32_t>(0.98f * radius_px);
        for (std::int32_t dy = -r_hi; dy <= r_hi; ++dy) {
            for (std::int32_t dx = -r_hi; dx <= r_hi; ++dx) {
                const float rr = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (rr < 0.55f * radius_px || rr > 0.98f * radius_px)
                    continue;
                const auto px = static_cast<std::uint32_t>(c.x + static_cast<float>(dx));
                const auto py = static_cast<std::uint32_t>(c.y + static_cast<float>(dy));
                const double a = hdr.luminance(px, py);
                const double b = hdr.luminance(px + 1, py);
                diff_energy += (b - a) * (b - a);
                energy += a;
                hottest = std::max(hottest, a);
                ++pairs;
            }
        }
    }
    std::printf("[specular_aa P2] pairs %u, diff_energy %.6g, annulus energy %.6g, hottest %.4g\n",
                pairs,
                diff_energy,
                energy,
                hottest);
    REQUIRE(pairs > 1000);
    REQUIRE(std::isfinite(diff_energy));

    // THE THRESHOLD, AND WHAT IT WAS DERIVED FROM. Measured 2026-10-07 on an RTX 3060 (Vulkan 1.4,
    // validation on), this scene, 9808 pixel pairs, 256x256, roughness 0.05:
    //
    //     filter OFF (filter_specular_alpha returning `alpha`)   diff_energy 5.079e8  hottest 15940
    //     filter ON                                              diff_energy 9.054e4  hottest   158
    //
    // a factor of ~5600 in difference energy and ~100 in the hottest pixel. The OFF number is the
    // sparkle: a few pixels at ~1.6e4 against near-zero neighbours. The threshold sits at 1e6: 11x
    // above the filtered measurement (room for a driver/rasteriser difference) and 500x below the
    // unfiltered one (so a filter that quietly stopped working fails by a wide margin). It is a
    // number about THIS content -- a smooth sphere, roughness 0.05, four sub-pixel phases -- and
    // not a claim about all materials.
    CHECK(diff_energy < 1.0e6);
    CHECK(hottest < 1000.0);
    // Filtering moves energy out of the spike, it must not annihilate the highlight: something is
    // still lit in the annulus.
    CHECK(energy > 100.0);
}

TEST_CASE(
    "specular aa: P2b -- the clamp holds: a sphere's rim does not reach alpha = 1 (ADR-0078)") {
    using ecs::WorldTransform;
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available -- skipping specular-AA proofs");
        return;
    }
    constexpr std::uint32_t kSize = 256;

    // The rim is where n turns fastest, so it is where an UNCLAMPED filter would drive alpha to 1.
    // alpha is not an output, so the proof reads a consequence of it. At alpha = 1, D is the
    // constant 1/pi, so a white metal's specular radiance is bounded by D*V*(n.l) <= 1/(2 pi) =
    // 0.159 for a unit light, WHEREVER the half-vector points. At the clamped ceiling alpha^2 <=
    // 0.18 + alpha0^2 the lobe is still peaked (D = 1/(pi*0.18) = 1.77 at n.h = 1), so a pixel
    // whose n bisects v and l reads well ABOVE 0.159. Put the highlight at the rim -- h 80 deg from
    // the view axis, i.e. l at 160 deg -- and require the hottest rim pixel to exceed that alpha =
    // 1 ceiling. The sun travels along -l = (-0.940, 0, 0.342): +160 deg about y.
    MeshRegistry meshes(*device);
    const MeshId sphere = meshes.add(make_uv_sphere(1.0f, 64, 128), "specaa-rim-sphere");
    REQUIRE(sphere != kInvalidMeshId);
    MaterialRegistry materials;
    const MaterialId metal = materials.add(smooth_metal());

    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(WorldTransform{}, MeshRef{sphere}, MaterialRef{metal});
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 0.0f, 8.0f};
    (void)world.spawn_with(WorldTransform{cam_tf}, Camera{});
    core::Transform light_tf{};
    light_tf.rotation =
        core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, static_cast<float>(160.0 * kPiD / 180.0));
    (void)world.spawn_with(WorldTransform{light_tf}, DirectionalLight{1.0f, 1.0f, 1.0f, 1.0f});

    const HdrImage hdr = render_hdr(*device, meshes, materials, world, kSize);
    const core::Mat4 view_proj =
        core::perspective(Camera{}.fov_y, 1.0f, Camera{}.z_near, Camera{}.z_far) *
        core::inverse(core::to_matrix(cam_tf));
    const float radius_px = project(view_proj, {1.0f, 0.0f, 0.0f}, kSize).x -
                            project(view_proj, {0.0f, 0.0f, 0.0f}, kSize).x;
    const Pixel c = project(view_proj, {0.0f, 0.0f, 0.0f}, kSize);
    double rim_max = 0.0;
    for (std::int32_t dy = -40; dy <= 40; ++dy) {
        for (std::int32_t dx = -40; dx <= 40; ++dx) {
            const float rr = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (rr < 0.93f * radius_px || rr > 0.995f * radius_px)
                continue;
            rim_max = std::max(rim_max,
                               static_cast<double>(hdr.luminance(
                                   static_cast<std::uint32_t>(c.x + static_cast<float>(dx)),
                                   static_cast<std::uint32_t>(c.y + static_cast<float>(dy)))));
        }
    }
    std::printf("[specular_aa P2b] rim max luminance %.4f (alpha = 1 ceiling 0.159)\n", rim_max);
    CHECK(rim_max > 0.159 * 1.5);
}
