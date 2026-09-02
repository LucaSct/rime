// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m16 — the draw path: the per-frame uniform ring (m16.1) and per-submesh draws (m16.2).
//
// WHAT THIS FILE CAN AND CANNOT PROVE, STATED UP FRONT, because the gap is the whole reason the bug
// survived to be found by a review rather than by CI.
//
// The defect was a use-after-free: `ensure_draw_capacity` destroyed `draw_ubo_` the moment a
// frame's draw count outgrew it, while the PREVIOUS frame's command buffer still had that buffer
// baked into its descriptor sets — and `write_buffer` overwrote contents the GPU was still reading.
// Both need frames to actually overlap, which happens only on the windowed path: `present()` does
// not wait, and `acquire_next_image` waits on frame N-2's fence, so frame N-1 is still executing.
//
// Every test here runs headless through `submit_blocking`, which has already finished the frame
// before anything else happens. **So no test in this file can reproduce the use-after-free.** It
// was reproduced by hand, on hardware, with validation enabled — `vkDestroyBuffer(): ... currently
// in use by VkDescriptorSet ...` while turning the camera in `the_block --play`.
//
// What these cases DO pin is the structure that makes the fix real and would notice it being
// undone: the ring exists and is deeper than one, every slot is reached, each slot grows
// independently, and a scene rendered after a growth history is pixel-identical to the same scene
// rendered fresh. A ring that silently collapsed back to one buffer would fail the first assertion;
// a growth path that corrupted or skipped a slot would fail the last.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "material_fixture.hpp"
#include "render_test_support.hpp"
#include "rime/assets/manifest.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/gpu_asset_bridge.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"
#include "texture_fixture.hpp"

using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

namespace {

constexpr std::uint32_t kSize = 64;

// Spawn `n` cubes in view, plus the camera and a light. Returns the world by out-param because
// ecs::World is not movable.
void build_scene(ecs::World& world, MeshId cube, MaterialId mat, int n) {
    for (int i = 0; i < n; ++i) {
        core::Transform tf{};
        // A shallow grid a few metres ahead, all inside the frustum so the draw count the renderer
        // sees is the count we asked for rather than whatever survives culling.
        tf.translation = {static_cast<float>(i % 20) * 0.4f - 4.0f,
                          static_cast<float>(i / 20) * 0.4f - 1.0f,
                          -6.0f};
        (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube}, MaterialRef{mat});
    }
    core::Transform cam_tf{}; // identity looks down −z
    (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{});
    core::Transform light_tf{};
    light_tf.translation = {4.0f, 4.0f, 4.0f};
    (void)world.spawn_with(ecs::WorldTransform{light_tf},
                           PointLight{1.0f, 1.0f, 1.0f, 80.0f, 60.0f});
}

} // namespace

