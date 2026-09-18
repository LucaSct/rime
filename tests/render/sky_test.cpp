// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky (m17.0): a procedural daytime sky composited into the frame wherever the depth buffer
// says nothing was drawn. This is the pass's structural proof — no golden images, the M5.6/M6.4
// pattern. Every claim below is a property the pass GUARANTEES by construction, measured against a
// control that isolates it:
//
//   1. It is a BACKGROUND composite. The background stops being the clear colour, and every shaded
//      pixel comes out BIT-IDENTICAL to the sky-off frame. Bit-identical rather than
//      approximately-equal is the honest claim: the shader's `depth < 1.0` branch copies the
//      texel, so any difference at all means the sky reached a pixel the scene owned.
//   2. The sun sits where the light comes FROM. `DirectionalLight::direction` is where light
//      TRAVELS and the shader wants the opposite, so this is a sign that is easy to get backwards
//      and invisible in any test that only checks "there is a bright spot somewhere". Flipping the
//      light must move the disc out of frame — a sign error fails this in both directions.
//   3. The Sky COMPONENT is what turns the pass on, and its fields land where they claim to. A
//      world with no Sky entity renders the pre-sky frame; one with a Sky renders a sky nobody
//      called set_sky() for, with the zenith colour overhead and the horizon colour at eye level.
//      That is the whole world → ExtractedScene → SkyParams → UBO → shader chain, asserted at the
//      far end rather than anywhere convenient in the middle.
//   4. Clouds draw, and `coverage` is genuinely the knob. The coverage-0 control is EXACT rather
//      than a margin: five octaves of value noise at halving amplitude sum to at most 0.96875, and
//      coverage 0 thresholds at 1.0, so no fragment can reach the cloud mix. A knob that is merely
//      "close to off" would not be the same proof.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
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

constexpr std::uint32_t kSize = 128;
constexpr float kHalfPi = 1.5707963f;
constexpr float kFovY = 1.1f;

// Both the camera and a directional light aim down their own −z (extract_scene's convention, shared
// with spot lights). A rotation of +pi/2 about x therefore takes (0,0,−1) to (0,+1,0): a camera
// that looks straight up, or a light that travels straight up. −pi/2 is the same thing downward.
[[nodiscard]] core::Quat pitched(float radians) {
    return core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, radians);
}

// Mean linear RGB over a block, so a single noisy texel cannot carry a claim.
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
    const auto inv = 1.0f / static_cast<float>(n);
    return {sum.r * inv, sum.g * inv, sum.b * inv};
}

// One frame, decoded. `build` populates a fresh world; the renderer is reused across frames within
// a case (its pipelines are expensive and its per-frame state is the scene's, not the frame's), so
// a case that wants the sky off simply renders before calling set_sky.
template <typename Build>
[[nodiscard]] HdrImage render_hdr(rhi::Device& device, SceneRenderer& renderer, Build&& build) {
    ecs::World world;
    register_render_components(world);
    build(world);
    RenderGraph graph(device);
    graph.reset();
    // Read the HDR the tonemap consumed, never the tonemapped LDR: the sky's colours are linear
    // radiance and asserting through the ACES curve would test the curve as much as the sky.
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize});
    REQUIRE(out.hdr.is_valid());
    graph.export_texture(out.hdr);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return decode_hdr(read_texture(device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
}

// Spawn a camera at `eye` with the given pitch. fov/near/far are fixed across every case here so
// the pixel-to-direction reasoning in the comments holds throughout.
void spawn_camera(ecs::World& world, core::Vec3 eye, float pitch) {
    core::Transform cam{};
    cam.translation = eye;
    cam.rotation = pitched(pitch);
    (void)world.spawn_with(ecs::WorldTransform{cam}, Camera{kFovY, 0.1f, 100.0f, true});
}

// A sky whose defaults are pinned for a test: clouds off (their noise is signal in exactly one case
// and contamination in the others) and enabled, so `set_sky` alone drives the pass.
[[nodiscard]] SkyParams plain_sky() {
    SkyParams sp{};
    sp.enabled = true;
    sp.clouds_enabled = false;
    return sp;
}

} // namespace

