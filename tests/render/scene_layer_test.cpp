// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the scene-layer brick (M5.5). Four claims:
//
//  (1) The procedural primitives are analytically correct — sphere positions are radius·normal
//      with unit normals, the cube's 24 flat normals are axis-aligned and its winding is CCW
//      seen from outside, the plane's uvs tile as asked. These are the geometry every later
//      shading proof leans on, so they get exact assertions, not smoke tests.
//
//  (2) The registries hand out dense ids and real GPU residency.
//
//  (3) The ECS render components register through reflection (the ADR-0016 editor-enabler bet):
//      spawn → get roundtrips, and the reflection TypeInfo actually describes the fields.
//
//  (4) End to end: a MeshRegistry cube, an OrbitCamera (freshly graduated from the viewer — its
//      first act as an engine citizen), and the render graph draw a depth-tested frame whose
//      pixels land where the projection math says they must.
//
// GPU-free on lavapipe, like every proof since M3.3.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "mesh_flat.frag.spv.h"
#include "mesh_flat.vert.spv.h"
#include "rime/core/math/mat.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/orbit_camera.hpp"
#include "rime/render/render_graph.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("scene layer: primitives are analytically exact (M5.5)") {
    using namespace rime::render;

    SUBCASE("uv sphere: position = radius·normal, normals unit") {
        const float radius = 2.5f;
        const CpuMesh sphere = make_uv_sphere(radius, 8, 16);
        REQUIRE(!sphere.vertices.empty());
        for (const MeshVertex& v : sphere.vertices) {
            const float nlen = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
            CHECK(nlen == doctest::Approx(1.0f).epsilon(1e-4));
            CHECK(v.px == doctest::Approx(radius * v.nx).epsilon(1e-4));
            CHECK(v.py == doctest::Approx(radius * v.ny).epsilon(1e-4));
            CHECK(v.pz == doctest::Approx(radius * v.nz).epsilon(1e-4));
        }
        for (const std::uint32_t i : sphere.indices)
            CHECK(i < sphere.vertices.size());
    }

    SUBCASE("cube: 24 flat axis-aligned normals, CCW-from-outside winding") {
        const CpuMesh cube = make_cube(0.5f);
        REQUIRE(cube.vertices.size() == 24);
        REQUIRE(cube.indices.size() == 36);
        for (const MeshVertex& v : cube.vertices) {
            // Exactly one axis component of ±1, the rest 0 — a flat face normal.
            const float ax = std::fabs(v.nx), ay = std::fabs(v.ny), az = std::fabs(v.nz);
            CHECK(ax + ay + az == doctest::Approx(1.0f));
            CHECK(ax * ay + ay * az + az * ax == doctest::Approx(0.0f));
        }
        // Winding: each triangle's geometric normal (cross of its edges) must point WITH its
        // vertices' shared normal — that is what "counter-clockwise seen from outside" means.
        for (std::size_t t = 0; t < cube.indices.size(); t += 3) {
            const MeshVertex& a = cube.vertices[cube.indices[t]];
            const MeshVertex& b = cube.vertices[cube.indices[t + 1]];
            const MeshVertex& c = cube.vertices[cube.indices[t + 2]];
            const float e1x = b.px - a.px, e1y = b.py - a.py, e1z = b.pz - a.pz;
            const float e2x = c.px - a.px, e2y = c.py - a.py, e2z = c.pz - a.pz;
            const float gx = e1y * e2z - e1z * e2y;
            const float gy = e1z * e2x - e1x * e2z;
            const float gz = e1x * e2y - e1y * e2x;
            CHECK(gx * a.nx + gy * a.ny + gz * a.nz > 0.0f);
        }
    }

    SUBCASE("plane: +y normal, uvs tile") {
        const CpuMesh plane = make_plane(4.0f, 8.0f);
        REQUIRE(plane.vertices.size() == 4);
        for (const MeshVertex& v : plane.vertices) {
            CHECK(v.ny == doctest::Approx(1.0f));
            CHECK(v.py == doctest::Approx(0.0f));
        }
        float max_u = 0.0f;
        for (const MeshVertex& v : plane.vertices)
            max_u = std::max(max_u, v.u);
        CHECK(max_u == doctest::Approx(8.0f)); // uv_tiles reached the far corner
    }
}

TEST_CASE("scene layer: ECS render components register through reflection (M5.5)") {
    using namespace rime::render;
    using rime::ecs::World;

    World world;
    register_render_components(world);

    // Spawn an entity wearing the full render wardrobe + a transform, and read it back.
    const auto e = world.spawn_with(MeshRef{7}, MaterialRef{3}, Camera{1.0f, 0.5f, 100.0f, true});
    REQUIRE(world.get<MeshRef>(e) != nullptr);
    CHECK(world.get<MeshRef>(e)->mesh == 7);
    CHECK(world.get<MaterialRef>(e)->material == 3);
    CHECK(world.get<Camera>(e)->z_far == doctest::Approx(100.0f));

    // The reflection contract (register once ⇒ describable forever): the TypeInfo really carries
    // the fields, which is what the M9 inspector and M11 replication will walk.
    const auto& cam_info = rime::core::ReflectionTraits<Camera>::info();
    CHECK(cam_info.fields.size() == 4);
    const auto& light_info = rime::core::ReflectionTraits<PointLight>::info();
    CHECK(light_info.fields.size() == 5);
}

