// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky LIGHTS the scene (m17.7b) — the structural proof. No golden images, the M5.6/M6.4
// pattern: every claim below is a property the technique guarantees, checked against a control
// that isolates it.
//
//   1. UNITS. Under a sky of uniform radiance L, a white Lambertian surface must come out at
//      exactly `albedo * L` — no factor of pi either way. This is the sharpest claim in the file
//      and the reason it is first: the SH path is nine coefficients, a cosine convolution and a
//      solid-angle weight, and getting any one of them wrong scales the whole scene by a constant
//      that looks like a tuning choice rather than a bug. It is also what makes m17.7b a
//      REPLACEMENT for the flat ambient constant rather than a re-tuning of it: feed the old
//      constant's value in as a uniform sky and the frame does not move.
//      (docs/math/sky-lighting.md §4 derives it: 0.282095^2 * 4*pi = 1.)
//
//   2. DIRECTION. An up-facing surface under a sky that is bright ABOVE must receive more than the
//      same surface under a sky of the same colours arranged bright at the HORIZON. This is what
//      "the sky lights the scene" means beyond "the scene got brighter" — and the specific bug it
//      catches is an SH path that collapsed to its l=0 term, which would light both identically.
//
//   3. COLOUR. The light a surface receives carries the SKY's colour, not the ambient constant's.
//      A red-zenith sky must tint an up-facing surface red; swapping zenith and horizon must flip
//      the tint. Asserted in both directions, so a hardcoded constant cannot pass it.
//
//   4. OFF IS THE OLD PATH. With no sky, a lit surface reads exactly `albedo * ambient * ao` — the
//      pre-m17.7b constant, unchanged. ADR-0032 §11's rule, asserted at the far end rather than by
//      inspecting which branch ran.
//
//   5. THE BAKE IS CACHED, AND THE CACHE CAN BE SEEN TO WORK. An unchanged sky must REUSE, a
//      changed one must REFILL. A cache that silently never refills renders a stale sky and looks
//      like a working one, so the counters are asserted rather than trusted.
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

// A sky of ONE radiance in every direction. Zenith == horizon makes the gradient constant;
// `ground = 1` stops the below-horizon darkening; a black sun kills the forward-scatter glow (the
// disc is already excluded from the lighting path by construction); clouds off removes the noise.
// What is left is a sky whose sky_lighting_radiance() is exactly `L` for every direction — the one
// case whose correct answer can be written down.
[[nodiscard]] SkyParams uniform_sky(float L) {
    SkyParams sp{};
    sp.enabled = true;
    sp.clouds_enabled = false;
    sp.intensity = 1.0f;
    sp.ground = 1.0f;
    sp.zenith[0] = sp.zenith[1] = sp.zenith[2] = L;
    sp.horizon[0] = sp.horizon[1] = sp.horizon[2] = L;
    sp.sun_radiance[0] = sp.sun_radiance[1] = sp.sun_radiance[2] = 0.0f;
    sp.use_scene_sun = false;
    return sp;
}

