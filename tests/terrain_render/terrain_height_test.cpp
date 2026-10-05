// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// THE m19.3 PROOF (ADR-0062): the terrain the GPU draws is the terrain physics collides with.
//
// M19.1 (ADR-0060) cooked the heightfield and gave it a collision shape; m19.3 draws it. The one
// claim worth proving about that draw is not that it looks like ground — it is that the SURFACE is
// the same surface, because the classic terrain bug is a ball that visibly floats above or sinks
// into ground drawn a few centimetres off the ground that is simulated, and nothing short of
// comparing the two numbers catches it.
//
// So, in the M5.6/M6.4 structural style (tests/render/pbr_pipeline_test.cpp) and with NO golden
// images:
//
//   * at 36 positions spread over a deliberately non-planar tile, the world height the engine's
//     terrain vertex stage reconstructs ON THE GPU is compared against the world height a
//     `rime::physics` raycast lands on, within a margin DERIVED (below) from the R16_UNORM round
//     trip — about 1 mm, three quantisation steps of this tile;
//   * the same comparison against the OPPOSITE cell diagonal is measured too, and must FAIL by a
//     wide margin. That is the test's built-in falsification: it is what distinguishes "the two
//     paths agree" from "the margin is so loose that any surface would pass";
//   * the real pass (`TerrainPass::add`, HDR target, Lambert shading) is driven end to end and its
//     radiance is checked against the analytic Lambert value computed from the CPU's own faceted
//     triangle normals — so the pass is proven to DRAW, not merely to be declarable;
//   * and the gate is proven structural: an unknown tile declares no pass at all.
//
// ── WHY THIS IS ITS OWN TEST TARGET ──────────────────────────────────────────────────────────
//
// It is the only test in the tree that needs `rime::render` AND `rime::physics` AND a Vulkan
// device at once. tests/destruction_render set the precedent for a cross-module target; this one
// adds the device, so like tests/render it only exists when the Vulkan backend is built.
//
// THE GLUE IS THE TEST'S OWN, DELIBERATELY. `rime::physics` does not depend on `engine/assets`
// (ADR-0060 §2) and never will, so nothing in the engine converts a `HeightfieldAsset` into a
// `HeightfieldDesc`. Here one `std::vector<std::uint16_t>` is handed to BOTH — which is the exact
// shape of the claim: one set of integers, two consumers.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include "fullscreen.vert.spv.h"
#include "rime/assets/asset_server.hpp"
#include "rime/assets/heightfield_asset.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"
#include "rime/render/gpu_asset_bridge.hpp"
#include "rime/render/lighting/sky.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/terrain_pass.hpp"
#include "rime/rhi/device.hpp"
#include "terrain.vert.spv.h"
#include "terrain_height_probe.frag.spv.h"
#include "terrain_layer_probe.frag.spv.h"
#include "terrain_m195_reference.frag.spv.h"

namespace {

using namespace rime;

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// ── The fixture ──────────────────────────────────────────────────────────────────────────────
//
// A 33x33 tile of 1 m cells (32 m square), placed OFF the origin and off y = 0 so that a dropped
// `origin` or a transposed axis moves the answer by metres rather than hiding inside the margin.
constexpr std::uint32_t kColumns = 33;
constexpr std::uint32_t kRows = 33;
constexpr float kCell = 1.0f;
constexpr float kHeightRange = 20.0f; // the u16 range maps onto [0, 20] m of local height
constexpr float kScale = kHeightRange / 65535.0f;
constexpr float kOffset = 0.0f;
constexpr core::Vec3 kOrigin{7.5f, 3.25f, -11.0f};

// The surface. Three requirements, each load-bearing:
//
//   * **NON-SEPARABLE**, so cells are genuinely non-planar. This one is subtle and the first
//     version of this fixture got it wrong: a height of the form g(x) + k(z) — any sum of a
//     function of x and a function of z, however curved — has a mixed second difference of
//     exactly ZERO, which means its grid cells are PLANAR and both cell diagonals describe the
//     same surface to within the bilinear remainder. The falsification check below then has
//     almost nothing to detect (measured: 2 mm, against a 1 mm margin). The `sin(x)·sin(z)`
//     product term is what makes a cell's four corners non-coplanar by ~0.27 m, so a flipped
//     diagonal moves the surface by tens of millimetres and the margin visibly separates them.
//   * **ASYMMETRIC in x and z** (different frequencies on each axis) — otherwise a transposed
//     (i, j) decomposition in the vertex stage would still produce the right height.
//   * strictly inside (0, 20) m so every sample quantises without clamping (asserted, not hoped).
double surface(double x, double z) {
    return 10.0 + 3.0 * std::sin(0.19 * x) + 2.0 * std::cos(0.11 * z) +
           2.5 * std::sin(0.37 * x) * std::sin(0.29 * z) + 0.04 * (x - 16.0) * (z - 8.0) / 8.0;
}

// Quantise exactly as a 16-bit height map would: q = round((f - offset) / scale).
std::vector<std::uint16_t> cook_samples() {
    std::vector<std::uint16_t> s(static_cast<std::size_t>(kColumns) * kRows);
    for (std::uint32_t j = 0; j < kRows; ++j) {
        for (std::uint32_t i = 0; i < kColumns; ++i) {
            const double h = surface(double(i) * kCell, double(j) * kCell);
            const double q = std::round((h - double(kOffset)) / double(kScale));
            REQUIRE(q > 0.0);     // no clamping at either end: the fixture must not
            REQUIRE(q < 65535.0); // quietly flatten against the format's limits
            s[i + kColumns * j] = static_cast<std::uint16_t>(q);
        }
    }
    return s;
}

// Dequantised sample height, LOCAL frame — the expression ADR-0060 §1 fixes and that both
// `HeightfieldShape::h()` and terrain.vert evaluate.
float sample_height(const std::vector<std::uint16_t>& s, std::uint32_t i, std::uint32_t j) {
    return kOffset + kScale * static_cast<float>(s[i + kColumns * j]);
}

// The height of the piecewise-planar surface at local (x, z), evaluated on the CPU for a given
// cell split. `min_to_max` is ADR-0060's own diagonal (v00–v11); false is the OTHER one (v10–v01),
// which exists here only so the proof can show that the margin tells the two apart.
//
// Over one triangle the height is affine in (x, z) — the heightfield's defining property, and what
// makes every evaluation here exact rather than iterative.
float cpu_surface_height(const std::vector<std::uint16_t>& s, float x, float z, bool min_to_max) {
    const auto ci = static_cast<std::uint32_t>(std::floor(x / kCell));
    const auto cj = static_cast<std::uint32_t>(std::floor(z / kCell));
    const float u = x / kCell - static_cast<float>(ci); // cell-local, in [0, 1)
    const float v = z / kCell - static_cast<float>(cj);
    const float h00 = sample_height(s, ci, cj);
    const float h10 = sample_height(s, ci + 1, cj);
    const float h01 = sample_height(s, ci, cj + 1);
    const float h11 = sample_height(s, ci + 1, cj + 1);
    if (min_to_max) {
        // Split along u == v. Triangle A (u >= v) is (v00, v10, v11); B (u < v) is (v00, v11, v01).
        return (u >= v) ? h00 + (h10 - h00) * u + (h11 - h10) * v
                        : h00 + (h11 - h01) * u + (h01 - h00) * v;
    }
    // Split along u + v == 1. (v00, v10, v01) below it; (v11, v10, v01) above.
    return (u + v < 1.0f) ? h00 + (h10 - h00) * u + (h01 - h00) * v
                          : (h01 + h10 - h11) + (h11 - h01) * u + (h11 - h10) * v;
}

// The unit upward normal of the triangle at local (x, z) under ADR-0060's diagonal: the gradient
// form (-dh/dx, 1, -dh/dz), normalised — the same construction `HeightfieldCell::normal` uses.
core::Vec3 cpu_surface_normal(const std::vector<std::uint16_t>& s, float x, float z) {
    const auto ci = static_cast<std::uint32_t>(std::floor(x / kCell));
    const auto cj = static_cast<std::uint32_t>(std::floor(z / kCell));
    const float u = x / kCell - static_cast<float>(ci);
    const float v = z / kCell - static_cast<float>(cj);
    const float h00 = sample_height(s, ci, cj);
    const float h10 = sample_height(s, ci + 1, cj);
    const float h01 = sample_height(s, ci, cj + 1);
    const float h11 = sample_height(s, ci + 1, cj + 1);
    const float sx = (u >= v) ? (h10 - h00) / kCell : (h11 - h01) / kCell;
    const float sz = (u >= v) ? (h11 - h10) / kCell : (h01 - h00) / kCell;
    return core::normalize(core::Vec3{-sx, 1.0f, -sz});
}

assets::HeightfieldAsset make_asset(const std::vector<std::uint16_t>& s) {
    assets::HeightfieldAsset a;
    a.columns = kColumns;
    a.rows = kRows;
    a.cell_size_x = kCell;
    a.cell_size_z = kCell;
    a.origin = kOrigin;
    a.height_scale = kScale;
    a.height_offset = kOffset;
    a.triangulation = assets::HeightfieldTriangulation::DiagonalMinToMax;
    const auto [lo, hi] = std::minmax_element(s.begin(), s.end());
    a.min_sample = *lo;
    a.max_sample = *hi;
    a.samples = s;
    return a;
}

// The SAME samples registered with physics. Nothing in the engine performs this conversion, and
// that is the point (see the file header): the span below and `HeightfieldAsset::samples` are one
// vector.
physics::BodyId add_terrain_body(physics::PhysicsWorld& world,
                                 const std::vector<std::uint16_t>& s) {
    physics::HeightfieldDesc d;
    d.samples = s;
    d.columns = kColumns;
    d.rows = kRows;
    d.cell_size_x = kCell;
    d.cell_size_z = kCell;
    d.height_scale = kScale;
    d.height_offset = kOffset;
    const physics::HeightfieldId id = world.register_heightfield(d);
    REQUIRE(id.is_valid());
    physics::BodyDesc b;
    b.motion = physics::MotionType::Static;
    b.shape.type = physics::ShapeType::Heightfield;
    b.shape.heightfield = id;
    b.position = kOrigin; // local (0,0,0) placed in the world, exactly as the asset's origin says
    const physics::BodyId body = world.create_body(b);
    REQUIRE(body.is_valid());
    return body;
}

// ── The camera, and the pixel ↔ world mapping the whole proof rests on ───────────────────────
//
// A TOP-DOWN ORTHOGRAPHIC view, fitted exactly to the tile. Orthographic, because it makes the
// pixel→world map affine and therefore invertible in closed form: there is no perspective divide
// to undo and no depth-dependent footprint. Fitted exactly, so pixel (px, py)'s CENTRE sits at
//
//     local x = (px + 0.5) * extent / size,    local z = (py + 0.5) * extent / size
//
// which for a 128² target over a 32 m tile is a 0.25 m lattice offset by 0.125 m — never on a cell
// boundary, so every probe lands strictly inside one triangle and no fill-rule tie-break can
// decide which surface the test is measuring.
constexpr std::uint32_t kSize = 128;
constexpr float kExtent = kCell * static_cast<float>(kColumns - 1); // 32 m
constexpr float kHalf = 0.5f * kExtent;

// The camera's world position, shared by the projection and by the BRDF's view vector (m19.5).
core::Vec3 top_down_eye() {
    return {kOrigin.x + kHalf, kOrigin.y + 200.0f, kOrigin.z + kHalf};
}

core::Mat4 top_down_view_proj() {
    const core::Vec3 eye = top_down_eye();
    const core::Vec3 target{eye.x, kOrigin.y, eye.z};
    // `up` must not be parallel to the view direction; (0, 0, -1) makes view-space +x = world +x
    // and view-space -y = world +z, which is the mapping asserted below rather than assumed.
    return core::ortho(-kHalf, kHalf, -kHalf, kHalf, 0.0f, 400.0f) *
           core::look_at(eye, target, {0.0f, 0.0f, -1.0f});
}

// Local (x, z) of pixel (px, py)'s centre — the analytic inverse of the projection above.
core::Vec3 pixel_to_local(std::uint32_t px, std::uint32_t py) {
    const float step = kExtent / static_cast<float>(kSize);
    return {(static_cast<float>(px) + 0.5f) * step, 0.0f, (static_cast<float>(py) + 0.5f) * step};
}

// Forward projection with the same matrix, so a convention slip in `pixel_to_local` cannot pass
// unnoticed (the pbr_pipeline_test `project()` pattern).
core::Vec3 project_to_pixel(const core::Mat4& view_proj, core::Vec3 world) {
    const core::Vec4 clip = view_proj * core::Vec4{world.x, world.y, world.z, 1.0f};
    return {(clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(kSize),
            (clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(kSize),
            clip.z / clip.w};
}

// The probe lattice: 36 pixels — 6 x 6, well inside the tile, spread over 27 of the 32 metres in
// each direction. ADR-0062's proof asks for at least 16.
constexpr std::uint32_t kProbeAxis[6] = {10, 30, 50, 70, 90, 110};

// ── The margin ───────────────────────────────────────────────────────────────────────────────
//
// DERIVED, not chosen. The two paths differ only in how the stored integer reaches an f32 height:
//
//  (1) R16_UNORM round trip. A fetch returns q/65535 as an f32 and the shader multiplies by
//      65535.0 again. Each step costs at most half an ULP, so q comes back within ~1.5 ULP, i.e.
//      1.5 * 65535 * 2^-24 ≈ 6e-3 of a quantisation STEP. One step here is
//      20 m / 65535 = 3.05e-4 m, so this is ≈ 1.9e-6 m.
//  (2) The dequantisation `offset + scale * q` in f32, on both sides: ≤ 1 ULP of ~20 m each,
//      ≈ 5e-6 m together.
//  (3) Interpolation and the ray. The rasteriser interpolates world y affinely (w = 1 under an
//      orthographic projection) at the pixel centre, which is exactly representable here; physics
//      solves the same plane analytically and reports origin + t*dir with origin.y = 203.25, whose
//      ULP is 1.5e-5 m. A few ULP of each gives ≈ 5e-5 m, and this term dominates.
//
// Sum ≈ 6e-5 m. The margin is 1 mm — about 16x that bound, to leave room for an implementation's
// barycentric precision, which Vulkan does not specify exactly. It is still only 3.3 quantisation
// steps of this tile, and it is ~35x SMALLER than the surface error a flipped cell diagonal would
// introduce here (measured by the falsification check below), which is what makes it meaningful.
constexpr float kMargin = 1.0e-3f;

// Readback: copy → host buffer → read, the tests/rhi pattern.
std::vector<std::uint8_t>
read_texture(rhi::Device& device, rhi::TextureHandle texture, std::uint32_t bytes_per_pixel) {
    const std::uint64_t bytes = std::uint64_t{kSize} * kSize * bytes_per_pixel;
    rhi::BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = rhi::BufferUsage::TransferDst;
    rbd.memory = rhi::MemoryUsage::GpuToCpu;
    rbd.debug_name = "terrain-readback";
    const rhi::BufferHandle rb = device.create_buffer(rbd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(texture, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

// IEEE 754 half → float (the HDR target is RGBA16Float). Same bit dance as pbr_pipeline_test's.
float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

} // namespace

TEST_CASE("m19.3: the height the GPU reconstructs is the height physics collides against") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the terrain height proof");
        return;
    }

    const std::vector<std::uint16_t> samples = cook_samples();

    // ── The physics side: the same integers, registered as a static terrain body ──────────────
    physics::PhysicsWorld physics_world;
    const physics::BodyId terrain = add_terrain_body(physics_world, samples);
    REQUIRE(terrain.is_valid());

    // ── The render side: the engine's pass, uploading the same integers as R16_UNORM ──────────
    render::TerrainPass pass(*device);
    const render::TerrainTileId tile_id = pass.upload(make_asset(samples));
    REQUIRE(tile_id != render::kInvalidTerrainTile);
    CHECK(pass.tiles_refused() == 0);
    const render::TerrainTile& tile = pass.tile(tile_id);
    CHECK(tile.vertex_count == kColumns * kRows);
    CHECK(tile.index_count == 6 * (kColumns - 1) * (kRows - 1));

    // ── The probe pipeline: the ENGINE's vertex stage, a test-owned fragment stage ────────────
    //
    // This is the M5.6 pattern — the proof wraps the real stage in a readback it can assert on,
    // rather than re-deriving what the real stage does. terrain.vert is compiled into this target
    // from engine/render/shaders (one source of truth per shader), and the constant block comes
    // from `render::terrain_push`, so there is no second copy of either to drift.
    rhi::ShaderDesc vsd{};
    vsd.stage = rhi::ShaderStage::Vertex;
    vsd.spirv = terrain_vert_spv;
    vsd.spirv_size_bytes = sizeof(terrain_vert_spv);
    vsd.debug_name = "terrain.vert";
    const rhi::ShaderHandle vs = device->create_shader(vsd);

    rhi::ShaderDesc fsd{};
    fsd.stage = rhi::ShaderStage::Fragment;
    fsd.spirv = terrain_height_probe_frag_spv;
    fsd.spirv_size_bytes = sizeof(terrain_height_probe_frag_spv);
    fsd.debug_name = "terrain_height_probe.frag";
    const rhi::ShaderHandle fs = device->create_shader(fsd);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Vertex},
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vs;
    pd.fragment_shader = fs;
    pd.color_format = rhi::Format::R32Uint; // exact 32-bit transport — see the probe shader
    pd.cull = rhi::CullMode::None;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.depth_format = render::kDepthFormat;
    pd.bindings = bindings;
    pd.push_constant_size = sizeof(render::TerrainPush);
    pd.debug_name = "terrain-height-probe";
    const rhi::PipelineHandle pipeline = device->create_graphics_pipeline(pd);

    rhi::SamplerDesc smp{};
    smp.mag_filter = rhi::Filter::Nearest;
    smp.min_filter = rhi::Filter::Nearest;
    smp.address_mode = rhi::AddressMode::ClampToEdge;
    smp.debug_name = "probe-heights";
    const rhi::SamplerHandle sampler = device->create_sampler(smp);

    const core::Mat4 view_proj = top_down_view_proj();

    // The pixel↔world mapping, asserted rather than assumed: project the local point this test
    // believes pixel (px, py) holds, and it must come back at that pixel's centre.
    for (const std::uint32_t px : kProbeAxis) {
        for (const std::uint32_t py : kProbeAxis) {
            const core::Vec3 local = pixel_to_local(px, py);
            const float h = cpu_surface_height(samples, local.x, local.z, true);
            const core::Vec3 world{kOrigin.x + local.x, kOrigin.y + h, kOrigin.z + local.z};
            const core::Vec3 p = project_to_pixel(view_proj, world);
            REQUIRE(p.x == doctest::Approx(static_cast<float>(px) + 0.5f).epsilon(1e-4));
            REQUIRE(p.y == doctest::Approx(static_cast<float>(py) + 0.5f).epsilon(1e-4));
            REQUIRE(p.z > 0.0f); // inside the ortho depth range, so the depth test keeps it
            REQUIRE(p.z < 1.0f);
        }
    }

    render::RenderGraph graph(*device);
    graph.reset();
    const render::RGTexture height_rt =
        graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "terrain-height-bits"});
    const render::RGTexture depth_rt =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "terrain-probe-depth"});
    graph.export_texture(height_rt);

    // Cleared to all-zero bits = +0.0 m. The tile sits at origin.y = 3.25 m with local heights in
    // (0, 20), so no drawn pixel can read 0 — "exactly zero" means no fragment landed, and the
    // coverage count below is a real vacuity witness rather than a formality.
    const render::RGColorAttachment colors[] = {
        {height_rt, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    const render::RGDepthAttachment depth_att{
        depth_rt, rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f, 0, false, 0};
    const render::RGTexture sampled[] = {
        graph.import_texture(tile.heights, rhi::ResourceState::ShaderRead)};
    render::RenderGraph::RasterPassDesc rpd{};
    rpd.colors = colors;
    rpd.depth = &depth_att;
    rpd.sampled = sampled;

    const render::TerrainPush push = render::terrain_push(tile, view_proj, top_down_eye(), {});
    graph.add_raster_pass("terrain-height-probe", rpd, [&](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipeline);
        cmd.bind_texture(0, tile.heights, sampler);
        cmd.bind_index_buffer(tile.indices, rhi::IndexType::Uint32);
        cmd.push_constants(&push, sizeof(push));
        cmd.draw_indexed(tile.index_count);
    });

    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);

    const std::vector<std::uint8_t> bits = read_texture(*device, graph.physical(height_rt), 4);

    // ── The comparison ───────────────────────────────────────────────────────────────────────
    int measured = 0;
    int flipped_rejected = 0;
    float worst = 0.0f;
    float flipped_worst = 0.0f;
    float flipped_closest = std::numeric_limits<float>::max();
    float lowest = std::numeric_limits<float>::max();
    float highest = std::numeric_limits<float>::lowest();
    for (const std::uint32_t px : kProbeAxis) {
        for (const std::uint32_t py : kProbeAxis) {
            std::uint32_t word = 0;
            std::memcpy(&word, &bits[(std::size_t{py} * kSize + px) * 4], sizeof(word));
            float gpu_height = 0.0f;
            std::memcpy(&gpu_height, &word, sizeof(gpu_height));
            if (word == 0) {
                continue; // no fragment here — counted, never silently skipped
            }

            const core::Vec3 local = pixel_to_local(px, py);
            const float wx = kOrigin.x + local.x;
            const float wz = kOrigin.z + local.z;

            // THE REFERENCE: what physics collides against, read through the public seam. A ray
            // straight down from well above the tile; its hit point IS the standable surface.
            physics::RayHit hit{};
            const physics::Ray ray{{wx, kOrigin.y + 200.0f, wz}, {0.0f, -1.0f, 0.0f}, 400.0f};
            REQUIRE(physics_world.raycast(ray, hit));
            CHECK(hit.body == terrain);

            const float error = std::fabs(gpu_height - hit.point.y);
            CHECK(error <= kMargin);
            worst = std::max(worst, error);

            // The CPU's own evaluation of the same triangle, as a third reading: it tells a future
            // failure apart ("the GPU moved" vs "the ray walk moved") instead of leaving a bare
            // inequality to interpret.
            const float cpu_same = kOrigin.y + cpu_surface_height(samples, local.x, local.z, true);
            CHECK(std::fabs(cpu_same - hit.point.y) <= kMargin);

            // THE FALSIFICATION. Under the OTHER cell diagonal the surface is a different
            // surface; if the margin could not tell the two apart, the agreement above would be
            // vacuous. Counted per probe rather than reduced to a single worst case, because the
            // two diagonals GENUINELY COINCIDE along the shared edge they both contain — a probe
            // that happens to sit near u == v cannot distinguish them and must not be asked to.
            // What has to hold is that the flipped surface is rejected almost everywhere, and by
            // far more than the margin where it is rejected.
            const float cpu_flipped =
                kOrigin.y + cpu_surface_height(samples, local.x, local.z, false);
            const float flipped_error = std::fabs(gpu_height - cpu_flipped);
            flipped_worst = std::max(flipped_worst, flipped_error);
            flipped_closest = std::min(flipped_closest, flipped_error);
            if (flipped_error > kMargin) {
                ++flipped_rejected;
            }

            lowest = std::min(lowest, gpu_height);
            highest = std::max(highest, gpu_height);
            ++measured;
        }
    }

    // ADR-0062's "N >= 16 positions". 36 are probed; every one of them must have been drawn, and
    // the requirement is stated as the ADR states it so a coverage regression reads as a failure.
    REQUIRE(measured >= 16);
    CHECK(measured == 36);
    CHECK(pass.tiles_drawn() ==
          0); // this case drove the vertex stage directly, not TerrainPass::add

    // Not a flat sheet: a terrain that happened to be constant would make the agreement above
    // trivially true, so the proof states how much height it actually swept.
    CHECK(highest - lowest > 5.0f);

    // The margin DISCRIMINATES. A flipped cell diagonal is rejected at the overwhelming majority
    // of the probes, and where it is rejected it misses by more than an order of magnitude past
    // the margin — so "the two surfaces agree to 1 mm" is a statement about this terrain and not
    // about any terrain.
    CHECK(flipped_rejected >= 30);
    CHECK(flipped_worst > 10.0f * kMargin);

    MESSAGE("m19.3 measured: worst |gpu - physics| = ",
            worst,
            " m over ",
            measured,
            " probes (margin ",
            kMargin,
            " m); height swept ",
            highest - lowest,
            " m; a flipped cell diagonal is rejected at ",
            flipped_rejected,
            "/",
            measured,
            " probes, by up to ",
            flipped_worst,
            " m (closest approach ",
            flipped_closest,
            " m)");

    device->wait_idle();
    device->destroy(pipeline);
    device->destroy(sampler);
    device->destroy(fs);
    device->destroy(vs);
}

