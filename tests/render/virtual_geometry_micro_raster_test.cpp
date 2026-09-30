// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for M18.4, the software half of the hybrid micro-triangle rasterizer (ADR-0043 gate 6,
// ADR-0058), on a real Vulkan device. Structural, never golden: every expectation is computed by
// the CPU oracle (rasterize_micro_triangle_reference) from the same inputs.
//
//   (a) PARITY. The GPU software rasterizer equals the CPU oracle BIT FOR BIT — identity and depth
//       bits, every pixel — on hand-built edge cases (shared edges through pixel centres, exact
//       depth ties, degenerate and clipped triangles) and on 16000 seeded random sub-pixel
//       triangles with dense overlap. Both atomic paths (64-bit single pass, 32-bit two-pass) must
//       agree.
//   (b) BOUNDARY. A mesh split across the hardware pass and the software pass: the two coverage
//       sets are disjoint (no double writes on shared edges), their union is exactly the oracle's
//       coverage of the whole mesh (no cracks), and the hybrid frame names the oracle's winner at
//       every pixel — including where two layers overlap and the depth test decides.
//   (c) SILHOUETTE. A wire made only of sub-pixel triangles: the hardware-routed half of the
//       frame alone covers nothing of it; the hybrid frame covers exactly what the oracle covers.
//
// Coordinates in (b) and (c) sit on a quarter-pixel lattice, so edges pass EXACTLY through pixel
// centres (the tie rule is exercised, not avoided) and every edge function is exact in float on
// both rasterizers — a disagreement is a rule disagreement, never a rounding one.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/render/micro_triangle.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_micro_raster_pass.hpp"
#include "rime/render/virtual_geometry_residency.hpp"
#include "rime/render/virtual_geometry_selection.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"
#include "rime/render/virtual_geometry_visibility_pass.hpp"
#include "rime/rhi/device.hpp"

