// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include "render_test_support.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

namespace {
using namespace rime;
using namespace rime::render;
constexpr std::uint32_t size = 256;

core::Transform placed(core::Vec3 position) {
    core::Transform t;
    t.translation = position;
    return t;
}

core::Transform camera(float x = 0.0f, float z = 8.0f) {
    auto t = placed({x, 8, z});
    t.rotation = core::quat_from_axis_angle({1, 0, 0}, -std::atan2(8.0f, z));
    return t;
}

ecs::Entity draw(ecs::World& world, MeshId mesh, MaterialId material, core::Transform transform) {
    return world.spawn_with(ecs::WorldTransform{transform}, MeshRef{mesh}, MaterialRef{material});
}

test::HdrImage frame(rhi::Device& device, SceneRenderer& renderer, ecs::World& world) {
    RenderGraph graph(device);
    const auto out = renderer.render(graph, world, {size, size}, true);
    REQUIRE(out.hdr.is_valid());
    graph.export_texture(out.hdr);
    auto commands = device.begin_commands();
    graph.execute(*commands);
    device.submit_blocking(*commands);
    return test::decode_hdr(
        test::read_texture(device, graph.physical(out.hdr), size, size, 8), size, size);
}

// Sample the same WORLD patch after camera movement, rather than compare unrelated screen pixels.
// A small footprint avoids a single raster boundary without averaging across the broad shadow.
float patch(const test::HdrImage& image, const core::Transform& cam, core::Vec3 position) {
    const auto vp =
        core::perspective(0.9f, 1.0f, 0.1f, 40.0f) * core::inverse(core::to_matrix(cam));
    const auto pixel = test::project(vp, position, size);
    REQUIRE(pixel.x >= 2);
    REQUIRE(pixel.y >= 2);
    REQUIRE(pixel.x < size - 2);
    REQUIRE(pixel.y < size - 2);
    float result = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            result +=
                image.luminance(std::uint32_t(int(pixel.x) + x), std::uint32_t(int(pixel.y) + y));
    return result / 9.0f;
}

MaterialId matte(MaterialRegistry& materials) {
    return materials.add({{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 1.0f});
}

bool unavailable(const std::unique_ptr<rhi::Device>& device) {
    if (!device && test::vulkan_required())
        FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
    return !device || test::shadow_depth_sampling_unsupported(*device);
}
} // namespace

TEST_CASE("shadow motion: local world casters move and disappear without manual invalidation") {
    auto device = rhi::create_device({});
    if (unavailable(device)) {
        MESSAGE("shadow depth sampling unavailable");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const auto material = matte(materials);
    const auto floor = meshes.add(make_plane(8), "motion-floor");
    const auto torso = meshes.add(make_box({0.5f, 0.55f, 0.3f}), "rigid-torso");
    const auto limb = meshes.add(make_box({0.18f, 0.3f, 0.25f}), "rigid-leg");
    const auto edge = meshes.add(make_cube(0.1f), "camera-cull-control");
    SceneRenderer renderer(*device, meshes, materials);
    test::disable_temporal_aa(renderer); // isolate shadow radiance, not temporal reconstruction
    renderer.set_ambient(0.02f, 0.02f, 0.02f);
    LightingSettings settings;
    settings.shadows_enabled = settings.local_shadows_enabled = true;
    settings.local_shadow_resolution = 1024;
    renderer.set_lighting(settings);
    ecs::World world;
    register_render_components(world);
    (void)draw(world, floor, material, {});
    // Insert the edge draw BEFORE the body: making it visible changes camera-culling draw order.
    // It is outside the spot cone and cannot be the cause of either measured body shadow.
    (void)draw(world, edge, material, placed({5, 3, 0}));
    // Three rigid scene pieces exercise the shared caster path, not skeletal animation or assets.
    const std::array body{draw(world, torso, material, placed({1.5f, 3, 0})),
                          draw(world, limb, material, placed({1.25f, 2.2f, 0})),
                          draw(world, limb, material, placed({1.75f, 2.2f, 0}))};
    auto spot = placed({0, 6, 0});
    spot.rotation = core::quat_from_axis_angle({1, 0, 0}, -core::kHalfPi);
    SpotLight light;
    light.intensity = 300;
    light.range = 20;
    (void)world.spawn_with(ecs::WorldTransform{spot}, light);
    auto cam = camera();
    const auto camera_entity =
        world.spawn_with(ecs::WorldTransform{cam}, Camera{0.9f, 0.1f, 40, true});
    renderer.reset_cull_stats();
    const auto cold = frame(*device, renderer, world);
    REQUIRE(renderer.local_shadow_stats().rendered == 1);
    const auto initially_culled = renderer.cull_stats().culled;
    const auto repeated = frame(*device, renderer, world);
    CHECK(renderer.local_shadow_stats().reused == 1);
    CHECK(renderer.local_shadow_stats().rendered == 0);
    CHECK(patch(repeated, cam, {3, 0, 0}) ==
          doctest::Approx(patch(cold, cam, {3, 0, 0})).epsilon(0.01));
    cam = camera(0.8f);
    world.get<ecs::WorldTransform>(camera_entity)->value = cam;
    renderer.reset_cull_stats();
    const auto camera_only = frame(*device, renderer, world);
    CHECK(renderer.cull_stats().culled < initially_culled); // sensitivity: order really changed
    CHECK(renderer.local_shadow_stats().reused == 1);
    CHECK(renderer.local_shadow_stats().rendered == 0);
    for (const auto entity : body)
        world.get<ecs::WorldTransform>(entity)->value.translation.x -= 3;
    const auto moved = frame(*device, renderer, world);
    CHECK(renderer.local_shadow_stats().rendered == 1);
    CHECK(renderer.local_shadow_stats().reused == 0);
    for (const auto entity : body)
        (void)world.despawn(entity);
    const auto removed = frame(*device, renderer, world);
    CHECK(renderer.local_shadow_stats().rendered == 1);
    (void)draw(world, torso, material, placed({1.5f, 3, 0}));
    (void)draw(world, limb, material, placed({1.25f, 2.2f, 0}));
    (void)draw(world, limb, material, placed({1.75f, 2.2f, 0}));
    settings.local_shadows_enabled = false;
    renderer.set_lighting(settings);
    const auto disabled = frame(*device, renderer, world);
    const float old_lit = patch(disabled, cam, {3, 0, 0});
    const float new_lit = patch(disabled, cam, {-3, 0, 0});
    REQUIRE(old_lit > 0.05f);
    REQUIRE(new_lit > 0.05f);
    CHECK(patch(camera_only, cam, {3, 0, 0}) < 0.4f * old_lit);
    CHECK(patch(moved, cam, {3, 0, 0}) > 0.7f * old_lit);
    CHECK(patch(moved, cam, {-3, 0, 0}) < 0.4f * new_lit);
    CHECK(patch(removed, cam, {-3, 0, 0}) > 0.7f * new_lit);
    CHECK(patch(removed, cam, {3, 0, 0}) > 0.7f * old_lit);
}

TEST_CASE("shadow motion: double-sided textured foliage transmits cutouts under the sun") {
    auto device = rhi::create_device({});
    if (unavailable(device)) {
        MESSAGE("shadow depth sampling unavailable");
        return;
    }
    // Real alpha texture: opaque left half, transparent right half. Interior probes are far from
    // bilinear seams, and flipping the card makes double-sided SHADOW depth necessary.
    rhi::TextureDesc desc;
    desc.extent = {8, 8};
    desc.format = rhi::Format::RGBA8Srgb;
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    const auto texture = device->create_texture(desc);
    REQUIRE(texture.is_valid());

    struct OwnedTexture {
        rhi::Device& device;
        rhi::TextureHandle texture;

        ~OwnedTexture() { device.destroy(texture); }
    } owned{*device, texture}; // declared before renderer/materials: borrowed texture outlives them

    std::array<std::uint8_t, 8 * 8 * 4> pixels{};
    for (std::size_t i = 0; i < 64; ++i) {
        pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 255;
        pixels[i * 4 + 3] = i % 8 < 4 ? 255 : 0;
    }
    device->write_texture(texture, pixels.data(), pixels.size());
    MeshRegistry meshes(*device);
    const auto floor = meshes.add(make_plane(8), "foliage-floor");
    const auto card = meshes.add(make_plane(2), "foliage-card");
    MaterialRegistry materials;
    const auto floor_material = matte(materials);
    PbrMaterialDesc leaf;
    leaf.roughness = 1;
    leaf.alpha_cutoff = 0.5f;
    leaf.double_sided = leaf.clamp_uv = true;
    leaf.base_color_texture = texture;
    const auto leaf_material = materials.add(leaf);
    SceneRenderer renderer(*device, meshes, materials);
    test::disable_temporal_aa(renderer); // shadow-factor diagnostic, not an AA acceptance claim
    renderer.set_ambient(0.02f, 0.02f, 0.02f);
    LightingSettings settings;
    settings.shadows_enabled = true;
    settings.shadow_map_resolution = 1024;
    renderer.set_lighting(settings);
    ecs::World world;
    register_render_components(world);
    (void)draw(world, floor, floor_material, {});
    auto card_pose = placed({0, 2, 0});
    card_pose.rotation = core::quat_from_axis_angle({1, 0, 0}, 2 * core::kHalfPi);
    const auto foliage = draw(world, card, leaf_material, card_pose);
    auto sun = core::Transform{};
    sun.rotation = core::quat_from_axis_angle({1, 0, 0}, -core::kHalfPi);
    (void)world.spawn_with(ecs::WorldTransform{sun}, DirectionalLight{1, 1, 1, 3});
    const auto cam = camera(0, 12); // view rays pass beyond the card before reaching floor probes
    (void)world.spawn_with(ecs::WorldTransform{cam}, Camera{0.9f, 0.1f, 40, true});
    const auto present = frame(*device, renderer, world);
    const auto repeated = frame(*device, renderer, world);
    world.get<ecs::WorldTransform>(foliage)->value.translation.x = 3;
    const auto moved = frame(*device, renderer, world);
    (void)world.despawn(foliage);
    const auto removed = frame(*device, renderer, world);
    (void)draw(world, card, leaf_material, card_pose);
    settings.shadows_enabled = false;
    renderer.set_lighting(settings);
    const auto disabled = frame(*device, renderer, world);
    const float lit = patch(disabled, cam, {-1, 0, 0});
    REQUIRE(lit > 0.05f);
    CHECK(patch(present, cam, {-1, 0, 0}) < 0.4f * lit); // opaque leaf really casts
    CHECK(patch(present, cam, {1, 0, 0}) > 0.7f * lit);  // texture hole transmits
    CHECK(patch(repeated, cam, {-1, 0, 0}) ==
          doctest::Approx(patch(present, cam, {-1, 0, 0})).epsilon(0.01));
    CHECK(patch(moved, cam, {-1, 0, 0}) > 0.7f * lit);
    CHECK(patch(moved, cam, {2, 0, 0}) < 0.4f * lit);
    CHECK(patch(moved, cam, {4, 0, 0}) > 0.7f * lit);
    CHECK(patch(removed, cam, {2, 0, 0}) > 0.7f * lit);
}