// CPU port of terrain.frag's shading for one pixel: brdf.glsl's shade_light (GGX D, height-
// correlated Smith V, Schlick F, kd = (1-F)(1-metallic), kd*albedo/pi + specular, times radiance
// times n.l) plus the flat-ambient term ambient*((1-metallic)*base + f0). Written from the GLSL,
// in double precision, so it is an independent evaluation rather than a copy of the shader's
// floats.
std::array<double, 3> cpu_terrain_shade(const core::Vec3& n,
                                        const core::Vec3& v,
                                        const core::Vec3& l,
                                        const render::TerrainLight& light) {
    constexpr double kPi = 3.14159265358979;
    const double base[3] = {light.albedo.x, light.albedo.y, light.albedo.z};
    const double metallic = light.metallic;
    const double rough = std::clamp(static_cast<double>(light.roughness), 0.045, 1.0);
    const double alpha = rough * rough;
    const double irradiance = light.sun_irradiance;
    std::array<double, 3> out{};
    const double n_dot_l = core::dot(n, l);
    double h[3] = {double(v.x) + l.x, double(v.y) + l.y, double(v.z) + l.z};
    const double hl = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    for (double& c : h) {
        c /= hl;
    }
    const double n_dot_v = std::max(double(core::dot(n, v)), 1e-4);
    const double n_dot_h = std::max(n.x * h[0] + n.y * h[1] + n.z * h[2], 0.0);
    const double v_dot_h = std::max(v.x * h[0] + v.y * h[1] + v.z * h[2], 0.0);
    const double a2 = alpha * alpha;
    const double t = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    const double d = a2 / (kPi * t * t);
    const double gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const double gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    const double vis = 0.5 / std::max(gv + gl, 1e-5);
    const double fw = std::pow(1.0 - v_dot_h, 5.0);
    for (int c = 0; c < 3; ++c) {
        const double f0 = 0.04 + (base[c] - 0.04) * metallic;
        double radiance = 0.0;
        if (n_dot_l > 0.0) {
            const double fresnel = f0 + (1.0 - f0) * fw;
            const double kd = (1.0 - fresnel) * (1.0 - metallic);
            radiance = (kd * base[c] / kPi + d * vis * fresnel) * irradiance * n_dot_l;
        }
        out[c] = radiance + light.ambient * ((1.0 - metallic) * base[c] + f0);
    }
    return out;
}

TEST_CASE("m19.3: TerrainPass draws the tile, lit by its own faceted triangle normals") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the terrain draw proof");
        return;
    }

    const std::vector<std::uint16_t> samples = cook_samples();
    render::TerrainPass pass(*device);
    const render::TerrainTileId tile_id = pass.upload(make_asset(samples));
    REQUIRE(tile_id != render::kInvalidTerrainTile);

    // THE GATE IS STRUCTURAL: an unknown tile declares no pass at all, so a frame without terrain
    // is byte-identical to a build without this file in it.
    {
        render::RenderGraph gate(*device);
        gate.reset();
        const render::RGTexture hdr =
            gate.create_texture({{kSize, kSize}, render::kHdrFormat, "gate-hdr"});
        const render::RGTexture depth =
            gate.create_texture({{kSize, kSize}, render::kDepthFormat, "gate-depth"});
        pass.add(gate, hdr, depth, render::kInvalidTerrainTile, core::Mat4{}, {}, {});
        CHECK(gate.pass_count() == 0);
        CHECK(pass.tiles_drawn() == 0);
    }

    // A sun travelling mostly along +x: a slope rising toward +x turns its normal toward -x and is
    // lit; one falling toward +x is turned away. The fixture's sin(0.19x) term guarantees both
    // exist on the tile, so this is not a light that grazes everything equally.
    render::TerrainLight light{};
    light.sun_direction = {0.8f, -0.6f, 0.0f};
    light.sun_irradiance = 4.0f;
    light.albedo = {0.4f, 0.3f, 0.2f};
    light.ambient = 0.1f;

    const core::Mat4 view_proj = top_down_view_proj();

    render::RenderGraph graph(*device);
    graph.reset();
    const render::RGTexture hdr =
        graph.create_texture({{kSize, kSize}, render::kHdrFormat, "terrain-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "terrain-depth"});
    graph.export_texture(hdr);

    // TerrainPass LOADS both attachments (terrain joins a frame, it does not own it), so the test
    // supplies the frame's start: an empty raster pass whose only job is the clear. That is also
    // what makes "did terrain write this pixel?" answerable — the background is exactly black.
    {
        const render::RGColorAttachment clears[] = {
            {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}}};
        const render::RGDepthAttachment dclear{
            depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
        render::RenderGraph::RasterPassDesc cd{};
        cd.colors = clears;
        cd.depth = &dclear;
        graph.add_raster_pass("frame-clear", cd, [](rhi::CommandBuffer&) {});
    }

    pass.add(graph, hdr, depth, tile_id, view_proj, top_down_eye(), light);
    CHECK(pass.tiles_drawn() == 1);

    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);

    {
        const auto order = graph.execution_order();
        REQUIRE(order.size() == 2);
        CHECK(graph.pass_name(order[0]) == "frame-clear");
        CHECK(graph.pass_name(order[1]) == "terrain");
    }

    const std::vector<std::uint8_t> raw = read_texture(*device, graph.physical(hdr), 8);
    const auto* halves = reinterpret_cast<const std::uint16_t*>(raw.data());

    // ── The shading check: analytic, from the CPU's own faceted normals ──────────────────────
    //
    // terrain.frag's normal is the DRAWN triangle's plane normal (recovered from dFdx/dFdy of the
    // interpolated world position), which is the same faceted normal physics reports. So the
    // expected radiance at a probe is computable: cpu_terrain_shade's GGX model (the m19.5 shader).
    // Checking against that — rather than against a stored image — is what makes this a structural
    // proof: it would fail for a smooth (central-difference) normal, for a dropped 1/π, and for a
    // light pointing the wrong way.
    const core::Vec3 to_light = core::normalize(
        core::Vec3{-light.sun_direction.x, -light.sun_direction.y, -light.sun_direction.z});
    int shaded = 0;
    float brightest = 0.0f;
    float dimmest = std::numeric_limits<float>::max();
    float worst_rel = 0.0f;
    for (const std::uint32_t px : kProbeAxis) {
        for (const std::uint32_t py : kProbeAxis) {
            const std::size_t p = (std::size_t{py} * kSize + px) * 4;
            const float r = half_to_float(halves[p + 0]);
            const float g = half_to_float(halves[p + 1]);
            REQUIRE(std::isfinite(r));
            REQUIRE(r > 0.0f); // terrain covered this pixel (the background is pure black)

            const core::Vec3 local = pixel_to_local(px, py);
            const core::Vec3 n = cpu_surface_normal(samples, local.x, local.z);
            const float h = cpu_surface_height(samples, local.x, local.z, true);
            const core::Vec3 world{kOrigin.x + local.x, kOrigin.y + h, kOrigin.z + local.z};
            const core::Vec3 eye = top_down_eye();
            const core::Vec3 v =
                core::normalize(core::Vec3{eye.x - world.x, eye.y - world.y, eye.z - world.z});
            const auto expect = cpu_terrain_shade(n, v, to_light, light);
            const float expect_r = static_cast<float>(expect[0]);
            const float expect_g = static_cast<float>(expect[1]);

            // Tolerance: RGBA16Float carries a 10-bit mantissa, so one stored step is ~1e-3
            // relative; 1% leaves room for that plus the f32 normalise on each side. GENUINELY
            // relative — `.scale(0)` removes doctest's default +1 absolute slack, which at these
            // radiances (~0.1..1) once let a wrong model pass. This now fails for a dropped
            // specular lobe, a dropped (1-F) energy split, a wrong ambient, and the old
            // Lambert-only model, as well as for a smooth normal or a light pointing the wrong way.
            worst_rel = std::max(worst_rel, std::fabs(r - expect_r) / expect_r);
            CHECK(r == doctest::Approx(expect_r).epsilon(0.01).scale(0));
            CHECK(g == doctest::Approx(expect_g).epsilon(0.01).scale(0));
            brightest = std::max(brightest, r);
            dimmest = std::min(dimmest, r);
            ++shaded;
        }
    }
    REQUIRE(shaded == 36);
    // The light is doing work: a tile shaded by a constant would satisfy the per-probe check only
    // if the analytic value were constant too, but stating the contrast makes a degenerate light
    // (or a flattened normal) a visible failure rather than a quiet one.
    CHECK(brightest > 3.0f * dimmest);

    MESSAGE("m19.3 shading: worst relative radiance error ",
            worst_rel,
            " over ",
            shaded,
            " probes; brightest/dimmest = ",
            brightest / dimmest);
}