TEST_CASE("sky: fills the background, and leaves every shaded pixel bit-identical (m17.0)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the sky background proof");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(12.0f), "sky-floor");
    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.6f;
    md.roughness = 0.8f;
    const MaterialId floor_mat = materials.add(md);

    // Camera 1 m up, level (pitch 0 → looking down −z). The horizon lands on the middle row, so the
    // frame splits cleanly: sky above, floor below. A sun overhead lights the floor and stays out
    // of frame (the top edge only reaches dir.y ≈ tan(0.55) ≈ 0.61), so no disc contaminates the
    // gradient reading.
    const auto build = [&](ecs::World& world) {
        (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor}, MaterialRef{floor_mat});
        core::Transform sun{};
        sun.rotation = pitched(-kHalfPi); // travels straight down: the sun is overhead
        (void)world.spawn_with(ecs::WorldTransform{sun}, DirectionalLight{1.0f, 1.0f, 1.0f, 3.0f});
        spawn_camera(world, {0.0f, 1.0f, 0.0f}, 0.0f);
    };

    // Off first, then on. One renderer and one world recipe across both frames, so the sky pass is
    // the only thing that differs between them.
    SceneRenderer renderer(*device, meshes, materials);
    const HdrImage off = render_hdr(*device, renderer, build);
    renderer.set_sky(plain_sky());
    const HdrImage on = render_hdr(*device, renderer, build);

    // ── (1) The background stops being the clear colour, and it is a daytime sky ────────────────
    // A block well above the horizon: nothing is drawn there, so with the sky off it is the black
    // the forward pass cleared to.
    const Rgb bg_off = block_mean(off, kSize / 2, kSize / 8, 6);
    const Rgb bg_on = block_mean(on, kSize / 2, kSize / 8, 6);
    MESSAGE("background: off lum=" << bg_off.luminance() << " on rgb=(" << bg_on.r << ", "
                                   << bg_on.g << ", " << bg_on.b << ")");
    CHECK(bg_off.luminance() < 1e-4f);
    CHECK(bg_on.luminance() > 0.2f);
    // Blue-dominant by a wide margin — the gradient's whole point. At this height the mix is ~81%
    // zenith, which puts blue around 3x red; 1.5x is the margin, not the prediction.
    CHECK(bg_on.b > bg_on.r * 1.5f);

    // ── (2) Above the horizon is brighter than at it ────────────────────────────────────────────
    // The gradient is monotone in dir.y by construction (pow of a clamped up-component), so the
    // saturated zenith must read as more blue-relative-to-red than the pale horizon does. Sampled
    // just above the middle row, where the horizon colour dominates.
    const Rgb horizon_on = block_mean(on, kSize / 2, kSize / 2 - 8, 3);
    MESSAGE("gradient: zenith b/r=" << bg_on.b / bg_on.r
                                    << "  horizon b/r=" << horizon_on.b / horizon_on.r);
    CHECK(bg_on.b / bg_on.r > horizon_on.b / horizon_on.r);

    // ── (3) The scene is untouched, EXACTLY ────────────────────────────────────────────────────
    // Rows 80–95% of the frame are floor (they look down at 1.8–2.7 m, well inside the 12 m plane).
    // Both frames run the same forward pass over the same draws, so the sky-off values are what the
    // sky-on frame must carry through unchanged. Denormals are the one thing that could make an
    // exact fp16 comparison fragile, so the block is required to be genuinely lit first — which
    // also keeps "identical" from being a claim about black.
    std::uint32_t compared = 0;
    std::uint32_t differing = 0;
    float min_lum = 1e30f;
    for (std::uint32_t y = kSize * 80 / 100; y < kSize * 95 / 100; ++y) {
        for (std::uint32_t x = kSize * 35 / 100; x < kSize * 65 / 100; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 3;
            min_lum = std::min(min_lum, off.luminance(x, y));
            ++compared;
            if (off.rgb[i] != on.rgb[i] || off.rgb[i + 1] != on.rgb[i + 1] ||
                off.rgb[i + 2] != on.rgb[i + 2]) {
                ++differing;
            }
        }
    }
    MESSAGE("shaded block: " << compared << " px, min lum=" << min_lum
                             << ", differing=" << differing);
    REQUIRE(compared > 0);
    REQUIRE(min_lum > 1e-3f); // genuinely lit, and far above the fp16 denormal floor
    CHECK(differing == 0);
}

