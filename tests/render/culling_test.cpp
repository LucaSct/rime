// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/culling.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"

// m13.2a — view-frustum culling, and the counters ADR-0035 §2a asks for.
//
// Two kinds of case here, and they answer different questions.
//
// The FRUSTUM MATH is pure and needs no device: given a camera, is this box in or out. Those cases
// are closed-form — a box behind the camera, a box off to one side, a box straddling the near plane
// — and they are cheap enough to be exhaustive about the sign conventions that are easy to get
// wrong (Vulkan's z ∈ [0, w] near plane in particular).
//
// The INTEGRATION needs pixels, because culling's whole contract is that it is INVISIBLE: it must
// remove only draws that could not have contributed. So the headline case renders the same scene
// with culling on and off and demands the two images be **byte-identical** — which is a stronger
// and more useful claim than "off is the baseline", the gate discipline used by every M10 lighting
// technique. Culling is not a technique that changes the picture; it is an optimization that must
// not.
//
// And one case exists purely because it caught a real bug: culling must not remove SHADOW CASTERS.
using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

namespace {

constexpr std::uint32_t kSize = 128;

// A camera at the origin looking down -Z: 60° vertical fov, square aspect, near 0.1, far 100.
[[nodiscard]] Frustum standard_frustum() {
    const core::Mat4 view =
        core::look_at({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    const core::Mat4 proj = core::perspective(std::numbers::pi_v<float> / 3.0f, 1.0f, 0.1f, 100.0f);
    return frustum_from_view_proj(proj * view);
}

} // namespace

TEST_CASE("m13.2a: the frustum admits what is in view and rejects what is not") {
    const Frustum f = standard_frustum();

    // Straight ahead, well inside.
    CHECK(aabb_in_frustum(f, {-0.5f, -0.5f, -5.5f}, {0.5f, 0.5f, -4.5f}));

    // BEHIND THE CAMERA. The single most important rejection: at a city block's scale roughly half
    // the world is behind you at any moment, and it is also the case a sign error in the near plane
    // gets wrong while leaving everything else looking fine.
    CHECK_FALSE(aabb_in_frustum(f, {-0.5f, -0.5f, 4.5f}, {0.5f, 0.5f, 5.5f}));

    // Far off to one side, at a depth that is otherwise in range.
    CHECK_FALSE(aabb_in_frustum(f, {-50.0f, -0.5f, -5.5f}, {-49.0f, 0.5f, -4.5f}));

    // Beyond the far plane.
    CHECK_FALSE(aabb_in_frustum(f, {-0.5f, -0.5f, -200.0f}, {0.5f, 0.5f, -190.0f}));

    // STRADDLING THE NEAR PLANE — half behind the camera, half in front. It must be ADMITTED: the
    // part in front is visible, and this is exactly where the OpenGL-vs-Vulkan near-plane
    // convention (z ≥ −w vs z ≥ 0) shows up. Getting it wrong culls correctly everywhere except
    // directly in front of the camera, which is the one place a player is guaranteed to be looking.
    CHECK(aabb_in_frustum(f, {-0.5f, -0.5f, -0.2f}, {0.5f, 0.5f, 0.2f}));

    // A wall wider than the frustum, crossing it: admitted, because it fills the view.
    CHECK(aabb_in_frustum(f, {-10.0f, -10.0f, -3.1f}, {10.0f, 10.0f, -2.9f}));
}

TEST_CASE("m13.2a: a transformed box is bounded conservatively") {
    // The cull tests a world-space AABB, which for a rotated object is bigger than the object. That
    // over-estimates — so it can only ever draw something unnecessary, never cull something
    // visible. Erring in that direction is the only acceptable one: a false cull is a hole in the
    // picture, a false keep is a wasted draw.
    core::Vec3 mn;
    core::Vec3 mx;

    transform_aabb(core::mat4_translation({1.0f, 2.0f, 3.0f}), {-1, -1, -1}, {1, 1, 1}, mn, mx);
    CHECK(mn.x == doctest::Approx(0.0f));
    CHECK(mn.y == doctest::Approx(1.0f));
    CHECK(mn.z == doctest::Approx(2.0f));
    CHECK(mx.x == doctest::Approx(2.0f));
    CHECK(mx.y == doctest::Approx(3.0f));
    CHECK(mx.z == doctest::Approx(4.0f));

    // A 45° rotation about Z grows a unit square's axis-aligned bounds by √2 — the conservatism,
    // measured rather than asserted vaguely.
    const core::Mat4 spin = core::to_matrix(core::Transform{
        {0.0f, 0.0f, 0.0f},
        core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, std::numbers::pi_v<float> / 4.0f),
        {1.0f, 1.0f, 1.0f}});
    transform_aabb(spin, {-1, -1, -1}, {1, 1, 1}, mn, mx);
    CHECK(mx.x == doctest::Approx(std::sqrt(2.0f)).epsilon(0.001));
    CHECK(mx.y == doctest::Approx(std::sqrt(2.0f)).epsilon(0.001));
    CHECK(mx.z == doctest::Approx(1.0f)); // untouched by a Z rotation
}

TEST_CASE("m13.2a: culling is invisible — on and off render byte-identical pixels") {
    // THE HEADLINE. Culling's contract is that it removes only draws that could not have
    // contributed a pixel, so the honest proof is not "off is the old baseline" but "ON IS OFF".
    // Compared as raw bytes: a tolerance would pass a cull that clipped something at the edge of
    // the view and merely changed the image a little, which is the exact failure to rule out.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping culling render proofs");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId cube = meshes.add(make_cube(0.5f), "cull-cube");
    REQUIRE(cube != kInvalidMeshId);
    MaterialRegistry materials;
    const MaterialId mat = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.5f});

    ecs::World world;
    register_render_components(world);

    // A grid of cubes IN VIEW, plus a batch far behind the camera that cannot possibly matter.
    int in_view = 0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            core::Transform tf{};
            tf.translation = {static_cast<float>(x) * 1.2f, static_cast<float>(y) * 1.2f, -6.0f};
            (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube}, MaterialRef{mat});
            ++in_view;
        }
    }
    constexpr int kBehind = 40;
    for (int i = 0; i < kBehind; ++i) {
        core::Transform tf{};
        tf.translation = {static_cast<float>(i) * 0.7f - 14.0f, 0.0f, 25.0f}; // behind the camera
        (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube}, MaterialRef{mat});
    }

    core::Transform cam_tf{}; // identity looks down −z
    (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{});
    core::Transform light_tf{};
    light_tf.translation = {4.0f, 4.0f, 4.0f};
    (void)world.spawn_with(ecs::WorldTransform{light_tf},
                           PointLight{1.0f, 1.0f, 1.0f, 80.0f, 60.0f});

    SceneRenderer renderer(*device, meshes, materials);

    const auto frame = [&](bool cull) {
        renderer.set_culling_enabled(cull);
        RenderGraph graph(*device);
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    };

    renderer.reset_cull_stats();
    const std::vector<std::uint8_t> culled = frame(true);
    const CullStats stats = renderer.cull_stats();
    const std::vector<std::uint8_t> uncalled = frame(false);

    MESSAGE("m13.2a: " << stats.submitted << " submitted, " << stats.culled << " culled of "
                       << stats.considered());

    // Identical pixels…
    CHECK(culled == uncalled);

    // …and the cull actually did something. THIS is the assertion ADR-0035 §2a asks for: without
    // it, a cull that had silently stopped culling would pass the identity check above perfectly.
    CHECK(stats.culled >= static_cast<std::uint64_t>(kBehind));
    CHECK(stats.submitted >= static_cast<std::uint64_t>(in_view));
    CHECK(stats.considered() == static_cast<std::uint64_t>(in_view + kBehind));
}