TEST_CASE("m19.3: upload refuses what it will not draw, and counts every refusal") {
    // GPU-free: `upload` validates before it touches the device, so this case is the one part of
    // the brick that runs on a machine with no Vulkan at all. It is still guarded, because
    // constructing the pass needs a device for its pipeline.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the terrain refusal proof");
        return;
    }
    const std::vector<std::uint16_t> samples = cook_samples();
    render::TerrainPass pass(*device);

    const assets::HeightfieldAsset good = make_asset(samples);
    std::uint64_t expected_refusals = 0;
    const auto refuse = [&](const assets::HeightfieldAsset& a) {
        CHECK(pass.upload(a) == render::kInvalidTerrainTile);
        CHECK(pass.tiles_refused() == ++expected_refusals);
    };

    assets::HeightfieldAsset bad = good;
    bad.columns = 1; // a line is not a surface
    refuse(bad);
    bad = good;
    bad.rows = 2; // the span no longer matches the grid
    refuse(bad);
    bad = good;
    bad.cell_size_x = 0.0f;
    refuse(bad);
    bad = good;
    bad.height_scale = -1.0f;
    refuse(bad);
    bad = good;
    bad.height_offset = std::numeric_limits<float>::quiet_NaN();
    refuse(bad);
    bad = good;
    // An appended triangulation value a later cook may introduce: refused by NAME rather than
    // drawn with the wrong diagonal, which is ADR-0060's "the reader rejects values it does not
    // know" applied at the GPU boundary.
    bad.triangulation = static_cast<assets::HeightfieldTriangulation>(7u);
    refuse(bad);
    bad = good;
    bad.columns = render::kMaxTileSamplesPerAxis + 1;
    bad.rows = 2;
    bad.samples.assign(std::size_t{bad.columns} * bad.rows, 0);
    refuse(bad);

    CHECK(pass.tile_count() == 0);
    CHECK(pass.upload(good) != render::kInvalidTerrainTile);
    CHECK(pass.tiles_refused() == expected_refusals); // the good one did not disturb the tally
}

// ═════════════════════════════════════════════════════════════════════════════════════════════
// m19.4 — SPLAT BLENDING (ADR-0063). Structural proofs, no golden images, no colour tolerances.
//
// Radiance under a fixed camera and light is LINEAR in albedo (terrain.frag: base * irradiance,
// where irradiance depends only on the geometry), so "the splat render of X" can be compared
// against "the plain v1 render with albedo X" PIXEL FOR PIXEL. Every claim below is one of:
// bit-identical to a v1 render, strictly between two v1 renders, or nearer one v1 render than
// another by a geometric region chosen with margin (never a colour margin).
// ═════════════════════════════════════════════════════════════════════════════════════════════
namespace {

using Palette4 = render::TerrainPalette;

render::TerrainLight splat_light(core::Vec3 albedo) {
    render::TerrainLight l{};
    l.sun_direction = {0.8f, -0.6f, 0.0f};
    l.sun_irradiance = 4.0f;
    l.albedo = albedo;
    l.ambient = 0.1f; // > 0, so no pixel's radiance is zero and a channel can never hide at black
    return l;
}

// A v2 asset over the fixture's heights. `weights` is 4 bytes per texel, row-major, x fastest.
assets::HeightfieldAsset make_splat_asset(const std::vector<std::uint16_t>& samples,
                                          std::uint32_t wc,
                                          std::uint32_t wr,
                                          std::vector<std::uint8_t> weights) {
    assets::HeightfieldAsset a = make_asset(samples);
    a.weight_columns = wc;
    a.weight_rows = wr;
    a.weights = std::move(weights);
    for (std::uint32_t k = 0; k < 4; ++k) {
        a.layers[k] = assets::AssetId{0x1000u + k};
    }
    return a;
}

std::vector<std::uint8_t>
uniform_weights(std::uint32_t wc, std::uint32_t wr, std::array<std::uint8_t, 4> texel) {
    std::vector<std::uint8_t> w;
    for (std::uint32_t t = 0; t < wc * wr; ++t) {
        w.insert(w.end(), texel.begin(), texel.end());
    }
    return w;
}

// Draw one tile through the real TerrainPass::add into a cleared HDR target; return the readback.
std::vector<std::uint8_t> render_tile(rhi::Device& device,
                                      render::TerrainPass& pass,
                                      render::TerrainTileId id,
                                      const render::TerrainLight& light,
                                      core::Vec3 eye = top_down_eye()) {
    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr =
        graph.create_texture({{kSize, kSize}, render::kHdrFormat, "splat-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "splat-depth"});
    graph.export_texture(hdr);
    {
        const render::RGColorAttachment clears[] = {
            {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}}};
        const render::RGDepthAttachment dclear{
            depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
        render::RenderGraph::RasterPassDesc cd{};
        cd.colors = clears;
        cd.depth = &dclear;
        graph.add_raster_pass("frame-clear", cd, [](rhi::CommandBuffer&) {});
    }
    pass.add(graph, hdr, depth, id, top_down_view_proj(), eye, light);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return read_texture(device, graph.physical(hdr), 8);
}

float chan(const std::vector<std::uint8_t>& img, std::uint32_t px, std::uint32_t py, int c) {
    std::uint16_t h = 0;
    std::memcpy(&h, &img[(std::size_t{py} * kSize + px) * 8 + std::size_t(c) * 2], sizeof(h));
    return half_to_float(h);
}

// Pixels whose RGB differs from the black clear: the vacuity witness ("something was drawn").
int covered_pixels(const std::vector<std::uint8_t>& img) {
    int n = 0;
    for (std::uint32_t py = 0; py < kSize; ++py) {
        for (std::uint32_t px = 0; px < kSize; ++px) {
            if (chan(img, px, py, 0) > 0.0f || chan(img, px, py, 1) > 0.0f ||
                chan(img, px, py, 2) > 0.0f) {
                ++n;
            }
        }
    }
    return n;
}

// The v1 reference render: the same heights, flat albedo `c`.
std::vector<std::uint8_t>
render_v1(rhi::Device& device, const std::vector<std::uint16_t>& samples, core::Vec3 c) {
    render::TerrainPass pass(device);
    const render::TerrainTileId id = pass.upload(make_asset(samples));
    REQUIRE(id != render::kInvalidTerrainTile);
    return render_tile(device, pass, id, splat_light(c));
}

std::vector<std::uint8_t> render_v2(rhi::Device& device,
                                    const std::vector<std::uint16_t>& samples,
                                    std::uint32_t wc,
                                    std::uint32_t wr,
                                    std::vector<std::uint8_t> weights,
                                    const Palette4& palette) {
    render::TerrainPass pass(device);
    const render::TerrainTileId id =
        pass.upload(make_splat_asset(samples, wc, wr, std::move(weights)), palette);
    REQUIRE(id != render::kInvalidTerrainTile);
    const auto img = render_tile(device, pass, id, splat_light({0.0f, 0.0f, 0.0f}));
    CHECK(pass.tiles_drawn() == 1);
    CHECK(pass.splat_refused() == 0);
    return img;
}

constexpr core::Vec3 kLayerColors[4] = {{0.8f, 0.2f, 0.1f},
                                        {0.1f, 0.7f, 0.9f},
                                        {0.9f, 0.9f, 0.05f},
                                        {0.05f, 0.1f, 0.3f}};

Palette4 distinct_palette() {
    Palette4 p{};
    for (std::size_t k = 0; k < 4; ++k) {
        p[k].base_color = kLayerColors[k];
    }
    return p;
}

std::unique_ptr<rhi::Device> splat_device() {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the splat proof");
    }
    return device;
}

} // namespace

TEST_CASE("m19.4: a splat tile whose four layers are equal is BIT-IDENTICAL to the flat tile") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    const core::Vec3 x{0.37f, 0.61f, 0.23f};
    const auto a = render_v1(*device, samples, x);

    // 5x3 — deliberately not the 33x33 height grid. Every texel sums to 255; the combinations are
    // chosen so that each weight channel, and sums that are not exactly representable (n/255),
    // occur.
    const std::uint8_t texels[15][4] = {{255, 0, 0, 0},
                                        {0, 255, 0, 0},
                                        {0, 0, 0, 255},
                                        {128, 127, 0, 0},
                                        {85, 85, 85, 0},
                                        {64, 64, 64, 63},
                                        {1, 1, 1, 252},
                                        {0, 0, 128, 127},
                                        {0, 0, 255, 0},
                                        {0, 128, 0, 127},
                                        {200, 50, 5, 0},
                                        {10, 20, 30, 195},
                                        {0, 0, 0, 255},
                                        {100, 100, 50, 5},
                                        {3, 252, 0, 0}};
    std::vector<std::uint8_t> w;
    for (const auto& t : texels) {
        w.insert(w.end(), t, t + 4);
    }
    Palette4 pal{};
    for (auto& l : pal) {
        l.base_color = x;
    }
    const auto b = render_v2(*device, samples, 5, 3, w, pal);

    const int covered = covered_pixels(a);
    CHECK(covered > 8000); // the tile fills the frame; a blank render would "match" itself
    CHECK(covered_pixels(b) == covered);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST_CASE("m19.4: a pure layer-0 map is bit-identical to the flat tile of that colour") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    const auto a = render_v1(*device, samples, kLayerColors[0]);
    const auto b = render_v2(
        *device, samples, 3, 3, uniform_weights(3, 3, {255, 0, 0, 0}), distinct_palette());
    CHECK(covered_pixels(a) > 8000);
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST_CASE("m19.4: a 50/50 blend lies strictly between the two layers' renders, per channel") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    const auto r0 = render_v1(*device, samples, kLayerColors[0]);
    const auto r1 = render_v1(*device, samples, kLayerColors[1]);
    const auto b = render_v2(
        *device, samples, 2, 2, uniform_weights(2, 2, {128, 127, 0, 0}), distinct_palette());
    int checked = 0;
    for (std::uint32_t py = 0; py < kSize; ++py) {
        for (std::uint32_t px = 0; px < kSize; ++px) {
            if (chan(r0, px, py, 0) <= 0.0f) {
                continue; // not covered
            }
            for (int c = 0; c < 3; ++c) {
                const float lo = std::min(chan(r0, px, py, c), chan(r1, px, py, c));
                const float hi = std::max(chan(r0, px, py, c), chan(r1, px, py, c));
                const float v = chan(b, px, py, c);
                REQUIRE(lo < hi);
                CHECK(v > lo);
                CHECK(v < hi);
            }
            ++checked;
        }
    }
    CHECK(checked > 8000);
}

TEST_CASE("m19.4: every layer is reachable — a pure layer-k map is nearest layer k's render") {
    // Catches a dropped w1/w2/w3 term, which the equal-layers anchor cannot see (every difference
    // is zero there). Four distinct colours, one uniform pure map per layer.
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    std::vector<std::uint8_t> refs[4];
    for (int k = 0; k < 4; ++k) {
        refs[k] = render_v1(*device, samples, kLayerColors[k]);
    }
    for (int k = 0; k < 4; ++k) {
        std::array<std::uint8_t, 4> t{0, 0, 0, 0};
        t[std::size_t(k)] = 255;
        const auto img =
            render_v2(*device, samples, 2, 2, uniform_weights(2, 2, t), distinct_palette());
        int wrong = 0;
        int checked = 0;
        for (std::uint32_t py = 0; py < kSize; ++py) {
            for (std::uint32_t px = 0; px < kSize; ++px) {
                if (chan(refs[0], px, py, 0) <= 0.0f) {
                    continue;
                }
                float best = std::numeric_limits<float>::max();
                int best_k = -1;
                for (int m = 0; m < 4; ++m) {
                    float d = 0.0f;
                    for (int c = 0; c < 3; ++c) {
                        d += std::fabs(chan(img, px, py, c) - chan(refs[m], px, py, c));
                    }
                    if (d < best) {
                        best = d;
                        best_k = m;
                    }
                }
                wrong += (best_k != k);
                ++checked;
            }
        }
        CHECK(checked > 8000);
        CHECK(wrong == 0);
    }
}

TEST_CASE("m19.4: the weight map is walked x-fastest, corner-aligned, left to right") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    // 4 wide x 2 tall: columns 0,1 pure layer 0; columns 2,3 pure layer 1 (both rows alike). A
    // transposed walk reads this as a top/bottom split, a flipped axis as a mirrored one; either
    // moves the left or right region's colour.
    std::vector<std::uint8_t> w;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t t[4] = {
                std::uint8_t(i < 2 ? 255 : 0), std::uint8_t(i < 2 ? 0 : 255), 0, 0};
            w.insert(w.end(), t, t + 4);
        }
    }
    const auto r0 = render_v1(*device, samples, kLayerColors[0]);
    const auto r1 = render_v1(*device, samples, kLayerColors[1]);
    const auto b = render_v2(*device, samples, 4, 2, w, distinct_palette());

    // Pixel column px has world-local x = (px + 0.5) * 0.25 m of a 32 m tile (orthographic,
    // fitted). Weight texel centres sit at x/extent = 0, 1/3, 2/3, 1, so x/extent < 1/3 is pure
    // layer 0 and > 2/3 pure layer 1. The regions below are the outer 23% / 23%, inside those spans
    // with margin.
    int left = 0;
    int right = 0;
    for (std::uint32_t py = 2; py < kSize - 2; ++py) {
        for (std::uint32_t px = 0; px < 30; ++px) {
            for (int c = 0; c < 3; ++c) {
                CHECK(chan(b, px, py, c) == chan(r0, px, py, c)); // exact: w1..w3 are 0 here
            }
            ++left;
        }
        for (std::uint32_t px = 98; px < kSize; ++px) {
            for (int c = 0; c < 3; ++c) {
                CHECK(std::fabs(chan(b, px, py, c) - chan(r1, px, py, c)) <
                      std::fabs(chan(b, px, py, c) - chan(r0, px, py, c)));
            }
            ++right;
        }
    }
    CHECK(left > 3000);
    CHECK(right > 3000);
}