namespace {

using namespace rime;
using namespace rime::render;

constexpr std::uint32_t kSize = 64;
constexpr assets::AssetId kAssetId{77};
constexpr std::uint32_t kMaxClusterTriangles = 128;

std::unique_ptr<rhi::Device> make_device_or_skip() {
    auto device = rhi::create_device({});
    if (!device && std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
        FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
    }
    if (!device) {
        MESSAGE("no Vulkan device available — skipping micro-raster proofs");
    }
    return device;
}

VirtualGeometryVisibilityWords sw_id(std::uint32_t i) {
    // A slot range the hardware fixture never uses, so a pixel's owner is readable from its ID.
    return *pack_virtual_geometry_visibility_id64(
        {1000u + i / kMaxClusterTriangles, 5u, 3u, i % kMaxClusterTriangles});
}

VirtualGeometryVisibilityWords hw_id(std::uint32_t cluster, std::uint32_t triangle) {
    return *pack_virtual_geometry_visibility_id64({cluster + 1u, 2u, 3u, triangle});
}

// ── The hardware half: a virtual-geometry asset holding arbitrary screen-space triangles ─────────
//
// One page per cluster (vertices then indices, as the visibility-pass test lays them out), all
// clusters in one permanent coarse leaf group, drawn with an identity MVP. A framebuffer position
// p maps to NDC p / 32 - 1, exact on the quarter-pixel lattice; Vulkan's viewport maps it back.
struct HardwareMesh {
    assets::VirtualGeometryAsset asset;
    std::vector<VirtualGeometryClusterDraw> draws;
};

HardwareMesh make_hardware_mesh(const std::vector<ProjectedTriangle>& tris) {
    HardwareMesh mesh;
    assets::VirtualGeometryAsset& a = mesh.asset;
    a.source_mesh = assets::AssetId{1};
    a.attribs = assets::kMeshV1Attribs;
    a.vertex_stride = assets::expected_vertex_stride(a.attribs);
    const std::uint32_t stride = a.vertex_stride;
    const auto cluster_count =
        static_cast<std::uint32_t>((tris.size() + kMaxClusterTriangles - 1) / kMaxClusterTriangles);
    for (std::uint32_t c = 0; c < cluster_count; ++c) {
        const std::size_t first = std::size_t{c} * kMaxClusterTriangles;
        const std::size_t count = std::min<std::size_t>(kMaxClusterTriangles, tris.size() - first);
        const auto vertex_count = static_cast<std::uint32_t>(count * 3);
        std::vector<std::byte> page(std::size_t{vertex_count} * stride + vertex_count * 4u);
        assets::Aabb bounds{{1e9f, 1e9f, 1e9f}, {-1e9f, -1e9f, -1e9f}};
        for (std::size_t t = 0; t < count; ++t) {
            const ProjectedTriangle& p = tris[first + t];
            const float xs[3] = {p.x0, p.x1, p.x2};
            const float ys[3] = {p.y0, p.y1, p.y2};
            const float zs[3] = {p.z0, p.z1, p.z2};
            for (std::uint32_t k = 0; k < 3; ++k) {
                const float pos[3] = {xs[k] / 32.0f - 1.0f, ys[k] / 32.0f - 1.0f, zs[k]};
                const std::size_t v = t * 3 + k;
                std::memcpy(page.data() + v * stride, pos, sizeof(pos));
                const auto index = static_cast<std::uint32_t>(v);
                std::memcpy(page.data() + std::size_t{vertex_count} * stride + v * 4u, &index, 4);
                bounds.min = {std::min(bounds.min.x, pos[0]),
                              std::min(bounds.min.y, pos[1]),
                              std::min(bounds.min.z, pos[2])};
                bounds.max = {std::max(bounds.max.x, pos[0]),
                              std::max(bounds.max.y, pos[1]),
                              std::max(bounds.max.z, pos[2])};
            }
        }
        const auto offset = static_cast<std::uint64_t>(a.page_bytes.size());
        a.page_bytes.insert(a.page_bytes.end(), page.begin(), page.end());
        a.pages.push_back({offset, static_cast<std::uint32_t>(page.size()), c, 1, 0, 0, true});
        assets::VirtualGeometryCluster cluster{};
        cluster.bounds = bounds;
        cluster.page = c;
        cluster.vertex_count = vertex_count;
        cluster.index_count = vertex_count;
        cluster.replacement_group = 0;
        a.clusters.push_back(cluster);
        mesh.draws.push_back({c, c + 1u, 2u});
    }
    a.groups = {{0, cluster_count, 0, 0, 0.0f, true}};
    a.coarse_group = 0;
    return mesh;
}

// ── Frame readback ─────────────────────────────────────────────────────────────────────────────

struct Frame {
    std::vector<VirtualGeometryVisibilityWords> ids;
    std::vector<std::uint32_t> depth_bits;
    VirtualGeometryMicroRasterCounters counters{};
    VirtualGeometryMicroRasterStats stats{};
    VirtualGeometryVisibilityStats hw_stats{};
};

// Render one frame: the hardware pass (clears, then draws `hw` if given), then — when `sw` is
// non-empty — the software pass on top. Exactly the order a real frame declares them in.
Frame render_frame(rhi::Device& device,
                   const HardwareMesh* hw,
                   const std::vector<VirtualGeometryMicroTriangle>& sw,
                   MicroRasterAtomics atomics,
                   bool cull_back_faces) {
    RenderGraph graph(device);
    const RGTexture ids = graph.create_texture({{kSize, kSize}, rhi::Format::RG32Uint, "ids"});
    const RGTexture depth_bits =
        graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "depth-bits"});
    const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "depth"});
    graph.export_texture(ids);
    graph.export_texture(depth_bits);

    VirtualGeometryVisibilityPass hw_pass(device);
    VirtualGeometryResidency residency;
    VirtualGeometrySelection selection;
    VirtualGeometryVisibilityRequest hw_request{};
    if (hw != nullptr) {
        REQUIRE(assets::validate_virtual_geometry(hw->asset) == assets::VirtualGeometryError::None);
        REQUIRE(residency.register_asset(kAssetId, hw->asset));
        selection.groups = {0};
        hw_request.asset = &hw->asset;
        hw_request.asset_id = kAssetId;
        hw_request.residency = &residency;
        hw_request.selection = &selection;
        hw_request.clusters = hw->draws;
        hw_request.clip_from_object = core::identity();
    }
    (void)hw_pass.declare(graph, ids, depth_bits, depth, hw_request);

    VirtualGeometryMicroRasterPass sw_pass(device);
    VirtualGeometryMicroRasterRequest sw_request{};
    sw_request.triangles = sw;
    sw_request.width = kSize;
    sw_request.height = kSize;
    sw_request.cull_back_faces = cull_back_faces;
    sw_request.atomics = atomics;
    (void)sw_pass.declare(graph, ids, depth_bits, depth, sw_request);

    rhi::BufferDesc bd{};
    bd.size = kSize * kSize * sizeof(std::uint32_t);
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle depth_buffer = device.create_buffer(bd);
    rhi::BufferDesc ibd = bd;
    ibd.size = kSize * kSize * sizeof(VirtualGeometryVisibilityWords);
    const rhi::BufferHandle id_buffer = device.create_buffer(ibd);

    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    cmd->copy_texture_to_buffer(graph.physical(ids), id_buffer);
    cmd->copy_texture_to_buffer(graph.physical(depth_bits), depth_buffer);
    device.submit_blocking(*cmd);

    Frame out;
    out.ids.resize(kSize * kSize);
    out.depth_bits.resize(kSize * kSize);
    device.read_buffer(id_buffer, out.ids.data(), ibd.size);
    device.read_buffer(depth_buffer, out.depth_bits.data(), bd.size);
    if (sw_pass.counters().is_valid()) {
        device.read_buffer(sw_pass.counters(), &out.counters, sizeof(out.counters));
    }
    out.stats = sw_pass.stats();
    out.hw_stats = hw_pass.stats();
    device.destroy(id_buffer);
    device.destroy(depth_buffer);
    return out;
}

