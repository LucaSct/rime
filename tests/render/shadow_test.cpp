// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Structural proofs for directional cascaded shadow maps (m10.1, ADR-0032 §3). No golden images:
//
//  (1) compute_cascades (GPU-free): the fit produces the requested cascade count, orders them
//      near→far (the near cascade is tighter than the far one), and the texel snap holds the shadow
//      grid still under a sub-texel camera nudge (the anti-shimmer property).
//
//  (2) The sun casts a shadow (on lavapipe): a floor lit by an overhead sun with a box between
//  them.
//      The floor pixel UNDER the box is far darker than one beside it (a) — remove the box and that
//      pixel lights back up, so the darkening is a cast shadow, not acne (c) — and with shadows
//      DISABLED the box no longer darkens the floor at all, i.e. the renderer is back on the M5.6
//      baseline (b, the ADR-0032 §11 regression bridge).
//
// Mirrors pbr_pipeline_test's harness via the shared render_test_support.hpp.

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
#include "rime/render/lighting/shadows.hpp"
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

// True if a world point projects inside a cascade's shadow map (its clip-space uv ∈ [0,1], z ∈
// [0,1]).
bool inside_cascade(const core::Mat4& view_proj, core::Vec3 w) {
    const core::Vec4 c = view_proj * core::Vec4{w.x, w.y, w.z, 1.0f};
    const core::Vec3 p{c.x / c.w, c.y / c.w, c.z / c.w};
    const float u = p.x * 0.5f + 0.5f;
    const float v = p.y * 0.5f + 0.5f;
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f && p.z >= 0.0f && p.z <= 1.0f;
}

} // namespace

