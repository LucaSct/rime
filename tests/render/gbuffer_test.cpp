// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The thin SSR G-buffer (m10.7a, ADR-0032). SSR (m10.7b) marches the depth buffer and, at each
// pixel, needs the surface's world normal (to reflect the view ray) and its roughness (to decide
// mirror-sharp vs. blurred/fallback). m10.7a is the producer: with LightingSettings::ssr_enabled
// the shadowed forward pass writes a SECOND colour attachment — octahedral world normal in RG,
// perceptual roughness in B, a geometry mask in A. This file proves that attachment carries the
// right data (a rendered pixel's decoded normal matches its surface, its roughness matches its
// material) and that the gate holds (off ⇒ no G-buffer at all). No golden images — every claim is a
// measured margin.
//
// The encode is the module's own ddgi_oct_encode (one encode, reused for probe directions and
// here); oct_decode below is its exact inverse, the same fold the atlas sampling in ddgi.md §10
// documents.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

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

// The exact inverse of ddgi_oct_encode (pbr_forward_shadowed.frag): given the octahedral pair in
// [-1,1], fold back to a unit direction. Mirrors the shader's own decode branch bit-for-bit.
core::Vec3 oct_decode(float ex, float ey) {
    core::Vec3 v{ex, ey, 1.0f - std::fabs(ex) - std::fabs(ey)};
    if (v.z < 0.0f) {
        const float x = (1.0f - std::fabs(v.y)) * (v.x >= 0.0f ? 1.0f : -1.0f);
        const float y = (1.0f - std::fabs(v.x)) * (v.y >= 0.0f ? 1.0f : -1.0f);
        v.x = x;
        v.y = y;
    }
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x / len, v.y / len, v.z / len};
}

// Rotate the plane's own +Y normal by a WorldTransform's rotation, the ground truth a decoded
// G-buffer normal is checked against (the same core::to_matrix the renderer feeds the vertex
// shader).
core::Vec3 rotated_up(const core::Quat& q) {
    const core::Mat4 m = core::to_matrix(core::Transform{{}, q, {1.0f, 1.0f, 1.0f}});
    const core::Vec4 n = m * core::Vec4{0.0f, 1.0f, 0.0f, 0.0f};
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return {n.x / len, n.y / len, n.z / len};
}

} // namespace

