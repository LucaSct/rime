// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky LIGHTS the scene — the structural proof. m17.7d replaces the authored gradient in the
// sky-view/SH body with physical single scattering, so these controls prove source scale, solar
// colour, cache visibility, the sky-off gate, and escaped-ray reflection. The m17.7d background
// samples that same physical body; this file explicitly proves legacy zenith/horizon colours do
// not leak into either physical path. No golden images: every claim is a transport property
// isolated by a control, following the M5.6/M6.4 pattern.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#include "render_test_support.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/sky.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::decode_hdr;
using rime::render::test::HdrImage;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

constexpr std::uint32_t kSize = 96;
constexpr float kAlbedo = 0.6f;

struct Rgb {
    float r = 0.0f, g = 0.0f, b = 0.0f;

    [[nodiscard]] float luminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
};

[[nodiscard]] Rgb
block_mean(const HdrImage& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t half) {
    Rgb sum;
    std::uint32_t n = 0;
    for (std::uint32_t y = y0 - half; y <= y0 + half; ++y) {
        for (std::uint32_t x = x0 - half; x <= x0 + half; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
            sum.r += img.rgb[i];
            sum.g += img.rgb[i + 1];
            sum.b += img.rgb[i + 2];
            ++n;
        }
    }
    const float inv = 1.0f / static_cast<float>(n);
    return {sum.r * inv, sum.g * inv, sum.b * inv};
}

// A floor filling the lower half of the frame, viewed from 1 m up looking level — the same camera
// geometry sky_test.cpp uses, so "the block at 3/4 height is floor" holds here too.
//
// Deliberately NO light of any kind. The sky is then the only thing that can illuminate the
// surface, which is what makes every number below attributable to it rather than to a sun that
// happens to be pointing the right way.
template <typename Build>
[[nodiscard]] HdrImage render_hdr(rhi::Device& device, SceneRenderer& renderer, Build&& build) {
    ecs::World world;
    register_render_components(world);
    build(world);
    RenderGraph graph(device);
    graph.reset();
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize});
    REQUIRE(out.hdr.is_valid());
    graph.export_texture(out.hdr);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return decode_hdr(read_texture(device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
}

// A clear physical sky lit by an authored unit-scale sun.  The LUT medium remains independent of
// this value; m17.7d applies the sun after its physical single-scattering integral so a colour or
// intensity edit re-bakes sky-view/SH without rebuilding the medium tables.
[[nodiscard]] SkyParams physical_sky(float solar) {
    SkyParams sp{};
    sp.enabled = true;
    sp.clouds_enabled = false;
    sp.intensity = 1.0f;
    sp.sun_direction[0] = 0.0f;
    sp.sun_direction[1] = 0.8f;
    sp.sun_direction[2] = -0.6f;
    sp.sun_radiance[0] = sp.sun_radiance[1] = sp.sun_radiance[2] = solar;
    sp.use_scene_sun = false;
    return sp;
}

[[nodiscard]] SkyParams coloured_sun(Rgb colour) {
    SkyParams sp = physical_sky(1.0f);
    sp.sun_radiance[0] = colour.r;
    sp.sun_radiance[1] = colour.g;
    sp.sun_radiance[2] = colour.b;
    return sp;
}

void build_floor(ecs::World& world, MeshId floor, MaterialId mat) {
    (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor}, MaterialRef{mat});
    core::Transform cam{};
    cam.translation = {0.0f, 1.0f, 0.0f};
    (void)world.spawn_with(ecs::WorldTransform{cam}, Camera{1.1f, 0.1f, 100.0f, true});
}

// A directional light that emits NOTHING. Its only job is to make `has_sun` true so the frame takes
// the shadowed pipeline; a light with radiance would contaminate the ambient reading this file is
// built on.
ecs::Entity world_spawn_dark_sun(ecs::World& world, const core::Transform& t) {
    return world.spawn_with(ecs::WorldTransform{t}, DirectionalLight{1.0f, 1.0f, 1.0f, 0.0f});
}

} // namespace

TEST_CASE("sky lighting: physical single scattering is live and tracks solar scale (m17.7d)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the sky lighting units proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "skylight-floor");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = kAlbedo;
    md.metallic = 0.0f;
    md.roughness = 0.8f;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);
    const auto build = [&](ecs::World& w) { build_floor(w, floor, mat); };

    // ── (4) Off is the old path ─────────────────────────────────────────────────────────────────
    // No sky at all: the surface must read albedo * the ambient constant. set_ambient is given a
    // value that is not the default, so passing this cannot be an accident of both being 0.02.
    renderer.set_ambient(0.11f, 0.11f, 0.11f);
    const HdrImage off = render_hdr(*device, renderer, build);
    const Rgb floor_off = block_mean(off, kSize / 2, kSize * 3 / 4, 5);
    MESSAGE("sky off: floor=" << floor_off.r << " expected=" << kAlbedo * 0.11f);
    CHECK(floor_off.r == doctest::Approx(kAlbedo * 0.11f).epsilon(0.02));

    // ── (1) Physical source and scale ───────────────────────────────────────────────────────────
    // A black sun gives the integral no incident light.  A clear white one then gives the same
    // surface a finite sky-derived ambient; doubling that source doubles a linear transport solve.
    renderer.set_sky(physical_sky(0.0f));
    const Rgb dark = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    renderer.set_sky(physical_sky(1.0f));
    const Rgb day = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    renderer.set_sky(physical_sky(2.0f));
    const Rgb bright =
        block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    SkyParams overcast_black = physical_sky(1.0f);
    overcast_black.clouds_enabled = true;
    overcast_black.coverage = 0.9f;
    overcast_black.intensity = 0.0f;
    renderer.set_sky(overcast_black);
    const Rgb overcast_black_floor =
        block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    MESSAGE("physical sky: dark=" << dark.r << " day rgb=(" << day.r << ", " << day.g << ", "
                                  << day.b << ") bright=" << bright.r
                                  << " cloudy intensity-0=" << overcast_black_floor.r);
    CHECK(dark.luminance() < 1e-4f);
    CHECK(day.luminance() > 0.002f);
    CHECK(bright.luminance() > day.luminance() * 1.8f);
    CHECK(overcast_black_floor.luminance() < 1e-4f);
}