// ── The oracle side ───────────────────────────────────────────────────────────────────────────

struct Oracle {
    std::vector<std::uint32_t> owner; // 0 = empty, else 1 + index into the triangle list
    std::vector<float> depth;
};

Oracle run_oracle(const std::vector<ProjectedTriangle>& tris) {
    Oracle o;
    o.owner.assign(kSize * kSize, 0u);
    o.depth.assign(kSize * kSize, 1.0f); // the hardware pass's depth clear
    for (std::size_t i = 0; i < tris.size(); ++i) {
        (void)rasterize_micro_triangle_reference(
            tris[i], static_cast<std::uint32_t>(i + 1), {o.owner, o.depth, kSize, kSize});
    }
    return o;
}

bool covered(const VirtualGeometryVisibilityWords& w) {
    return !(w == kInvalidVirtualGeometryVisibilityWords);
}

std::vector<VirtualGeometryMicroTriangle> as_software(const std::vector<ProjectedTriangle>& tris) {
    std::vector<VirtualGeometryMicroTriangle> out;
    for (std::size_t i = 0; i < tris.size(); ++i) {
        out.push_back({tris[i], sw_id(static_cast<std::uint32_t>(i)), 0u});
    }
    return out;
}

// Pixel-by-pixel bit equality of a software-only frame against the oracle. Returns mismatches.
std::size_t parity_mismatches(const Frame& f, const Oracle& o) {
    std::size_t bad = 0;
    for (std::size_t p = 0; p < o.owner.size(); ++p) {
        if (o.owner[p] == 0u) {
            bad += (covered(f.ids[p]) || f.depth_bits[p] != 0u) ? 1u : 0u;
            continue;
        }
        const bool same_id = f.ids[p] == sw_id(o.owner[p] - 1u);
        const bool same_depth = f.depth_bits[p] == std::bit_cast<std::uint32_t>(o.depth[p]);
        if (!same_id || !same_depth) {
            if (bad < 8) {
                MESSAGE("pixel (",
                        p % kSize,
                        ",",
                        p / kSize,
                        ") oracle owner ",
                        o.owner[p] - 1u,
                        " depth ",
                        o.depth[p],
                        " vs gpu id.lo ",
                        f.ids[p].lo,
                        " depth bits ",
                        f.depth_bits[p],
                        " (oracle bits ",
                        std::bit_cast<std::uint32_t>(o.depth[p]),
                        ")");
            }
            ++bad;
        }
    }
    return bad;
}

std::size_t count_covered(const std::vector<std::uint32_t>& owner) {
    std::size_t n = 0;
    for (const std::uint32_t v : owner) {
        n += v != 0u ? 1u : 0u;
    }
    return n;
}