TEST_CASE("m19.4: splat refusals are counted, and v1 assets are untouched by them") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    render::TerrainPass pass(*device);
    const auto good = make_splat_asset(samples, 2, 2, uniform_weights(2, 2, {255, 0, 0, 0}));

    // v2 through the palette-less overload: no materials, so refused rather than guessed.
    CHECK(pass.upload(good) == render::kInvalidTerrainTile);
    CHECK(pass.tiles_refused() == 1);
    CHECK(pass.splat_refused() == 1);

    // A weight map past the cap (4097 wide).
    auto wide = make_splat_asset(samples, 4097, 1, uniform_weights(4097, 1, {255, 0, 0, 0}));
    CHECK(pass.upload(wide, distinct_palette()) == render::kInvalidTerrainTile);
    CHECK(pass.tiles_refused() == 2);
    CHECK(pass.splat_refused() == 2);

    // A weight span that does not match its size.
    auto torn = good;
    torn.weights.pop_back();
    CHECK(pass.upload(torn, distinct_palette()) == render::kInvalidTerrainTile);
    CHECK(pass.tiles_refused() == 3);
    CHECK(pass.splat_refused() == 3);

    // A heights failure moves tiles_refused() only.
    auto bad = good;
    bad.cell_size_x = 0.0f;
    CHECK(pass.upload(bad, distinct_palette()) == render::kInvalidTerrainTile);
    CHECK(pass.tiles_refused() == 4);
    CHECK(pass.splat_refused() == 3);

    // A v1 asset uploads through either overload and never touches the splat counter.
    CHECK(pass.upload(make_asset(samples)) != render::kInvalidTerrainTile);
    CHECK(pass.upload(make_asset(samples), distinct_palette()) != render::kInvalidTerrainTile);
    CHECK(pass.upload(good, distinct_palette()) != render::kInvalidTerrainTile);
    CHECK(pass.tiles_refused() == 4);
    CHECK(pass.splat_refused() == 3);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════
// m19.5 — THE BRDF AND THE METALLIC/ROUGHNESS BLEND (ADR-0064). Still structural: bit-identity
// and strict inequalities, no golden images and no tuned colour margins.
// ═════════════════════════════════════════════════════════════════════════════════════════════
namespace {

render::TerrainLight pbr_light(core::Vec3 albedo, float metallic, float roughness) {
    render::TerrainLight l = splat_light(albedo);
    l.metallic = metallic;
    l.roughness = roughness;
    return l;
}

// Palette whose four layers share one colour; `layer1` overrides slot 1's material only.
Palette4 material_palette(core::Vec3 c, float m0, float r0, float m1, float r1) {
    Palette4 p{};
    for (std::size_t k = 0; k < 4; ++k) {
        p[k].base_color = c;
        p[k].metallic = k == 1 ? m1 : m0;
        p[k].roughness = k == 1 ? r1 : r0;
    }
    return p;
}

std::vector<std::uint8_t> render_v2_pbr(rhi::Device& device,
                                        const std::vector<std::uint16_t>& samples,
                                        std::uint32_t wc,
                                        std::uint32_t wr,
                                        std::vector<std::uint8_t> weights,
                                        const Palette4& palette) {
    return render_v2(device, samples, wc, wr, std::move(weights), palette);
}

int differing_covered_pixels(const std::vector<std::uint8_t>& a,
                             const std::vector<std::uint8_t>& b) {
    int n = 0;
    for (std::uint32_t py = 0; py < kSize; ++py) {
        for (std::uint32_t px = 0; px < kSize; ++px) {
            for (int c = 0; c < 3; ++c) {
                if (chan(a, px, py, c) != chan(b, px, py, c)) {
                    ++n;
                    break;
                }
            }
        }
    }
    return n;
}

// A planar tile: every sample equal, so the normal is exactly up everywhere and "mirror geometry"
// is a statement about two direction vectors rather than about the fixture's relief.
std::vector<std::uint16_t> flat_samples() {
    return std::vector<std::uint16_t>(cook_samples().size(), 20000);
}

// The brightest pixel (by channel sum) of a render of the flat tile, viewed from straight above
// with the sun straight down: the sun's mirror direction IS the view direction at the tile's
// centre, so the specular lobe's peak is in frame. Returns its RGB.
std::array<float, 3> flat_highlight(rhi::Device& device, float metallic, float roughness) {
    render::TerrainPass pass(device);
    const render::TerrainTileId id = pass.upload(make_asset(flat_samples()));
    REQUIRE(id != render::kInvalidTerrainTile);
    render::TerrainLight l = pbr_light({0.8f, 0.1f, 0.1f}, metallic, roughness);
    l.sun_direction = {0.0f, -1.0f, 0.0f};
    const auto img = render_tile(device, pass, id, l);
    CHECK(covered_pixels(img) > 8000);
    float best = -1.0f;
    std::array<float, 3> rgb{};
    for (std::uint32_t py = 0; py < kSize; ++py) {
        for (std::uint32_t px = 0; px < kSize; ++px) {
            const float sum = chan(img, px, py, 0) + chan(img, px, py, 1) + chan(img, px, py, 2);
            if (sum > best) {
                best = sum;
                rgb = {chan(img, px, py, 0), chan(img, px, py, 1), chan(img, px, py, 2)};
            }
        }
    }
    return rgb;
}

} // namespace

TEST_CASE("m19.5: four equal metallic layers are BIT-IDENTICAL to the flat tile of that material") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    const core::Vec3 x{0.37f, 0.61f, 0.23f};
    render::TerrainPass flat_pass(*device);
    const render::TerrainTileId flat_id = flat_pass.upload(make_asset(samples));
    REQUIRE(flat_id != render::kInvalidTerrainTile);
    const auto a = render_tile(*device, flat_pass, flat_id, pbr_light(x, 1.0f, 0.3f));

    const std::uint8_t texels[10][4] = {{255, 0, 0, 0},
                                        {0, 255, 0, 0},
                                        {0, 0, 0, 255},
                                        {128, 127, 0, 0},
                                        {85, 85, 85, 0},
                                        {64, 64, 64, 63},
                                        {1, 1, 1, 252},
                                        {0, 0, 128, 127},
                                        {200, 50, 5, 0},
                                        {10, 20, 30, 195}};
    std::vector<std::uint8_t> w;
    for (const auto& t : texels) {
        w.insert(w.end(), t, t + 4);
    }
    const auto b =
        render_v2_pbr(*device, samples, 5, 2, w, material_palette(x, 1.0f, 0.3f, 1.0f, 0.3f));
    CHECK(covered_pixels(a) > 8000);
    CHECK(covered_pixels(b) == covered_pixels(a));
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST_CASE("m19.5: metallic and roughness are reachable through the splat blend") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    const core::Vec3 x{0.7f, 0.5f, 0.3f};
    const auto pure0 = uniform_weights(2, 2, {255, 0, 0, 0});
    const auto pure1 = uniform_weights(2, 2, {0, 255, 0, 0});

    // Layer 1 differs from layer 0 ONLY in metallic.
    const Palette4 pm = material_palette(x, 0.0f, 0.5f, 1.0f, 0.5f);
    const auto m0 = render_v2_pbr(*device, samples, 2, 2, pure0, pm);
    const auto m1 = render_v2_pbr(*device, samples, 2, 2, pure1, pm);
    CHECK(covered_pixels(m0) > 8000);
    CHECK(differing_covered_pixels(m0, m1) > 8000);

    // ... and ONLY in roughness (both metal, so the specular lobe carries the difference).
    const Palette4 pr = material_palette(x, 1.0f, 0.2f, 1.0f, 0.9f);
    const auto r0 = render_v2_pbr(*device, samples, 2, 2, pure0, pr);
    const auto r1 = render_v2_pbr(*device, samples, 2, 2, pure1, pr);
    CHECK(covered_pixels(r0) > 8000);
    CHECK(differing_covered_pixels(r0, r1) > 8000);
}

TEST_CASE("m19.5: a metal's highlight takes its base colour, a dielectric's is white") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto metal = flat_highlight(*device, 1.0f, 0.2f);
    const auto diel = flat_highlight(*device, 0.0f, 0.2f);
    REQUIRE(metal[0] > 0.0f);
    REQUIRE(diel[0] > 0.0f);
    const float metal_gr = metal[1] / metal[0];
    const float diel_gr = diel[1] / diel[0];
    MESSAGE("m19.5 highlight G/R: metal ", metal_gr, ", dielectric ", diel_gr);
    // Base colour is (0.8, 0.1, 0.1): the metal's G/R heads for 0.125, the dielectric's for 1.
    CHECK(metal_gr < diel_gr);
}

TEST_CASE("m19.5: roughness spreads the highlight") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto smooth = flat_highlight(*device, 1.0f, 0.15f);
    const auto rough = flat_highlight(*device, 1.0f, 0.9f);
    const float s = smooth[0] + smooth[1] + smooth[2];
    const float r = rough[0] + rough[1] + rough[2];
    MESSAGE("m19.5 peak (channel sum): roughness 0.15 -> ", s, ", roughness 0.9 -> ", r);
    CHECK(s > r);
}

TEST_CASE("m19.5: terrain_push sanitises the flat material") {
    render::TerrainTile tile{};
    render::TerrainLight l{};
    l.metallic = std::numeric_limits<float>::quiet_NaN();
    l.roughness = std::numeric_limits<float>::infinity();
    render::TerrainPush p = render::terrain_push(tile, core::Mat4{}, {}, l);
    CHECK(p.material[0] == 0.0f);
    CHECK(p.material[1] == 1.0f);
    l.metallic = 1.5f;
    l.roughness = -0.2f;
    p = render::terrain_push(tile, core::Mat4{}, {}, l);
    CHECK(p.material[0] == 1.0f);
    CHECK(p.material[1] == 0.0f);
}

TEST_CASE("m19.5: out-of-range or non-finite layer materials are refused and counted") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    render::TerrainPass pass(*device);
    const auto good = make_splat_asset(samples, 2, 2, uniform_weights(2, 2, {255, 0, 0, 0}));
    std::uint64_t expected = 0;
    const auto refuse = [&](const Palette4& pal) {
        CHECK(pass.upload(good, pal) == render::kInvalidTerrainTile);
        ++expected;
        CHECK(pass.splat_refused() == expected);
        CHECK(pass.tiles_refused() == expected);
    };
    Palette4 p = distinct_palette();
    p[2].metallic = 1.5f;
    refuse(p);
    p = distinct_palette();
    p[3].roughness = -0.1f;
    refuse(p);
    p = distinct_palette();
    p[1].roughness = std::numeric_limits<float>::quiet_NaN();
    refuse(p);
    // The boundary values themselves are legal.
    p = distinct_palette();
    p[1].metallic = 1.0f;
    p[1].roughness = 0.0f;
    CHECK(pass.upload(good, p) != render::kInvalidTerrainTile);
    CHECK(pass.splat_refused() == expected);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════
// m19.6 — TERRAIN REFLECTS THE SKY (ADR-0065). Structural: one-f16-ULP agreement with a frozen
// m19.5 shader, and strict inequalities between renders. The sky is the engine's real SkyPass bake
// (sky-view LUT + SH projection), run in this binary on the same device — no synthetic fixture.
// ═════════════════════════════════════════════════════════════════════════════════════════════
namespace {

// Clear the frame exactly as render_tile does, so every m19.6 render starts from the same bytes.
void declare_clear(render::RenderGraph& graph, render::RGTexture hdr, render::RGTexture depth) {
    const render::RGColorAttachment clears[] = {
        {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}}};
    const render::RGDepthAttachment dclear{
        depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
    render::RenderGraph::RasterPassDesc cd{};
    cd.colors = clears;
    cd.depth = &dclear;
    graph.add_raster_pass("frame-clear", cd, [](rhi::CommandBuffer&) {});
}

// THE m19.5 PATH, rebuilt around the frozen reference shader: the engine's terrain.vert, the
// frozen m19.5 terrain.frag, m19.5's three bindings, and the SAME tile resources and push block
// the engine pass draws with (`pass.tile(id)`, `render::terrain_push`). The samplers mirror
// TerrainPass's two. Anything this render shares with the live pass is shared on purpose; the one
// thing that differs is the fragment stage, which is the thing under test.
std::vector<std::uint8_t> render_m195_reference(rhi::Device& device,
                                                const render::TerrainTile& tile,
                                                const render::TerrainLight& light) {
    rhi::ShaderDesc vsd{};
    vsd.stage = rhi::ShaderStage::Vertex;
    vsd.spirv = terrain_vert_spv;
    vsd.spirv_size_bytes = sizeof(terrain_vert_spv);
    vsd.debug_name = "terrain.vert";
    const rhi::ShaderHandle vs = device.create_shader(vsd);
    rhi::ShaderDesc fsd{};
    fsd.stage = rhi::ShaderStage::Fragment;
    fsd.spirv = terrain_m195_reference_frag_spv;
    fsd.spirv_size_bytes = sizeof(terrain_m195_reference_frag_spv);
    fsd.debug_name = "terrain_m195_reference.frag";
    const rhi::ShaderHandle fs = device.create_shader(fsd);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Vertex},
        {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment},
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vs;
    pd.fragment_shader = fs;
    pd.color_format = render::kHdrFormat;
    pd.cull = rhi::CullMode::None;
    pd.blend = rhi::BlendMode::None;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.depth_format = render::kDepthFormat;
    pd.bindings = bindings;
    pd.push_constant_size = sizeof(render::TerrainPush);
    pd.debug_name = "terrain-m19.5-reference";
    const rhi::PipelineHandle pipeline = device.create_graphics_pipeline(pd);

    rhi::SamplerDesc hs{};
    hs.mag_filter = rhi::Filter::Nearest;
    hs.min_filter = rhi::Filter::Nearest;
    hs.mip_filter = rhi::Filter::Nearest;
    hs.address_mode = rhi::AddressMode::ClampToEdge;
    hs.debug_name = "ref-heights";
    const rhi::SamplerHandle height_sampler = device.create_sampler(hs);
    rhi::SamplerDesc ws{};
    ws.mag_filter = rhi::Filter::Linear;
    ws.min_filter = rhi::Filter::Linear;
    ws.mip_filter = rhi::Filter::Nearest;
    ws.address_mode = rhi::AddressMode::ClampToEdge;
    ws.debug_name = "ref-weights";
    const rhi::SamplerHandle weight_sampler = device.create_sampler(ws);