TEST_CASE("sky lighting: physical light follows the sun, not authored gradient colours (m17.7d)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the sky lighting direction proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "skylight-floor");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = kAlbedo;
    md.metallic = 0.0f;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);
    const auto build = [&](ecs::World& w) { build_floor(w, floor, mat); };

    // The visible background is still the authored m17.0 gradient in this brick, but the LUT/SH
    // path is not allowed to inherit it.  Altering those colours must leave the lit floor alone.
    renderer.set_sky(physical_sky(1.0f));
    const Rgb white = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    SkyParams gradient_edit = physical_sky(1.0f);
    gradient_edit.zenith[0] = 0.95f;
    gradient_edit.zenith[1] = 0.02f;
    gradient_edit.zenith[2] = 0.01f;
    gradient_edit.horizon[0] = 0.01f;
    gradient_edit.horizon[1] = 0.02f;
    gradient_edit.horizon[2] = 0.95f;
    // A fresh renderer forces a distinct bake; reusing the white LUT here would make a shader that
    // accidentally read zenith/horizon appear correct merely because the cache hid the new body.
    SceneRenderer gradient_renderer(*device, meshes, materials);
    gradient_renderer.set_sky(gradient_edit);
    const Rgb gradient =
        block_mean(render_hdr(*device, gradient_renderer, build), kSize / 2, kSize * 3 / 4, 5);

    // In contrast, the solar colour is applied to the physical source exactly once, so red and blue
    // suns must tint the same up-facing surface in opposite directions.
    renderer.set_sky(coloured_sun({1.0f, 0.08f, 0.08f}));
    const Rgb red = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    renderer.set_sky(coloured_sun({0.08f, 0.08f, 1.0f}));
    const Rgb blue = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    SkyParams below_horizon = physical_sky(1.0f);
    below_horizon.sun_direction[1] = -0.8f;
    renderer.set_sky(below_horizon);
    const Rgb night = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    SkyParams denser_rayleigh = physical_sky(1.0f);
    denser_rayleigh.atmosphere.rayleigh_scattering[0] *= 3.0f;
    denser_rayleigh.atmosphere.rayleigh_scattering[1] *= 3.0f;
    denser_rayleigh.atmosphere.rayleigh_scattering[2] *= 3.0f;
    renderer.set_sky(denser_rayleigh);
    const Rgb denser =
        block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    MESSAGE("white=" << white.r << "," << white.g << "," << white.b << " gradient=" << gradient.r
                     << "," << gradient.g << "," << gradient.b << " red r/b=" << red.r << "/"
                     << red.b << " blue r/b=" << blue.r << "/" << blue.b
                     << " denser blue=" << denser.b << " below-horizon=" << night.luminance());
    CHECK(gradient.r == doctest::Approx(white.r).epsilon(0.03));
    CHECK(gradient.g == doctest::Approx(white.g).epsilon(0.03));
    CHECK(gradient.b == doctest::Approx(white.b).epsilon(0.03));
    CHECK(red.r > red.b * 2.0f);
    CHECK(blue.b > blue.r * 2.0f);
    CHECK(night.luminance() < white.luminance() * 0.01f);
    CHECK(denser.b > white.b * 1.2f);
}

TEST_CASE(
    "sky lighting: the bake is reused until the sky changes, and the counters say so (m17.7b)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the sky bake cache proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "skylight-floor");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = kAlbedo;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);
    const auto build = [&](ecs::World& w) { build_floor(w, floor, mat); };

    renderer.set_sky(physical_sky(0.2f));
    (void)render_hdr(*device, renderer, build);
    const auto after_first = renderer.sky_lighting_stats();
    CHECK(after_first.filled == 1);
    CHECK(after_first.reused == 0);

    // Same sky, three more frames: the bake must be REUSED, not repeated. If this reads 4 fills the
    // dirty test is comparing something that is not stable frame to frame (padding, say), and the
    // engine is re-baking the sky forever while looking perfectly correct.
    (void)render_hdr(*device, renderer, build);
    (void)render_hdr(*device, renderer, build);
    (void)render_hdr(*device, renderer, build);
    const auto after_reuse = renderer.sky_lighting_stats();
    MESSAGE("after 4 frames of one sky: filled=" << after_reuse.filled
                                                 << " reused=" << after_reuse.reused);
    CHECK(after_reuse.filled == 1);
    CHECK(after_reuse.reused == 3);

    // A background-only gradient edit is deliberately invisible to physical lighting and must not
    // pay another sky-view/SH bake. This catches the tempting but wrong old cache key directly.
    SkyParams background_only = physical_sky(0.2f);
    background_only.zenith[0] = 0.91f;
    background_only.horizon[2] = 0.17f;
    renderer.set_sky(background_only);
    (void)render_hdr(*device, renderer, build);
    const auto after_background_only = renderer.sky_lighting_stats();
    CHECK(after_background_only.filled == 1);
    CHECK(after_background_only.reused == 4);

    // A physical lighting-source edit must still refill. The other half of the claim — a cache
    // that never refills — passes the unchanged-frame assertion perfectly.
    renderer.set_sky(physical_sky(0.5f));
    (void)render_hdr(*device, renderer, build);
    const auto after_change = renderer.sky_lighting_stats();
    MESSAGE("after changing the sky: filled=" << after_change.filled
                                              << " reused=" << after_change.reused);
    CHECK(after_change.filled == 2);
    CHECK(after_change.reused == 4);
}