ProjectedTriangle tri(float x0, float y0, float x1, float y1, float x2, float y2, float z) {
    return {x0, y0, x1, y1, x2, y2, z, z, z};
}

// Deterministic, platform-independent pseudo-random floats (no <random> distribution, whose
// output is implementation-defined).
struct Lcg {
    std::uint64_t state;

    float next01() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>(state >> 40) / 16777216.0f; // 24 bits: exact in float
    }
};

// ── (a) inputs: hand-picked edge cases, then random sub-pixel triangles ─────────────────────────

std::vector<ProjectedTriangle> edge_cases() {
    std::vector<ProjectedTriangle> t;
    // Two triangles sharing an edge along the pixel-centre column x = 10.5, and a shared diagonal
    // running through pixel centres: each shared sample must go to exactly one of the pair.
    t.push_back(tri(8.5f, 4.5f, 10.5f, 4.5f, 10.5f, 8.5f, 0.3f));
    t.push_back(tri(10.5f, 4.5f, 12.5f, 4.5f, 10.5f, 8.5f, 0.3f));
    t.push_back(tri(8.5f, 4.5f, 10.5f, 8.5f, 8.5f, 8.5f, 0.3f));
    // A horizontal shared edge on the pixel-centre row y = 12.5, both windings.
    t.push_back(tri(4.5f, 10.5f, 8.5f, 12.5f, 4.5f, 12.5f, 0.4f));
    t.push_back(tri(4.5f, 12.5f, 8.5f, 12.5f, 4.5f, 14.5f, 0.4f));
    // Exact depth ties: the same footprint twice at the same depth — the FIRST must win — and a
    // nearer third that must beat both.
    t.push_back(tri(20.0f, 20.0f, 23.0f, 20.0f, 20.0f, 23.0f, 0.5f));
    t.push_back(tri(20.0f, 20.0f, 20.0f, 23.0f, 23.0f, 20.0f, 0.5f));
    t.push_back(tri(21.0f, 20.0f, 23.0f, 20.0f, 21.0f, 22.0f, 0.25f));
    // Sub-pixel triangles that do and do not contain a pixel centre.
    t.push_back(tri(30.3f, 30.3f, 30.7f, 30.4f, 30.4f, 30.8f, 0.6f));
    t.push_back(tri(31.1f, 30.1f, 31.3f, 30.1f, 31.1f, 30.3f, 0.6f));
    // Vertices exactly on pixel centres, and a sliver whose edge grazes one within epsilon.
    t.push_back(tri(40.5f, 40.5f, 41.5f, 40.5f, 40.5f, 41.5f, 0.7f));
    t.push_back(tri(44.0f, 44.5f, 47.0f, 44.5000005f, 44.0f, 45.0f, 0.7f));
    // Clipped by the viewport on every side.
    t.push_back(tri(-1.5f, -1.0f, 1.5f, -1.0f, -1.0f, 2.5f, 0.1f));
    t.push_back(tri(62.0f, 62.0f, 65.5f, 62.0f, 62.0f, 65.5f, 0.1f));
    // A depth gradient across [0, 1] (exercises the reciprocal and the full depth range).
    t.push_back(ProjectedTriangle{50.0f, 10.0f, 56.0f, 10.0f, 50.0f, 16.0f, 0.0f, 1.0f, 0.5f});
    // Degenerate: collinear and coincident — the oracle draws nothing.
    t.push_back(tri(10.0f, 50.0f, 12.0f, 52.0f, 14.0f, 54.0f, 0.2f));
    t.push_back(tri(15.0f, 50.0f, 15.0f, 50.0f, 15.0f, 50.0f, 0.2f));
    return t;
}

std::vector<ProjectedTriangle> random_micro_triangles(std::size_t count, std::uint64_t seed) {
    Lcg rng{seed};
    std::vector<ProjectedTriangle> t;
    for (std::size_t i = 0; i < count; ++i) {
        const float cx = rng.next01() * 68.0f - 2.0f;
        const float cy = rng.next01() * 68.0f - 2.0f;
        const auto offset = [&] { return (rng.next01() - 0.5f) * 3.0f; }; // ~sub-pixel to 1.5 px
        ProjectedTriangle p{cx + offset(),
                            cy + offset(),
                            cx + offset(),
                            cy + offset(),
                            cx + offset(),
                            cy + offset(),
                            rng.next01(),
                            rng.next01(),
                            rng.next01()};
        // A third of them share one of four flat depths, so exact ties are common, not rare.
        if (i % 3 == 0) {
            const float level = 0.2f * static_cast<float>(1 + (i / 3) % 4);
            p.z0 = p.z1 = p.z2 = level;
        }
        t.push_back(p);
    }
    return t;
}

} // namespace