TEST_CASE(
    "gbuffer: the shadowed pass writes correct world normals + per-surface roughness (m10.7a)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the G-buffer proof");
        return;
    }

    constexpr std::uint32_t kSize = 128;

    MeshRegistry meshes(*device);
    const MeshId plane_mesh = meshes.add(make_plane(2.0f), "gbuffer-plane");

    // Two materials with clearly different roughness — the G-buffer's B channel must read each
    // surface's own value, not one global constant.
    MaterialRegistry materials;
    PbrMaterialDesc rough_md{};
    rough_md.metallic = 0.0f;
    rough_md.roughness = 0.85f;
    const MaterialId rough_mat = materials.add(rough_md);
    PbrMaterialDesc smooth_md{};
    smooth_md.metallic = 0.0f;
    smooth_md.roughness = 0.30f;
    const MaterialId smooth_mat = materials.add(smooth_md);

    SceneRenderer renderer(*device, meshes, materials);

    // Two quads side by side under a top-down camera:
    //   * left (x < 0): a flat floor, normal +Y, the ROUGH material.
    //   * right (x > 0): the SAME plane tilted 25° about world Z, the SMOOTH material — a non-axis
    //     normal that only a correct model→world normal transform AND a correct octahedral
    //     round-trip will reproduce.
    const core::Quat flat = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 0.0f);
    const core::Quat tilt = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 0.4363323f); // 25°

    ecs::World world;
    register_render_components(world);
    {
        core::Transform tf{};
        tf.translation = {-1.6f, 0.0f, 0.0f};
        tf.rotation = flat;
        (void)world.spawn_with(
            ecs::WorldTransform{tf}, MeshRef{plane_mesh}, MaterialRef{rough_mat});
    }
    {
        core::Transform tf{};
        tf.translation = {1.6f, 0.0f, 0.0f};
        tf.rotation = tilt;
        (void)world.spawn_with(
            ecs::WorldTransform{tf}, MeshRef{plane_mesh}, MaterialRef{smooth_mat});
    }

    // Camera high above, looking straight down — both quads fill the lower/upper halves of the
    // view.
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 6.0f, 0.0f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f); // straight down
    (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{1.2f, 0.1f, 30.0f, true});

    const core::Mat4 view_proj =
        core::perspective(1.2f, 1.0f, 0.1f, 30.0f) * core::inverse(core::to_matrix(cam_tf));
    const test::Pixel rough_px = project(view_proj, {-1.6f, 0.0f, 0.0f}, kSize);
    const test::Pixel smooth_px = project(view_proj, {1.6f, 0.0f, 0.0f}, kSize);

    LightingSettings ls;
    ls.ssr_enabled =
        true; // SSR alone pulls the frame onto the shadowed shader and allocates the G-buffer
    renderer.set_lighting(ls);

    RenderGraph graph(*device);
    graph.reset();
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
    REQUIRE(out.hdr.is_valid());
    REQUIRE(out.gbuffer.is_valid()); // the gate is ON, so it exists
    graph.export_texture(out.gbuffer);
    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);
    const HdrImage g = decode_hdr(
        read_texture(*device, graph.physical(out.gbuffer), kSize, kSize, 8), kSize, kSize);

    // decode_hdr keeps RGB: R,G = the octahedral normal, B = roughness. (A, the geometry mask, is
    // dropped — but roughness is clamped ≥ 0.045, so a nonzero B is itself "geometry here".)
    const auto texel = [&](const test::Pixel& px) {
        const std::size_t i =
            (static_cast<std::size_t>(px.y) * kSize + static_cast<std::size_t>(px.x)) * 3;
        return std::array<float, 3>{g.rgb[i], g.rgb[i + 1], g.rgb[i + 2]};
    };
    const auto normal_dot = [&](const std::array<float, 3>& t, core::Vec3 expected) {
        const core::Vec3 n = oct_decode(t[0], t[1]);
        return n.x * expected.x + n.y * expected.y + n.z * expected.z;
    };

    const std::array<float, 3> rough_t = texel(rough_px);
    const std::array<float, 3> smooth_t = texel(smooth_px);
    MESSAGE("rough quad: normal·(0,1,0)=" << normal_dot(rough_t, {0.0f, 1.0f, 0.0f})
                                          << " roughness=" << rough_t[2]);
    const core::Vec3 tilt_n = rotated_up(tilt);
    MESSAGE("tilt quad: normal·expected=" << normal_dot(smooth_t, tilt_n)
                                          << " roughness=" << smooth_t[2]);

    // Normals: each decoded G-buffer normal points along its surface's true world normal to within
    // ~4° (dot > 0.997). The flat quad pins the axis-aligned case; the tilted quad pins that the
    // model→world normal transform and the octahedral round-trip both survive a real rotation.
    CHECK(normal_dot(rough_t, {0.0f, 1.0f, 0.0f}) > 0.997f);
    CHECK(normal_dot(smooth_t, tilt_n) > 0.997f);

    // Roughness: each surface reads its OWN material value (0.85 vs 0.30), not a shared constant —
    // a 0.03 margin (fp16 storage + the 0.045 clamp are the only perturbations).
    CHECK(rough_t[2] == doctest::Approx(0.85f).epsilon(0.04));
    CHECK(smooth_t[2] == doctest::Approx(0.30f).epsilon(0.04));

    // The geometry mask: a corner the quads do not cover stays at the clear value (all zero) — the
    // SSR march reads this as "no surface here, fall back".
    const std::array<float, 3> corner = texel({2.0f, 2.0f});
    CHECK(corner[0] == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(corner[1] == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(corner[2] == doctest::Approx(0.0f).epsilon(0.01)); // roughness 0 ⇒ no geometry
}

TEST_CASE("gbuffer: the gate holds — no G-buffer is produced with SSR off (m10.7a, ADR-0032 §11)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the G-buffer gate proof");
        return;
    }

    constexpr std::uint32_t kSize = 64;
    MeshRegistry meshes(*device);
    const MeshId plane_mesh = meshes.add(make_plane(2.0f), "gate-plane");
    MaterialRegistry materials;
    const MaterialId mat = materials.add(PbrMaterialDesc{});
    SceneRenderer renderer(*device, meshes, materials);

    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{plane_mesh}, MaterialRef{mat});
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 4.0f, 0.0f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{1.0f, 0.1f, 20.0f, true});

    // SSR OFF (the default) — the frame still renders, but the G-buffer is never allocated.
    LightingSettings ls; // ssr_enabled defaults false
    renderer.set_lighting(ls);
    RenderGraph graph(*device);
    graph.reset();
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
    REQUIRE(out.ldr.is_valid());         // the frame is produced as normal
    CHECK_FALSE(out.gbuffer.is_valid()); // but no G-buffer exists — zero extra work, the §11 gate
}