TEST_CASE("sky: the sun disc sits where the light comes FROM, not where it travels (m17.0)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the sun-coupling proof");
        return;
    }

    MeshRegistry meshes(*device);
    MaterialRegistry materials;

    // No geometry at all: the whole frame is background, so the disc has nowhere to hide. The
    // camera looks straight up, so a sun whose light TRAVELS downward is dead centre.
    const auto build_with_sun = [&](float light_pitch) {
        return [&, light_pitch](ecs::World& world) {
            core::Transform sun{};
            sun.rotation = pitched(light_pitch);
            (void)world.spawn_with(ecs::WorldTransform{sun},
                                   DirectionalLight{1.0f, 1.0f, 1.0f, 1.0f});
            spawn_camera(world, {0.0f, 0.0f, 0.0f}, kHalfPi);
        };
    };
    SceneRenderer renderer(*device, meshes, materials);
    SkyParams sp = plain_sky();
    // A 128 px frame at 1.1 rad is 0.0086 rad per pixel, so the default 0.012 rad disc is about
    // 1.4 px across and ANY block average of it is mostly sky — the reading would be measuring the
    // frame's resolution, not the sun. Widen it to roughly 7 px so a small block sits inside the
    // disc. This is the parameter's own stated purpose (a small editor viewport needs the same
    // nudge), not a fudge introduced for the test.
    sp.angular_radius = 0.06f;
    renderer.set_sky(sp);

    // Light travelling DOWN (−pi/2) ⇒ it comes from above ⇒ the disc is where the camera points.
    const HdrImage sun_above = render_hdr(*device, renderer, build_with_sun(-kHalfPi));
    // The same light flipped: travelling UP, so the sun is below the horizon and the disc must
    // leave the frame entirely. A shader that dropped the negation renders these two the other way
    // round, and this pair fails in both directions.
    const HdrImage sun_below = render_hdr(*device, renderer, build_with_sun(kHalfPi));

    const float centre_above = block_mean(sun_above, kSize / 2, kSize / 2, 2).luminance();
    const float centre_below = block_mean(sun_below, kSize / 2, kSize / 2, 2).luminance();
    const float corner_above = block_mean(sun_above, 8, 8, 4).luminance();
    MESSAGE("sun centre: above=" << centre_above << " below=" << centre_below
                                 << "; corner (sun above)=" << corner_above);

    // The disc adds 12x the sun radiance on top of the gradient, so it is an order of magnitude
    // brighter than the sky beside it — the margin is "unmistakably a disc", not a fitted number.
    CHECK(centre_above > corner_above * 5.0f);
    // And it is the LIGHT that puts it there: flipping the light's direction takes the disc away.
    CHECK(centre_above > centre_below * 10.0f);
    // The rest of the sky survives the flip — this is a disc moving, not the sky going dark.
    const float corner_below = block_mean(sun_below, 8, 8, 4).luminance();
    CHECK(corner_below > corner_above * 0.25f);
}

TEST_CASE("sky: the Sky component turns it on, and zenith/horizon reach the shader (m17.0)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the Sky component proof");
        return;
    }

    MeshRegistry meshes(*device);
    MaterialRegistry materials;

    // Deliberately unmistakable, and deliberately NOT each other: a swapped zenith/horizon in the
    // component → SkyParams copy inverts both readings below rather than shifting them slightly.
    Sky authored{};
    authored.zenith_r = 1.0f;
    authored.zenith_g = 0.0f;
    authored.zenith_b = 0.0f;
    authored.horizon_r = 0.0f;
    authored.horizon_g = 0.0f;
    authored.horizon_b = 1.0f;
    authored.clouds = false;

    // No light in the world, so the sun keeps SkyParams' own default direction and cannot swing the
    // reading around; and no set_sky() anywhere in this case, so the component is the only thing
    // that can turn the pass on.
    const auto build = [&](bool with_sky, float pitch) {
        return [&, with_sky, pitch](ecs::World& world) {
            if (with_sky) {
                (void)world.spawn_with(authored);
            }
            spawn_camera(world, {0.0f, 2.0f, 0.0f}, pitch);
        };
    };
    // set_sky is never called on this renderer — that is the point of the case.
    SceneRenderer renderer(*device, meshes, materials);

    // The control: same world, no Sky entity. The renderer's own default is off, so this is the
    // pre-sky frame — and if anything else had quietly enabled the pass, this is what would catch
    // it.
    const HdrImage none = render_hdr(*device, renderer, build(false, kHalfPi));
    const Rgb none_c = block_mean(none, kSize / 2, kSize / 2, 6);
    MESSAGE("no Sky entity: centre lum=" << none_c.luminance());
    CHECK(none_c.luminance() < 1e-4f);

    // Looking straight up: t = 1, so the centre is the ZENITH colour.
    const HdrImage up = render_hdr(*device, renderer, build(true, kHalfPi));
    const Rgb up_c = block_mean(up, kSize / 2, kSize / 2, 6);
    // Looking level, sampled a few rows BELOW the middle. The gradient's pow(up, 0.42) rises very
    // steeply off the horizon — six pixels up is already ~30% of the way to the zenith — so a block
    // straddling the horizon row reads as a mixture and proves much less than it appears to. Below
    // it the shader holds the horizon colour (darkened toward the ground), which is unambiguously
    // the horizon field and unambiguously not the zenith one.
    const HdrImage level = render_hdr(*device, renderer, build(true, 0.0f));
    const Rgb level_c = block_mean(level, kSize / 2, kSize / 2 + 6, 3);
    MESSAGE("zenith (looking up) rgb=(" << up_c.r << ", " << up_c.g << ", " << up_c.b
                                        << ")  horizon (level) rgb=(" << level_c.r << ", "
                                        << level_c.g << ", " << level_c.b << ")");

    // The component alone lit the sky up, and the two colours landed the right way round. The sun
    // glow adds a few hundredths to every channel, which is why these are ratios rather than
    // equalities — 5x is far inside the ~20x the geometry actually gives.
    CHECK(up_c.r > 0.5f);
    CHECK(up_c.r > up_c.b * 5.0f);
    CHECK(level_c.b > 0.5f);
    CHECK(level_c.b > level_c.r * 5.0f);
}