TEST_CASE("micro raster: the GPU software rasterizer equals the CPU oracle bit for bit (M18.4a)") {
    auto device = make_device_or_skip();
    if (!device) {
        return;
    }
    const bool has64 = device->adapter().buffer_int64_atomics;
    MESSAGE("adapter '", device->adapter().name, "' 64-bit buffer atomics: ", has64);

    const auto check_parity = [&](const std::vector<ProjectedTriangle>& tris) {
        const Oracle oracle = run_oracle(tris);
        const auto sw = as_software(tris);
        const Frame f64 = render_frame(*device, nullptr, sw, MicroRasterAtomics::Auto, false);
        const Frame f32 = render_frame(*device, nullptr, sw, MicroRasterAtomics::Portable32, false);
        CHECK(f64.stats.used_int64_atomics == (has64 ? 1u : 0u));
        CHECK(f32.stats.used_int64_atomics == 0u);
        CHECK(parity_mismatches(f64, oracle) == 0);
        CHECK(parity_mismatches(f32, oracle) == 0);
        // The two atomic paths are the same minimum, so their counters are identical too.
        CHECK(std::memcmp(&f64.counters, &f32.counters, sizeof(f64.counters)) == 0);
        CHECK(f64.counters.processed == tris.size());
        CHECK(f64.counters.non_finite_depth == 0);
        CHECK(f64.counters.dropped_oversized == 0);
        return f64;
    };

    SUBCASE("hand-built edge cases: shared edges, ties, clipping, degenerates") {
        const std::vector<ProjectedTriangle> tris = edge_cases();
        const Frame f = check_parity(tris);
        CHECK(f.counters.rejected_degenerate == 2); // the collinear and the coincident one
        CHECK(count_covered(run_oracle(tris).owner) > 30);
    }

    SUBCASE("16000 seeded random sub-pixel triangles with dense overlap") {
        const std::vector<ProjectedTriangle> tris = random_micro_triangles(16000, 0x5eed1234u);
        const Oracle oracle = run_oracle(tris);
        // The set must actually exercise the atomics: most pixels written, many more than once.
        CHECK(count_covered(oracle.owner) > kSize * kSize * 3 / 4);
        const Frame f = check_parity(tris);
        CHECK(f.counters.samples_covered > 2 * count_covered(oracle.owner)); // many overwrites
    }

    SUBCASE("rejected inputs land in their own counters and draw nothing") {
        std::vector<VirtualGeometryMicroTriangle> sw;
        sw.push_back({tri(1.0f, 1.0f, 5.0f, 1.0f, 1.0f, 5.0f, 0.5f), {}, 0u}); // empty ID
        sw.push_back({tri(NAN, 1.0f, 5.0f, 1.0f, 1.0f, 5.0f, 0.5f), sw_id(1), 0u});
        sw.push_back({tri(1.0f, 1.0f, 5.0f, 1.0f, 1.0f, 5.0f, 1.5f), sw_id(2), 0u});   // z > 1
        sw.push_back({tri(1.0f, 1.0f, 40.0f, 1.0f, 1.0f, 40.0f, 0.5f), sw_id(3), 0u}); // too big
        sw.push_back({tri(-9.0f, -9.0f, -5.0f, -9.0f, -9.0f, -5.0f, 0.5f), sw_id(4), 0u});
        const Frame f = render_frame(*device, nullptr, sw, MicroRasterAtomics::Auto, false);
        CHECK(f.counters.processed == 5);
        CHECK(f.counters.rejected_invalid_id == 1);
        CHECK(f.counters.rejected_invalid == 2);
        CHECK(f.counters.dropped_oversized == 1);
        CHECK(f.counters.no_sample == 1);
        CHECK(f.counters.rasterized == 0);
        std::size_t written = 0;
        for (const auto& w : f.ids) {
            written += covered(w) ? 1u : 0u;
        }
        CHECK(written == 0);
    }

    SUBCASE("back-face culling mirrors the hardware pass when asked") {
        // Positive oracle area is a back face in the y-down framebuffer (CCW front, see shader).
        std::vector<VirtualGeometryMicroTriangle> sw;
        sw.push_back({tri(2.0f, 2.0f, 6.0f, 2.0f, 2.0f, 6.0f, 0.5f), sw_id(0), 0u});    // back
        sw.push_back({tri(10.0f, 2.0f, 10.0f, 6.0f, 14.0f, 2.0f, 0.5f), sw_id(1), 0u}); // front
        const Frame f = render_frame(*device, nullptr, sw, MicroRasterAtomics::Auto, true);
        CHECK(f.counters.culled_backface == 1);
        CHECK(f.counters.rasterized == 1);
        CHECK_FALSE(covered(f.ids[3 * kSize + 3]));
        CHECK(f.ids[3 * kSize + 11] == sw_id(1));
    }
}