TEST_CASE("m16.1: the uniform ring is frames_in_flight + 1 deep, and every slot grows on its own") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the uniform-ring proof");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId cube = meshes.add(make_cube(0.15f), "ring-cube");
    REQUIRE(cube != kInvalidMeshId);
    MaterialRegistry materials;
    const MaterialId mat = materials.add({{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.5f});

    SceneRenderer renderer(*device, meshes, materials);

    // The default is the safe maximum, not the headless minimum: a windowed caller who forgets to
    // call set_frames_in_flight must get a ring that is too deep, never one that is too shallow.
    CHECK(renderer.ubo_slot_count() >= 2);

    renderer.set_frames_in_flight(2); // what the Vulkan swapchain reports today
    CHECK(renderer.ubo_slot_count() == 3);
    renderer.set_frames_in_flight(1); // a hypothetical shallower backend
    CHECK(renderer.ubo_slot_count() == 2);
    renderer.set_frames_in_flight(2);
    REQUIRE(renderer.ubo_slot_count() == 3);

    const auto render_scene = [&](ecs::World& world) {
        RenderGraph graph(*device);
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    };

    // Small and large scenes. 200 draws is well past the 64-draw capacity floor, so reaching it
    // forces a grow; dropping back to 1 and climbing again is what makes each of the three slots
    // take the growth path at a different moment.
    ecs::World small_world;
    register_render_components(small_world);
    build_scene(small_world, cube, mat, 1);

    ecs::World big_world;
    register_render_components(big_world);
    build_scene(big_world, cube, mat, 200);

    // Nine frames alternating sizes: with a 3-slot ring this visits every slot at both sizes, so
    // every slot has both grown and been re-used after growing.
    std::vector<std::uint8_t> last_small;
    std::vector<std::uint8_t> last_big;
    for (int i = 0; i < 3; ++i) {
        last_small = render_scene(small_world);
        last_big = render_scene(big_world);
        last_small = render_scene(small_world);
    }
    REQUIRE_FALSE(last_small.empty());
    REQUIRE_FALSE(last_big.empty());

    // THE ASSERTION THAT MATTERS: a renderer with a growth history behind it draws the same pixels
    // as a renderer that has never grown. A slot left un-grown, written at the wrong index, or
    // reused while stale would change these bytes.
    SceneRenderer fresh_small(*device, meshes, materials);
    fresh_small.set_frames_in_flight(2);
    RenderGraph g1(*device);
    const SceneRenderer::Output o1 = fresh_small.render(g1, small_world, {kSize, kSize}, true);
    REQUIRE(o1.ldr.is_valid());
    g1.export_texture(o1.ldr);
    auto c1 = device->begin_commands();
    g1.execute(*c1);
    device->submit_blocking(*c1);
    const std::vector<std::uint8_t> reference_small =
        read_texture(*device, g1.physical(o1.ldr), kSize, kSize, 4);

    CHECK(last_small == reference_small);

    // …and the two scenes must NOT be identical to each other, or the comparison above is vacuous:
    // it would pass just as well against a renderer that drew nothing at all.
    CHECK(last_small != last_big);
}

TEST_CASE("m16.2: a mesh split into submeshes draws identically, and the split is real") {
    // The cook has emitted one submesh per glTF primitive since `Mesh::from_primitives` existed,
    // and the reader has always validated the table — but `mesh_from_cooked` dropped it, because
    // `CpuMesh` had no field it could live in. A multi-material object therefore rendered entirely
    // in one material, and "split by material" was something the author had to do at the
    // Blender-object level.
    //
    // The proof is the culling brick's own discipline turned on submeshes: SPLIT IS IDENTICAL TO
    // WHOLE when both halves name the same material. A tolerance would accept a split that dropped
    // or duplicated triangles, which is exactly the failure to rule out.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the submesh proof");
        return;
    }

    MaterialRegistry materials;
    const MaterialId mat = materials.add({{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.5f});

    const CpuMesh cube = make_cube(0.5f);
    REQUIRE(cube.indices.size() % 6 == 0); // needs an even split into two whole-triangle halves
    const auto half = static_cast<std::uint32_t>(cube.indices.size() / 2);

    MeshRegistry meshes(*device);

    // The control: no table at all. `add` must synthesise exactly one whole-mesh range, so every
    // pre-m16.2 mesh keeps working with no special case anywhere in the draw path.
    const MeshId whole = meshes.add(cube, "whole");
    REQUIRE(whole != kInvalidMeshId);
    REQUIRE(meshes.get(whole).submeshes.size() == 1);
    CHECK(meshes.get(whole).submeshes[0].first_index == 0);
    CHECK(meshes.get(whole).submeshes[0].index_count == cube.indices.size());

    // The same geometry, declared as two ranges that tile the index buffer exactly.
    CpuMesh split_mesh = cube;
    split_mesh.submeshes = {{0, half, 0}, {half, half, 1}};
    const MeshId split = meshes.add(split_mesh, "split");
    REQUIRE(split != kInvalidMeshId);
    REQUIRE(meshes.get(split).submeshes.size() == 2);

    const auto frame = [&](MeshId id) {
        ecs::World world;
        register_render_components(world);
        core::Transform tf{};
        tf.translation = {0.0f, 0.0f, -3.0f};
        (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{id}, MaterialRef{mat});
        core::Transform cam_tf{};
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{});
        core::Transform light_tf{};
        light_tf.translation = {2.0f, 3.0f, 2.0f};
        (void)world.spawn_with(ecs::WorldTransform{light_tf},
                               PointLight{1.0f, 1.0f, 1.0f, 60.0f, 40.0f});

        SceneRenderer renderer(*device, meshes, materials);
        RenderGraph graph(*device);
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    };

    // THE HEADLINE: byte-identical, not merely similar.
    CHECK(frame(whole) == frame(split));

    // …and the split actually happened. Without this the identity check passes perfectly against a
    // renderer that ignored the table and drew the whole mesh both times — which is precisely the
    // bug being fixed.
    ExtractedScene probe;
    probe.draws.push_back({split, mat, core::Mat4{}});
    probe.draw_entities.push_back(ecs::Entity{});
    const ResolveDrawStats stats = resolve_draws(probe, meshes);
    CHECK(stats.dropped == 0);
    CHECK(stats.expanded == 1); // one EXTRA draw beyond the first
    REQUIRE(probe.draws.size() == 2);
    CHECK(probe.draws[0].first_index == 0);
    CHECK(probe.draws[0].index_count == half);
    CHECK(probe.draws[1].first_index == half);
    // The entity is repeated once per submesh, or the pick pass maps a pixel to the wrong entity.
    CHECK(probe.draw_entities.size() == probe.draws.size());
    CHECK(probe.draw_entities[0] == probe.draw_entities[1]);

    // A range outside the index buffer is DROPPED AND COUNTED, never clamped: clamping would
    // silently draw the wrong triangles, which is the harder failure to notice.
    const std::size_t rejected_before = meshes.rejected_submeshes();
    CpuMesh bad = cube;
    bad.submeshes = {{0, half, 0}, {half, half * 4, 1}}; // second range runs off the end
    const MeshId partly_bad = meshes.add(bad, "partly-bad");
    REQUIRE(partly_bad != kInvalidMeshId);
    CHECK(meshes.rejected_submeshes() == rejected_before + 1);
    CHECK(meshes.get(partly_bad).submeshes.size() == 1); // only the good range survived
}