TEST_CASE("sky lighting: on the shadowed path, the GATE is what keeps the old ambient (m17.7b)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the sky lighting gate proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "skylight-floor");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = kAlbedo;
    md.metallic = 0.0f;
    const MaterialId mat = materials.add(md);

    // This case exists because the obvious version of "sky off is unchanged" does NOT test what it
    // appears to. With no sky and no lighting features, the frame never reaches the shadowed
    // pipeline at all — it runs the M5.6 baseline shader, which has no sky binding and could not
    // read one if it wanted to. So that test passes whether or not the gate works, and a
    // falsification pass that forces sky_sh_enabled() to true sails straight through it.
    //
    // Enabling shadows and giving the world a directional light of ZERO radiance forces the
    // shadowed pipeline WITHOUT adding any light: the sun contributes nothing, so the floor's
    // entire value is the ambient term, and the gate is the only thing deciding which ambient. Now
    // breaking the gate breaks this test, which is what makes it evidence.
    LightingSettings ls{};
    ls.shadows_enabled = true;
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_lighting(ls);
    renderer.set_ambient(0.11f, 0.11f, 0.11f);

    const auto build = [&](ecs::World& w) {
        build_floor(w, floor, mat);
        core::Transform sun{};
        sun.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
        (void)world_spawn_dark_sun(w, sun);
    };

    const HdrImage off = render_hdr(*device, renderer, build);
    const Rgb floor_off = block_mean(off, kSize / 2, kSize * 3 / 4, 5);
    MESSAGE("shadowed path, no sky: floor=" << floor_off.r << " expected=" << kAlbedo * 0.11f);
    CHECK(floor_off.r == doctest::Approx(kAlbedo * 0.11f).epsilon(0.02));

    // And with a sky, the same pipeline must switch to the sky's value — so the check above is the
    // gate holding, not the binding being broken in both directions.
    renderer.set_sky(physical_sky(1.0f));
    const HdrImage on = render_hdr(*device, renderer, build);
    const Rgb floor_on = block_mean(on, kSize / 2, kSize * 3 / 4, 5);
    MESSAGE("shadowed path, physical sky floor=" << floor_on.r << ", " << floor_on.g << ", "
                                                 << floor_on.b);
    CHECK(floor_on.luminance() > 0.002f);
}

TEST_CASE(
    "sky lighting: a reflection that leaves the screen finds the SKY, not a flat colour (m17.7b)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the SSR sky fallback proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "skylight-mirror");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    // A dark, smooth, metallic floor: dark so the ambient term cannot dominate the reading,
    // smooth so SSR takes the sharp end of its roughness cone, metallic so what it shows is
    // reflection rather than diffuse.
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.02f;
    md.metallic = 1.0f;
    md.roughness = 0.05f;
    const MaterialId mat = materials.add(md);

    // This case is also what keeps the LUT's resource-state bookkeeping honest. Until SSR sampled
    // it, the table ended each frame in the general layout its compute write left it in; now that
    // something reads it, it ends in ShaderRead, and claiming the wrong one produces
    // `texture_barrier 'from' disagrees with the tracked layout` from the second frame onward.
    // Several frames are rendered below precisely so that a wrong claim has somewhere to show up.
    LightingSettings ls;
    ls.ssr_enabled = true;
    ls.ssr_max_distance = 8.0f;
    ls.ssr_thickness = 0.5f;
    ls.ssr_max_steps = 64;
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_lighting(ls);

    const auto build = [&](ecs::World& w) { build_floor(w, floor, mat); };

    // Two skies whose physical solar source differs. Most of the floor's reflection rays leave the
    // screen, which is exactly the path that used to return one flat colour regardless of
    // direction; m17.7d must carry the physical sky-view table's colour through it.
    renderer.set_sky(coloured_sun({0.9f, 0.05f, 0.05f}));
    (void)render_hdr(*device, renderer, build); // warm the bake, then measure a steady frame
    const Rgb red = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);

    renderer.set_sky(coloured_sun({0.05f, 0.05f, 0.9f}));
    (void)render_hdr(*device, renderer, build);
    const Rgb blue = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);

    MESSAGE("mirror floor — red sun: r=" << red.r << " b=" << red.b << " | blue sun: r=" << blue.r
                                         << " b=" << blue.b);
    // The reflection carries the sky's colour, and swapping the sky swaps it. A flat fallback
    // could not do this: it would return the same constant in both frames.
    CHECK(red.r > red.b);
    CHECK(blue.b > blue.r);
}

// ── m19.6b: a metal takes no DIFFUSE ambient, and mirrors the sky exactly once ───────────────────
//
// Everything below reads the FLOOR — rows [kFloorTop, kSize), which the level camera 1 m above a
// 12 m plane fills with floor on every column (the horizon is row kSize/2). The claims are exact
// (a product with a float zero IS zero; one program fed the same inputs IS the same image) or
// strict inequalities the algebra forces, never a tuned margin — ADR-0065's addendum derives each.
namespace {

constexpr std::uint32_t kFloorTop = kSize * 5 / 8;

// The NEAR floor, for the SSR cases. At the far, grazing end of the floor (measured: rows 55-61
// of 96) the screen march's thickness test lets a nearly floor-parallel ray "hit" the floor it
// left, so those pixels reflect the FLOOR — m10.7b's behaviour, and correct for what it is, but
// not the environment the claims below are about. From 3/4 height down every reflection ray
// climbs off the top of the screen without meeting anything: a pure miss, the probe alone.
constexpr std::uint32_t kNearFloorTop = kSize * 3 / 4;

// One f16 unit in the last place at `v`: binary16 keeps 11 significant bits, so a normal value
// m * 2^e with m in [0.5, 1) is spaced 2^(e-11) from its neighbours. Read from the exponent, not
// tuned — the same construction terrain's m19.6 anchor uses.
[[nodiscard]] float f16_ulp(float v) {
    int e = 0;
    (void)std::frexp(v, &e);
    return std::ldexp(1.0f, e - 11);
}

struct FloorStats {
    float max_abs = 0.0f;    // the largest |channel| on the floor
    std::uint32_t lit = 0;   // floor pixels with any channel > 0
    std::uint32_t count = 0; // floor pixels visited
    Rgb mean;
};

[[nodiscard]] FloorStats floor_stats(const HdrImage& img, std::uint32_t top = kFloorTop) {
    FloorStats s;
    for (std::uint32_t y = top; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
            const float r = img.rgb[i], g = img.rgb[i + 1], b = img.rgb[i + 2];
            s.max_abs = std::max({s.max_abs, std::fabs(r), std::fabs(g), std::fabs(b)});
            s.lit += (r > 0.0f || g > 0.0f || b > 0.0f) ? 1u : 0u;
            s.mean.r += r;
            s.mean.g += g;
            s.mean.b += b;
            ++s.count;
        }
    }
    const float inv = 1.0f / static_cast<float>(s.count);
    s.mean = {s.mean.r * inv, s.mean.g * inv, s.mean.b * inv};
    return s;
}