TEST_CASE("m13.2a: a shadow caster outside the view still casts into it") {
    // THE CASE THAT EXISTS BECAUSE IT CAUGHT A BUG.
    //
    // One extracted draw list feeds the camera passes AND the shadow passes, which render the same
    // geometry from the LIGHT's point of view. The first version of the cull DELETED the culled
    // entries, so an occluder outside the camera frustum stopped being drawn into the shadow map
    // and stopped shadowing anything. `gi_thesis_test`'s covered room went from 0.04 to 0.74 on its
    // floor pixel; every other render proof stayed green.
    //
    // The fix is that the cull PARTITIONS rather than deletes: the camera passes take the visible
    // prefix, the shadow passes take the whole list. This case pins that directly rather than
    // relying on the GI test to notice again.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping culling render proofs");
        return;
    }
    if (shadow_depth_sampling_unsupported(*device)) {
        MESSAGE("device cannot sample depth — skipping the shadow-caster case");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId floor = meshes.add(make_plane(6.0f), "cull-floor");
    const MeshId blocker = meshes.add(make_cube(1.0f), "cull-blocker");
    MaterialRegistry materials;
    const MaterialId mat = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.9f});

    // A sun straight down, a floor the camera looks at, and a blocker high ABOVE the camera's view
    // — out of frustum, but directly between the sun and the patch of floor being measured.
    const auto build = [&](ecs::World& world, bool with_blocker) {
        register_render_components(world);
        core::Transform floor_tf{};
        (void)world.spawn_with(ecs::WorldTransform{floor_tf}, MeshRef{floor}, MaterialRef{mat});

        if (with_blocker) {
            core::Transform blocker_tf{};
            blocker_tf.translation = {0.0f, 9.0f, 0.0f}; // far above; the camera never sees it
            (void)world.spawn_with(
                ecs::WorldTransform{blocker_tf}, MeshRef{blocker}, MaterialRef{mat});
        }

        core::Transform cam_tf{};
        cam_tf.translation = {0.0f, 1.5f, 6.0f};
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{});

        // A sun pointing straight down: an entity's −z is its light direction, so pitch it down.
        core::Transform sun_tf{};
        sun_tf.rotation =
            core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -std::numbers::pi_v<float> / 2.0f);
        (void)world.spawn_with(ecs::WorldTransform{sun_tf},
                               DirectionalLight{1.0f, 1.0f, 1.0f, 3.0f});
    };

    SceneRenderer renderer(*device, meshes, materials);
    LightingSettings ls;
    ls.shadows_enabled = true;
    ls.cascade_count = 1;
    renderer.set_lighting(ls);
    renderer.set_culling_enabled(true);

    const auto floor_luminance = [&](ecs::World& world) {
        RenderGraph graph(*device);
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.hdr.is_valid());
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const HdrImage img = decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
        // The floor directly under the blocker, at the centre of the view.
        return img.luminance(kSize / 2, kSize * 3 / 4);
    };

    ecs::World lit;
    build(lit, /*with_blocker=*/false);
    ecs::World shadowed;
    build(shadowed, /*with_blocker=*/true);

    const float open = floor_luminance(lit);
    const float under = floor_luminance(shadowed);
    MESSAGE("m13.2a shadow caster out of view: floor lit " << open << ", shadowed " << under);

    // Non-vacuity first: the floor really is lit without the blocker.
    CHECK(open > 0.05f);
    // …and the OUT-OF-VIEW blocker really does darken it. If the cull deleted rather than
    // partitioned, these two would be equal.
    CHECK(under < open * 0.75f);

    // And the blocker really was culled from the CAMERA's view — otherwise this case would be
    // proving nothing about culling at all, just that shadows work.
    CHECK(renderer.cull_stats().culled > 0);
}