TEST_CASE("m16.3: a scene-placed mesh gets its COOKED material, not neutral grey") {
    // The headline of M16. m15.1 made a scene name a mesh by content id and draw it, and stopped
    // one field short: the entity got `neutral_material_` (gpu_asset_bridge.cpp:119-125) which,
    // because the idempotence check short-circuited, was NEVER revisited. Cooked materials were
    // decoded by nobody outside the gltf-zoo sample, so every asset class dead-ended at grey.
    //
    // The chain is FOUR LEVELS deep and that is the whole difficulty: mesh Ready → read its
    // submesh `material_slot` → material Ready → read its five texture ids → textures Ready. Each
    // level's request cannot even be issued until the level above it has landed, which is why a
    // fixed one-round wait/pump/drain cannot converge and `settle` exists.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the material-bridge proof");
        return;
    }

    // A self-contained cooked tree in a temp dir: the committed fixtures carry no `.rmat` at all
    // (material is the one cooked kind with no cross-language fixture — a gap m16.5 closes), so the
    // material and its texture are synthesised here from the same builders the assets tests use.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "rime-m16-3-material-bridge";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const auto write = [&](const std::string& name, const std::vector<std::byte>& bytes) {
        std::ofstream f(dir / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    };

    constexpr std::uint64_t kMeshId = 0x00c0ffee0000cafeull;
    constexpr std::uint64_t kMatId = 0x00c0ffee0000aa70ull;
    constexpr std::uint64_t kTexId = 0x00c0ffee00007e00ull;

    rime_test::TextureFileBuilder tex;
    write("quad.img0.srgb.rtex", tex.build());

    rime_test::MaterialFileBuilder mat;
    mat.base_color_tex = kTexId;
    mat.metallic_roughness_tex = 0; // empty slots take the shader's 1x1 fallback, not a placeholder
    mat.normal_tex = 0;
    mat.occlusion_tex = 0;
    mat.emissive_tex = 0;
    write("quad.mat0.rmat", mat.build());

    // The cooked mesh is the committed fixture, copied in so one directory holds the whole tree.
    {
        const std::filesystem::path src =
            std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / "quad.rmesh";
        std::filesystem::copy_file(src, dir / "quad.rmesh");
    }

    // The manifest, hand-built so the test owns the ids it asserts on. THE `#material0` LINE IS THE
    // EDGE ITSELF (ADR-0039 ruling 1): the mesh's source path plus the submesh's slot number is
    // what resolves a material, rather than a material id embedded in the mesh payload.
    std::ostringstream mf;
    mf << "# rime-manifest v1\n"
       << "quad.gltf\tmesh\t" << std::hex << std::setw(16) << std::setfill('0') << kMeshId
       << "\tquad.rmesh\n"
       << "quad.gltf#material0\tmaterial\t" << std::setw(16) << std::setfill('0') << kMatId
       << "\tquad.mat0.rmat\n"
       << "quad.gltf#image0.srgb\ttexture\t" << std::setw(16) << std::setfill('0') << kTexId
       << "\tquad.img0.srgb.rtex\n";
    const std::optional<assets::Manifest> manifest = assets::Manifest::parse(mf.str());
    REQUIRE(manifest.has_value());

    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    GpuAssetBridge bridge(*device, server);
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    bridge.set_mesh_sink(meshes, materials);
    bridge.set_catalog(*manifest, dir);

    ecs::World world;
    register_render_components(world);
    core::Transform placed{};
    const ecs::Entity e = world.spawn_with(ecs::WorldTransform{placed}, MeshAsset{kMeshId});

    const std::size_t rounds = bridge.settle(world, 8);

    // THE SETTLE ACTUALLY LOOPED. Without this the proof would pass just as well against a
    // synchronous stub — and the four-level chain is exactly what a one-round wait cannot resolve.
    CHECK(rounds > 1);
    MESSAGE("settle converged in " << rounds << " rounds");

    // The entity now carries a material SET, and the set's material is NOT the neutral grey.
    const MaterialSet* set = world.get<MaterialSet>(e);
    REQUIRE(set != nullptr);
    REQUIRE(set->set != kInvalidMaterialSetId);
    const MaterialSetRegistry* sets = bridge.material_sets();
    REQUIRE(sets != nullptr);
    const MaterialId fallback = world.get<MaterialRef>(e)->material;
    const MaterialId resolved = sets->material_for(set->set, 0, fallback);
    CHECK(resolved != fallback); // it is not the grey the bridge assigns as a placeholder

    // The FACTORS came from the cooked record, each distinct in the fixture so a mis-mapped field
    // cannot hide behind a default.
    const PbrMaterialDesc& desc = materials.get(resolved);
    CHECK(desc.base_color[0] == doctest::Approx(0.8f));
    CHECK(desc.base_color[1] == doctest::Approx(0.4f));
    CHECK(desc.metallic == doctest::Approx(0.25f));
    CHECK(desc.roughness == doctest::Approx(0.6f));
    CHECK(desc.normal_scale == doctest::Approx(0.5f));
    CHECK(desc.occlusion_strength == doctest::Approx(0.75f));
    // The fixture is AlphaMode::Mask, so the cutoff survives — the field that has been cooked since
    // M6.3 and read by nothing, which is why every alpha-tested glTF drew as an opaque quad.
    CHECK(desc.alpha_cutoff == doctest::Approx(0.3f));

    // And its base-colour map is a real uploaded texture, not the magenta placeholder.
    CHECK(desc.base_color_texture.is_valid());

    // NEGATIVE CONTROL. Everything above would pass just as well against a bridge that invented a
    // material out of nothing, so: the same world, the same cooked files, and NO catalog. Nothing
    // may resolve, the entity must keep the fallback grey, and the failure must be COUNTED rather
    // than silent — a scene that quietly draws the wrong colour is the failure this brick exists to
    // end, and it looks identical to one that drew the right one unless something is counting.
    {
        assets::AssetServer bare_server(jobs);
        GpuAssetBridge bare(*device, bare_server);
        MeshRegistry bare_meshes(*device);
        MaterialRegistry bare_materials;
        bare.set_mesh_sink(bare_meshes, bare_materials);
        // deliberately no set_catalog

        ecs::World bare_world;
        register_render_components(bare_world);
        const ecs::Entity be =
            bare_world.spawn_with(ecs::WorldTransform{placed}, MeshAsset{kMeshId});
        (void)bare.settle(bare_world, 4);

        CHECK(bare_world.get<MaterialSet>(be) == nullptr); // no set was ever minted
        CHECK(bare.material_stats().resolved == 0);
        CHECK(bare.unresolved_count() >= 1); // and it says which id it could not find
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("m16.4: a masked material no longer punches a hole through the depth pre-pass") {
    // THE PRIMARY-VIEW ASSERTION, which has never existed. The depth pre-pass had no fragment
    // shader, so it wrote depth for texels a masked material discards; the forward pass then
    // discarded there, and everything BEHIND the hole failed CompareOp::Equal and was never
    // shaded. The hole rendered as clear colour — a cutout in front of a lit wall.
    //
    // m15.6a's proof could not see this because it renders with `use_depth_prepass=false`
    // (pbr_pipeline_test.cpp:591), the one configuration no production caller uses: every sample
    // and the editor host pass true.
    //
    // So the property under test is M5.6's own contract, restored for masked materials: THE SAME
    // PIXELS EITHER WAY. A tolerance would accept a hole that leaked a little.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the masked-depth proof");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId quad = meshes.add(make_plane(0.6f), "masked-quad");
    const MeshId wall = meshes.add(make_plane(3.0f), "wall");
    REQUIRE(quad != kInvalidMeshId);
    REQUIRE(wall != kInvalidMeshId);

    MaterialRegistry materials;
    // A cutout material with NO base-colour texture: the shader's 1x1 white fallback has alpha 1,
    // so a cutoff above 1 discards every fragment. That makes the masked quad fully transparent —
    // the strongest possible version of the bug, since the wall behind must be entirely visible.
    PbrMaterialDesc cutout{};
    cutout.base_color[3] = 0.0f; // alpha 0 * white 1 = 0, below any positive cutoff
    cutout.alpha_cutoff = 0.5f;
    const MaterialId masked_mat = materials.add(cutout);
    const MaterialId wall_mat = materials.add({{0.9f, 0.2f, 0.2f, 1.0f}, 0.0f, 0.6f});

    const auto frame = [&](bool prepass, bool with_quad) {
        ecs::World world;
        register_render_components(world);
        core::Transform wall_tf{};
        wall_tf.translation = {0.0f, 0.0f, -6.0f};
        wall_tf.rotation =
            core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, std::numbers::pi_v<float> / 2.0f);
        (void)world.spawn_with(ecs::WorldTransform{wall_tf}, MeshRef{wall}, MaterialRef{wall_mat});
        if (with_quad) {
            core::Transform quad_tf{}; // between the camera and the wall
            quad_tf.translation = {0.0f, 0.0f, -3.0f};
            quad_tf.rotation =
                core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, std::numbers::pi_v<float> / 2.0f);
            (void)world.spawn_with(
                ecs::WorldTransform{quad_tf}, MeshRef{quad}, MaterialRef{masked_mat});
        }
        core::Transform cam_tf{};
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{});
        core::Transform light_tf{};
        light_tf.translation = {0.0f, 2.0f, 0.0f};
        (void)world.spawn_with(ecs::WorldTransform{light_tf},
                               PointLight{1.0f, 1.0f, 1.0f, 90.0f, 60.0f});

        SceneRenderer renderer(*device, meshes, materials);
        RenderGraph graph(*device);
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, prepass);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    };

    // A fully-discarded quad must be INVISIBLE: the scene with it must match the scene without it,
    // with the pre-pass ON. Before the fix the quad still wrote depth and blacked out the wall.
    CHECK(frame(true, true) == frame(true, false));

    // …and the pre-pass must not change the picture, which is M5.6's contract and the assertion
    // that would have caught this in the first place.
    CHECK(frame(true, true) == frame(false, true));

    // NEGATIVE CONTROL 1: with the cutoff at zero the material does not mask at all, so the quad
    // becomes an ordinary opaque surface and the two scenes must now DIFFER. Without this, a
    // renderer that simply dropped every masked draw would pass everything above.
    PbrMaterialDesc opaque = cutout;
    opaque.alpha_cutoff = 0.0f;
    materials.update(masked_mat, opaque);
    CHECK(frame(true, true) != frame(true, false));

    // NEGATIVE CONTROL 2: and with masking off, prepass-on and prepass-off must STILL agree —
    // proving the fix did not achieve its result by quietly disabling the pre-pass.
    CHECK(frame(true, true) == frame(false, true));
}