// The worst |pixel - expected| over every floor channel, for a floor that should be one flat value.
[[nodiscard]] float floor_worst_error(const HdrImage& img, float expected) {
    float worst = 0.0f;
    for (std::uint32_t y = kFloorTop; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 3;
            for (std::size_t c = 0; c < 3; ++c) {
                worst = std::max(worst, std::fabs(img.rgb[i + c] - expected));
            }
        }
    }
    return worst;
}

// FNV-1a over the decoded floats: a fingerprint to LOG, so a before/after run of this file on two
// builds can be compared by eye. Never asserted against a stored value — that would be a golden.
[[nodiscard]] std::uint64_t fingerprint(const HdrImage& img) {
    std::uint64_t h = 1469598103934665603ull;
    for (const float f : img.rgb) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int k = 0; k < 4; ++k) {
            h = (h ^ ((bits >> (8 * k)) & 0xffu)) * 1099511628211ull;
        }
    }
    return h;
}

// Σ|a − b| / Σ a over the floor: how much of the surface moved, as a fraction of the surface —
// terrain's m19.6 metric, so the two "follows the sky" proofs are the same measurement.
[[nodiscard]] double floor_relative_change(const HdrImage& a, const HdrImage& b) {
    double diff = 0.0;
    double total = 0.0;
    for (std::uint32_t y = kFloorTop; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * a.width + x) * 3;
            for (std::size_t c = 0; c < 3; ++c) {
                diff += std::fabs(double(a.rgb[i + c]) - double(b.rgb[i + c]));
                total += double(a.rgb[i + c]);
            }
        }
    }
    return total > 0.0 ? diff / total : 0.0;
}

[[nodiscard]] MaterialId
add_material(MaterialRegistry& materials, Rgb base, float metallic, float roughness) {
    PbrMaterialDesc md{};
    md.base_color[0] = base.r;
    md.base_color[1] = base.g;
    md.base_color[2] = base.b;
    md.metallic = metallic;
    md.roughness = roughness;
    return materials.add(md);
}

// The floor plus the zero-radiance sun that (with shadows enabled) forces the SHADOWED pipeline —
// the only one with an ambient branch to test — without adding any light of its own.
void build_floor_dark_sun(ecs::World& world, MeshId floor, MaterialId mat) {
    build_floor(world, floor, mat);
    core::Transform sun{};
    sun.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    (void)world_spawn_dark_sun(world, sun);
}

[[nodiscard]] SkyParams sky_at_elevation(float elevation) {
    SkyParams sp = physical_sky(1.0f);
    sp.sun_direction[1] = std::sin(elevation);
    sp.sun_direction[2] = -std::cos(elevation);
    return sp;
}

const Rgb kWarm{0.9f, 0.5f, 0.1f};
const Rgb kCool{0.1f, 0.3f, 0.9f};

} // namespace

TEST_CASE(
    "m19.6b: under the flat ambient a metal is exactly black, and a dielectric is unchanged") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the m19.6b flat-ambient proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "m196b-floor");
    MaterialRegistry materials;
    const MaterialId dielectric = add_material(materials, {kAlbedo, kAlbedo, kAlbedo}, 0.0f, 0.5f);
    const MaterialId half = add_material(materials, {kAlbedo, kAlbedo, kAlbedo}, 0.5f, 0.5f);
    const MaterialId metal = add_material(materials, kWarm, 1.0f, 0.5f);

    LightingSettings ls{};
    ls.shadows_enabled = true;
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_lighting(ls);
    constexpr float kAmbient = 0.11f;
    renderer.set_ambient(kAmbient, kAmbient, kAmbient);
    const auto render = [&](MaterialId mat) {
        return render_hdr(
            *device, renderer, [&](ecs::World& w) { build_floor_dark_sun(w, floor, mat); });
    };

    // (b) METALLIC 0 IS WHAT IT WAS. With no light, white maps and AO = 1 the old shader's whole
    // output was albedo * ambient, and the new one's is (albedo * (1 - 0)) * ambient: a float
    // times 1.0 is itself, so the f32 value is the same product. The shader is a different
    // program than before, though, and how a driver rounds f32 into the f16 target is its own
    // business — so the bound is ONE f16 ULP of the expected value at every floor pixel, read
    // from the format, not the 2% the m17.7b gate test allows.
    const float expected = kAlbedo * kAmbient;
    const HdrImage diel_img = render(dielectric);
    const float diel_err = floor_worst_error(diel_img, expected);
    MESSAGE("m19.6b flat ambient, metallic 0: worst |pixel - albedo*ambient| = "
            << diel_err << " (one f16 ULP = " << f16_ulp(expected) << "), fingerprint "
            << fingerprint(diel_img));
    CHECK(diel_err <= f16_ulp(expected));

    // The weight is the BRDF's (1 - metallic), not a switch: half metal is exactly half the
    // dielectric's product (a power-of-two scale is exact in binary floating point).
    const float half_err = floor_worst_error(render(half), 0.5f * expected);
    MESSAGE("m19.6b flat ambient, metallic 0.5: worst error = " << half_err);
    CHECK(half_err <= f16_ulp(0.5f * expected));

    // (a) A METAL HAS NO DIFFUSE. albedo * (1 - 1) is +0 in every channel, there is no light and
    // no sky, so nothing else can reach the pixel: the floor is 0.0 EXACTLY, whatever its colour.
    // Before m19.6b it read albedo * ambient (0.099 in red here).
    const FloorStats metal_floor = floor_stats(render(metal));
    MESSAGE("m19.6b flat ambient, metallic 1: max |channel| = "
            << metal_floor.max_abs << ", lit pixels " << metal_floor.lit << "/"
            << metal_floor.count);
    CHECK(metal_floor.max_abs == 0.0f);
}