namespace {

// The (b) mesh: a single-layer grid over [8, 56]^2 whose column and row lines mix wide and
// sub-pixel spacings, so hardware-sized and sub-pixel triangles share edges everywhere. Every line
// sits on the quarter-pixel lattice and several on pixel-centre lines (x = k + 0.5). All triangles
// are front-facing for the hardware pass (negative oracle area), as a real mesh's visible side is.
std::vector<ProjectedTriangle> split_mesh(float z_base) {
    const float lines[] = {8.0f,
                           8.5f,
                           9.25f,
                           12.0f,
                           12.5f,
                           13.0f,
                           20.0f,
                           20.5f,
                           20.75f,
                           21.0f,
                           28.5f,
                           29.0f,
                           29.5f,
                           36.0f,
                           36.25f,
                           44.5f,
                           45.0f,
                           56.0f};
    std::vector<ProjectedTriangle> t;
    for (std::size_t j = 0; j + 1 < std::size(lines); ++j) {
        for (std::size_t i = 0; i + 1 < std::size(lines); ++i) {
            const float x0 = lines[i], x1 = lines[i + 1], y0 = lines[j], y1 = lines[j + 1];
            // Depth is an affine function of position, so shared vertices share depth exactly.
            const auto z = [&](float x, float y) { return z_base + x / 256.0f + y / 512.0f; };
            // Alternate the diagonal so both diagonal directions meet at shared edges.
            if ((i + j) % 2 == 0) {
                t.push_back({x0, y0, x0, y1, x1, y0, z(x0, y0), z(x0, y1), z(x1, y0)});
                t.push_back({x1, y0, x0, y1, x1, y1, z(x1, y0), z(x0, y1), z(x1, y1)});
            } else {
                t.push_back({x0, y0, x1, y1, x1, y0, z(x0, y0), z(x1, y1), z(x1, y0)});
                t.push_back({x0, y0, x0, y1, x1, y1, z(x0, y0), z(x0, y1), z(x1, y1)});
            }
        }
    }
    return t;
}

struct Split {
    std::vector<ProjectedTriangle> hw;
    std::vector<ProjectedTriangle> sw;
    std::uint32_t rerouted = 0;
};

Split route(const std::vector<ProjectedTriangle>& tris) {
    Split s;
    for (const ProjectedTriangle& t : tris) {
        const MicroTriangleRoute r = route_micro_triangle(t);
        s.rerouted += r.rerouted_oversized ? 1u : 0u;
        (r.path == MicroTrianglePath::Software ? s.sw : s.hw).push_back(t);
    }
    return s;
}

// The owner the oracle names at each pixel, as the ID the hybrid frame must carry. The oracle
// list is hardware-first, then software — the order the merge's Less test realises.
std::vector<VirtualGeometryVisibilityWords> oracle_ids(const Split& s) {
    std::vector<ProjectedTriangle> all = s.hw;
    all.insert(all.end(), s.sw.begin(), s.sw.end());
    const Oracle o = run_oracle(all);
    std::vector<VirtualGeometryVisibilityWords> ids(o.owner.size());
    for (std::size_t p = 0; p < o.owner.size(); ++p) {
        if (o.owner[p] == 0u) {
            continue;
        }
        const std::uint32_t i = o.owner[p] - 1u;
        ids[p] = i < s.hw.size() ? hw_id(i / kMaxClusterTriangles, i % kMaxClusterTriangles)
                                 : sw_id(i - static_cast<std::uint32_t>(s.hw.size()));
    }
    return ids;
}

} // namespace

