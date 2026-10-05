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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "rime/assets/heightfield_asset.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/physics/physics.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/terrain_pass.hpp"
#include "rime/rhi/device.hpp"
#include "terrain.vert.spv.h"
#include "terrain_height_probe.frag.spv.h"

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

core::Mat4 top_down_view_proj() {
    const core::Vec3 eye{kOrigin.x + kHalf, kOrigin.y + 200.0f, kOrigin.z + kHalf};
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

    const render::TerrainPush push = render::terrain_push(tile, view_proj, {});
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
        pass.add(gate, hdr, depth, render::kInvalidTerrainTile, core::Mat4{}, {});
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

    pass.add(graph, hdr, depth, tile_id, view_proj, light);
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
    // expected radiance at a probe is computable exactly: albedo * (E * max(n·l, 0) / π +
    // ambient). Checking against that — rather than against a stored image — is what makes this a
    // structural proof: it would fail for a smooth (central-difference) normal, for a dropped 1/π,
    // and for a light pointing the wrong way.
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
            const float n_dot_l = std::max(core::dot(n, to_light), 0.0f);
            const float irradiance =
                light.sun_irradiance * n_dot_l * 0.31830988618f + light.ambient;
            const float expect_r = light.albedo.x * irradiance;
            const float expect_g = light.albedo.y * irradiance;

            // Tolerance: RGBA16Float carries a 10-bit mantissa, so one stored step is ~1e-3
            // relative; 1% leaves room for that plus the f32 normalise on each side. Relative,
            // because the quantity spans an order of magnitude across the tile.
            worst_rel = std::max(worst_rel, std::fabs(r - expect_r) / expect_r);
            CHECK(r == doctest::Approx(expect_r).epsilon(0.01));
            CHECK(g == doctest::Approx(expect_g).epsilon(0.01));
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