    const render::TerrainPush push =
        render::terrain_push(tile, top_down_view_proj(), top_down_eye(), light);

    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr =
        graph.create_texture({{kSize, kSize}, render::kHdrFormat, "ref-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "ref-depth"});
    graph.export_texture(hdr);
    declare_clear(graph, hdr, depth);
    const render::RGColorAttachment colors[] = {{hdr, rhi::LoadOp::Load, rhi::StoreOp::Store, {}}};
    const render::RGDepthAttachment depth_att{
        depth, rhi::LoadOp::Load, rhi::StoreOp::Store, 1.0f, 0, false, 0};
    const render::RGTexture sampled[] = {
        graph.import_texture(tile.heights, rhi::ResourceState::ShaderRead),
        graph.import_texture(tile.weights, rhi::ResourceState::ShaderRead)};
    render::RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.depth = &depth_att;
    desc.sampled = sampled;
    graph.add_raster_pass("terrain-m19.5-reference", desc, [&](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipeline);
        cmd.bind_texture(0, tile.heights, height_sampler);
        cmd.bind_texture(1, tile.weights, weight_sampler);
        cmd.bind_uniform_buffer(2, tile.splat_ubo);
        cmd.bind_index_buffer(tile.indices, rhi::IndexType::Uint32);
        cmd.push_constants(&push, sizeof(push));
        cmd.draw_indexed(tile.index_count);
    });
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    auto out = read_texture(device, graph.physical(hdr), 8);

    device.destroy(weight_sampler);
    device.destroy(height_sampler);
    device.destroy(pipeline);
    device.destroy(fs);
    device.destroy(vs);
    return out;
}

// A clear physical sky (no clouds, so the bake is a function of the sun alone) whose sun sits at
// `elevation` radians, independent of TerrainLight's sun — the sky's appearance is what changes.
render::SkyParams sky_at(float elevation) {
    render::SkyParams sp{};
    sp.enabled = true;
    sp.clouds_enabled = false;
    sp.use_scene_sun = false;
    sp.sun_direction[0] = 0.0f;
    sp.sun_direction[1] = std::sin(elevation);
    sp.sun_direction[2] = -std::cos(elevation);
    return sp;
}

// Draw one tile through the real TerrainPass::add, lit by a REAL sky bake declared in the same
// graph — the order SceneRenderer uses (add_lighting first, so the graph orders the bake before
// the read). Afterwards the LUT's consumer state is reported back to its owner, the contract
// TerrainPass::add documents.
std::vector<std::uint8_t> render_tile_sky(rhi::Device& device,
                                          render::TerrainPass& pass,
                                          render::TerrainTileId id,
                                          const render::TerrainLight& light,
                                          render::SkyPass& sky,
                                          const render::SkyParams& params) {
    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr =
        graph.create_texture({{kSize, kSize}, render::kHdrFormat, "sky-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "sky-depth"});
    graph.export_texture(hdr);
    render::SkyInputs inputs{};
    inputs.camera_pos = top_down_eye();
    inputs.extent = {kSize, kSize};
    const render::SkyLightBinding binding = sky.add_lighting(graph, params, inputs);
    declare_clear(graph, hdr, depth);
    pass.add(graph, hdr, depth, id, top_down_view_proj(), top_down_eye(), light, binding);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    sky.note_skyview_state(rhi::ResourceState::ShaderRead);
    return read_texture(device, graph.physical(hdr), 8);
}

// Σ|a − b| / Σ a over every pixel and RGB channel: how much of a render moved, as a fraction of
// the render. Relative, so a bright and a dim surface are compared on the same footing.
double relative_change(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double diff = 0.0;
    double total = 0.0;
    for (std::uint32_t py = 0; py < kSize; ++py) {
        for (std::uint32_t px = 0; px < kSize; ++px) {
            for (int c = 0; c < 3; ++c) {
                diff += std::fabs(double(chan(a, px, py, c)) - double(chan(b, px, py, c)));
                total += chan(a, px, py, c);
            }
        }
    }
    return total > 0.0 ? diff / total : 0.0;
}

// Pixels where `bright` is strictly brighter (channel sum) than `dim`.
int strictly_brighter_pixels(const std::vector<std::uint8_t>& bright,
                             const std::vector<std::uint8_t>& dim) {
    int n = 0;
    for (std::uint32_t py = 0; py < kSize; ++py) {
        for (std::uint32_t px = 0; px < kSize; ++px) {
            const float b =
                chan(bright, px, py, 0) + chan(bright, px, py, 1) + chan(bright, px, py, 2);
            const float d = chan(dim, px, py, 0) + chan(dim, px, py, 1) + chan(dim, px, py, 2);
            if (b > d) {
                ++n;
            }
        }
    }
    return n;
}

// ── WHY THE NO-SKY ANCHOR IS "ONE f16 ULP", NOT memcmp ──────────────────────────────────────
//
// The m19.4/m19.5 anchors are bit-exact because they compare ONE program against itself: the
// blend's differences from layer 0 are exactly zero, so no compiler choice can move them. This
// anchor compares TWO SEPARATELY COMPILED programs — the live terrain.frag (sky branch present,
// not taken) and the frozen m19.5 copy — and no driver promises those compile to the same
// arithmetic: each may contract a multiply-add into an FMA, or reorder a sum, differently
// depending on what ELSE the shader contains. Measured, not assumed: on an RTX 3060 the shipped
// shader matches to the bit (0 pixels differ), but deleting one line from the sky branch moved
// 1-2 no-sky pixels by one f16 step; on RADV (AMD Raphael) the shipped shader itself differs at
// 2 pixels by 2^-10, which is exactly one f16 ULP of a value in [1, 2). lavapipe, macOS and
// Windows are unmeasured.
//
// So the claim is the honest one: every channel within ONE half-float ULP of the reference, where
// the ULP is read exactly from the f16 exponent of the larger magnitude — not a tuned epsilon. A
// rounding-mode difference in the last FMA can move an f16 result by at most that; a different
// branch (the falsification: force the sky path on) moves it by orders of magnitude more.
float half_ulp(std::uint16_t h) {
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    // Subnormals (and zero) share the smallest exponent's spacing, 2^-24; a normal half with
    // biased exponent e has 10 mantissa bits, so its spacing is 2^(e - 15 - 10).
    return exp == 0 ? std::ldexp(1.0f, -24) : std::ldexp(1.0f, static_cast<int>(exp) - 25);
}

struct UlpComparison {
    int differing_pixels = 0; // any channel not bit-equal
    int beyond_ulp = 0;       // channels more than one f16 ULP apart (NaN/inf count here too)
    float worst = 0.0f;       // largest per-channel absolute difference
};

UlpComparison compare_within_ulp(const std::vector<std::uint8_t>& a,
                                 const std::vector<std::uint8_t>& b) {
    UlpComparison r{};
    for (std::size_t px = 0; px < std::size_t{kSize} * kSize; ++px) {
        bool differs = false;
        for (std::size_t c = 0; c < 4; ++c) {
            std::uint16_t ha = 0;
            std::uint16_t hb = 0;
            std::memcpy(&ha, &a[px * 8 + c * 2], sizeof(ha));
            std::memcpy(&hb, &b[px * 8 + c * 2], sizeof(hb));
            if (ha == hb) {
                continue;
            }
            differs = true;
            const float fa = half_to_float(ha);
            const float fb = half_to_float(hb);
            const float diff = std::fabs(fa - fb);
            const std::uint16_t larger = std::fabs(fa) >= std::fabs(fb) ? ha : hb;
            if (!std::isfinite(fa) || !std::isfinite(fb) || !(diff <= half_ulp(larger))) {
                ++r.beyond_ulp;
            }
            if (std::isfinite(diff)) {
                r.worst = std::max(r.worst, diff);
            }
        }
        if (differs) {
            ++r.differing_pixels;
        }
    }
    return r;
}

// The anchor, plus the size of any difference — reported always, so a driver that matches to the
// bit and one that sits a ULP off are told apart in the log, not only on failure.
void check_within_one_half_ulp(const std::vector<std::uint8_t>& got,
                               const std::vector<std::uint8_t>& reference) {
    REQUIRE(got.size() == reference.size());
    const UlpComparison r = compare_within_ulp(got, reference);
    MESSAGE("vs m19.5: ",
            r.differing_pixels,
            " pixels differ, worst channel difference ",
            r.worst,
            ", channels beyond one f16 ULP: ",
            r.beyond_ulp);
    CHECK(r.beyond_ulp == 0);
}

} // namespace

TEST_CASE("m19.6: with no sky bound, terrain matches the m19.5 shader to one f16 ULP") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // The non-planar fixture, so the normals (and so every BRDF term) vary across the frame.
    const auto samples = cook_samples();

    struct Material {
        const char* name;
        float metallic;
        float roughness;
    };

    const Material materials[] = {{"metal", 1.0f, 0.3f}, {"dielectric", 0.0f, 0.6f}};
    for (const Material& m : materials) {
        INFO("material: ", m.name);
        const render::TerrainLight light = pbr_light({0.8f, 0.55f, 0.3f}, m.metallic, m.roughness);
        render::TerrainPass pass(*device);
        const render::TerrainTileId id = pass.upload(make_asset(samples));
        REQUIRE(id != render::kInvalidTerrainTile);

        const auto reference = render_m195_reference(*device, pass.tile(id), light);
        CHECK(covered_pixels(reference) > 8000);

        // (1) No sky argument at all: the pass binds its own placeholders.
        const auto no_sky = render_tile(*device, pass, id, light);
        check_within_one_half_ulp(no_sky, reference);
        CHECK(pass.sky_bound_draws() == 0);

        // (2) A caller's SkyPass::empty_binding: bound, but its SH flag is zero — same picture.
        render::SkyPass sky(*device);
        render::RenderGraph graph(*device);
        graph.reset();
        const render::RGTexture hdr =
            graph.create_texture({{kSize, kSize}, render::kHdrFormat, "empty-sky-hdr"});
        const render::RGTexture depth =
            graph.create_texture({{kSize, kSize}, render::kDepthFormat, "empty-sky-depth"});
        graph.export_texture(hdr);
        declare_clear(graph, hdr, depth);
        pass.add(graph,
                 hdr,
                 depth,
                 id,
                 top_down_view_proj(),
                 top_down_eye(),
                 light,
                 sky.empty_binding(graph));
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const auto empty = read_texture(*device, graph.physical(hdr), 8);
        check_within_one_half_ulp(empty, reference);
        CHECK(pass.sky_bound_draws() == 1);
        CHECK(pass.tiles_drawn() == 2);
        // The default binding of (1) is legitimate "no sky" and was not counted as partial.
        CHECK(pass.sky_partial_bindings() == 0);
    }
}

TEST_CASE("m19.6: a partial sky binding draws on the placeholders, counted") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    const render::TerrainLight light = pbr_light({0.8f, 0.55f, 0.3f}, 1.0f, 0.3f);
    render::TerrainPass pass(*device);
    const render::TerrainTileId id = pass.upload(make_asset(samples));
    REQUIRE(id != render::kInvalidTerrainTile);
    const auto reference = render_m195_reference(*device, pass.tile(id), light);

    // Only the LUT is set: the SH buffer and the sampler are missing.
    render::SkyPass sky(*device);
    render::RenderGraph graph(*device);
    graph.reset();
    const render::RGTexture hdr =
        graph.create_texture({{kSize, kSize}, render::kHdrFormat, "partial-sky-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "partial-sky-depth"});
    graph.export_texture(hdr);
    declare_clear(graph, hdr, depth);
    render::SkyLightBinding partial{};
    partial.skyview = sky.empty_binding(graph).skyview;
    pass.add(graph, hdr, depth, id, top_down_view_proj(), top_down_eye(), light, partial);
    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);
    const auto img = read_texture(*device, graph.physical(hdr), 8);
    CHECK(pass.sky_partial_bindings() == 1);
    CHECK(pass.sky_bound_draws() == 0);
    CHECK(pass.tiles_drawn() == 1);
    check_within_one_half_ulp(img, reference); // the draw still happened, on the flat path

    // A default binding afterwards does not move the counter.
    (void)render_tile(*device, pass, id, light);
    CHECK(pass.sky_partial_bindings() == 1);
    CHECK(pass.tiles_drawn() == 2);
}

TEST_CASE("m19.6: a smooth metal follows the sky more than a rough dielectric does") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    render::SkyPass sky(*device);
    const render::SkyParams high = sky_at(1.2f); // ~69 degrees
    const render::SkyParams low = sky_at(0.08f); // ~5 degrees: a sunset sky
    // Same base colour, same TerrainLight sun (which does NOT move): only the sky changes.
    const auto render_pair = [&](float metallic, float roughness) {
        render::TerrainPass pass(*device);
        const render::TerrainTileId id = pass.upload(make_asset(samples));
        REQUIRE(id != render::kInvalidTerrainTile);
        const render::TerrainLight l = pbr_light({0.8f, 0.8f, 0.8f}, metallic, roughness);
        auto a = render_tile_sky(*device, pass, id, l, sky, high);
        auto b = render_tile_sky(*device, pass, id, l, sky, low);
        CHECK(covered_pixels(a) > 8000);
        CHECK(pass.sky_bound_draws() == 2);
        return std::make_pair(std::move(a), std::move(b));
    };
    const auto metal = render_pair(1.0f, 0.1f);
    const auto diel = render_pair(0.0f, 1.0f);
    CHECK(sky.stats().filled >= 2); // the swap really re-baked (high, low, high, low)
    const double metal_change = relative_change(metal.first, metal.second);
    const double diel_change = relative_change(diel.first, diel.second);
    MESSAGE("m19.6 relative change under the sky swap: smooth metal ",
            metal_change,
            ", rough dielectric ",
            diel_change);
    CHECK(metal_change > 0.0);
    CHECK(metal_change > diel_change);
}

TEST_CASE("m19.6: with the sun off, a sky-lit metal is no longer black") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const auto samples = cook_samples();
    render::TerrainLight l = pbr_light({0.9f, 0.6f, 0.2f}, 1.0f, 0.3f);
    l.sun_irradiance = 0.0f;
    l.ambient = 0.0f; // and no flat ambient: without a sky nothing lights this tile
    render::TerrainPass pass(*device);
    const render::TerrainTileId id = pass.upload(make_asset(samples));
    REQUIRE(id != render::kInvalidTerrainTile);
    render::SkyPass sky(*device);
    const auto dark = render_tile(*device, pass, id, l);
    const auto lit = render_tile_sky(*device, pass, id, l, sky, sky_at(0.6f));
    CHECK(covered_pixels(dark) == 0); // the claim's premise: a metal under no light is black
    const int covered = covered_pixels(lit);
    MESSAGE("m19.6 sun-off metal: ", covered, " sky-lit pixels");
    CHECK(covered > 8000);
    CHECK(strictly_brighter_pixels(lit, dark) == covered);
}

TEST_CASE("m19.6: the sky's diffuse carries the albedo of a rough dielectric") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // With the sun off, a dielectric's sky light is diffuse (1-metallic)*base*SH plus a specular
    // term whose f0 is 0.04 whatever the base colour. So two base colours differ ONLY through the
    // SH diffuse: drop it and the renders are identical.
    const auto samples = cook_samples();
    render::SkyPass sky(*device);
    const render::SkyParams sp = sky_at(0.6f);
    const auto render_base = [&](float g) {
        render::TerrainLight l = pbr_light({g, g, g}, 0.0f, 1.0f);
        l.sun_irradiance = 0.0f;
        l.ambient = 0.0f;
        render::TerrainPass pass(*device);
        const render::TerrainTileId id = pass.upload(make_asset(samples));
        REQUIRE(id != render::kInvalidTerrainTile);
        return render_tile_sky(*device, pass, id, l, sky, sp);
    };
    const auto bright = render_base(0.9f);
    const auto dark = render_base(0.1f);
    const int covered = covered_pixels(bright);
    CHECK(covered > 8000);
    CHECK(strictly_brighter_pixels(bright, dark) == covered);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════
