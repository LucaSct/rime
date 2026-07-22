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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/lighting/local_shadows.hpp" // WorldAabb
#include "rime/render/lighting/sdf_clipmap.hpp"
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

// ── Analytically-exact box SDF (the identical construction gi_thesis_test.cpp / ddgi_test.cpp keep
// their own copy of — each GPU test TU carries one; see ddgi_test.cpp's header for why). m10.7c
// uses it to give the reflected "wall" an SDF twin the DDGI probes can trace, WITHOUT a visual mesh
// — so the screen march physically cannot hit it (it is not in the depth/G-buffer) and the
// reflection has to come from the probe fallback or not at all.
// ────────────────────────────────────────────────
float analytic_box_distance(core::Vec3 p, core::Vec3 h) {
    const core::Vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
    const core::Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    const float outside_len =
        std::sqrt(outside.x * outside.x + outside.y * outside.y + outside.z * outside.z);
    const float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
    return outside_len + inside;
}

assets::MeshSdfAsset build_box_sdf(core::Vec3 half_extents, std::uint32_t target_resolution = 24) {
    const float longest = std::max({half_extents.x, half_extents.y, half_extents.z}) * 2.0f;
    const float voxel_size = longest / static_cast<float>(target_resolution);
    const float pad = 2.0f * voxel_size;
    std::uint32_t res[3] = {0, 0, 0};
    float origin[3] = {0.0f, 0.0f, 0.0f};
    const float half[3] = {half_extents.x, half_extents.y, half_extents.z};
    for (int a = 0; a < 3; ++a) {
        const float padded_extent = 2.0f * half[a] + 2.0f * pad;
        res[a] = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil(padded_extent / voxel_size)), 4u);
        origin[a] = -0.5f * static_cast<float>(res[a]) * voxel_size;
    }
    assets::MeshSdfAsset sdf;
    sdf.grid_origin = {origin[0], origin[1], origin[2]};
    sdf.voxel_size = voxel_size;
    sdf.resolution = {res[0], res[1], res[2]};
    sdf.local_bounds =
        assets::Aabb{core::Vec3{-half_extents.x, -half_extents.y, -half_extents.z}, half_extents};
    sdf.distances.resize(sdf.voxel_count());
    float max_abs = 0.0f;
    for (std::uint32_t kz = 0; kz < res[2]; ++kz) {
        for (std::uint32_t jy = 0; jy < res[1]; ++jy) {
            for (std::uint32_t ix = 0; ix < res[0]; ++ix) {
                const core::Vec3 p{sdf.grid_origin.x + (static_cast<float>(ix) + 0.5f) * voxel_size,
                                   sdf.grid_origin.y + (static_cast<float>(jy) + 0.5f) * voxel_size,
                                   sdf.grid_origin.z +
                                       (static_cast<float>(kz) + 0.5f) * voxel_size};
                const float d = analytic_box_distance(p, half_extents);
                sdf.distances[sdf.index(ix, jy, kz)] = d;
                max_abs = std::max(max_abs, std::fabs(d));
            }
        }
    }
    sdf.max_abs_distance = max_abs;
    return sdf;
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