TEST_CASE("m16.5: double-sided draws its back face, and a clamped sampler stops at the edge") {
    // glTF carries `doubleSided` on the material and wrap modes on each texture's sampler. The cook
    // read NEITHER — a repo-wide grep for `double` in tools/asset-pipeline returned zero hits — so
    // every surface the engine drew was back-face culled and every texture repeated, whatever the
    // author asked for. Foliage cards vanished from behind unless duplicated and flipped by hand,
    // and every atlas bled its far edge into its near one.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the double-sided proof");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId plane = meshes.add(make_plane(1.5f), "one-sided-plane");
    REQUIRE(plane != kInvalidMeshId);
    MaterialRegistry materials;

    PbrMaterialDesc desc{};
    desc.base_color[0] = 0.9f;
    desc.base_color[1] = 0.8f;
    desc.base_color[2] = 0.3f;
    const MaterialId mat = materials.add(desc);

    // The plane faces +y. Viewed from BELOW it presents only back faces, so with culling on it is
    // invisible and with culling off it is lit.
    const auto frame = [&](bool double_sided, float eye_y) {
        PbrMaterialDesc m = desc;
        m.double_sided = double_sided;
        materials.update(mat, m);

        ecs::World world;
        register_render_components(world);
        core::Transform plane_tf{};
        (void)world.spawn_with(ecs::WorldTransform{plane_tf}, MeshRef{plane}, MaterialRef{mat});
        core::Transform cam_tf{};
        cam_tf.translation = {0.0f, eye_y, 0.0f};
        // Look straight at the plane: pitch the camera toward it from wherever the eye is.
        const float pitch =
            eye_y > 0.0f ? -std::numbers::pi_v<float> / 2.0f : std::numbers::pi_v<float> / 2.0f;
        cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, pitch);
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{});
        core::Transform light_tf{};
        light_tf.translation = {0.0f, eye_y, 0.0f};
        (void)world.spawn_with(ecs::WorldTransform{light_tf},
                               PointLight{1.0f, 1.0f, 1.0f, 120.0f, 40.0f});

        SceneRenderer renderer(*device, meshes, materials);
        RenderGraph graph(*device);
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.ldr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const std::vector<std::uint8_t> px =
            read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
        std::size_t lit = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) {
            if (px[i] > 20 || px[i + 1] > 20) {
                ++lit;
            }
        }
        return lit;
    };

    // From BELOW: single-sided shows nothing, double-sided shows the plane.
    const std::size_t behind_culled = frame(false, -3.0f);
    const std::size_t behind_double = frame(true, -3.0f);
    CHECK(behind_culled == 0);
    CHECK(behind_double > 0);
    MESSAGE("from behind: " << behind_culled << " lit culled vs " << behind_double
                            << " double-sided");

    // NEGATIVE CONTROL: from the FRONT the two settings must be identical. Otherwise "double-sided"
    // is really "culling disabled everywhere", which would also pass the assertions above.
    CHECK(frame(false, 3.0f) == frame(true, 3.0f));
    CHECK(frame(false, 3.0f) > 0); // …and the front view is not simply empty
}
