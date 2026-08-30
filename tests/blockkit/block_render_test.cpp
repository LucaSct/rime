// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.2d — the block, DRAWN. The half of the render bridge that needs a device.
//
// tests/destruction_render proves the leaf life cycle headless (created once per part, following
// the sim, despawned on eviction). What it cannot see is anything involving a MeshId, because
// `MeshRegistry` holds an `rhi::Device&` and uploads on add. So three claims land here:
//
//   1. **Mesh per (pattern, part), not per (instance, part).** Two instances of the same cook must
//      draw the IDENTICAL MeshId for the same part index. This is the difference between ~148
//      vertex buffers for the whole block and 2,016 copies of the same geometry, and it is a claim
//      about ids — invisible without a registry that mints them.
//   2. **The frustum cull has real work.** ADR-0035 §2a wanted the block precisely so "culled 0 of
//      4,000" would be a red number rather than a warm frame. Standing at one end of a 44 m street,
//      most of the block is behind or beside the camera, so both halves of the counter must be
//      non-trivial.
//   3. **Something is actually there.** The dusk rig is dark by design, so "the frame is not black"
//      is a weaker claim than usual and is made carefully: the assertion is that the block's own
//      pixels are brighter than a frame rendered with the same camera and no block at all.
//
// No golden images — every claim is structural, the M5.6/M6.4 discipline.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "render_test_support.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/blockkit/block.hpp"
#include "rime/blockkit/palette.hpp"
#include "rime/blockkit/role.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction/components.hpp"
#include "rime/destruction/world.hpp"
#include "rime/destruction_render/part_leaves.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/physics/physics.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/render/components.hpp"
#include "rime/render/culling.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/scene/scene_format.hpp"

#ifndef RIME_BLOCK_COOKED_DIR
#define RIME_BLOCK_COOKED_DIR "cooked"
#endif

using namespace rime;

namespace {

constexpr std::uint32_t kSize = 160;

using render::test::vulkan_required;

// Average luminance of an RGBA8 readback — enough to say "there is something lit here" without
// pretending to be a golden image.
[[nodiscard]] double mean_luma(const std::vector<std::uint8_t>& rgba) {
    double sum = 0.0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
    }
    return sum / static_cast<double>(rgba.size() / 4);
}

} // namespace