// A two-colour sky with the glow and clouds removed, so the only thing varying is WHERE each
// colour sits on the sphere.
[[nodiscard]] SkyParams split_sky(Rgb zenith, Rgb horizon) {
    SkyParams sp = uniform_sky(1.0f);
    sp.zenith[0] = zenith.r;
    sp.zenith[1] = zenith.g;
    sp.zenith[2] = zenith.b;
    sp.horizon[0] = horizon.r;
    sp.horizon[1] = horizon.g;
    sp.horizon[2] = horizon.b;
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

TEST_CASE(
    "sky lighting: a uniform sky of radiance L lights a surface at exactly albedo*L (m17.7b)") {
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

    // ── (1) Units ───────────────────────────────────────────────────────────────────────────────
    // The SAME value, delivered as a uniform sky instead of as the constant, must produce the SAME
    // frame. That is what "unit-for-unit replacement" means, and it is the strongest form of the
    // claim: not "close to", but "the number you already had".
    renderer.set_sky(uniform_sky(0.11f));
    const HdrImage same = render_hdr(*device, renderer, build);
    const Rgb floor_same = block_mean(same, kSize / 2, kSize * 3 / 4, 5);
    MESSAGE("uniform sky L=0.11: floor=" << floor_same.r << " (constant path gave " << floor_off.r
                                         << ")");
    CHECK(floor_same.r == doctest::Approx(floor_off.r).epsilon(0.02));

    // And it tracks L, so the agreement above is not two constants coinciding.
    renderer.set_sky(uniform_sky(0.40f));
    const HdrImage bright = render_hdr(*device, renderer, build);
    const Rgb floor_bright = block_mean(bright, kSize / 2, kSize * 3 / 4, 5);
    MESSAGE("uniform sky L=0.40: floor=" << floor_bright.r << " expected=" << kAlbedo * 0.40f);
    CHECK(floor_bright.r == doctest::Approx(kAlbedo * 0.40f).epsilon(0.02));
}

TEST_CASE("sky lighting: WHERE the sky is bright changes what a surface receives (m17.7b)") {
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

    // ── (2) Direction ───────────────────────────────────────────────────────────────────────────
    // Two skies built from the SAME two colours, swapped. An up-facing floor weights the zenith far
    // more than the horizon (that is what the cosine in the integral does), so the bright-above sky
    // must deliver materially more light. An SH path that collapsed to its l=0 term would average
    // the sphere and light these two nearly identically — which is exactly the failure this
    // catches.
    renderer.set_sky(split_sky({1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}));
    const Rgb up_bright =
        block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);

    renderer.set_sky(split_sky({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}));
    const Rgb horizon_bright =
        block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);

    MESSAGE("bright above=" << up_bright.r << "  bright at horizon=" << horizon_bright.r
                            << "  ratio=" << up_bright.r / std::max(horizon_bright.r, 1e-6f));
    CHECK(up_bright.r > horizon_bright.r * 1.5f);

    // ── (3) Colour, asserted in both directions ────────────────────────────────────────────────
    // A red zenith over a blue horizon must tint the floor RED; swapping the two must flip it. One
    // direction alone could be passed by any constant that happens to be reddish.
    renderer.set_sky(split_sky({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}));
    const Rgb red_up =
        block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);
    renderer.set_sky(split_sky({0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}));
    const Rgb blue_up =
        block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);

    MESSAGE("red zenith: r=" << red_up.r << " b=" << red_up.b << "   blue zenith: r=" << blue_up.r
                             << " b=" << blue_up.b);
    CHECK(red_up.r > red_up.b);
    CHECK(blue_up.b > blue_up.r);
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

    renderer.set_sky(uniform_sky(0.2f));
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

    // Change the sky: it must notice. The other half of the claim — a cache that never refills
    // passes the assertion above perfectly.
    renderer.set_sky(uniform_sky(0.5f));
    (void)render_hdr(*device, renderer, build);
    const auto after_change = renderer.sky_lighting_stats();
    MESSAGE("after changing the sky: filled=" << after_change.filled
                                              << " reused=" << after_change.reused);
    CHECK(after_change.filled == 2);
    CHECK(after_change.reused == 3);
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
    renderer.set_sky(uniform_sky(0.40f));
    const HdrImage on = render_hdr(*device, renderer, build);
    const Rgb floor_on = block_mean(on, kSize / 2, kSize * 3 / 4, 5);
    MESSAGE("shadowed path, uniform sky L=0.40: floor=" << floor_on.r
                                                        << " expected=" << kAlbedo * 0.40f);
    CHECK(floor_on.r == doctest::Approx(kAlbedo * 0.40f).epsilon(0.02));
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

    // Two skies whose ZENITH differs — the part of the sky a floor's reflection rays point at.
    // Most of those rays leave the screen, which is exactly the path that used to return one flat
    // colour regardless of direction.
    renderer.set_sky(split_sky({0.9f, 0.05f, 0.05f}, {0.05f, 0.05f, 0.05f}));
    (void)render_hdr(*device, renderer, build); // warm the bake, then measure a steady frame
    const Rgb red = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);

    renderer.set_sky(split_sky({0.05f, 0.05f, 0.9f}, {0.05f, 0.05f, 0.05f}));
    (void)render_hdr(*device, renderer, build);
    const Rgb blue = block_mean(render_hdr(*device, renderer, build), kSize / 2, kSize * 3 / 4, 5);

    MESSAGE("mirror floor — red zenith: r=" << red.r << " b=" << red.b
                                            << " | blue zenith: r=" << blue.r << " b=" << blue.b);
    // The reflection carries the sky's colour, and swapping the sky swaps it. A flat fallback
    // could not do this: it would return the same constant in both frames.
    CHECK(red.r > red.b);
    CHECK(blue.b > blue.r);
}