TEST_CASE("m19.6b: SSR reflects each surface at its own F0 — a metal tinted, a dielectric not") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the m19.6b SSR Fresnel proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "m196b-floor");
    MaterialRegistry materials;
    // Red is 1.0 on purpose: 255/255 survives the 8-bit G-buffer exactly, and F0 = 1 makes
    // Schlick's Fresnel exactly 1 at every angle — the channel every exact claim below reads.
    constexpr float kSmooth = 0.05f;
    const Rgb red{1.0f, 0.2f, 0.2f};
    const MaterialId red_metal = add_material(materials, red, 1.0f, kSmooth);
    const MaterialId warm_diel = add_material(materials, kWarm, 0.0f, 0.3f);
    const MaterialId cool_diel = add_material(materials, kCool, 0.0f, 0.3f);

    LightingSettings ls{};
    ls.ssr_enabled = true;
    ls.ssr_max_distance = 8.0f;
    ls.ssr_thickness = 0.5f;
    ls.ssr_max_steps = 64;
    SceneRenderer ssr_on(*device, meshes, materials);
    render::test::disable_temporal_aa(ssr_on); // compares the plain frame (ADR-0078 1e)
    ssr_on.set_lighting(ls);
    SceneRenderer ssr_off(*device, meshes, materials);
    render::test::disable_temporal_aa(ssr_off); // compares the plain frame (ADR-0078 1e)
    // This case's SSR on/off bound below is derived from the ANALYTIC environment BRDF
    // (env_brdf_approx against plain Schlick), so it pins the path that arithmetic describes: the
    // prefiltered chain OFF. That path is still shipped (it is what a frame runs with the setting
    // off), so the proof stays; the chain-ON counterpart -- where both sides compute the identical
    // split-sum product and the ratio is 1 by construction rather than by a fit -- is the case
    // "ADR-0078 s2: with the prefiltered chain on, SSR on and SSR off mirror the sky alike" below.
    ssr_on.set_sky_specular_prefilter_enabled(false);
    ssr_off.set_sky_specular_prefilter_enabled(false);
    const auto render = [&](SceneRenderer& renderer, MaterialId mat) {
        return render_hdr(*device, renderer, [&](ecs::World& w) { build_floor(w, floor, mat); });
    };

    // ── A NEUTRAL GREY ENVIRONMENT (no sky): the exact half ─────────────────────────────────────
    // On the near floor (kNearFloorTop) every reflection ray leaves the screen, so SSR reflects
    // the flat ambient E. The pixel is then  forward + E * F(f0, n.v)  and every term is known.
    constexpr float kAmbient = 0.11f;
    ssr_on.set_ambient(kAmbient, kAmbient, kAmbient);

    // A METAL: forward is albedo * (1 - 1) * E = 0, and red's F0 is 1, so F = 1 + (1 - 1) * f = 1:
    // the red channel is E itself. The resolve blends E with E twice on the way and the target
    // rounds once, so the bound is two f16 ULPs of E — the format's, not a margin. Were the
    // diffuse still applied this would read 2E; were F0 still 0.04, under half of E.
    //
    // And the TINT, exactly: green's F0 is 0.2, so its Fresnel 0.2 + 0.8 * (1 - n.v)^5 is below 1
    // wherever n.v > 0, i.e. at every pixel that can see the floor. Under a grey environment red
    // is therefore strictly above green at EVERY near-floor pixel — no threshold.
    const HdrImage metal_flat = render(ssr_on, red_metal);
    float red_err = 0.0f;
    std::uint32_t red_over_green = 0;
    for (std::uint32_t y = kNearFloorTop; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 3;
            red_err = std::max(red_err, std::fabs(metal_flat.rgb[i] - kAmbient));
            red_over_green += metal_flat.rgb[i] > metal_flat.rgb[i + 1] ? 1u : 0u;
        }
    }
    const FloorStats flat_floor = floor_stats(metal_flat, kNearFloorTop);
    MESSAGE("m19.6b fix 1, grey ambient, SSR on, red metal: worst |red - E| = "
            << red_err << " (two f16 ULP = " << 2.0f * f16_ulp(kAmbient) << "), red > green at "
            << red_over_green << "/" << flat_floor.count << ", mean g=" << flat_floor.mean.g);
    CHECK(red_err <= 2.0f * f16_ulp(kAmbient));
    CHECK(red_over_green == flat_floor.count);

    // A DIELECTRIC IS NOT TINTED. Its pixel is albedo * E + E * F(0.04, n.v): subtract the
    // diffuse (known on the CPU) and what is left is the reflection, which must not know the
    // base colour. Two very different colours, each channel: the residuals agree to the four f16
    // roundings between them — each frame stores its forward target and its resolved target in
    // half floats. A reflection tinted by even 1% of the albedo would miss this by orders.
    const HdrImage warm_flat = render(ssr_on, warm_diel);
    const HdrImage cool_flat = render(ssr_on, cool_diel);
    const float warm_c[3] = {kWarm.r, kWarm.g, kWarm.b};
    const float cool_c[3] = {kCool.r, kCool.g, kCool.b};
    float worst_excess = -1.0f; // |residual difference| - its bound, worst over the floor
    float worst_resid_diff = 0.0f;
    float min_reflection = 1.0e9f;
    for (std::uint32_t y = kNearFloorTop; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 3;
            for (std::size_t c = 0; c < 3; ++c) {
                const float dw = warm_c[c] * kAmbient;
                const float dc = cool_c[c] * kAmbient;
                const float rw = warm_flat.rgb[i + c] - dw;
                const float rc = cool_flat.rgb[i + c] - dc;
                const float bound = f16_ulp(dw) + f16_ulp(dc) + f16_ulp(warm_flat.rgb[i + c]) +
                                    f16_ulp(cool_flat.rgb[i + c]);
                worst_resid_diff = std::max(worst_resid_diff, std::fabs(rw - rc));
                worst_excess = std::max(worst_excess, std::fabs(rw - rc) - bound);
                min_reflection = std::min({min_reflection, rw, rc});
            }
        }
    }
    MESSAGE("m19.6b fix 1, grey ambient, SSR on, dielectrics: worst reflection difference "
            << worst_resid_diff << ", worst excess over the four-ULP bound " << worst_excess
            << ", smallest reflection " << min_reflection);
    CHECK(worst_excess <= 0.0f);
    CHECK(min_reflection > 0.0f); // there IS a reflection to be untinted

    // ── UNDER THE SKY ───────────────────────────────────────────────────────────────────────────
    ssr_on.set_sky(physical_sky(1.0f));
    ssr_off.set_sky(physical_sky(1.0f));
    (void)render(ssr_on, red_metal); // warm both bakes; every measured frame below reuses them
    (void)render(ssr_off, red_metal);

    // The metallic-0 frames, fingerprinted so two builds can be compared (never asserted): these
    // are the same two materials the pre-fix build logged.
    const HdrImage warm_sky = render(ssr_on, warm_diel);
    const HdrImage cool_sky = render(ssr_on, cool_diel);
    const FloorStats wd = floor_stats(warm_sky);
    const FloorStats cd = floor_stats(cool_sky);
    MESSAGE("m19.6b SSR on, sky: dielectric warm r/b="
            << wd.mean.r << "/" << wd.mean.b << " cool r/b=" << cd.mean.r << "/" << cd.mean.b
            << " fingerprints " << fingerprint(warm_sky) << " " << fingerprint(cool_sky));
    CHECK(wd.mean.r > cd.mean.r); // the base colour reaches the frame (through the diffuse)
    CHECK(cd.mean.b > wd.mean.b);

    // THE METAL, SSR ON AGAINST SSR OFF. Same floor, same sky; only who mirrors it differs.
    //   SSR on :  lut(r) * F(f0)                                 (ssr_resolve.frag)
    //   SSR off:  mix(lut(r), sh, alpha) * EnvBRDFApprox(f0, roughness, n.v)   (the forward pass)
    // In the red channel f0 = 1, where both weights are closed-form: Schlick is exactly 1, and the
    // fit is A + B = 1 - 0.55 * roughness (the n.v terms cancel). So
    //   on / off = 1 / ((1 - 0.55 * roughness) * (1 + alpha * (sh / lut - 1))).
    // The sky is not negative, so sh / lut >= 0 and the ratio cannot exceed
    //   1 / ((1 - 0.55 * roughness) * (1 - alpha))                — the UPPER bound, derived.
    // Downward it is limited only by how much brighter the hemisphere's average is than the
    // mirrored direction; kSkyContrast = 8 is the stated assumption (measured here: the floor
    // mean sits near the top of the interval, so the average is not far above the mirror).
    // If SSR's reflection and the forward term were BOTH applied the ratio would be about 2; with
    // the old dielectric Fresnel it was 0.29 — either falls far outside.
    const HdrImage on = render(ssr_on, red_metal);
    const HdrImage off = render(ssr_off, red_metal);
    const float alpha = kSmooth * kSmooth;
    const float fit = 1.0f - 0.55f * kSmooth;
    constexpr float kSkyContrast = 8.0f;
    const double upper = 1.0 / (double(fit) * (1.0 - double(alpha)));
    const double lower = 1.0 / (double(fit) * (1.0 + double(alpha) * (kSkyContrast - 1.0)));
    const FloorStats on_floor = floor_stats(on, kNearFloorTop);
    const FloorStats off_floor = floor_stats(off, kNearFloorTop);
    const double ratio = double(on_floor.mean.r) / double(off_floor.mean.r);
    std::uint32_t sky_red_over_green = 0;
    for (std::uint32_t y = kNearFloorTop; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 3;
            sky_red_over_green += on.rgb[i] > on.rgb[i + 1] ? 1u : 0u;
        }
    }
    MESSAGE("m19.6b fix 1, sky, red metal: SSR on r/g/b="
            << on_floor.mean.r << "/" << on_floor.mean.g << "/" << on_floor.mean.b
            << " SSR off r/g/b=" << off_floor.mean.r << "/" << off_floor.mean.g << "/"
            << off_floor.mean.b << "; red on/off = " << ratio << " in [" << lower << ", " << upper
            << "]; red > green at " << sky_red_over_green << "/" << on_floor.count);
    CHECK(ratio <= upper);
    CHECK(ratio >= lower);
    // The reflected sky is RED-tinted: red above green at every near-floor pixel. (Blue stays
    // above red here — the sky is blue, and a tint scales a colour, it does not replace it.)
    CHECK(sky_red_over_green == on_floor.count);
}

