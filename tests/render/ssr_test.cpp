// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Screen-space reflections (m10.7b, ADR-0032 §5). SSR marches the reflection ray through the depth
// buffer and, on a hit, samples the frame's own colour there — so a smooth floor shows what floats
// above it. This proves the reflection actually lands (a rendered floor pixel brightens with SSR
// on), that the roughness fade turns it off on a matte floor, and that the gate leaves the frame
// untouched with SSR off. No golden images — every claim is a measured margin against a control.
//
// The reflection geometry (worked in the header comment of the first case) is chosen so the bright
// emissive cube's reflection lands on a KNOWN floor pixel the camera sees, so the test can read
// that exact pixel rather than hoping a region average catches it.

#include <doctest/doctest.h>

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
#include "rime/render/lighting/settings.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::decode_hdr;
using rime::render::test::HdrImage;
using rime::render::test::project;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

ecs::Entity spawn_box(ecs::World& world,
                      MeshId mesh,
                      MaterialId mat,
                      core::Vec3 center,
                      core::Vec3 half_extents) {
    core::Transform tf{};
    tf.translation = center;
    tf.scale = half_extents;
    return world.spawn_with(ecs::WorldTransform{tf}, MeshRef{mesh}, MaterialRef{mat});
}

} // namespace

TEST_CASE("ssr: a smooth floor reflects a bright object floating above it (m10.7b, ADR-0032 §5)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the SSR proof");
        return;
    }

    constexpr std::uint32_t kSize = 160;

    MeshRegistry meshes(*device);
    const MeshId floor_mesh = meshes.add(make_plane(12.0f), "ssr-floor");
    const MeshId cube_mesh = meshes.add(make_cube(1.0f), "ssr-cube");

    MaterialRegistry materials;
    // The floor: dark and SMOOTH, so its own lit colour is low and a reflection stands out. The
    // roughness is set per-SUBCASE below (smooth vs. matte) — build both up front.
    PbrMaterialDesc smooth_md{};
    smooth_md.base_color[0] = smooth_md.base_color[1] = smooth_md.base_color[2] = 0.12f;
    smooth_md.metallic = 0.0f;
    smooth_md.roughness = 0.05f;
    const MaterialId smooth_floor = materials.add(smooth_md);
    PbrMaterialDesc matte_md = smooth_md;
    matte_md.roughness = 0.95f;
    const MaterialId matte_floor = materials.add(matte_md);
    // The reflector: a small, very bright EMISSIVE cube — emissive so it reflects the same whether
    // or not a light reaches it, and bright so the reflected sample clears noise by a wide margin.
    PbrMaterialDesc emit_md{};
    emit_md.base_color[0] = emit_md.base_color[1] = emit_md.base_color[2] = 0.0f;
    emit_md.emissive[0] = emit_md.emissive[1] = emit_md.emissive[2] = 8.0f;
    const MaterialId emitter = materials.add(emit_md);

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.01f, 0.01f, 0.01f); // near-black sky, so ambient is not the signal

    // ── The scene / reflection geometry ─────────────────────────────────────────────────────────
    //   * Floor at y = 0.
    //   * A bright emissive cube centred at (0, 1, −2): 1 m above the floor, 2 m in front.
    //   * Camera at (0, 1.5, 1) looking at (0, 0, −1.5): above the floor, angled down-forward.
    // A floor pixel at (0, 0, z0) reflects the view ray about +Y. Solving for the ray from the eye,
    // reflected off the floor, to pass through the cube's centre gives z0 ≈ −0.8 — so THAT floor
    // pixel should carry the cube's reflection, and a floor pixel nearer the camera (z0 = −0.3,
    // still on-screen) reflects up-and-over the cube into empty sky and stays dark. Those are the
    // two the test reads.
    const core::Vec3 cube_center{0.0f, 1.0f, -2.0f};

    // ecs::World is not movable; build it in place. `matte` swaps the floor material for the
    // roughness-fade subcase; a dim sun keeps the floor from being pure black (SSR must add ON TOP
    // of a real frame).
    const auto build = [&](ecs::World& world, bool matte) {
        register_render_components(world);
        (void)world.spawn_with(ecs::WorldTransform{},
                               MeshRef{floor_mesh},
                               MaterialRef{matte ? matte_floor : smooth_floor});
        (void)spawn_box(world, cube_mesh, emitter, cube_center, {0.35f, 0.35f, 0.35f});
        core::Transform sun{};
        sun.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.0f);
        (void)world.spawn_with(ecs::WorldTransform{sun}, DirectionalLight{1.0f, 1.0f, 1.0f, 0.4f});
        core::Transform cam{};
        cam.translation = {0.0f, 1.5f, 1.0f};
        // Look at (0,0,-1.5): pitch down by atan2(1.5, 2.5) ≈ 0.5404 rad.
        cam.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.5404f);
        (void)world.spawn_with(ecs::WorldTransform{cam}, Camera{1.1f, 0.1f, 40.0f, true});
    };

    core::Transform cam{};
    cam.translation = {0.0f, 1.5f, 1.0f};
    cam.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.5404f);
    const float aspect = 1.0f;
    const core::Mat4 view_proj =
        core::perspective(1.1f, aspect, 0.1f, 40.0f) * core::inverse(core::to_matrix(cam));
    const test::Pixel refl_px = project(view_proj, {0.0f, 0.0f, -0.8f}, kSize);
    // A floor pixel NEARER the camera (same x = 0 column, so still on-screen): its mirror ray flies
    // up-and-forward and passes in FRONT of the cube (clearing its near face), so it reflects empty
    // sky, not the cube. The reflection control. (z0 > −0.395 is the front-clearance boundary.)
    const test::Pixel dark_px = project(view_proj, {0.0f, 0.0f, -0.3f}, kSize);
    for (const test::Pixel& p : {refl_px, dark_px}) {
        REQUIRE(p.x >= 0.0f);
        REQUIRE(p.x < static_cast<float>(kSize));
        REQUIRE(p.y >= 0.0f);
        REQUIRE(p.y < static_cast<float>(kSize));
    }

    LightingSettings ls;
    // SSR alone routes the frame onto the shadowed shader (which writes the G-buffer); no shadows
    // are needed, so the valid empty 2-layer shadow binding is used and there is no CSM to set up.
    ls.ssr_enabled = true;
    ls.ssr_max_distance = 8.0f;
    ls.ssr_thickness = 0.5f;
    ls.ssr_max_steps = 64;

    const auto render_floor = [&](bool matte, bool ssr_on) {
        ls.ssr_enabled = ssr_on;
        renderer.set_lighting(ls);
        ecs::World world;
        build(world, matte);
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.hdr.is_valid());
        // Read back the resolved HDR (Output::hdr — the reflection-added target when SSR is on, the
        // raw forward HDR when off), not the tonemapped LDR: the DDGI thesis proof reads HDR for
        // the same reason. Asserting on linear radiance is also cleaner than through the ACES
        // curve.
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
    };
    const auto lum = [](const HdrImage& img, const test::Pixel& px) {
        return img.luminance(static_cast<std::uint32_t>(px.x), static_cast<std::uint32_t>(px.y));
    };

    // Smooth floor, SSR off vs on.
    const HdrImage smooth_off = render_floor(/*matte=*/false, /*ssr_on=*/false);
    const HdrImage smooth_on = render_floor(/*matte=*/false, /*ssr_on=*/true);
    const float refl_off = lum(smooth_off, refl_px);
    const float refl_on = lum(smooth_on, refl_px);
    const float dark_off = lum(smooth_off, dark_px);
    const float dark_on = lum(smooth_on, dark_px);
    MESSAGE("smooth floor — reflection pixel: off=" << refl_off << " on=" << refl_on
                                                    << "; sky-facing pixel: off=" << dark_off
                                                    << " on=" << dark_on);

    // (1) The reflection lands: the floor pixel that mirrors the cube brightens materially with SSR
    // on. The cube is bright (emissive 8) and the floor dark (albedo 0.12), so even a
    // Fresnel-scaled reflection clears a wide margin.
    CHECK(refl_on > refl_off * 1.5f);
    CHECK(refl_on - refl_off > 0.05f);
    // (2) It is a REFLECTION, not a global lift: a floor pixel whose mirror ray flies off into
    // empty sky does NOT brighten (it stays within a hair of its SSR-off self).
    CHECK(std::fabs(dark_on - dark_off) < 0.02f);

    // (3) The roughness fade: on a MATTE floor the same scene shows no such reflection — the sharp
    // screen-space sample is wrong for a rough surface, so SSR fades itself out.
    const HdrImage matte_off = render_floor(/*matte=*/true, /*ssr_on=*/false);
    const HdrImage matte_on = render_floor(/*matte=*/true, /*ssr_on=*/true);
    const float matte_refl_off = lum(matte_off, refl_px);
    const float matte_refl_on = lum(matte_on, refl_px);
    MESSAGE("matte floor — reflection pixel: off=" << matte_refl_off << " on=" << matte_refl_on);
    CHECK(std::fabs(matte_refl_on - matte_refl_off) < 0.02f);
}