// m19.7b — EACH LAYER SAMPLES ITS OWN ALBEDO+HEIGHT TEXTURE AT WORLD-XZ UVs (ADR-0066 addendum).
// Structural, like everything above: bit identity for the anchor, strict inequalities for the
// orientation and period, one f16 ULP (derived below) for the seam, exact counts for refusals, and
// the f32 bits a shader sees for the alpha channel. No golden images.
//
// Most of these renders light with the AMBIENT TERM ONLY (sun irradiance 0, ambient 1, flat tile,
// no sky): the radiance is then (1 - metallic) * base + f0 = base + 0.04 for a dielectric — a
// strictly increasing function of the texture's colour that does not depend on the view vector or
// the normal. So a pixel's colour is a statement about the texture lookup and nothing else.
// ═════════════════════════════════════════════════════════════════════════════════════════════
namespace {

// A layer texture as a builder holds one: RGBA8_SRGB, row-major, x fastest — the format the
// cooked `Rgba8Srgb` terrain-layer texture is uploaded as (GpuAssetBridge's to_rhi_format).
rhi::TextureHandle make_layer_texture(rhi::Device& device,
                                      std::uint32_t w,
                                      std::uint32_t h,
                                      const std::vector<std::uint8_t>& rgba) {
    REQUIRE(rgba.size() == std::size_t{w} * h * 4);
    rhi::TextureDesc td{};
    td.extent = {w, h};
    td.format = rhi::Format::RGBA8Srgb;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "m19.7b-layer";
    const rhi::TextureHandle t = device.create_texture(td);
    REQUIRE(t.is_valid());
    device.write_texture(t, rgba.data(), rgba.size());
    return t;
}

// top_down_view_proj(), generalised to a 32 m square centred anywhere: pixel (px, py)'s centre is
// world (cx - kHalf + (px + 0.5) * 0.25, cz - kHalf + (py + 0.5) * 0.25). The m19.3 proof pins that
// mapping for the original; this is the same matrix with a different centre.
struct TopDown {
    core::Mat4 view_proj;
    core::Vec3 eye;
    float cx;
    float cz;
};

TopDown top_down_centred(float cx, float cz) {
    const core::Vec3 eye{cx, kOrigin.y + 200.0f, cz};
    const core::Vec3 target{cx, kOrigin.y, cz};
    return {core::ortho(-kHalf, kHalf, -kHalf, kHalf, 0.0f, 400.0f) *
                core::look_at(eye, target, {0.0f, 0.0f, -1.0f}),
            eye,
            cx,
            cz};
}

constexpr float kPixel = kExtent / static_cast<float>(kSize); // 0.25 m

// The pixel whose centre is world (x, z) under `view` — REQUIRED to be a pixel centre exactly.
std::array<std::uint32_t, 2> pixel_at(const TopDown& view, float x, float z) {
    const float fx = (x - (view.cx - kHalf)) / kPixel - 0.5f;
    const float fz = (z - (view.cz - kHalf)) / kPixel - 0.5f;
    REQUIRE(fx == std::floor(fx));
    REQUIRE(fz == std::floor(fz));
    REQUIRE(fx >= 0.0f);
    REQUIRE(fz >= 0.0f);
    REQUIRE(fx < static_cast<float>(kSize));
    REQUIRE(fz < static_cast<float>(kSize));
    return {static_cast<std::uint32_t>(fx), static_cast<std::uint32_t>(fz)};
}

// Draw several tiles of ONE pass into one cleared frame (render_tile, for more than one tile).
std::vector<std::uint8_t> render_tiles(rhi::Device& device,
                                       render::TerrainPass& pass,
                                       std::initializer_list<render::TerrainTileId> ids,
                                       const render::TerrainLight& light,
                                       const TopDown& view) {
    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr =
        graph.create_texture({{kSize, kSize}, render::kHdrFormat, "m19.7b-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "m19.7b-depth"});
    graph.export_texture(hdr);
    declare_clear(graph, hdr, depth);
    for (const render::TerrainTileId id : ids) {
        pass.add(graph, hdr, depth, id, view.view_proj, view.eye, light);
    }
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return read_texture(device, graph.physical(hdr), 8);
}

render::TerrainLight ambient_only_light() {
    render::TerrainLight l{};
    l.sun_irradiance = 0.0f;
    l.ambient = 1.0f;
    l.albedo = {0.0f, 0.0f, 0.0f};
    return l;
}

// A flat v2 tile at `origin` whose every weight texel is `texel`.
assets::HeightfieldAsset flat_splat_at(core::Vec3 origin, std::array<std::uint8_t, 4> texel) {
    assets::HeightfieldAsset a =
        make_splat_asset(flat_samples(), 2, 2, uniform_weights(2, 2, texel));
    a.origin = origin;
    return a;
}

std::uint16_t
half_bits(const std::vector<std::uint8_t>& img, std::uint32_t px, std::uint32_t py, int c) {
    std::uint16_t h = 0;
    std::memcpy(&h, &img[(std::size_t{py} * kSize + px) * 8 + std::size_t(c) * 2], sizeof(h));
    return h;
}

// sRGB → linear, the IEC 61966-2-1 curve Vulkan's *_SRGB formats apply to R, G and B on sample.
double srgb_decode(unsigned byte) {
    const double c = byte / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

} // namespace

TEST_CASE("m19.7b: a palette with no textures is BIT-IDENTICAL to the same draw with all-white "
          "textures") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // A varied weight map over the NON-flat fixture, the sun on: every blended quantity and every
    // shading term is live, so "identical" is not identical-because-nothing-happened.
    const std::array<std::uint8_t, 4> mix[5] = {
        {255, 0, 0, 0}, {0, 255, 0, 0}, {60, 70, 80, 45}, {10, 0, 200, 45}, {0, 0, 0, 255}};
    std::vector<std::uint8_t> weights;
    for (std::uint32_t t = 0; t < 16; ++t) {
        weights.insert(weights.end(), mix[t % 5].begin(), mix[t % 5].end());
    }
    Palette4 plain = distinct_palette();
    for (std::size_t k = 0; k < 4; ++k) {
        plain[k].metallic = 0.2f * static_cast<float>(k);
        plain[k].roughness = 0.9f - 0.2f * static_cast<float>(k);
    }

    // Two DISTINCT white textures, shared pairwise (layers 0/2 and 1/3), so the draw exercises the
    // pass's import de-duplication as well as several real bindings. RGB = 255 everywhere; A is a
    // ramp — the HEIGHT, which this brick samples and must not let into the picture.
    std::vector<std::uint8_t> white_a;
    std::vector<std::uint8_t> white_b;
    for (std::uint32_t t = 0; t < 16; ++t) {
        const std::uint8_t ramp = static_cast<std::uint8_t>(17 * t);
        white_a.insert(white_a.end(), {255, 255, 255, ramp});
        white_b.insert(white_b.end(), {255, 255, 255, static_cast<std::uint8_t>(255 - ramp)});
    }
    const rhi::TextureHandle tex_a = make_layer_texture(*device, 4, 4, white_a);
    const rhi::TextureHandle tex_b = make_layer_texture(*device, 4, 4, white_b);
    Palette4 textured = plain;
    for (std::size_t k = 0; k < 4; ++k) {
        textured[k].albedo_height = (k % 2 == 0) ? tex_a : tex_b;
        textured[k].uv_scale[0] = 1.5f + static_cast<float>(k);  // irrelevant to a white texture —
        textured[k].uv_scale[1] = 0.75f + static_cast<float>(k); // and so must not matter either
    }

    const auto draw = [&](const Palette4& palette) {
        render::TerrainPass pass(*device);
        const render::TerrainTileId id =
            pass.upload(make_splat_asset(cook_samples(), 4, 4, weights), palette);
        REQUIRE(id != render::kInvalidTerrainTile);
        auto img = render_tile(*device, pass, id, splat_light({0.0f, 0.0f, 0.0f}));
        CHECK(pass.tiles_drawn() == 1);
        CHECK(pass.splat_refused() == 0);
        return img;
    };
    const auto fallback = draw(plain);
    const auto explicit_white = draw(textured);
    REQUIRE(fallback.size() == explicit_white.size());
    CHECK(covered_pixels(fallback) > static_cast<int>(kSize * kSize / 2));
    CHECK(std::memcmp(fallback.data(), explicit_white.data(), fallback.size()) == 0);
    device->destroy(tex_b);
    device->destroy(tex_a);
}

TEST_CASE("m19.7b: the layer texture reaches the pixel with its orientation and its period") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // A 4 x 2 texture — NON-square, so a U<->V swap cannot map it onto itself. Texel (i, j): red
    // rises with i (u, world X), green with j (v, world Z), blue constant.
    constexpr std::uint32_t kW = 4;
    constexpr std::uint32_t kH = 2;
    const auto texel = [](std::uint32_t i, std::uint32_t j) -> std::array<std::uint8_t, 4> {
        return {static_cast<std::uint8_t>(40 + 60 * i),
                static_cast<std::uint8_t>(60 + 120 * j),
                128,
                0};
    };
    std::vector<std::uint8_t> real;
    for (std::uint32_t j = 0; j < kH; ++j) {
        for (std::uint32_t i = 0; i < kW; ++i) {
            const auto t = texel(i, j);
            real.insert(real.end(), t.begin(), t.end());
        }
    }
    // THE FALSIFICATION, in data: the same texels stored transposed (2 x 4, texel (j, i) at (i,
    // j)), which is exactly what a shader that swapped u and v would read out of the real texture.
    std::vector<std::uint8_t> swapped;
    for (std::uint32_t i = 0; i < kW; ++i) {
        for (std::uint32_t j = 0; j < kH; ++j) {
            const auto t = texel(i, j);
            swapped.insert(swapped.end(), t.begin(), t.end());
        }
    }

    // One texel per metre on both axes: a 4 m period along X over 4 texels, 2 m along Z over 2.
    // The tile origin is chosen so pixel centres (every 0.25 m, offset 0.125 m) land EXACTLY on
    // texel centres (world k + 0.5): -20.125 + 0.125 = -20, an integer, and likewise 12.375 +
    // 0.125. It straddles world x = 0, so negative coordinates (REPEAT of a negative uv) are probed
    // too.
    const core::Vec3 origin{-20.125f, 3.25f, 12.375f};
    const TopDown view = top_down_centred(origin.x + kHalf, origin.z + kHalf);

    const auto render_with = [&](rhi::TextureHandle tex) {
        Palette4 p = distinct_palette();
        p[1].base_color = {1.0f, 1.0f, 1.0f}; // the colour IS the texture
        p[1].albedo_height = tex;
        p[1].uv_scale[0] = 4.0f;
        p[1].uv_scale[1] = 2.0f;
        render::TerrainPass pass(*device);
        const render::TerrainTileId id = pass.upload(flat_splat_at(origin, {0, 255, 0, 0}), p);
        REQUIRE(id != render::kInvalidTerrainTile);
        auto img = render_tiles(*device, pass, {id}, ambient_only_light(), view);
        CHECK(pass.tiles_drawn() == 1);
        return img;
    };

    // Probe texel centres over SEVERAL periods on both axes: world x = 4n + i + 0.5 for
    // n = -5..2 (x from -19.5 to 11.5), z = 2m + j + 0.5 for m = 6..21 (z from 12.5 to 43.5).
    // Returns how many of the strict orderings hold, and how many were checked.
    const auto orderings = [&](const std::vector<std::uint8_t>& img) {
        int held = 0;
        int checked = 0;
        const auto red = [&](int n, std::uint32_t i, int m, std::uint32_t j) {
            const auto px = pixel_at(view, 4.0f * n + i + 0.5f, 2.0f * m + j + 0.5f);
            return chan(img, px[0], px[1], 0);
        };
        const auto green = [&](int n, std::uint32_t i, int m, std::uint32_t j) {
            const auto px = pixel_at(view, 4.0f * n + i + 0.5f, 2.0f * m + j + 0.5f);
            return chan(img, px[0], px[1], 1);
        };
        for (int n = -5; n <= 2; ++n) {
            for (int m = 6; m <= 21; ++m) {
                for (std::uint32_t j = 0; j < kH; ++j) {
                    // Red rises texel by texel along +X within a period …
                    for (std::uint32_t i = 0; i + 1 < kW; ++i) {
                        ++checked;
                        held += red(n, i, m, j) < red(n, i + 1, m, j) ? 1 : 0;
                    }
                    // … and falls back exactly ONE period later: the sawtooth restarts at 4 m.
                    if (n < 2) {
                        ++checked;
                        held += red(n, kW - 1, m, j) > red(n + 1, 0, m, j) ? 1 : 0;
                    }
                }
                for (std::uint32_t i = 0; i < kW; ++i) {
                    ++checked;
                    held += green(n, i, m, 0) < green(n, i, m, 1) ? 1 : 0; // rises along +Z
                    if (m < 21) {
                        ++checked;
                        held += green(n, i, m, 1) > green(n, i, m + 1, 0) ? 1 : 0; // 2 m period
                    }
                }
            }
        }
        return std::pair<int, int>{held, checked};
    };

    const rhi::TextureHandle tex = make_layer_texture(*device, kW, kH, real);
    const rhi::TextureHandle tex_swapped = make_layer_texture(*device, kH, kW, swapped);
    const auto img = render_with(tex);
    const auto img_swapped = render_with(tex_swapped);
    CHECK(covered_pixels(img) == static_cast<int>(kSize * kSize));

    const auto [held, checked] = orderings(img);
    MESSAGE("orientation/period orderings held: " << held << " / " << checked);
    CHECK(checked > 1000);
    CHECK(held == checked);
    const auto [held_swapped, checked_swapped] = orderings(img_swapped);
    MESSAGE("with U<->V swapped: " << held_swapped << " / " << checked_swapped);
    CHECK(held_swapped < checked_swapped / 2); // broken, and broadly — not by one probe

    device->destroy(tex_swapped);
    device->destroy(tex);
}

TEST_CASE("m19.7b: the texture coordinate is WORLD xz — the pattern runs on across a tile seam") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // Two flat tiles side by side, B's origin exactly one tile (32 m) east of A's — and 32 m is
    // NOT a whole number of the 3 m period. A tile-local coordinate would restart the pattern at
    // B's origin, shifting it by 32 mod 3 = 2 m (two of three texels) right at the seam; a world
    // coordinate does not see the seam at all.
    //
    // The claim: colour(x, z) == colour(x + 3 m, z) for EVERY probed pair, including the pairs
    // whose members lie on different tiles.
    //
    // THE BOUND, AND WHY IT IS NOT "BITS" OR "ONE f16 ULP". The two members of a pair read u and
    // u + 1 in exact arithmetic, but the GPU forms each from its own interpolated world position
    // in f32, and — the part that dominates — its texture unit QUANTISES the bilinear weight to
    // `subTexelPrecisionBits` bits of a texel, at a precision that depends on |u| (the coordinate
    // is a float, so u and u + 1 in different binades are rounded on different grids). The first
    // version of this test asserted one f16 ULP and failed on the RTX 3060: 344 of 4988 pairs
    // differed, by up to 20 ULP, every one of them a pair whose u straddled 2 or 4, and the worst
    // difference was 0.0024 = (largest texel contrast 0.631) / 256 — exactly ONE 8-bit weight step
    // (measured). So per channel the honest bound is
    //
    //     |a - b| <= 2 * 2^-s * (max - min of the decoded channel over the texels) + one f16 ULP
    //
    // — one weight step on each of the two filter axes, times the most a step can move the colour,
    // plus the target's own rounding. `s` is 4, the MINIMUM the Vulkan specification guarantees
    // (the RHI does not expose the device's value), so the bound holds on any conformant device;
    // it is far looser than this hardware needs, and the measured worst is reported beside it.
    // It still discriminates: a tile-local coordinate shifts the pattern by two whole texels at
    // the seam, a colour change of the order of the full contrast, not of a sixteenth of it.
    const core::Vec3 origin_a{-20.0f, 3.25f, 12.0f};
    const core::Vec3 origin_b{origin_a.x + kExtent, origin_a.y, origin_a.z};
    const float seam = origin_b.x; // world x = 12
    // The camera sits 0.1 m off the tile grid on purpose: with every pixel centre on a dyadic
    // coordinate (k/8 m) the f32 arithmetic happens to be exact and the pairs come out
    // bit-identical (measured), which would test only the easy case. Off the grid the positions
    // round, and the bound below is exercised.
    const TopDown view = top_down_centred(seam + 0.1f, origin_a.z + kHalf + 0.1f);

    // 3 x 2 texels, all distinct, so every 1 m step along X or Z changes the colour.
    std::vector<std::uint8_t> rgba;
    for (std::uint32_t j = 0; j < 2; ++j) {
        for (std::uint32_t i = 0; i < 3; ++i) {
            rgba.insert(rgba.end(),
                        {static_cast<std::uint8_t>(30 + 90 * i),
                         static_cast<std::uint8_t>(200 - 150 * j),
                         static_cast<std::uint8_t>(70 + 40 * i + 60 * j),
                         0});
        }
    }
    const rhi::TextureHandle tex = make_layer_texture(*device, 3, 2, rgba);
    // The bound above, per channel, from the texels themselves (base colour 1, ambient 1, so a
    // pixel's radiance is the decoded texel mix + 0.04).
    constexpr double kWeightStep = 1.0 / 16.0; // 2^-subTexelPrecisionBits at Vulkan's minimum, 4
    std::array<double, 3> bound{};
    for (int c = 0; c < 3; ++c) {
        double lo = 1.0;
        double hi = 0.0;
        for (std::size_t t = 0; t < 6; ++t) {
            lo = std::min(lo, srgb_decode(rgba[t * 4 + std::size_t(c)]));
            hi = std::max(hi, srgb_decode(rgba[t * 4 + std::size_t(c)]));
        }
        bound[std::size_t(c)] = 2.0 * kWeightStep * (hi - lo);
    }
    Palette4 p = distinct_palette();
    p[1].base_color = {1.0f, 1.0f, 1.0f};
    p[1].albedo_height = tex;
    p[1].uv_scale[0] = 3.0f;
    p[1].uv_scale[1] = 2.0f;

    render::TerrainPass pass(*device);
    const render::TerrainTileId a = pass.upload(flat_splat_at(origin_a, {0, 255, 0, 0}), p);
    const render::TerrainTileId b = pass.upload(flat_splat_at(origin_b, {0, 255, 0, 0}), p);
    REQUIRE(a != render::kInvalidTerrainTile);
    REQUIRE(b != render::kInvalidTerrainTile);
    const auto img = render_tiles(*device, pass, {a, b}, ambient_only_light(), view);
    CHECK(pass.tiles_drawn() == 2);
    CHECK(covered_pixels(img) == static_cast<int>(kSize * kSize));