// SSR ON AGAINST SSR OFF, WITH THE PREFILTERED CHAIN (ADR-0078 section 2). The property whose
// absence kept the feature switched off. With SSR off the forward pass mirrors the sky
// (prefiltered(dominant(n, r, rough), rough) * envBRDF(f0, rough, n.v)); with SSR on the forward
// pass's sky term is compiled out and ssr_resolve.frag must add the SAME product. Both include
// sky_specular_eval.glsl, so for a pixel whose reflection ray leaves the screen -- every pixel of
// the near floor, by construction (see kNearFloorTop) -- the two images differ only by the
// precision of what the SSR path reads back out of the G-buffer: the shading normal and perceptual
// roughness, stored as RGBA16Float (11 significant bits, a relative step of about 5e-4 each).
// That perturbs the reflection direction and the lookup roughness slightly (inferred from the
// format, not isolated by a probe), and the sky is not constant across either, so a small residual
// is EXPECTED rather than a defect. The
// bounds below are measured on both devices (RTX 3060: worst pixel 1.0%, worst mean 0.7%;
// lavapipe: worst pixel 1.1%, worst mean 0.8%) with about 2.7x headroom: 3% per pixel, 2% on the
// mean. Anything structural -- a missing weight, the wrong roughness, the wrong BRDF -- moves a
// rough metal by tens of percent (the control below: 3.5x), so these cannot hide one.
//
// The control is the SAME measurement with the chain OFF, where SSR still reflects the raw LUT
// with plain Schlick: it must disagree with the forward pass by at least 2x on the roughest metal
// (the ADR's recorded "about 2x"; measured 3.5x), or this test could not see the bug it exists
// for.
TEST_CASE("ADR-0078 s2: with the prefiltered chain on, SSR on and SSR off mirror the sky alike") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the SSR/forward sky coherence proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "adr78-coherence-floor");
    MaterialRegistry materials;

    struct Case {
        const char* name;
        Rgb base;
        float metallic;
        float roughness;
    };

    const Case cases[] = {
        {"red metal, r=0.05", {1.0f, 0.2f, 0.2f}, 1.0f, 0.05f},
        {"warm metal, r=0.30", kWarm, 1.0f, 0.30f},
        {"warm metal, r=0.60", kWarm, 1.0f, 0.60f},
        {"grey metal, r=1.00", {0.8f, 0.8f, 0.8f}, 1.0f, 1.00f},
        {"grey dielectric, r=1.00", {0.8f, 0.8f, 0.8f}, 0.0f, 1.00f},
    };

    LightingSettings ls{};
    ls.ssr_enabled = true;
    ls.ssr_max_distance = 8.0f;
    ls.ssr_thickness = 0.5f;
    ls.ssr_max_steps = 64;

    // One renderer per (SSR, chain) corner, each warmed on the same sky so every measured frame
    // reuses its bake. Same floor, same sky, same camera: ONLY who mirrors the sky differs.
    const auto make = [&](bool ssr, bool chain) {
        auto r = std::make_unique<SceneRenderer>(*device, meshes, materials);
        render::test::disable_temporal_aa(*r); // compares the plain frame (ADR-0078 1e)
        if (ssr) {
            r->set_lighting(ls);
        }
        r->set_sky_specular_prefilter_enabled(chain);
        r->set_sky(physical_sky(1.0f));
        return r;
    };
    auto on_chain = make(true, true);
    auto off_chain = make(false, true);
    auto on_legacy = make(true, false);
    auto off_legacy = make(false, false);

    const auto render = [&](SceneRenderer& renderer, MaterialId mat) {
        (void)render_hdr(*device, renderer, [&](ecs::World& w) { build_floor(w, floor, mat); });
        return render_hdr(*device, renderer, [&](ecs::World& w) { build_floor(w, floor, mat); });
    };

    // Per-channel mean ratio on/off, and the worst per-pixel |on - off| / off over the near floor.
    struct Agreement {
        double ratio[3] = {0, 0, 0};
        double worst_pixel = 0.0;
    };

    const auto compare = [&](const HdrImage& on, const HdrImage& off) {
        Agreement a;
        double sum_on[3] = {0, 0, 0}, sum_off[3] = {0, 0, 0};
        for (std::uint32_t y = kNearFloorTop; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 3;
                for (std::size_t c = 0; c < 3; ++c) {
                    sum_on[c] += on.rgb[i + c];
                    sum_off[c] += off.rgb[i + c];
                    a.worst_pixel = std::max(a.worst_pixel,
                                             std::fabs(double(on.rgb[i + c]) - off.rgb[i + c]) /
                                                 std::max(double(off.rgb[i + c]), 1.0e-4));
                }
            }
        }
        for (std::size_t c = 0; c < 3; ++c) {
            a.ratio[c] = sum_on[c] / sum_off[c];
        }
        return a;
    };

    for (const Case& c : cases) {
        const MaterialId mat = add_material(materials, c.base, c.metallic, c.roughness);
        const Agreement chain = compare(render(*on_chain, mat), render(*off_chain, mat));
        const Agreement legacy = compare(render(*on_legacy, mat), render(*off_legacy, mat));
        // Per pixel, then on the mean: the mean could hide opposite errors, the pixel cannot.
        CHECK_MESSAGE(chain.worst_pixel <= 0.03, c.name);
        for (const double r : chain.ratio) {
            CHECK_MESSAGE(std::fabs(r - 1.0) <= 0.02, c.name);
        }
        // The chain is strictly closer to agreement than the legacy pair, on every case.
        CHECK_MESSAGE(std::fabs(chain.ratio[0] - 1.0) < std::fabs(legacy.ratio[0] - 1.0), c.name);
        if (c.metallic == 1.0f && c.roughness == 1.0f) {
            CHECK_MESSAGE(legacy.ratio[0] >= 2.0, "control: " << c.name);
        }
        MESSAGE(c.name << ": chain ON  on/off r/g/b = " << chain.ratio[0] << "/" << chain.ratio[1]
                       << "/" << chain.ratio[2] << " worst pixel " << chain.worst_pixel
                       << " | chain OFF on/off r/g/b = " << legacy.ratio[0] << "/"
                       << legacy.ratio[1] << "/" << legacy.ratio[2]);
    }
    CHECK(on_chain->sky_specular_stats().prefilter_filled >= 1);
    CHECK(off_chain->sky_specular_stats().prefilter_filled >= 1);
}

