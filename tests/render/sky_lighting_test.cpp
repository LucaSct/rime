// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky LIGHTS the scene — the structural proof. m17.7d replaces the authored gradient in the
// sky-view/SH body with physical single scattering, so these controls prove source scale, solar
// colour, cache visibility, the sky-off gate, and escaped-ray reflection. The m17.0 background
// remains authored on purpose; this file explicitly proves its zenith/horizon colours no longer
// leak into physical lighting. No golden images: every claim is a transport property isolated by a
// control, following the M5.6/M6.4 pattern.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

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