TEST_CASE("shadows: compute_cascades orders the splits and the texel snap is stable (m10.1)") {
    LightingSettings s;
    s.cascade_count = 3;
    s.shadow_map_resolution = 1024;
    s.cascade_split_lambda = 0.5f;

    CascadeInputs in;
    in.camera_view = core::look_at({0.0f, 3.0f, 12.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    in.fov_y = 0.9f;
    in.aspect = 1.0f;
    in.z_near = 0.1f;
    in.z_far = 60.0f;
    in.light_dir = core::normalize(core::Vec3{0.4f, -1.0f, 0.3f});

    const CascadeFit fit = compute_cascades(in, s);
    CHECK(fit.count == 3);

    // Near→far tightness: a point a few units in front of the camera is covered by cascade 0, while
    // a point out near the far plane is NOT — that is what the far cascades are for.
    CHECK(inside_cascade(fit.view_proj[0], core::Vec3{0.0f, 0.0f, 7.0f}));
    CHECK_FALSE(inside_cascade(fit.view_proj[0], core::Vec3{0.0f, 0.0f, -40.0f}));
    // The far cascade covers what the near one drops.
    CHECK(inside_cascade(fit.view_proj[2], core::Vec3{0.0f, 0.0f, -40.0f}));

    // Texel snap: nudge the camera a hair (well under one shadow texel) and a FIXED world point's
    // cascade-0 projection must not drift — the snap holds the texel grid still (no shimmer). An
    // unsnapped fit would slide the projection continuously with the camera.
    CascadeInputs nudged = in;
    nudged.camera_view =
        core::look_at({0.0002f, 3.0f, 12.0f}, {0.0002f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const CascadeFit fit2 = compute_cascades(nudged, s);
    CHECK(core::approx_eq(fit.view_proj[0], fit2.view_proj[0], 1e-3f));
}

TEST_CASE("shadows: the sun casts a shadow, gated by LightingSettings (m10.1)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the shadow proof");
        return;
    }
    if (test::shadow_depth_sampling_unsupported(*device)) {
        // MoltenVK/Metal: sampling the layered shadow-depth map reads 0 → fully occluded. Skip
        // (even under RIME_REQUIRE_VULKAN); the maths is proven on lavapipe/native CI. See the
        // helper.
        MESSAGE("portability (MoltenVK) device — layered shadow-depth sampling unsupported; "
                "skipping the directional shadow proof");
        return;
    }

    constexpr std::uint32_t kSize = 256;

    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(8.0f), "shadow-floor");
    const MeshId box = meshes.add(make_cube(0.5f), "shadow-occluder");

    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.8f;
    md.metallic = 0.0f;
    md.roughness = 1.0f;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.02f, 0.02f, 0.02f);

    // The camera looks down at the floor from above-and-in-front; the sun points straight down so a
    // box floating over the floor drops its shadow directly below it (docs/math/shadow-mapping.md).
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 8.0f, 8.0f};
    cam_tf.rotation =
        core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.7853982f); // pitch −45° → origin
    core::Transform light_tf{};
    light_tf.rotation =
        core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f); // −z → −y (down)

    const float aspect = 1.0f;
    const core::Mat4 view_proj =
        core::perspective(0.9f, aspect, 0.1f, 40.0f) * core::inverse(core::to_matrix(cam_tf));
    // The shadow lands at the origin (under the box); a lit control sits well clear of it.
    const test::Pixel shadow_px = project(view_proj, {0.0f, 0.0f, 0.0f}, kSize);
    const test::Pixel lit_px = project(view_proj, {3.5f, 0.0f, 0.0f}, kSize);
    REQUIRE(shadow_px.x >= 0.0f);
    REQUIRE(shadow_px.x < static_cast<float>(kSize));
    REQUIRE(lit_px.x >= 0.0f);
    REQUIRE(lit_px.x < static_cast<float>(kSize));

    // Render the floor (± the box) with shadows on/off and hand back the decoded HDR radiance.
    const auto render_config = [&](bool shadows_on, bool with_box) {
        ecs::World world;
        register_render_components(world);
        // WorldTransform set directly (as pbr_pipeline_test does), so no LocalTransform→World
        // propagation is needed — extract_scene reads WorldTransform straight.
        (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor}, MaterialRef{mat});
        if (with_box) {
            core::Transform box_tf{};
            box_tf.translation = {0.0f, 2.0f, 0.0f};
            (void)world.spawn_with(ecs::WorldTransform{box_tf}, MeshRef{box}, MaterialRef{mat});
        }
        (void)world.spawn_with(ecs::WorldTransform{light_tf},
                               DirectionalLight{1.0f, 1.0f, 1.0f, 3.0f});
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{0.9f, 0.1f, 40.0f, true});

        LightingSettings ls;
        ls.shadows_enabled = shadows_on;
        ls.cascade_count = 3;
        ls.shadow_map_resolution = 1024;
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

    const auto lum = [](const HdrImage& img, const test::Pixel& px) {
        return img.luminance(static_cast<std::uint32_t>(px.x), static_cast<std::uint32_t>(px.y));
    };

    const HdrImage occ_on = render_config(/*shadows_on=*/true, /*with_box=*/true);
    const HdrImage no_occ = render_config(/*shadows_on=*/true, /*with_box=*/false);
    const HdrImage occ_off = render_config(/*shadows_on=*/false, /*with_box=*/true);

    const float lit = lum(occ_on, lit_px);         // floor beside the box: full sun
    const float shadowed = lum(occ_on, shadow_px); // floor under the box: in shadow
    const float relit = lum(no_occ, shadow_px);    // same spot, box removed
    const float off = lum(occ_off, shadow_px);     // same spot, shadows disabled

    REQUIRE(lit > 0.05f); // the control really is lit (sanity, not the proof)

    // (a) the sun's shadow darkens the floor under the box — well under half the lit level.
    CHECK(shadowed < 0.4f * lit);
    // (c) remove the occluder and that exact spot lights back up: the darkening was a cast shadow,
    //     not self-shadowing acne on a lit surface.
    CHECK(relit > 0.7f * lit);
    // (b) the regression bridge: shadows OFF ⇒ the box casts nothing, the spot is lit like the
    //     baseline. (The off path is literally the unmodified M5.6 forward pipeline.)
    CHECK(off > 0.7f * lit);
}