TEST_CASE("m19.6b: with SSR off, the forward pass mirrors the sky — a smooth metal follows it") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the m19.6b specular sky proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "m196b-floor");
    MaterialRegistry materials;
    const MaterialId warm_metal = add_material(materials, kWarm, 1.0f, 0.1f);
    const MaterialId cool_metal = add_material(materials, kCool, 1.0f, 0.1f);
    const MaterialId grey_metal = add_material(materials, {0.8f, 0.8f, 0.8f}, 1.0f, 0.1f);
    const MaterialId grey_diel = add_material(materials, {0.8f, 0.8f, 0.8f}, 0.0f, 1.0f);

    SceneRenderer renderer(*device, meshes, materials); // SSR off: nothing else mirrors the sky
    const auto render = [&](MaterialId mat, const SkyParams& sky) {
        renderer.set_sky(sky);
        return render_hdr(*device, renderer, [&](ecs::World& w) { build_floor(w, floor, mat); });
    };
    const SkyParams high = sky_at_elevation(1.2f); // ~69 degrees
    const SkyParams low = sky_at_elevation(0.08f); // ~5 degrees: a sunset sky

    // A METAL IS LIT, AND BY ITS OWN F0. No light, no diffuse (metallic 1), SSR off: every photon
    // on this floor is the forward pass's specular sky term, env * (f0 * A + B) with f0 = the base
    // colour and A > 0. So the floor is lit everywhere, and the metal whose base colour is larger
    // in a channel is strictly brighter in that channel — under the SAME sky, in the same frame
    // geometry. (With SSR on these two are the same image: the case above.)
    const FloorStats warm = floor_stats(render(warm_metal, high));
    const FloorStats cool = floor_stats(render(cool_metal, high));
    MESSAGE("m19.6b SSR off, sky: warm metal r/b="
            << warm.mean.r << "/" << warm.mean.b << " cool metal r/b=" << cool.mean.r << "/"
            << cool.mean.b << " lit " << warm.lit << "/" << warm.count);
    CHECK(warm.lit == warm.count);
    CHECK(warm.mean.r > cool.mean.r);
    CHECK(cool.mean.b > warm.mean.b);

    // (c) THE MIRROR OF TERRAIN'S m19.6 (b). Swap the sky and nothing else. The smooth metal
    // shows the sky-view LUT along its reflection ray (alpha = 0.01: 99% point sample); the
    // roughness-1 dielectric shows only the nine-coefficient SH average, in both its diffuse and
    // its (alpha = 1) specular. A sunset moves one direction of the sky more than it moves the
    // hemisphere's average, so the metal must move strictly more.
    const HdrImage metal_high = render(grey_metal, high);
    const HdrImage metal_low = render(grey_metal, low);
    const HdrImage diel_high = render(grey_diel, high);
    const HdrImage diel_low = render(grey_diel, low);
    CHECK(renderer.sky_lighting_stats().filled >= 4); // every swap really re-baked
    const double metal_change = floor_relative_change(metal_high, metal_low);
    const double diel_change = floor_relative_change(diel_high, diel_low);
    MESSAGE("m19.6b relative change under the sky swap: smooth metal "
            << metal_change << ", rough dielectric " << diel_change << "; fingerprint diel high "
            << fingerprint(diel_high));
    CHECK(metal_change > 0.0);
    CHECK(metal_change > diel_change);
}