// ── m10.7c: the DDGI probe specular fallback + the roughness cone (ADR-0032 §5)
// ───────────────────
//
// m10.7b's march can only reflect what is on screen; a ray that leaves the frame reflected a flat
// sky constant, and a rough surface reflected nothing. m10.7c fills both blind spots from the DDGI
// probe field: a MISS now samples the field in the reflection direction (reflections everywhere,
// coupled to the GI thesis), and the roughness fade becomes a CONE that blends the sharp screen hit
// toward that (inherently blurry, pre-integrated) field as roughness rises — so a rough surface
// reflects a BLURRED version of the world rather than a black hole.
//
// This proof puts the reflected thing WHERE THE SCREEN CANNOT SEE IT: a sunlit "wall" that exists
// only as an SDF twin the DDGI probes trace, never as a visual mesh. Because it is absent from the
// depth/G-buffer, the screen march physically cannot hit it — so any reflection of it is proof the
// probe fallback engaged. A reflective floor is angled (via the camera) so its reflection ray
// points up-and-forward at that hidden sunlit wall. Two claims, both structural margins against a
// control (never a golden image):
//
//  (1) THE FALLBACK. A SMOOTH floor's reflection pixel — whose ray misses the screen entirely —
//      brightens materially with DDGI on (probe radiance) versus off (the flat sky it used to
//      reflect). The screen had no answer; the probes did.
//  (2) THE CONE. A ROUGH floor, where m10.7b faded SSR to nothing, now also reflects that probe
//      field (cone == 1 ⇒ pure probe) — its reflection pixel brightens with DDGI on just as the
//      smooth one does, where m10.7b would have shown the bare lit floor. Rough surfaces reflect
//      the blurred world instead of a hole.
//
// The DDGI-off control keeps m10.7b's behaviour exactly (the probe fallback reduces to the flat
// ambient constant when DDGI is off), which the smooth SSR proof above already pins as
// byte-compatible — so this file's two cases add the ON path without disturbing the regression
// bridge.
TEST_CASE("ssr m10.7c: the probe fallback reflects a sunlit wall the screen cannot see, on smooth "
          "AND rough floors (ADR-0032 §5)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the SSR probe-fallback proof");
        return;
    }

    constexpr std::uint32_t kSize = 160;

    MeshRegistry meshes(*device);
    const MeshId floor_mesh = meshes.add(make_plane(12.0f), "ssr7c-floor");

    MaterialRegistry materials;
    // The floor: dark, so its own lit colour is low and a reflection stands out. Two roughnesses —
    // a near-mirror (screen march runs, but here always misses ⇒ probe fallback) and a rough one
    // (cone == 1 ⇒ pure probe, no march). Same dark albedo so the two differ only in roughness.
    PbrMaterialDesc smooth_md{};
    smooth_md.base_color[0] = smooth_md.base_color[1] = smooth_md.base_color[2] = 0.035f;
    smooth_md.metallic = 0.0f;
    smooth_md.roughness = 0.04f;
    const MaterialId smooth_floor = materials.add(smooth_md);
    PbrMaterialDesc rough_md = smooth_md;
    rough_md.roughness = 0.92f;
    const MaterialId rough_floor = materials.add(rough_md);

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.02f, 0.02f, 0.02f); // the flat sky a DDGI-off miss reflects

    // ── The scene ────────────────────────────────────────────────────────────────────────────────
    //   * Reflective floor at y = 0.
    //   * A sunlit "wall" up and forward (−Z, the camera's own look direction), 5 m wide × 4 m
    //   tall,
    //     present ONLY as an SDF twin — the DDGI probes trace it, the screen never sees it. Its +Z
    //     face (toward the scene) is lit by a 45° sun, so a probe looking up-and-forward records
    //     bright bounce there.
    //   * Camera low and back, looking down-forward (the engine's camera looks along local −Z): a
    //     floor pixel around z = −2 reflects up-and-forward (≈45°) straight at that hidden wall.
    const core::Vec3 wall_center{0.0f, 2.0f, -4.5f};
    const core::Vec3 wall_half{5.0f, 2.0f, 0.4f};
    const std::uint64_t kWallSdfId = 1;
    renderer.sdf_clipmap().update_instance(
        kWallSdfId, build_box_sdf(wall_half), core::mat4_translation(wall_center));

    // Sun travels (0, −0.707, −0.707): down and −Z, lighting both the floor and the wall's +Z face
    // (the gi_thesis_test.cpp 45° sun — local −z rotated −45° about world X).
    core::Transform sun_tf{};
    sun_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.7853982f);

    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 1.5f, -0.5f};
    // Look down-forward (−Z) at the floor around z ≈ −2.6: pitch = atan2(1.5, 2.1) ≈ 0.6202 rad
    // down.
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.6202f);
    const float fov_y = 1.1f;
    const float aspect = 1.0f;
    const core::Mat4 view_proj =
        core::perspective(fov_y, aspect, 0.1f, 40.0f) * core::inverse(core::to_matrix(cam_tf));
    // The floor pixel whose mirror ray flies up-and-forward into the hidden wall (worked above).
    const test::Pixel refl_px = project(view_proj, {0.0f, 0.0f, -2.0f}, kSize);
    REQUIRE(refl_px.x >= 0.0f);
    REQUIRE(refl_px.x < static_cast<float>(kSize));
    REQUIRE(refl_px.y >= 0.0f);
    REQUIRE(refl_px.y < static_cast<float>(kSize));

    LightingSettings ls;
    ls.ssr_enabled = true;
    ls.ssr_max_distance = 8.0f;
    ls.ssr_thickness = 0.5f;
    ls.ssr_max_steps = 64;
    ls.sdf_clipmap_enabled = true;
    ls.ddgi_enabled = true;
    // A lattice covering the floor strip in front of the camera; count_z long enough to reach
    // toward the wall. Camera y = 1.5 with spacing 0.5 snaps the lowest probe layer clear of the
    // floor (ideal Y origin 0.25, mid-cell — the gi_thesis snap lesson).
    ls.ddgi_probe_count_x = 6;
    ls.ddgi_probe_count_y = 6;
    ls.ddgi_probe_count_z = 10;
    ls.ddgi_probe_spacing = 0.5f;
    ls.ddgi_rays_per_probe = 64;
    ls.ddgi_max_trace_distance = 10.0f;
    ls.ddgi_hysteresis = 0.9f;

    const auto populate = [&](ecs::World& world, MaterialId floor_mat) {
        register_render_components(world);
        (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor_mesh}, MaterialRef{floor_mat});
        (void)world.spawn_with(ecs::WorldTransform{sun_tf},
                               DirectionalLight{1.0f, 1.0f, 1.0f, 4.0f});
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{fov_y, 0.1f, 40.0f, true});
    };

    const auto step = [&](ecs::World& world, bool ddgi_on) {
        ls.ddgi_enabled = ddgi_on;
        renderer.set_lighting(ls);
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.hdr.is_valid());
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
    };
    const auto lum = [&](const HdrImage& img) {
        return img.luminance(static_cast<std::uint32_t>(refl_px.x),
                             static_cast<std::uint32_t>(refl_px.y));
    };

    // Converge the probe field with the smooth floor (the floor's roughness never enters the DDGI
    // trace — it bounces the grey-world albedo — so one convergence serves both subcases).
    ecs::World smooth_world;
    populate(smooth_world, smooth_floor);
    constexpr int kWarmup = 18;
    for (int i = 0; i < kWarmup; ++i) {
        (void)step(smooth_world, /*ddgi_on=*/true);
    }

    // (1) THE FALLBACK — smooth floor, reflection ray misses the screen entirely.
    const float smooth_on = lum(step(smooth_world, /*ddgi_on=*/true));
    const float smooth_off = lum(step(smooth_world, /*ddgi_on=*/false));
    MESSAGE("smooth floor — reflection pixel: ddgi-on=" << smooth_on << " ddgi-off=" << smooth_off);
    // The probe fallback lit the miss: on is materially brighter than the flat-sky control.
    CHECK(smooth_on > smooth_off * 1.5f);
    CHECK(smooth_on - smooth_off > 0.02f);

    // (2) THE CONE — rough floor, where m10.7b faded SSR to nothing.
    ecs::World rough_world;
    populate(rough_world, rough_floor);
    const float rough_on = lum(step(rough_world, /*ddgi_on=*/true));
    const float rough_off = lum(step(rough_world, /*ddgi_on=*/false));
    MESSAGE("rough floor — reflection pixel: ddgi-on=" << rough_on << " ddgi-off=" << rough_off);
    // The rough floor reflects the probe field too (cone == 1 ⇒ pure probe), rather than the black
    // hole m10.7b left: on is materially brighter than the DDGI-off control (which is the bare lit
    // floor + flat ambient, i.e. m10.7b's rough behaviour).
    CHECK(rough_on > rough_off * 1.5f);
    CHECK(rough_on - rough_off > 0.02f);
}