TEST_CASE("micro raster: hardware and software agree at their boundary (M18.4b)") {
    auto device = make_device_or_skip();
    if (!device) {
        return;
    }
    if (!device->adapter().gpu_driven_draw) {
        MESSAGE("no GPU-driven draw — the hardware visibility pass refuses; skipping");
        return;
    }

    SUBCASE("a single-layer mesh split across both paths: no cracks, no double writes") {
        const Split s = route(split_mesh(0.2f));
        REQUIRE(s.hw.size() > 20);
        REQUIRE(s.sw.size() > 20);
        MESSAGE("mesh: ", s.hw.size(), " hardware + ", s.sw.size(), " software triangles");
        const HardwareMesh mesh = make_hardware_mesh(s.hw);
        const auto sw = as_software(s.sw);

        const Frame hw_only = render_frame(*device, &mesh, {}, MicroRasterAtomics::Auto, true);
        const Frame sw_only = render_frame(*device, nullptr, sw, MicroRasterAtomics::Auto, true);
        const Frame hybrid = render_frame(*device, &mesh, sw, MicroRasterAtomics::Auto, true);
        CHECK(hw_only.hw_stats.drawn == mesh.draws.size());
        CHECK(sw_only.counters.culled_backface == 0);

        // The fill rule, measured on the HARDWARE: its coverage of the hardware triangles alone
        // must be exactly the oracle's. This is the precondition for everything below.
        std::vector<ProjectedTriangle> hw_list = s.hw;
        const Oracle hw_oracle = run_oracle(hw_list);
        std::size_t hw_rule_mismatch = 0;
        for (std::size_t p = 0; p < hw_oracle.owner.size(); ++p) {
            hw_rule_mismatch += (hw_oracle.owner[p] != 0u) != covered(hw_only.ids[p]) ? 1u : 0u;
        }
        CHECK(hw_rule_mismatch == 0);

        const std::vector<VirtualGeometryVisibilityWords> expected = oracle_ids(s);
        std::size_t double_writes = 0, cracks = 0, extra = 0, wrong_owner = 0, oracle_covered = 0;
        for (std::size_t p = 0; p < expected.size(); ++p) {
            const bool h = covered(hw_only.ids[p]);
            const bool w = covered(sw_only.ids[p]);
            const bool o = covered(expected[p]);
            oracle_covered += o ? 1u : 0u;
            double_writes += (h && w) ? 1u : 0u;
            cracks += (o && !h && !w) ? 1u : 0u;
            extra += (!o && (h || w)) ? 1u : 0u;
            wrong_owner += hybrid.ids[p] == expected[p] ? 0u : 1u;
        }
        MESSAGE("oracle covers ",
                oracle_covered,
                " px; double writes ",
                double_writes,
                ", cracks ",
                cracks,
                ", extra ",
                extra,
                ", wrong owner ",
                wrong_owner);
        CHECK(oracle_covered == 48u * 48u); // the mesh tiles [8, 56)^2 with no gaps
        CHECK(double_writes == 0);
        CHECK(cracks == 0);
        CHECK(extra == 0);
        CHECK(wrong_owner == 0);
    }

    SUBCASE("two overlapping layers, each split: the depth winner is the oracle's") {
        // The near layer at z ~ 0.2 and a far one at z ~ 0.6, shifted by a quarter pixel so their
        // hardware/software boundaries do not line up — every mix of near/far x hw/sw occurs.
        std::vector<ProjectedTriangle> far_layer = split_mesh(0.6f);
        for (ProjectedTriangle& t : far_layer) {
            t.x0 += 0.25f, t.x1 += 0.25f, t.x2 += 0.25f;
        }
        std::vector<ProjectedTriangle> all = far_layer;
        const std::vector<ProjectedTriangle> near_layer = split_mesh(0.2f);
        all.insert(all.end(), near_layer.begin(), near_layer.end());
        const Split s = route(all);
        const HardwareMesh mesh = make_hardware_mesh(s.hw);
        const Frame hybrid =
            render_frame(*device, &mesh, as_software(s.sw), MicroRasterAtomics::Auto, true);
        const std::vector<VirtualGeometryVisibilityWords> expected = oracle_ids(s);
        std::size_t wrong_owner = 0, sw_won = 0;
        for (std::size_t p = 0; p < expected.size(); ++p) {
            wrong_owner += hybrid.ids[p] == expected[p] ? 0u : 1u;
            const auto id = unpack_virtual_geometry_visibility_id64(hybrid.ids[p]);
            sw_won += (id.has_value() && id->cluster >= 1000u) ? 1u : 0u;
        }
        MESSAGE("two layers: ", sw_won, " px won by software, ", wrong_owner, " wrong owners");
        CHECK(sw_won > 50); // the software path wins real depth contests, not just empty pixels
        CHECK(wrong_owner == 0);
    }
}