TEST_CASE("m19.6b: DDGI's indirect diffuse does not light a metal either") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the m19.6b DDGI proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "m196b-floor");
    MaterialRegistry materials;
    const MaterialId dielectric = add_material(materials, kWarm, 0.0f, 0.5f);
    const MaterialId metal = add_material(materials, kWarm, 1.0f, 0.5f);

    // An empty SDF field: every probe ray escapes and returns the flat ambient, so the lattice
    // fills with a known, non-zero irradiance and the DDGI branch is the floor's only light.
    LightingSettings ls{};
    ls.sdf_clipmap_enabled = true;
    ls.ddgi_enabled = true;
    ls.ddgi_hysteresis = 0.7f;
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_lighting(ls);
    renderer.set_ambient(0.5f, 0.5f, 0.5f);
    const auto render = [&](MaterialId mat) {
        return render_hdr(*device, renderer, [&](ecs::World& w) { build_floor(w, floor, mat); });
    };
    for (int i = 0; i < 8; ++i) {
        (void)render(dielectric); // let the probes accumulate
    }

    // The control first: the field is live — a dielectric on this floor is lit by it. Then the
    // same field, the same frame, metallic 1: indirect * (albedo * 0) is exactly zero.
    const FloorStats diel_floor = floor_stats(render(dielectric));
    const FloorStats metal_floor = floor_stats(render(metal));
    MESSAGE("m19.6b DDGI: dielectric lit " << diel_floor.lit << "/" << diel_floor.count
                                           << " mean r=" << diel_floor.mean.r
                                           << "; metal max |channel| = " << metal_floor.max_abs);
    CHECK(diel_floor.lit > diel_floor.count / 2);
    CHECK(metal_floor.max_abs == 0.0f);
}