TEST_CASE("m13.2d: the block draws — shared part meshes, a working cull, and lit pixels") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the block render proof");
        return;
    }

    const std::filesystem::path cooked = RIME_BLOCK_COOKED_DIR;
    const blockkit::BlockParams params;

    render::MeshRegistry meshes(*device);
    render::MaterialRegistry materials;

    ecs::World world;
    physics::PhysicsWorld physics;
    destruction::DestructionWorld destruction;
    destruction_render::PartLeafRenderer leaves;

    ecs::register_transform_components(world);
    blockkit::register_blockkit_components(world);
    destruction::register_destruction_components(world);
    render::register_render_components(world);

    // The cooks: one DestructionWorld pattern and one leaf-mesh set each.
    std::unordered_map<std::uint64_t, destruction::PatternId> patterns;
    std::size_t uploaded = 0;
    for (const blockkit::CookSpec& spec : blockkit::cook_specs()) {
        const auto bytes = platform::read_file(cooked / (std::string(spec.name) + ".rdest"));
        REQUIRE_MESSAGE(bytes.has_value(), "missing cook: " << std::string(spec.name));
        assets::AssetError err{};
        auto asset = assets::read_destructible(*bytes, err);
        REQUIRE(asset.has_value());

        const destruction::PatternId pattern = destruction.register_pattern(*asset, physics);
        REQUIRE(pattern.is_valid());
        patterns[spec.asset] = pattern;
        uploaded += leaves.register_pattern(pattern, *asset, &meshes);
    }

    // ~148 part meshes for the whole block: the sum of the nine cooks' part counts, uploaded ONCE
    // each. The alternative — a mesh per (instance, part) — would be 2,016.
    std::size_t distinct_parts = 0;
    for (const blockkit::CookSpec& spec : blockkit::cook_specs()) {
        distinct_parts += spec.parts;
    }
    CHECK(uploaded == distinct_parts);
    CHECK(uploaded < 200);
    CHECK(leaves.pattern_count() == blockkit::cook_specs().size());

    // The block, through the file, exactly as the demo will take it.
    ecs::World authoring;
    const blockkit::BlockStats stats = blockkit::assemble(authoring, params);
    REQUIRE(scene::load_scene_from_string(world, scene::save_scene_to_string(authoring)).ok);

    blockkit::BlockPalette palette = blockkit::build_palette(materials);
    blockkit::upload_prop_meshes(palette, meshes);
    const blockkit::PaletteStats painted = blockkit::apply_palette(world, palette);
    CHECK(painted.missing == 0);
    CHECK(painted.meshed > 0); // with a device the props DO get their unit primitives

    CHECK(blockkit::derive_world_transforms(world) == stats.entities);

    REQUIRE(destruction::bind_destructibles(world, destruction, physics, [&](std::uint64_t a) {
                const auto it = patterns.find(a);
                return it == patterns.end() ? destruction::PatternId{} : it->second;
            }).bound == stats.destructibles());

    const destruction_render::LeafStats leafed = leaves.update(world, destruction, physics);
    CHECK(leafed.instances_without_meshes == 0);
    CHECK(leafed.leaves_created == stats.parts); // 2,016 — one per part, no more and no fewer
    CHECK(leafed.leaves_live == stats.parts);

    SUBCASE("two instances of one cook share the same part meshes") {
        // Buildings 0 and 3 are both background, so their front ground half-walls come from the
        // same cook. If the bridge uploaded per instance these ids would differ, every count above
        // would still pass, and the block would cost 13x the vertex memory it needs.
        std::vector<std::vector<ecs::Entity>> per_building(params.building_count());
        world.query<blockkit::SlabRole, destruction::DestructibleInstanceRef>().for_each(
            [&](blockkit::SlabRole& role, destruction::DestructibleInstanceRef& ref) {
                if (role.kind == blockkit::slab_kind::kHalfWall &&
                    role.building < per_building.size()) {
                    per_building[role.building] =
                        leaves.leaves_of(destruction::InstanceId{ref.instance});
                }
            });
        REQUIRE(!per_building[0].empty());
        REQUIRE(!per_building[3].empty());
        REQUIRE(per_building[0].size() == per_building[3].size());

        std::size_t shared = 0;
        for (std::size_t p = 0; p < per_building[0].size(); ++p) {
            const auto* a = world.get<render::MeshRef>(per_building[0][p]);
            const auto* b = world.get<render::MeshRef>(per_building[3][p]);
            REQUIRE(a != nullptr);
            REQUIRE(b != nullptr);
            CHECK(a->mesh == b->mesh);
            if (a->mesh == b->mesh) {
                ++shared;
            }
        }
        CHECK(shared == per_building[0].size());
    }

    // ── Render it ────────────────────────────────────────────────────────────────────────────────
    // Clustered forward is REQUIRED, not optional: the block has 36 local lights and the ADR-0022
    // uniform-block path caps at kMaxPointLights (16). Without this the frame is not a worse
    // picture, it is a different scene.
    render::LightingSettings lighting;
    lighting.clustered_enabled = true;
    render::SceneRenderer renderer(*device, meshes, materials);
    renderer.set_lighting(lighting);
    renderer.set_ambient(blockkit::kAmbient[0], blockkit::kAmbient[1], blockkit::kAmbient[2]);

    const auto frame = [&]() {
        render::RenderGraph graph(*device);
        const render::SceneRenderer::Output out =
            renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);

        return render::test::read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    };

    renderer.reset_cull_stats();
    const std::vector<std::uint8_t> block_pixels = frame();
    const render::CullStats cull = renderer.cull_stats();

    MESSAGE("block frame: " << cull.culled << " culled of " << cull.considered() << " considered, "
                            << cull.submitted << " submitted");

    // ADR-0035 §2a's whole reason for wanting this content: at block scale the counter must show a
    // cull doing real work in BOTH directions. All-culled would mean an empty frame; none-culled
    // would mean the cull has silently stopped culling, which the m13.2a identity proof passes
    // perfectly.
    CHECK(cull.considered() >= stats.parts);
    CHECK(cull.submitted > 0);
    CHECK(cull.culled > 0);

    SUBCASE("the block's pixels are the block's, not the sky's") {
        // The dusk rig is dark on purpose, so "not black" is a weak claim. The honest one is
        // comparative: the same camera with nothing to look at must be dimmer than the same camera
        // looking down a lit street.
        ecs::World empty;
        ecs::register_transform_components(empty);
        render::register_render_components(empty);
        (void)empty.register_component<ecs::WorldTransform>();
        world.query<ecs::WorldTransform, render::Camera>().for_each(
            [&](ecs::WorldTransform& tf, render::Camera& cam) {
                (void)empty.spawn_with(ecs::WorldTransform{tf.value}, render::Camera{cam});
            });

        render::RenderGraph graph(*device);
        const render::SceneRenderer::Output out =
            renderer.render(graph, empty, {kSize, kSize}, true);
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const std::vector<std::uint8_t> bare =
            render::test::read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);

        CHECK(mean_luma(block_pixels) > mean_luma(bare) + 1.0);
    }
}
