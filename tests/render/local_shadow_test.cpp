// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Structural proofs for local-light (spot) shadow maps and their DESTRUCTIBILITY-AWARE CACHE
// (m10.2, ADR-0032 §3 + C1/C2). No golden images:
//
//  (1) extract_scene (GPU-free): a SpotLight entity extracts to the CPU spot record the shadow fit
//      consumes — world position, the −z cone axis, pre-cosined cone angles (inner ≥ outer), and
//      radiance = colour × intensity.
//
//  (2) The money test (on lavapipe): a spot lights a floor with a wall between it and one patch.
//  That
//      patch is in shadow (a). Then the wall is DESTROYED — removed from the world:
//        * with NO invalidation the cache serves last frame's depth, so the patch stays (wrongly)
//          dark and stats report a pure cache reuse (b) — proof the cache actually caches, and that
//          a light-parameter check alone cannot see geometry change;
//        * calling invalidate() over the wall's region (the C2 destruction hook) forces a re-render
//          and the patch lights back up (c) — *break a wall, the shadow it cast lifts*, the thesis
//          of this milestone.
//
// Shares pbr_pipeline/shadow_test's harness via render_test_support.hpp.

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
#include "rime/render/lighting/local_shadows.hpp"
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

TEST_CASE("local shadows: extract_scene shapes spot lights (m10.2)") {
    ecs::World world;
    register_render_components(world);

    core::Transform tf{};
    tf.translation = {2.0f, 5.0f, -1.0f}; // identity rotation ⇒ the cone points down local −z
    SpotLight sl{};
    sl.color_r = 0.5f;
    sl.color_g = 1.0f;
    sl.color_b = 0.25f;
    sl.intensity = 4.0f;
    sl.range = 15.0f;
    sl.inner_angle = 0.3f;
    sl.outer_angle = 0.5f;
    (void)world.spawn_with(ecs::WorldTransform{tf}, sl);

    const ExtractedScene scene = extract_scene(world);
    REQUIRE(scene.spot_lights.size() == 1);
    const SpotLightData& s = scene.spot_lights[0];

    CHECK(core::length(s.position - core::Vec3{2.0f, 5.0f, -1.0f}) < 1e-5f);
    CHECK(core::length(s.direction - core::Vec3{0.0f, 0.0f, -1.0f}) < 1e-5f); // −z convention
    CHECK(s.range == doctest::Approx(15.0f));
    // Pre-cosined: inner is the tighter (larger cosine) cone; both are cos of the authored angles.
    CHECK(s.cos_inner == doctest::Approx(std::cos(0.3f)));
    CHECK(s.cos_outer == doctest::Approx(std::cos(0.5f)));
    CHECK(s.cos_inner > s.cos_outer);
    // Radiance folds colour × intensity so the shader never multiplies.
    CHECK(s.radiance[0] == doctest::Approx(0.5f * 4.0f));
    CHECK(s.radiance[1] == doctest::Approx(1.0f * 4.0f));
    CHECK(s.radiance[2] == doctest::Approx(0.25f * 4.0f));
}

