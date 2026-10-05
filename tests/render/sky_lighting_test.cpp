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

[[nodiscard]] FloorStats floor_stats(const HdrImage& img) {
    FloorStats s;
    for (std::uint32_t y = kFloorTop; y < kSize; ++y) {
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

// Whole-frame pixels (floor AND background) at which two decoded images differ. decode_hdr is
// exact, so equal floats here are equal half-floats in the target.
[[nodiscard]] std::uint32_t differing_pixels(const HdrImage& a, const HdrImage& b) {
    std::uint32_t n = 0;
    for (std::size_t p = 0; p < a.rgb.size() / 3; ++p) {
        n += (a.rgb[p * 3] != b.rgb[p * 3] || a.rgb[p * 3 + 1] != b.rgb[p * 3 + 1] ||
              a.rgb[p * 3 + 2] != b.rgb[p * 3 + 2])
                 ? 1u
                 : 0u;
    }
    return n;
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

TEST_CASE("m19.6b: with SSR on, two metals of different colour are the same image under the sky") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the m19.6b SSR gating proof");
        return;
    }
    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "m196b-floor");
    MaterialRegistry materials;
    const MaterialId warm_metal = add_material(materials, kWarm, 1.0f, 0.3f);
    const MaterialId cool_metal = add_material(materials, kCool, 1.0f, 0.3f);
    const MaterialId warm_diel = add_material(materials, kWarm, 0.0f, 0.3f);
    const MaterialId cool_diel = add_material(materials, kCool, 0.0f, 0.3f);

    LightingSettings ls{};
    ls.ssr_enabled = true;
    ls.ssr_max_distance = 8.0f;
    ls.ssr_thickness = 0.5f;
    ls.ssr_max_steps = 64;
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_lighting(ls);
    renderer.set_sky(physical_sky(1.0f));
    const auto render = [&](MaterialId mat) {
        return render_hdr(*device, renderer, [&](ecs::World& w) { build_floor(w, floor, mat); });
    };
    (void)render(warm_metal); // warm the sky bake; every measured frame below reuses it

    // (a) + the SSR GATING, in one exact property. With SSR on, the pixel is
    //     forward + reflection * fresnel(0.04)          (ssr_resolve.frag)
    // and the reflection reads only the G-buffer (normal, roughness), the depth and the sky —
    // never the base colour. So the base colour can reach the pixel ONLY through the forward
    // pass, and there a metal's diffuse is albedo * (1 - 1) = 0. If the forward pass also added
    // its own specular sky term here, f0 = albedo would tint it and the two frames would differ;
    // if the diffuse were still applied, albedo * sky would. Neither happens: the two frames are
    // the SAME IMAGE, pixel for pixel, float for float — one program, identical inputs.
    const HdrImage warm = render(warm_metal);
    const HdrImage cool = render(cool_metal);
    const std::uint32_t metal_diff = differing_pixels(warm, cool);
    const FloorStats warm_floor = floor_stats(warm);
    MESSAGE("m19.6b SSR on, sky: warm vs cool metal differ at "
            << metal_diff << " pixels; floor lit " << warm_floor.lit << "/" << warm_floor.count
            << " mean r=" << warm_floor.mean.r << " b=" << warm_floor.mean.b);
    CHECK(metal_diff == 0);
    // ...and that image is not black: SSR's sky reflection is there, once.
    CHECK(warm_floor.lit == warm_floor.count);

    // The control that makes "0 pixels differ" evidence: the SAME two colours at metallic 0 do
    // differ, everywhere on the floor, so the colour is reaching the shader and it is the metal
    // weighting — not a dead uniform — that removed it.
    const HdrImage warm_d = render(warm_diel);
    const HdrImage cool_d = render(cool_diel);
    const FloorStats wd = floor_stats(warm_d);
    const FloorStats cd = floor_stats(cool_d);
    MESSAGE("m19.6b SSR on, sky: dielectric warm r/b="
            << wd.mean.r << "/" << wd.mean.b << " cool r/b=" << cd.mean.r << "/" << cd.mean.b
            << " fingerprints " << fingerprint(warm_d) << " " << fingerprint(cool_d));
    CHECK(differing_pixels(warm_d, cool_d) >= wd.count);
    CHECK(wd.mean.r > cd.mean.r);
    CHECK(cd.mean.b > wd.mean.b);
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