TEST_CASE("scene layer: a registry cube through the graph and the graduated camera (M5.5)") {
    using namespace rime::rhi;
    using namespace rime::render;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping scene-layer render");
        return;
    }
    const std::uint32_t size = 64;

    MeshRegistry meshes(*device);
    const MeshId cube = meshes.add(make_cube(0.5f), "scene-cube");
    REQUIRE(cube != kInvalidMeshId);

    MaterialRegistry materials;
    const MaterialId red = materials.add({{0.8f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.7f});
    CHECK(materials.get(red).roughness == doctest::Approx(0.7f));

    // Shaders: MVP + color push constants over the registry vertex layout.
    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = mesh_flat_vert_spv;
    vsd.spirv_size_bytes = sizeof(mesh_flat_vert_spv);
    vsd.debug_name = "mesh_flat.vert";
    const ShaderHandle vsh = device->create_shader(vsd);
    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = mesh_flat_frag_spv;
    fsd.spirv_size_bytes = sizeof(mesh_flat_frag_spv);
    fsd.debug_name = "mesh_flat.frag";
    const ShaderHandle fsh = device->create_shader(fsd);

    struct Push {
        rime::core::Mat4 mvp;
        float color[4];
    };

    GraphicsPipelineDesc pd{};
    pd.vertex_shader = vsh;
    pd.fragment_shader = fsh;
    pd.vertex_layout.stride = MeshRegistry::vertex_stride();
    pd.vertex_layout.attributes = MeshRegistry::vertex_attributes();
    pd.color_format = Format::RGBA8Unorm;
    pd.cull = CullMode::Back; // the winding proof above is what makes back-face culling safe
    pd.depth_test = true;
    pd.depth_format = Format::D32Float;
    pd.push_constant_size = sizeof(Push);
    pd.debug_name = "scene-flat-pipeline";
    const PipelineHandle pipe = device->create_graphics_pipeline(pd);

    // The graduated camera's first act as an engine citizen: orbit to a known pose and project.
    OrbitCamera cam;
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.distance = 3.0f;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    Push push{};
    push.mvp = cam.view_proj(1.0f); // model = identity ⇒ mvp = proj·view
    push.color[0] = 0.9f;
    push.color[1] = 0.2f;
    push.color[2] = 0.1f;
    push.color[3] = 1.0f;

    RenderGraph graph(*device);
    graph.reset();
    RGTexture color = graph.create_texture({{size, size}, Format::RGBA8Unorm, "scene-color"});
    RGTexture depth = graph.create_texture({{size, size}, Format::D32Float, "scene-depth"});
    const RGColorAttachment color_att[] = {
        {color, LoadOp::Clear, StoreOp::Store, {0.0f, 0.0f, 0.3f, 1.0f}}};
    const RGDepthAttachment depth_att{depth, LoadOp::Clear, StoreOp::DontCare, 1.0f, 0, false};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = color_att;
    desc.depth = &depth_att;
    graph.add_raster_pass("scene-cube", desc, [&](CommandBuffer& cmd) {
        const GpuMesh& g = meshes.get(cube);
        cmd.bind_pipeline(pipe);
        cmd.bind_vertex_buffer(g.vertices);
        cmd.bind_index_buffer(g.indices, IndexType::Uint32);
        cmd.push_constants(&push, sizeof(push));
        cmd.draw_indexed(g.index_count);
    });
    graph.export_texture(color);

    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);

    // Readback: the cube (half-extent 0.5 at distance 3, fov ~50°) covers the center but not the
    // corners — the projection math in one assert pair.
    const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "scene-readback";
    const BufferHandle rb = device->create_buffer(rbd);
    auto cmd2 = device->begin_commands();
    cmd2->copy_texture_to_buffer(graph.physical(color), rb);
    device->submit_blocking(*cmd2);
    std::vector<std::uint8_t> px(bytes);
    device->read_buffer(rb, px.data(), px.size(), 0);

    const std::size_t center = (static_cast<std::size_t>(size / 2) * size + size / 2) * 4;
    CHECK(px[center + 0] > 180); // cube face color
    CHECK(px[center + 2] < 80);
    const std::size_t corner = (2u * size + 2u) * 4;
    CHECK(px[corner + 2] > 60); // clear color: the cube must NOT reach the corner
    CHECK(px[corner + 0] < 40);

    device->destroy(rb);
    device->destroy(pipe);
    device->destroy(fsh);
    device->destroy(vsh);
}