TEST_CASE("local shadows: a spot casts a shadow, and the cache is destruction-aware (m10.2)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the local-shadow proof");
        return;
    }
    if (test::shadow_depth_sampling_unsupported(*device)) {
        // MoltenVK/Metal: sampling the layered shadow-depth map reads 0 → fully occluded. Skip
        // (even under RIME_REQUIRE_VULKAN); the maths is proven on lavapipe/native CI. See the
        // helper.
        MESSAGE("portability (MoltenVK) device — layered shadow-depth sampling unsupported; "
                "skipping the local-shadow proof");
        return;
    }

    constexpr std::uint32_t kSize = 256;

    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(8.0f), "spot-floor");
    const MeshId wall = meshes.add(make_cube(0.5f), "spot-occluder");

    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.8f;
    md.metallic = 0.0f;
    md.roughness = 1.0f;
    const MaterialId mat = materials.add(md);

    // ONE renderer across every frame below — its LocalShadowMap holds the cache we are testing.
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.02f, 0.02f, 0.02f);

    // A spot 6 units up shining straight down; the camera looks at the floor from above-and-front.
    const core::Vec3 spot_pos{0.0f, 6.0f, 0.0f};
    const core::Quat spot_rot =
        core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f); // local −z → world −y (down)
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 8.0f, 8.0f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.7853982f); // −45° pitch

    const core::Mat4 view_proj =
        core::perspective(0.9f, 1.0f, 0.1f, 40.0f) * core::inverse(core::to_matrix(cam_tf));
    // A wall at (1.5,3,0) sits on the spot→floor ray to (3,0,0): its shadow lands there. A control
    // point on the far side of the cone is lit and never occluded.
    const test::Pixel shadow_px = project(view_proj, {3.0f, 0.0f, 0.0f}, kSize);
    const test::Pixel lit_px = project(view_proj, {-2.5f, 0.0f, 0.0f}, kSize);
    REQUIRE(shadow_px.x >= 0.0f);
    REQUIRE(shadow_px.x < static_cast<float>(kSize));
    REQUIRE(lit_px.x >= 0.0f);
    REQUIRE(lit_px.x < static_cast<float>(kSize));

    // Render the floor (± the wall) with a shadowing spot; hand back the decoded HDR radiance.
    const auto render_scene = [&](bool with_wall) {
        ecs::World world;
        register_render_components(world);
        (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor}, MaterialRef{mat});
        if (with_wall) {
            core::Transform wall_tf{};
            wall_tf.translation = {1.5f, 3.0f, 0.0f};
            (void)world.spawn_with(ecs::WorldTransform{wall_tf}, MeshRef{wall}, MaterialRef{mat});
        }
        core::Transform spot_tf{};
        spot_tf.translation = spot_pos;
        spot_tf.rotation = spot_rot;
        SpotLight sl{};
        sl.intensity = 300.0f; // inverse-square falloff over ~6 units ⇒ a bright, clearly-lit floor
        sl.range = 20.0f;
        (void)world.spawn_with(ecs::WorldTransform{spot_tf}, sl);
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{0.9f, 0.1f, 40.0f, true});

        LightingSettings ls;
        ls.shadows_enabled = true;
        ls.local_shadows_enabled = true;
        ls.local_shadow_resolution = 1024;
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

    // Frame 1 — wall present: the spot's shadow darkens the floor under it. First sight, so the one
    // spot slot is rendered (the cache is cold).
    const HdrImage f1 = render_scene(/*with_wall=*/true);
    const LocalShadowStats s1 = renderer.local_shadow_stats();
    const float lit = lum(f1, lit_px);
    const float shadowed = lum(f1, shadow_px);
    REQUIRE(lit > 0.05f); // the spot really lights the control point
    CHECK(s1.rendered == 1);
    CHECK(s1.reused == 0);
    CHECK(shadowed < 0.4f * lit); // (a) the spot casts a shadow

    // Frame 2 — the wall is DESTROYED (removed) but NOTHING invalidates the cache. The spot itself
    // is unchanged, so the cache reuses last frame's depth: the shadow persists even though the
    // caster is gone. This is the proof the cache caches — a light-parameter check alone can't see
    // geometry move.
    const HdrImage f2 = render_scene(/*with_wall=*/false);
    const LocalShadowStats s2 = renderer.local_shadow_stats();
    CHECK(s2.rendered == 0);                // (b) pure cache hit …
    CHECK(s2.reused == 1);                  //     … one slot served from cache
    CHECK(lum(f2, shadow_px) < 0.5f * lit); // … and the stale shadow is still there

    // Frame 3 — the C2 destruction hook fires: invalidate the wall's world region. Now the slot is
    // re-rendered (without the wall) and the floor lights back up. Break a wall, the shadow lifts.
    renderer.invalidate_shadow_region(WorldAabb{{1.0f, 2.4f, -0.6f}, {2.0f, 3.6f, 0.6f}});
    const HdrImage f3 = render_scene(/*with_wall=*/false);
    const LocalShadowStats s3 = renderer.local_shadow_stats();
    CHECK(s3.rendered == 1);                // (c) invalidation forced a re-render …
    CHECK(lum(f3, shadow_px) > 0.7f * lit); //     … and the destruction-aware money test passes
}