TEST_CASE("micro raster: a sub-pixel silhouette survives only with the hybrid path (M18.4c)") {
    auto device = make_device_or_skip();
    if (!device) {
        return;
    }
    if (!device->adapter().gpu_driven_draw) {
        MESSAGE("no GPU-driven draw — the hardware visibility pass refuses; skipping");
        return;
    }
    // A wire 0.5 px thick running diagonally across the target, cut into segments ~1.4 px long:
    // every triangle has 0.35 px^2 area, so the classifier routes all of it to software. One big
    // hardware backdrop triangle sits far behind it, so "the hardware path alone" is a real frame.
    std::vector<ProjectedTriangle> tris;
    tris.push_back(
        {4.0f, 60.0f, 60.0f, 60.0f, 4.0f, 4.0f, 0.9f, 0.9f, 0.9f}); // backdrop, front-facing
    for (int k = 0; k < 40; ++k) {
        const float a = 6.0f + static_cast<float>(k) * 1.25f;
        const float b = a + 1.25f;
        // The band between the lines y = x + 0.25 and y = x - 0.25 (on the quarter lattice).
        tris.push_back({a, a + 0.25f, b, b - 0.25f, a, a - 0.25f, 0.3f, 0.3f, 0.3f});
        tris.push_back({a, a + 0.25f, b, b + 0.25f, b, b - 0.25f, 0.3f, 0.3f, 0.3f});
    }
    const Split s = route(tris);
    REQUIRE(s.hw.size() == 1);
    REQUIRE(s.sw.size() == 80);
    const HardwareMesh mesh = make_hardware_mesh(s.hw);

    const std::vector<VirtualGeometryVisibilityWords> expected = oracle_ids(s);
    std::size_t wire_expected = 0;
    for (const auto& w : expected) {
        const auto id = unpack_virtual_geometry_visibility_id64(w);
        wire_expected += (id.has_value() && id->cluster >= 1000u) ? 1u : 0u;
    }
    const auto wire_pixels = [](const Frame& f) {
        std::size_t n = 0;
        for (const auto& w : f.ids) {
            const auto id = unpack_virtual_geometry_visibility_id64(w);
            n += (id.has_value() && id->cluster >= 1000u) ? 1u : 0u;
        }
        return n;
    };
    const Frame hw_alone = render_frame(*device, &mesh, {}, MicroRasterAtomics::Auto, true);
    const Frame hybrid =
        render_frame(*device, &mesh, as_software(s.sw), MicroRasterAtomics::Auto, true);
    MESSAGE("wire pixels: oracle ",
            wire_expected,
            ", hardware alone ",
            wire_pixels(hw_alone),
            ", hybrid ",
            wire_pixels(hybrid));
    CHECK(wire_expected >= 40);        // the wire crosses a pixel centre at least per px
    CHECK(wire_pixels(hw_alone) == 0); // dropped: nothing routed it to a rasterizer
    CHECK(wire_pixels(hybrid) == wire_expected);
    std::size_t wrong_owner = 0;
    for (std::size_t p = 0; p < expected.size(); ++p) {
        wrong_owner += hybrid.ids[p] == expected[p] ? 0u : 1u;
    }
    CHECK(wrong_owner == 0);
}