    // The seam lies between pixel 63 (centre 2.5 cm west of it, on A) and pixel 64 (on B).
    constexpr std::uint32_t kPeriodPx = 12; // 3 m at 0.25 m per pixel
    const auto px_x = [&](std::uint32_t px) { return view.cx - kHalf + (px + 0.5f) * kPixel; };
    REQUIRE(px_x(63) < seam);
    REQUIRE(px_x(64) > seam);

    int pairs = 0;
    int straddling = 0;
    int within = 0;
    int one_ulp = 0;
    double worst_fraction = 0.0; // the worst |a - b| as a fraction of its bound
    for (std::uint32_t py = 0; py < kSize; py += 3) {
        for (std::uint32_t px = 0; px + kPeriodPx < kSize; ++px) {
            const bool crosses = px_x(px) < seam && px_x(px + kPeriodPx) > seam;
            bool ok = true;
            int ulps = 0;
            for (int c = 0; c < 3; ++c) {
                const std::uint16_t ha = half_bits(img, px, py, c);
                const std::uint16_t hb = half_bits(img, px + kPeriodPx, py, c);
                // va/vb, not a/b: the tile ids `a` and `b` are still in scope out here, and MSVC
                // at /W4 /WX rejects the shadowing (C4456) that GCC accepts without a word.
                const double va = half_to_float(ha);
                const double vb = half_to_float(hb);
                const double limit = bound[std::size_t(c)] + half_ulp(std::max(ha, hb));
                ok = ok && std::abs(va - vb) <= limit;
                worst_fraction = std::max(worst_fraction, std::abs(va - vb) / limit);
                // Positive halves order like their bit patterns: the integer distance is ULPs.
                ulps = std::max(ulps, std::abs(int(ha) - int(hb)));
            }
            ++pairs;
            straddling += crosses ? 1 : 0;
            within += ok ? 1 : 0;
            one_ulp += ulps <= 1 ? 1 : 0;
        }
    }
    MESSAGE("seam continuity: " << within << " / " << pairs << " pairs within the bound ("
                                << straddling << " straddle the seam); worst = " << worst_fraction
                                << " of the bound; " << one_ulp << " pairs within 1 f16 ULP");
    CHECK(straddling > 400);
    CHECK(within == pairs);

    // Vacuity: the pattern really varies within a period — a constant colour would pass the
    // equality above trivially. Neighbouring pixels a quarter-period apart must differ widely.
    int varied = 0;
    for (std::uint32_t py = 0; py < kSize; py += 3) {
        for (std::uint32_t px = 0; px + 4 < kSize; ++px) {
            varied +=
                std::abs(int(half_bits(img, px, py, 0)) - int(half_bits(img, px + 4, py, 0))) > 16
                    ? 1
                    : 0;
        }
    }
    CHECK(varied > pairs / 2);
    device->destroy(tex);
}

TEST_CASE("m19.7b: a zero, negative or non-finite uv_scale is refused and counted") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    render::TerrainPass pass(*device);
    const auto splat_asset = [] { return flat_splat_at(kOrigin, {0, 255, 0, 0}); };
    const float bad[] = {0.0f,
                         -0.0f,
                         -2.0f,
                         std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity()};
    std::uint64_t expected = 0;
    for (const float v : bad) {
        for (int axis = 0; axis < 2; ++axis) {
            // Layer 2: a slot other than 0, so the check is per layer and not "layer 0 only".
            Palette4 p = distinct_palette();
            p[2].uv_scale[axis] = v;
            CHECK(pass.upload(splat_asset(), p) == render::kInvalidTerrainTile);
            ++expected;
            CHECK(pass.splat_refused() == expected);
            CHECK(pass.tiles_refused() == expected);
        }
    }
    CHECK(expected == 10);

    // Not over-refusing: tiny and huge periods are legitimate.
    Palette4 ok = distinct_palette();
    ok[0].uv_scale[0] = 1.0e-3f;
    ok[3].uv_scale[1] = 1.0e4f;
    CHECK(pass.upload(splat_asset(), ok) != render::kInvalidTerrainTile);

    // A v1 asset ignores the palette (the m19.4 contract), bad periods included.
    Palette4 nan_palette = distinct_palette();
    nan_palette[1].uv_scale[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(pass.upload(make_asset(flat_samples()), nan_palette) != render::kInvalidTerrainTile);
    CHECK(pass.splat_refused() == expected);
    CHECK(pass.tiles_refused() == expected);
    CHECK(pass.tile_count() == 2);
}

TEST_CASE("m19.7b: a layer texture's height (A) reaches the shader LINEAR through RGBA8_SRGB") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // m19.7a's cooked fixture — a 4x4 packed albedo+height, A = round(height16 / 257) — loaded and
    // uploaded by the ENGINE's GpuAssetBridge, the path a terrain builder takes, rather than by a
    // texture this test made up. The CPU asset the bridge uploaded supplies the expected bytes.
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::GpuAssetBridge bridge(*device, server);
    const assets::TextureAssetHandle handle = bridge.request_texture(
        std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / "terrain_layer_albedo_height.rtex");
    server.wait_for_pending_loads();
    server.pump();
    REQUIRE(bridge.drain() == 1);
    const rhi::TextureHandle tex = bridge.texture_or_placeholder(handle);
    REQUIRE(tex != bridge.placeholder_texture());
    const assets::TextureAsset* cpu = server.get(handle);
    REQUIRE(cpu != nullptr);
    REQUIRE(cpu->format == assets::TextureFormat::Rgba8Srgb);
    REQUIRE(cpu->width == 4);
    REQUIRE(cpu->height == 4);
    REQUIRE(cpu->mips.size() == 3);

    // The probe: the engine's full-screen triangle over a 4x4 target, so pixel (x, y)'s centre is
    // texel (x, y)'s centre, sampled through a sampler configured exactly like TerrainPass's layer
    // sampler. Output: the f32 BITS of the sampled red and alpha.
    rhi::ShaderDesc vsd{};
    vsd.stage = rhi::ShaderStage::Vertex;
    vsd.spirv = fullscreen_vert_spv;
    vsd.spirv_size_bytes = sizeof(fullscreen_vert_spv);
    vsd.debug_name = "fullscreen.vert";
    const rhi::ShaderHandle vs = device->create_shader(vsd);
    rhi::ShaderDesc fsd{};
    fsd.stage = rhi::ShaderStage::Fragment;
    fsd.spirv = terrain_layer_probe_frag_spv;
    fsd.spirv_size_bytes = sizeof(terrain_layer_probe_frag_spv);
    fsd.debug_name = "terrain_layer_probe.frag";
    const rhi::ShaderHandle fs = device->create_shader(fsd);
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}};
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vs;
    pd.fragment_shader = fs;
    pd.color_format = rhi::Format::RG32Uint;
    pd.cull = rhi::CullMode::None;
    pd.blend = rhi::BlendMode::None;
    pd.bindings = bindings;
    pd.debug_name = "m19.7b-alpha-probe";
    const rhi::PipelineHandle pipeline = device->create_graphics_pipeline(pd);
    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Linear;
    sd.min_filter = rhi::Filter::Linear;
    sd.mip_filter = rhi::Filter::Linear;
    sd.address_mode = rhi::AddressMode::Repeat;
    sd.debug_name = "m19.7b-layer-sampler";
    const rhi::SamplerHandle sampler = device->create_sampler(sd);

    render::RenderGraph graph(*device);
    graph.reset();
    const render::RGTexture target =
        graph.create_texture({{4, 4}, rhi::Format::RG32Uint, "m19.7b-alpha-probe"});
    graph.export_texture(target);
    const render::RGColorAttachment colors[] = {
        {target, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    const render::RGTexture sampled[] = {graph.import_texture(tex, rhi::ResourceState::ShaderRead)};
    render::RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.sampled = sampled;
    graph.add_raster_pass("m19.7b-alpha-probe", desc, [&](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipeline);
        cmd.bind_texture(0, tex, sampler);
        cmd.draw(3);
    });
    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);

    rhi::BufferDesc rbd{};
    rbd.size = 4 * 4 * 8;
    rbd.usage = rhi::BufferUsage::TransferDst;
    rbd.memory = rhi::MemoryUsage::GpuToCpu;
    rbd.debug_name = "m19.7b-alpha-readback";
    const rhi::BufferHandle rb = device->create_buffer(rbd);
    auto copy = device->begin_commands();
    copy->copy_texture_to_buffer(graph.physical(target), rb);
    device->submit_blocking(*copy);
    std::vector<std::uint32_t> bits(4 * 4 * 2);
    device->read_buffer(rb, bits.data(), bits.size() * sizeof(std::uint32_t), 0);

    double worst_a = 0.0;
    double worst_r = 0.0;
    int mid_alpha = 0;
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::size_t t = (std::size_t{y} * 4 + x) * 4;
            const unsigned r_byte = std::to_integer<unsigned>(cpu->pixels[t + 0]);
            const unsigned a_byte = std::to_integer<unsigned>(cpu->pixels[t + 3]);
            float r = 0.0f;
            float a = 0.0f;
            std::memcpy(&r, &bits[(std::size_t{y} * 4 + x) * 2 + 0], sizeof(r));
            std::memcpy(&a, &bits[(std::size_t{y} * 4 + x) * 2 + 1], sizeof(a));

            // A is LINEAR: the UNORM value n/255, not the sRGB curve of it.
            worst_a = std::max(worst_a, std::abs(double(a) - a_byte / 255.0));
            CHECK(std::abs(double(a) - a_byte / 255.0) <= 1.0e-5);
            // The witness that this IS an sRGB texture — R, in the same fetch, is decoded. Without
            // it, "A is linear" could just mean "the texture was uploaded as UNORM".
            worst_r = std::max(worst_r, std::abs(double(r) - srgb_decode(r_byte)));
            CHECK(std::abs(double(r) - srgb_decode(r_byte)) <= 2.0e-3);
            if (r_byte >= 32) {
                CHECK(std::abs(double(r) - r_byte / 255.0) > 0.01);
            }
            // Where the two curves are far apart, A sits on the linear one and FAR from the sRGB
            // one.
            if (a_byte >= 64 && a_byte <= 200) {
                ++mid_alpha;
                CHECK(std::abs(double(a) - srgb_decode(a_byte)) > 0.05);
            }
            if (a_byte == 128) {
                CHECK(a > 0.5f); // 128/255 = 0.502; sRGB-decoded it would be 0.216
                CHECK(a < 0.505f);
            }
        }
    }
    MESSAGE("alpha: worst |A - n/255| = " << worst_a << "; red: worst |R - srgb(n)| = " << worst_r);
    CHECK(mid_alpha == 5); // 78, 127, 128, 156, 195 — the probe is not vacuous

    device->destroy(rb);
    device->destroy(sampler);
    device->destroy(pipeline);
    device->destroy(fs);
    device->destroy(vs);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════
// m19.7c — HEIGHT-BLENDED SPLAT TRANSITIONS (ADR-0066 §5 and its m19.7c addendum).
//
// terrain.frag's height_blend() turns the painted weights w into b_k = w_k g_k / sum(w_j g_j),
// g_k = exp2(c_k (h_k - h_max)), and every case below draws through TerrainPass — the shader's
// one copy of that function IS the thing under test; no CPU re-implementation stands in for it.
// The claims are bit identities between draws of ONE program, strict orderings between such
// draws, and counters. No golden image, no colour margin.
//
// All scenes are the m19.7b one: a FLAT tile, the sun off, ambient 1 — so a pixel's colour is
// `base + 0.04` and a statement about the blend alone.
// ═════════════════════════════════════════════════════════════════════════════════════════════
namespace {

// A w x h layer texture of ONE texel value: albedo `rgb` (sRGB bytes), height `a`.
rhi::TextureHandle solid_layer(rhi::Device& device,
                               std::uint32_t w,
                               std::uint32_t h,
                               std::array<std::uint8_t, 3> rgb,
                               std::uint8_t a) {
    std::vector<std::uint8_t> px;
    for (std::uint32_t t = 0; t < w * h; ++t) {
        px.insert(px.end(), {rgb[0], rgb[1], rgb[2], a});
    }
    return make_layer_texture(device, w, h, px);
}

// The m19.7c tile: flat, 32 m, its corner at world (0, ., 0), so pixel px's centre is world
// x = (px + 0.5) * 0.25 m (and likewise z).
constexpr core::Vec3 kBlendOrigin{0.0f, 3.25f, 0.0f};

std::vector<std::uint8_t> draw_blend(rhi::Device& device,
                                     std::uint32_t wc,
                                     std::uint32_t wr,
                                     std::vector<std::uint8_t> weights,
                                     const Palette4& palette) {
    assets::HeightfieldAsset asset = make_splat_asset(flat_samples(), wc, wr, std::move(weights));
    asset.origin = kBlendOrigin;
    render::TerrainPass pass(device);
    const render::TerrainTileId id = pass.upload(asset, palette);
    REQUIRE(id != render::kInvalidTerrainTile);
    auto img = render_tiles(
        device, pass, {id}, ambient_only_light(), top_down_centred(kHalf, kHalf));
    CHECK(pass.tiles_drawn() == 1);
    CHECK(pass.splat_refused() == 0);
    return img;
}

bool same_image(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

TEST_CASE("m19.7c: equal heights with contrast are BIT-IDENTICAL to contrast 0, and to m19.7b's "
          "blend") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // ── WHY THE COLOURS ARE ABSURD ────────────────────────────────────────────────────────────
    //
    // What the bypass protects is ONE f32 ULP: without it b = w / sum(w), and sum(w) is 1.0 only
    // to rounding. The frame is f16, whose step is ~8000 f32 steps wide, so an ordinary scene
    // rounds that ULP away at all but a stray pixel and the anchor could not see the bypass go.
    // So the scene AMPLIFIES it. Layer 1 is painted at exactly 127/255 everywhere, and
    //     c0 = -(127/255) K,   c1 = c0 + K      =>   base = c0 + b1 (c1 - c0) = K (b1 - 127/255),
    // a difference of two nearly equal numbers: with K ~ 2^20 the result is a few hundredths made
    // ENTIRELY of the rounding in b1, and one ULP of b1 moves it by about its own size. (A base
    // colour need only be finite.) Bit-identical inputs still give bit-identical pixels — one
    // program, one device — so memcmp stays the bar; the scene only makes it a sharp one.
    const core::Vec3 k{1048576.0f, 786432.0f, 1572864.0f};
    const float f = 127.0f / 255.0f;
    Palette4 pal{};
    for (std::size_t n = 0; n < 4; ++n) {
        pal[n].base_color = {-f * k.x, -f * k.y, -f * k.z}; // layers 0, 2 and 3 are ONE material
    }
    pal[1].base_color = {pal[0].base_color.x + k.x,
                         pal[0].base_color.y + k.y,
                         pal[0].base_color.z + k.z};

    // Every layer's height is the SAME constant (128), from textures of two different sizes at
    // four different periods — "equal heights" as a painter would meet it, not one shared texel.
    const rhi::TextureHandle flat_a = solid_layer(*device, 4, 4, {255, 255, 255}, 128);
    const rhi::TextureHandle flat_b = solid_layer(*device, 2, 8, {255, 255, 255}, 128);
    const rhi::TextureHandle tall = solid_layer(*device, 4, 4, {255, 255, 255}, 200);
    for (std::size_t n = 0; n < 4; ++n) {
        pal[n].albedo_height = (n % 2 == 0) ? flat_a : flat_b;
        pal[n].uv_scale[0] = 1.5f + static_cast<float>(n);
        pal[n].uv_scale[1] = 0.75f + static_cast<float>(n);
    }
    Palette4 contrasty = pal;
    const float contrast[4] = {8.0f, 3.0f, 0.5f, 20.0f};
    for (std::size_t n = 0; n < 4; ++n) {
        contrasty[n].height_contrast = contrast[n];
    }

    // Two weight maps with the SAME layer-1 channel (127 in every texel). `split` shares the other
    // 128 among layers 0, 2 and 3 differently in each texel; `plain` gives it all to layer 0.
    std::vector<std::uint8_t> split;
    for (std::uint32_t t = 0; t < 16; ++t) {
        const std::uint32_t a = (t * 37) % 129;
        const std::uint32_t b = (t * 11) % (128 - a + 1);
        split.insert(split.end(),
                     {static_cast<std::uint8_t>(a),
                      127,
                      static_cast<std::uint8_t>(b),
                      static_cast<std::uint8_t>(128 - a - b)});
    }
    const auto plain = uniform_weights(4, 4, {128, 127, 0, 0});

    const auto zero = draw_blend(*device, 4, 4, split, pal);
    const auto with_contrast = draw_blend(*device, 4, 4, split, contrasty);
    const auto zero_plain = draw_blend(*device, 4, 4, plain, pal);

    // (1) Equal heights: contrast changes NOTHING, to the bit.
    CHECK(same_image(zero, with_contrast));
    // (2) …and what both equal is m19.7b's blend. m19.7b never read w0 — its picture was a
    // function of (w1, w2, w3) alone, and here layers 2 and 3 ARE layer 0, so of w1 alone. The two
    // maps agree on w1 and on nothing else, so they must draw the same bits. The renormalised form
    // cannot do that: it divides by sum(w), whose rounding depends on how the 128 is split. This
    // is the leg that goes red when the bypass is removed OUTRIGHT; leg (1) cannot, because then
    // both of its draws take the same renormalised path (it catches losing the equal-heights
    // clause alone).
    CHECK(same_image(zero, zero_plain));

    // Witnesses that the identity is not vacuous: the scene is drawn, and in this very scene the
    // contrast and the heights ARE live — raise layer 1's height and the picture changes.
    int nonzero = 0;
    for (std::uint32_t py = 0; py < kSize; ++py) {
        for (std::uint32_t px = 0; px < kSize; ++px) {
            nonzero += half_bits(zero, px, py, 0) != 0 ? 1 : 0;
        }
    }
    CHECK(nonzero == static_cast<int>(kSize * kSize)); // 0.04 + base; the clear is 0
    Palette4 unequal = contrasty;
    unequal[1].albedo_height = tall;
    CHECK_FALSE(same_image(zero, draw_blend(*device, 4, 4, split, unequal)));

    device->destroy(tall);
    device->destroy(flat_b);
    device->destroy(flat_a);
}

TEST_CASE("m19.7c: where a layer's height is high it takes more of a 50/50 texel, where low, "
          "less") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // Layer 1's height map over the 32 m tile, one period: 8 texels along X, each 4 m — the first
    // four HIGH (255), the last four LOW (0). Layer 0 sits at mid height (128) everywhere. Texel
    // centres are at x = 2, 6, 10, 14 | 18, 22, 26, 30, and a linear filter between two EQUAL
    // texels returns that value, so h1 is exactly 1 for x in [2, 14] and exactly 0 in [18, 30].
    // The probed columns stay a metre inside those spans: x in [3, 13] and [19, 29].
    const auto halves = [&](std::uint8_t first, std::uint8_t second) {
        std::vector<std::uint8_t> px;
        for (std::uint32_t i = 0; i < 8; ++i) {
            px.insert(px.end(), {255, 255, 255, i < 4 ? first : second});
        }
        return make_layer_texture(*device, 8, 1, px);
    };
    const rhi::TextureHandle high_low = halves(255, 0);
    const rhi::TextureHandle low_high = halves(0, 255); // the falsification, in data
    const rhi::TextureHandle mid = solid_layer(*device, 2, 2, {255, 255, 255}, 128);