TEST_CASE("sky: clouds draw, and coverage is the knob that turns them off (m17.0)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the cloud-layer proof");
        return;
    }

    MeshRegistry meshes(*device);
    MaterialRegistry materials;

    // Looking straight up, so the whole frame is well past the horizon fade (dir.y ≥ cos(0.78) ≈
    // 0.71, and the fade saturates by 0.28) and the cloud layer is at full strength everywhere.
    //
    // THE SCALE IS RAISED DELIBERATELY, and the reason is worth writing down. The shipped default
    // (0.00035 /m) makes clouds hundreds of metres across, which is right for a sky you look at —
    // but the ray/slab intersection then puts this entire 128 px frame inside about half of ONE
    // noise cell, so "what fraction of the frame is cloud" would be measuring which cell the frame
    // happened to land in rather than the cloud layer. A finer scale puts roughly 28 cells across
    // the frame, which turns the same measurement into a statistic — and carries cloud_scale
    // through the component on the way.
    const auto build = [&](bool clouds, float coverage) {
        return [&, clouds, coverage](ecs::World& world) {
            Sky s{};
            s.clouds = clouds;
            s.cloud_coverage = coverage;
            s.cloud_scale = 0.01f;
            (void)world.spawn_with(s);
            spawn_camera(world, {0.0f, 0.0f, 0.0f}, kHalfPi);
        };
    };
    SceneRenderer renderer(*device, meshes, materials);

    const HdrImage clear = render_hdr(*device, renderer, build(false, 0.0f));
    const HdrImage overcast = render_hdr(*device, renderer, build(true, 0.90f));
    const HdrImage broken = render_hdr(*device, renderer, build(true, 0.55f));
    const HdrImage zero_cov = render_hdr(*device, renderer, build(true, 0.0f));

    const auto differing_fraction = [&](const HdrImage& a, const HdrImage& b, float threshold) {
        std::uint32_t n = 0;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                if (std::fabs(a.luminance(x, y) - b.luminance(x, y)) > threshold) {
                    ++n;
                }
            }
        }
        return static_cast<float>(n) / static_cast<float>(kSize * kSize);
    };

    const float overcast_frac = differing_fraction(overcast, clear, 0.05f);
    const float broken_frac = differing_fraction(broken, clear, 0.05f);
    // The coverage-0 control is EXACT, not approximate: five octaves at halving amplitude sum to at
    // most 0.96875 and coverage 0 thresholds the noise at 1.0, so no fragment can reach the mix.
    // Any difference at all here means coverage is not the parameter it claims to be.
    const float zero_frac = differing_fraction(zero_cov, clear, 0.0f);
    MESSAGE("cloud cover changed vs a clear sky — coverage 0.90: "
            << overcast_frac * 100.0f << "%, 0.55: " << broken_frac * 100.0f
            << "%, 0.00: " << zero_frac * 100.0f << "%");

    CHECK(overcast_frac > 0.60f); // clouds are genuinely drawn, over most of an overcast frame
    CHECK(broken_frac > 0.02f);   // and a broken sky still has some
    // Monotone in the knob, which is the claim a single on/off pair cannot make: more coverage,
    // more sky covered. Measured 97.7% against 48.1%, so the real ratio is ~2.0 and 1.5 is the
    // margin rather than the prediction.
    CHECK(overcast_frac > broken_frac * 1.5f);
    CHECK(zero_frac == 0.0f);
}