    const auto palette = [&](rhi::TextureHandle layer1, float contrast) {
        Palette4 p = distinct_palette(); // layer 0 red-orange, layer 1 cyan: far apart on R, G, B
        p[0].albedo_height = mid;
        p[1].albedo_height = layer1;
        p[1].uv_scale[0] = kExtent;
        p[1].uv_scale[1] = kExtent;
        p[0].height_contrast = contrast;
        p[1].height_contrast = contrast;
        return p;
    };
    const auto half_half = uniform_weights(2, 2, {128, 127, 0, 0});
    const auto pure0 = draw_blend(*device, 2, 2, uniform_weights(2, 2, {255, 0, 0, 0}),
                                  palette(high_low, 4.0f));
    const auto pure1 = draw_blend(*device, 2, 2, uniform_weights(2, 2, {0, 255, 0, 0}),
                                  palette(high_low, 4.0f));
    const auto unblended = draw_blend(*device, 2, 2, half_half, palette(high_low, 0.0f));
    const auto blended = draw_blend(*device, 2, 2, half_half, palette(high_low, 4.0f));
    const auto blended_swapped = draw_blend(*device, 2, 2, half_half, palette(low_high, 4.0f));

    // "Moves strictly toward layer k": strictly between the plain 50/50 render and layer k's pure
    // render, on every channel. Returns {held, checked}.
    const auto toward = [&](const std::vector<std::uint8_t>& img) {
        int held = 0;
        int checked = 0;
        for (std::uint32_t py = 4; py < kSize - 4; ++py) {
            for (std::uint32_t px = 12; px <= 115; ++px) {
                const bool high_half = px <= 51;       // x in [3.125, 12.875]
                if (!high_half && px < 76) {
                    continue; // the filtered step between the halves, and its margins
                }
                const auto& goal = high_half ? pure1 : pure0;
                for (int c = 0; c < 3; ++c) {
                    const float from = chan(unblended, px, py, c);
                    const float to = chan(goal, px, py, c);
                    const float v = chan(img, px, py, c);
                    ++checked;
                    held += (v > std::min(from, to) && v < std::max(from, to)) ? 1 : 0;
                }
            }
        }
        return std::pair<int, int>{held, checked};
    };
    const auto [held, checked] = toward(blended);
    MESSAGE("redistribution orderings held: " << held << " / " << checked);
    CHECK(checked > 20000);
    CHECK(held == checked);
    // The same heights mirrored move every probe the OTHER way: none may hold.
    const auto [held_swapped, checked_swapped] = toward(blended_swapped);
    MESSAGE("with the height map's halves swapped: " << held_swapped << " / " << checked_swapped);
    CHECK(held_swapped == 0);

    device->destroy(mid);
    device->destroy(low_high);
    device->destroy(high_low);
}

TEST_CASE("m19.7c: a layer painted at weight 0 has no influence, however high it stands") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    // Layers 0, 1 and 3 are painted (varied, 4x4), with varied height maps and UNEQUAL contrasts;
    // layer 2's weight byte is 0 in every texel.
    std::vector<std::uint8_t> weights;
    std::vector<std::uint8_t> painted2; // the same map with layer 2 painted in — the witness
    std::vector<std::uint8_t> ha;
    std::vector<std::uint8_t> hb;
    std::vector<std::uint8_t> hc;
    for (std::uint32_t t = 0; t < 16; ++t) {
        const std::uint32_t w1 = 20 + 13 * t;
        const std::uint32_t w3 = (t % 3) * 20;
        const std::uint32_t w0 = 255 - w1 - w3;
        weights.insert(weights.end(),
                       {static_cast<std::uint8_t>(w0),
                        static_cast<std::uint8_t>(w1),
                        0,
                        static_cast<std::uint8_t>(w3)});
        const std::uint32_t w2 = std::min<std::uint32_t>(w0, 40);
        painted2.insert(painted2.end(),
                        {static_cast<std::uint8_t>(w0 - w2),
                         static_cast<std::uint8_t>(w1),
                         static_cast<std::uint8_t>(w2),
                         static_cast<std::uint8_t>(w3)});
        ha.insert(ha.end(), {255, 255, 255, static_cast<std::uint8_t>(40 + (t * 53) % 160)});
        hb.insert(hb.end(), {255, 255, 255, static_cast<std::uint8_t>((t * 91) % 256)});
        hc.insert(hc.end(), {255, 255, 255, static_cast<std::uint8_t>(240 - 15 * t)});
    }
    const rhi::TextureHandle tex_a = make_layer_texture(*device, 4, 4, ha);
    const rhi::TextureHandle tex_b = make_layer_texture(*device, 4, 4, hb);
    const rhi::TextureHandle tex_c = make_layer_texture(*device, 4, 4, hc);
    // Layer 2, two ways. LOUD: the highest height there is, magenta, a wild base colour, metal,
    // smooth, a huge contrast. QUIET: height 0, green, another wild colour, contrast 0.
    const rhi::TextureHandle loud = solid_layer(*device, 4, 4, {255, 0, 255}, 255);
    const rhi::TextureHandle quiet = solid_layer(*device, 2, 2, {0, 255, 0}, 0);

    Palette4 base = distinct_palette();
    base[0].albedo_height = tex_a;
    base[1].albedo_height = tex_b;
    base[3].albedo_height = tex_c;
    base[0].uv_scale[0] = base[0].uv_scale[1] = 5.0f;
    base[1].uv_scale[0] = base[1].uv_scale[1] = 7.0f;
    base[3].uv_scale[0] = base[3].uv_scale[1] = 3.0f;
    base[0].height_contrast = 2.0f;
    base[1].height_contrast = 6.0f;
    base[3].height_contrast = 3.0f;

    Palette4 with_loud = base;
    with_loud[2].albedo_height = loud;
    with_loud[2].base_color = {50.0f, 0.0f, 50.0f};
    with_loud[2].metallic = 1.0f;
    with_loud[2].roughness = 0.1f;
    with_loud[2].height_contrast = 30.0f;
    with_loud[2].uv_scale[0] = with_loud[2].uv_scale[1] = 2.5f;
    Palette4 with_quiet = base;
    with_quiet[2].albedo_height = quiet;
    with_quiet[2].base_color = {0.0f, 9.0f, 0.0f};
    with_quiet[2].uv_scale[0] = with_quiet[2].uv_scale[1] = 9.0f;

    // ── WHY THIS COMPARISON IS EXACT ──────────────────────────────────────────────────────────
    //
    // Every weight texel's layer-2 byte is 0, and a linear filter over zeros returns exactly 0, so
    // w2 == 0.0 at every pixel. Then layer 2 (a) is not among the painted layers, so its height
    // and contrast enter neither h_max nor the bypass decision; (b) contributes q2 = 0 * g2 = 0 to
    // the sum, and x + 0 is x exactly; (c) gets b2 = 0 / sum = 0; and (d) adds b2 * (c2 - c0) =
    // 0 * (finite) = 0 to the blend, again exactly — the same for metallic and roughness. Nothing
    // of layer 2 reaches the pixel through anything but a multiplication by exact zero, in one
    // program, so the three draws must agree bit for bit. No margin is needed and none is given.
    const auto a = draw_blend(*device, 4, 4, weights, with_loud);
    const auto b = draw_blend(*device, 4, 4, weights, with_quiet);
    const auto c = draw_blend(*device, 4, 4, weights, base); // layer 2 untextured, contrast 0
    CHECK(same_image(a, b));
    CHECK(same_image(a, c));

    // Witnesses. The blend is LIVE in this scene (not the bypass): dropping the contrasts changes
    // the picture. And layer 2's change is not invisible in general: once it is painted, loud and
    // quiet differ.
    Palette4 no_contrast = with_loud;
    for (auto& l : no_contrast) {
        l.height_contrast = 0.0f;
    }
    CHECK_FALSE(same_image(a, draw_blend(*device, 4, 4, weights, no_contrast)));
    CHECK_FALSE(same_image(draw_blend(*device, 4, 4, painted2, with_loud),
                           draw_blend(*device, 4, 4, painted2, with_quiet)));

    device->destroy(quiet);
    device->destroy(loud);
    device->destroy(tex_c);
    device->destroy(tex_b);
    device->destroy(tex_a);
}

TEST_CASE("m19.7c: at fixed heights, more painted weight is monotonically more of the layer") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    const rhi::TextureHandle at_200 = solid_layer(*device, 2, 2, {255, 255, 255}, 200);
    const rhi::TextureHandle at_60 = solid_layer(*device, 4, 4, {255, 255, 255}, 60);
    constexpr int kSteps = 9;
    const std::uint8_t paint[kSteps] = {0, 32, 64, 96, 128, 160, 192, 224, 255};

    // Two height arrangements, so the claim is not an accident of which layer is on top: layer 1
    // BELOW layer 0 (the contrast works against it) and ABOVE it (the contrast works for it).
    std::vector<std::uint8_t> mid[2];
    for (int arrangement = 0; arrangement < 2; ++arrangement) {
        Palette4 p = distinct_palette();
        p[0].albedo_height = arrangement == 0 ? at_200 : at_60;
        p[1].albedo_height = arrangement == 0 ? at_60 : at_200;
        p[0].height_contrast = 6.0f;
        p[1].height_contrast = 6.0f;
        std::vector<std::uint8_t> img[kSteps];
        for (int s = 0; s < kSteps; ++s) {
            const std::uint8_t n = paint[s];
            img[s] = draw_blend(*device,
                                2,
                                2,
                                uniform_weights(2, 2, {static_cast<std::uint8_t>(255 - n), n, 0, 0}),
                                p);
        }
        mid[arrangement] = img[4];
        // img[0] is pure layer 0 and img[8] pure layer 1. Every step in between must move every
        // channel STRICTLY in the direction of layer 1 — a strict chain from one to the other.
        int checked = 0;
        int held = 0;
        for (std::uint32_t py = 8; py < kSize; py += 8) {
            for (std::uint32_t px = 8; px < kSize; px += 8) {
                for (int c = 0; c < 3; ++c) {
                    const float from = chan(img[0], px, py, c);
                    const float to = chan(img[kSteps - 1], px, py, c);
                    REQUIRE(from != to);
                    for (int s = 0; s + 1 < kSteps; ++s) {
                        const float lo = chan(img[s], px, py, c);
                        const float hi = chan(img[s + 1], px, py, c);
                        ++checked;
                        held += (to > from ? hi > lo : hi < lo) ? 1 : 0;
                    }
                }
            }
        }
        MESSAGE("arrangement " << arrangement << ": strict steps held " << held << " / "
                               << checked);
        CHECK(checked == 15 * 15 * 3 * (kSteps - 1));
        CHECK(held == checked);
    }
    // The witness that height is doing something here: at the SAME painted 128/255, layer 1 shows
    // more when it is the higher layer than when it is the lower one.
    const Palette4 ref = distinct_palette();
    const auto pure1 = draw_blend(*device, 2, 2, uniform_weights(2, 2, {0, 255, 0, 0}), ref);
    for (int c = 0; c < 3; ++c) {
        CHECK(std::fabs(chan(mid[1], 64, 64, c) - chan(pure1, 64, 64, c)) <
              std::fabs(chan(mid[0], 64, 64, c) - chan(pure1, 64, 64, c)));
    }
    device->destroy(at_60);
    device->destroy(at_200);
}

TEST_CASE("m19.7c: a negative or non-finite height_contrast is refused and counted") {
    auto device = splat_device();
    if (!device) {
        return;
    }
    render::TerrainPass pass(*device);
    const auto splat_asset = [] { return flat_splat_at(kOrigin, {0, 255, 0, 0}); };
    const float bad[] = {-1.0f,
                         -1.0e-6f,
                         std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity()};
    std::uint64_t expected = 0;
    for (const float v : bad) {
        for (std::size_t layer = 0; layer < 4; ++layer) { // every slot is checked, not just one
            Palette4 p = distinct_palette();
            p[layer].height_contrast = v;
            CHECK(pass.upload(splat_asset(), p) == render::kInvalidTerrainTile);
            ++expected;
            CHECK(pass.splat_refused() == expected);
            CHECK(pass.tiles_refused() == expected);
        }
    }
    CHECK(expected == 20);

    // Not over-refusing: 0 (the default, "no height blending") and a very large contrast are
    // legitimate — a large one is simply a hard edge between painted layers.
    Palette4 ok = distinct_palette();
    ok[0].height_contrast = 0.0f;
    ok[3].height_contrast = 1.0e6f;
    CHECK(pass.upload(splat_asset(), ok) != render::kInvalidTerrainTile);

    // A v1 asset ignores the palette (the m19.4 contract), a bad contrast included.
    Palette4 nan_palette = distinct_palette();
    nan_palette[1].height_contrast = std::numeric_limits<float>::quiet_NaN();
    CHECK(pass.upload(make_asset(flat_samples()), nan_palette) != render::kInvalidTerrainTile);
    CHECK(pass.splat_refused() == expected);
    CHECK(pass.tiles_refused() == expected);
    CHECK(pass.tile_count() == 2);
}
