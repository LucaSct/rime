// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m19.8d2 — rendering the terrain LOD chain (ADR-0071): CDLOD selection, geomorphing onto the
// parent's triangulation, crack-free edges, parent fallback and a pinned root cover.
//
// What is proven here, and how (structural, no golden images; GPU readback as in m19.3):
//
//   (a) MORPH ENDPOINTS — a tile drawn as POINTS through the engine's terrain.vert: at morph 0
//       every vertex is the tile's own sample, BIT for bit (y, x and z); at morph 1 every vertex
//       lies on the PARENT's triangulated surface (within a derived f32 bound), and the even
//       vertices are the parent's own vertices, bit for bit.
//   (b) SHARED EDGES — every leaf of real selections (with and without forced fallback) drawn as
//       points; along every edge two leaves share, the two tiles' vertices lie on ONE polyline:
//       bit-identical between same-level tiles, and on the coarse segment within the bound for a
//       T-junction. Every orientation is required to have occurred: 4 sides × {same, coarser,
//       finer} × {ideal, fallback}.
//   (c) 2:1 — over a camera sweep (slow fly, teleport, fast fly) no two edge-adjacent leaves differ
//       by more than one level; with every tile usable the ranges alone guarantee it (zero balance
//       collapses), and with fallback forced on random nodes the balance pass restores it.
//   (d) COVERAGE — every frame of the sweep, the leaves tile the world exactly: area sum == world
//       area and no level-0 tile covered twice.
//   (e) POP-FREE SWITCH — at every frame the ideal selection changes, the surface drawn with the
//       old leaves and with the new ones, at the new camera, agree per pixel within the bound;
//       with the morph disabled the same switches pop by decimetres.
//   (f) DETERMINISM — the same camera path selects the same sequence whether the asset server
//       completes its loads in request order (1 worker) or in any order (4 workers).
//   (g) PINNED ROOTS — through the real residency at maximum pressure: the roots are resident
//       every frame once loaded, never evicted, and coverage holds; a root cover larger than the
//       budget is refused at construction, counted.
//   (h) m19.8a UNCHANGED — a level-0 world is driven by the radii, not the selection, with every
//       LOD counter at zero (and the 46 m19.3–m19.8a cases are green beside this file).
//   plus the 8d1 handoff: a parent whose samples disagree with a child is refused, counted, and
//   never drawn, while its area stays covered.
#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/heightfield_asset.hpp"
#include "rime/assets/terrain_world.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/terrain_lod.hpp"
#include "rime/render/terrain_pass.hpp"
#include "rime/render/terrain_residency.hpp"
#include "rime/rhi/device.hpp"
#include "terrain.vert.spv.h"
#include "terrain_height_probe.frag.spv.h"
#include "terrain_lod_probe.frag.spv.h"
#include "terrain_test_files.hpp"
#include "terrain_vertex_probe.frag.spv.h"

namespace {

using namespace rime;
using namespace rime::terrain_test;
using assets::TerrainTileCoord;
using assets::TerrainTileKey;
using render::TerrainLodLeaf;

// ── The world ────────────────────────────────────────────────────────────────────────────────────
//
// 9 samples per tile (8 cells), 1 m level-0 cells, 1 cm quantisation. Placed off the origin so a
// dropped origin moves the answer by metres; x = −23.5 + integer, so no vertex ever has x == 0
// (and y is metres above 0 everywhere — the probe's coverage witness).
constexpr std::uint32_t kN = 9;
constexpr std::int32_t kCells = static_cast<std::int32_t>(kN) - 1;
constexpr float kScale = 0.01f;
constexpr float kOffset = 0.0f;
constexpr core::Vec3 kOrigin{-23.5f, 2.25f, 16.0f};

// One GLOBAL integer field every level is cut from, so every border and every parent/child pair
// agrees by construction. Non-separable and asymmetric (the m19.3 lesson: a separable field has
// planar cells and both diagonals would look alike) plus a hashed ±0.3 m roughness, so parents
// genuinely differ from their children — a morph that did nothing would be visible.
std::uint16_t field(std::int64_t gx, std::int64_t gz) {
    const double x = static_cast<double>(gx);
    const double z = static_cast<double>(gz);
    const double h = 1500.0 + 300.0 * std::sin(0.21 * x) + 200.0 * std::cos(0.13 * z) +
                     150.0 * std::sin(0.37 * x) * std::sin(0.29 * z);
    std::uint64_t k = static_cast<std::uint64_t>(gx) * 0x9E3779B97F4A7C15ull ^
                      static_cast<std::uint64_t>(gz) * 0xC2B2AE3D27D4EB4Full;
    k ^= k >> 31;
    k *= 0xBF58476D1CE4E5B9ull;
    k ^= k >> 29;
    return static_cast<std::uint16_t>(std::lround(h) + static_cast<long>(k % 61));
}

assets::TerrainWorldGrid lod_grid() {
    assets::TerrainWorldGrid g{};
    g.samples = kN;
    g.cell_size_x = 1.0f;
    g.cell_size_z = 1.0f;
    g.height_scale = kScale;
    g.height_offset = kOffset;
    g.origin = kOrigin;
    return g;
}

// ADR-0070's engine order, origin.y + (offset + scale · q), one rounding per statement (separate
// statements so no compiler contracts them into an FMA — the shader's `precise` does the same).
float world_height(std::uint16_t q) {
    const float step = kScale * static_cast<float>(q);
    const float local = kOffset + step;
    return kOrigin.y + local;
}

// The world x (or z) of global level-L sample index g, in terrain.vert's LOD order.
float world_x(std::int64_t g, std::uint32_t level) {
    const float cell = 1.0f * static_cast<float>(1u << level);
    const float along = static_cast<float>(g) * cell;
    return kOrigin.x + along;
}

float world_z(std::int64_t g, std::uint32_t level) {
    const float cell = 1.0f * static_cast<float>(1u << level);
    const float along = static_cast<float>(g) * cell;
    return kOrigin.z + along;
}

std::uint16_t q_at(const assets::HeightfieldAsset& a, std::int32_t i, std::int32_t j) {
    return a.samples[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * kN];
}

// The tile's triangulated height, in quantisation steps, at fractional sample position (pi, pj):
// ADR-0060's diagonal (i, j)→(i+1, j+1), barycentric — triangle A = (v00, v10, v11) where u ≥ v.
double tri_q(const assets::HeightfieldAsset& a, double pi, double pj) {
    const auto ci = std::min<std::int32_t>(static_cast<std::int32_t>(std::floor(pi)), kCells - 1);
    const auto cj = std::min<std::int32_t>(static_cast<std::int32_t>(std::floor(pj)), kCells - 1);
    const double u = pi - ci;
    const double v = pj - cj;
    const double h00 = q_at(a, ci, cj);
    const double h10 = q_at(a, ci + 1, cj);
    const double h01 = q_at(a, ci, cj + 1);
    const double h11 = q_at(a, ci + 1, cj + 1);
    if (u >= v) {
        return h00 + u * (h10 - h00) + v * (h11 - h10);
    }
    return h00 + v * (h01 - h00) + u * (h11 - h01);
}

struct LodWorld {
    assets::TerrainWorld world;
    std::map<TerrainTileKey, assets::HeightfieldAsset> tiles;
    std::int32_t nx = 0; // level-0 tiles per axis
    std::int32_t nz = 0;
};

std::string tile_path(TerrainTileKey k) {
    return "lod_L" + std::to_string(k.level) + "_" + std::to_string(k.coord.x) + "_" +
           std::to_string(k.coord.z) + ".rhf";
}

// The chain the cook (ADR-0070) would write: nested subsampling, descendant bounds, and the
// SATURATED geometric error (raw deviation, maxed with the children's), measured here by brute
// force over every level-0 sample beneath each tile.
LodWorld make_lod_world(std::int32_t nx, std::int32_t nz, std::uint32_t levels) {
    auto w = assets::TerrainWorld::make(lod_grid());
    REQUIRE(w.has_value());
    LodWorld out{*w, {}, nx, nz};
    std::map<TerrainTileKey, float> errors;
    for (std::uint32_t l = 0; l < levels; ++l) {
        for (std::int32_t z = 0; z < (nz >> l); ++z) {
            for (std::int32_t x = 0; x < (nx >> l); ++x) {
                const TerrainTileKey k{l, {x, z}};
                assets::HeightfieldAsset a{};
                a.columns = kN;
                a.rows = kN;
                a.cell_size_x = static_cast<float>(1u << l);
                a.cell_size_z = static_cast<float>(1u << l);
                a.height_scale = kScale;
                a.height_offset = kOffset;
                a.origin = out.world.grid().tile_origin(k);
                for (std::int32_t j = 0; j < kCells + 1; ++j) {
                    for (std::int32_t i = 0; i < kCells + 1; ++i) {
                        a.samples.push_back(field(std::int64_t{x * kCells + i} << l,
                                                  std::int64_t{z * kCells + j} << l));
                    }
                }
                const auto [lo, hi] = std::minmax_element(a.samples.begin(), a.samples.end());
                a.min_sample = *lo;
                a.max_sample = *hi;
                // Bounds and error over every level-0 sample beneath the tile.
                const std::int64_t span = std::int64_t{kCells} << l;
                std::uint16_t qmin = 0xFFFF;
                std::uint16_t qmax = 0;
                double dev = 0.0;
                for (std::int64_t gz = 0; gz <= span; ++gz) {
                    for (std::int64_t gx = 0; gx <= span; ++gx) {
                        const std::uint16_t q0 = field(x * span + gx, z * span + gz);
                        qmin = std::min(qmin, q0);
                        qmax = std::max(qmax, q0);
                        if (l > 0) {
                            const double s = static_cast<double>(1u << l);
                            dev = std::max(dev, std::fabs(q0 - tri_q(a, gx / s, gz / s)));
                        }
                    }
                }
                float err = 0.0f;
                if (l > 0) {
                    err = std::nextafter(static_cast<float>(kScale * dev),
                                         std::numeric_limits<float>::infinity());
                    for (std::int32_t c = 0; c < 4; ++c) {
                        err =
                            std::max(err, errors.at({l - 1, {2 * x + (c & 1), 2 * z + (c >> 1)}}));
                    }
                }
                errors[k] = err;
                assets::TerrainWorldTile t{};
                t.coord = k.coord;
                t.min_y = world_height(qmin);
                t.max_y = world_height(qmax);
                t.path = tile_path(k);
                t.level = l;
                t.geometric_error = err;
                REQUIRE(out.world.add_tile(t));
                out.tiles.emplace(k, std::move(a));
            }
        }
    }
    REQUIRE(out.world.validate_levels());
    REQUIRE(out.world.level_count() == levels);
    return out;
}

// A view that makes LOD switch INSIDE a world of a hundred metres: a small viewport and a
// generous pixel error give K ≈ 6.9 m of distance per metre of error.
render::TerrainLodView test_view() {
    render::TerrainLodView v{};
    v.viewport_height_px = 64.0f;
    v.vertical_fov = 1.0471976f;
    v.pixel_error = 8.0f;
    v.step_margin = 1.0f;
    v.morph_fraction = 0.25f;
    return v;
}

// ── Coverage and 2:1, measured on the level-0 tile grid ─────────────────────────────────────────

struct Cover {
    bool exact = false;           // every level-0 tile covered exactly once, area sum matches
    std::uint32_t max_step = 0;   // the largest level difference across any shared edge
    std::uint64_t area_cells = 0; // Σ 4^L over the leaves, in level-0 tiles
};

Cover measure_cover(const LodWorld& w, const std::vector<TerrainLodLeaf>& leaves) {
    std::vector<int> level(static_cast<std::size_t>(w.nx * w.nz), -1);
    Cover c;
    bool overlap = false;
    for (const TerrainLodLeaf& leaf : leaves) {
        const std::int32_t s = 1 << leaf.key.level;
        c.area_cells += static_cast<std::uint64_t>(s) * static_cast<std::uint64_t>(s);
        for (std::int32_t z = leaf.key.coord.z * s; z < (leaf.key.coord.z + 1) * s; ++z) {
            for (std::int32_t x = leaf.key.coord.x * s; x < (leaf.key.coord.x + 1) * s; ++x) {
                int& slot = level[static_cast<std::size_t>(z * w.nx + x)];
                overlap = overlap || slot != -1;
                slot = static_cast<int>(leaf.key.level);
            }
        }
    }
    const bool full = std::none_of(level.begin(), level.end(), [](int l) { return l < 0; });
    c.exact = !overlap && full && c.area_cells == static_cast<std::uint64_t>(w.nx * w.nz);
    for (std::int32_t z = 0; z < w.nz; ++z) {
        for (std::int32_t x = 0; x < w.nx; ++x) {
            const int a = level[static_cast<std::size_t>(z * w.nx + x)];
            if (x + 1 < w.nx) {
                const int b = level[static_cast<std::size_t>(z * w.nx + x + 1)];
                c.max_step = std::max<std::uint32_t>(c.max_step,
                                                     static_cast<std::uint32_t>(std::abs(a - b)));
            }
            if (z + 1 < w.nz) {
                const int b = level[static_cast<std::size_t>((z + 1) * w.nx + x)];
                c.max_step = std::max<std::uint32_t>(c.max_step,
                                                     static_cast<std::uint32_t>(std::abs(a - b)));
            }
        }
    }
    return c;
}

// ── The camera path: a slow fly (≤ the step margin), a teleport, a fast fly ─────────────────────

struct PathFrame {
    core::Vec3 eye;
    bool slow; // the step from the previous frame is within the step margin
};

std::vector<PathFrame> camera_path(const LodWorld& w) {
    const float ex = static_cast<float>(w.nx * kCells);
    const float ez = static_cast<float>(w.nz * kCells);
    std::vector<PathFrame> path;
    const auto fly = [&](core::Vec3 a, core::Vec3 b, float step, bool slow) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        const int n = std::max(1, static_cast<int>(std::ceil(len / step)));
        for (int s = 1; s <= n; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(n);
            path.push_back({{a.x + t * dx, a.y + t * dy, a.z + t * dz}, slow});
        }
    };
    const core::Vec3 o = kOrigin;
    // Slow: low across a corner (heights reach ~25 m), climbing over the middle, then along an
    // edge.
    const core::Vec3 p0{o.x + 3.0f, 27.0f, o.z + 5.0f};
    path.push_back({p0, false});
    fly(p0, {o.x + 0.55f * ex, 31.0f, o.z + 0.45f * ez}, 0.5f, true);
    fly(path.back().eye, {o.x + 0.9f * ex, 28.0f, o.z + 0.2f * ez}, 0.5f, true);
    // Teleport to the far corner, dwell, then a fast fly back (12 m steps).
    path.push_back({{o.x + ex - 2.0f, 26.0f, o.z + ez - 3.0f}, false});
    path.push_back({{o.x + ex - 2.0f, 26.0f, o.z + ez - 3.0f}, true});
    fly(path.back().eye, {o.x + 4.0f, 40.0f, o.z + 6.0f}, 12.0f, false);
    return path;
}

// Deterministic "random" usability: roots always, others with probability ~0.7 per frame.
bool forced_fallback_usable(TerrainTileKey k, std::uint32_t top, std::uint64_t frame) {
    if (k.level == top) {
        return true;
    }
    std::uint64_t h = (static_cast<std::uint64_t>(k.level) << 48) ^
                      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.coord.x)) << 24) ^
                      static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.coord.z)) ^
                      (frame * 0x9E3779B97F4A7C15ull);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h % 10 < 7;
}

bool same_leaves(const std::vector<TerrainLodLeaf>& a, const std::vector<TerrainLodLeaf>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!(a[i].key == b[i].key) || a[i].coarser_edges != b[i].coarser_edges ||
            a[i].fallback != b[i].fallback) {
            return false;
        }
    }
    return true;
}

// ── GPU probing ─────────────────────────────────────────────────────────────────────────────────

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

std::unique_ptr<rhi::Device> make_device() {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the m19.8d2 GPU proof");
    }
    return device;
}

std::vector<std::uint8_t> read_back(rhi::Device& device,
                                    rhi::TextureHandle texture,
                                    std::uint32_t w,
                                    std::uint32_t h,
                                    std::uint32_t bpp) {
    const std::uint64_t bytes = std::uint64_t{w} * h * bpp;
    rhi::BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = rhi::BufferUsage::TransferDst;
    rbd.memory = rhi::MemoryUsage::GpuToCpu;
    rbd.debug_name = "m19.8d2-readback";
    const rhi::BufferHandle rb = device.create_buffer(rbd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(texture, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

std::uint32_t word_at(const std::vector<std::uint8_t>& img, std::size_t index) {
    std::uint32_t w = 0;
    std::memcpy(&w, &img[index * 4], sizeof(w));
    return w;
}

float bits_float(std::uint32_t w) {
    float f = 0.0f;
    std::memcpy(&f, &w, sizeof(f));
    return f;
}

std::uint32_t float_bits(float f) {
    std::uint32_t w = 0;
    std::memcpy(&w, &f, sizeof(w));
    return w;
}

// Top-down orthographic view of world rectangle [x0, x0 + w·cell] × [z0, z0 + h·cell] onto a
// w × h target, one `cell` per pixel. Which way the axes map is not assumed: `pixel_of` projects
// with the same matrix, and every read goes through it.
struct TopDown {
    core::Mat4 view_proj;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

TopDown top_down(float x0, float z0, float cell, std::uint32_t w, std::uint32_t h) {
    const float hx = 0.5f * cell * static_cast<float>(w);
    const float hz = 0.5f * cell * static_cast<float>(h);
    const core::Vec3 eye{x0 + hx, 400.0f, z0 + hz};
    return {core::ortho(-hx, hx, -hz, hz, 0.0f, 800.0f) *
                core::look_at(eye, {eye.x, 0.0f, eye.z}, {0.0f, 0.0f, -1.0f}),
            w,
            h};
}

// The pixel a world point lands in, or nullopt off-target.
std::optional<std::size_t> pixel_of(const TopDown& v, float x, float y, float z) {
    const core::Vec4 clip = v.view_proj * core::Vec4{x, y, z, 1.0f};
    const float px = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(v.w);
    const float py = (clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(v.h);
    if (px < 0.0f || py < 0.0f || px >= static_cast<float>(v.w) || py >= static_cast<float>(v.h)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(py) * v.w + static_cast<std::size_t>(px);
}

// The proof's pipelines around the ENGINE's terrain.vert: as points into (y bits, xz bits), and as
// triangles into y bits (the surface, interpolated — the m19.3 probe).
struct Probe {
    rhi::Device& device;
    rhi::ShaderHandle vs{};
    rhi::ShaderHandle fs_vertex{};
    rhi::ShaderHandle fs_height{};
    rhi::PipelineHandle points{};
    rhi::PipelineHandle surface{};
    rhi::SamplerHandle sampler{};

    explicit Probe(rhi::Device& d) : device(d) {
        rhi::ShaderDesc vsd{};
        vsd.stage = rhi::ShaderStage::Vertex;
        vsd.spirv = terrain_vert_spv;
        vsd.spirv_size_bytes = sizeof(terrain_vert_spv);
        vsd.debug_name = "terrain.vert";
        vs = device.create_shader(vsd);
        rhi::ShaderDesc fsd{};
        fsd.stage = rhi::ShaderStage::Fragment;
        fsd.spirv = terrain_vertex_probe_frag_spv;
        fsd.spirv_size_bytes = sizeof(terrain_vertex_probe_frag_spv);
        fsd.debug_name = "terrain_vertex_probe.frag";
        fs_vertex = device.create_shader(fsd);
        fsd.spirv = terrain_height_probe_frag_spv;
        fsd.spirv_size_bytes = sizeof(terrain_height_probe_frag_spv);
        fsd.debug_name = "terrain_height_probe.frag";
        fs_height = device.create_shader(fsd);

        static const rhi::BindingDesc bindings[] = {
            {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Vertex},
        };
        static const rhi::Format vertex_formats[] = {rhi::Format::R32Uint, rhi::Format::RG32Uint};
        rhi::GraphicsPipelineDesc pd{};
        pd.vertex_shader = vs;
        pd.fragment_shader = fs_vertex;
        pd.color_formats = vertex_formats;
        pd.topology = rhi::PrimitiveTopology::PointList;
        pd.cull = rhi::CullMode::None;
        pd.bindings = bindings;
        pd.push_constant_size = sizeof(render::TerrainPush);
        pd.debug_name = "m19.8d2-vertex-probe";
        points = device.create_graphics_pipeline(pd);

        rhi::GraphicsPipelineDesc sd{};
        sd.vertex_shader = vs;
        sd.fragment_shader = fs_height;
        sd.color_format = rhi::Format::R32Uint;
        sd.cull = rhi::CullMode::None;
        sd.bindings = bindings;
        sd.push_constant_size = sizeof(render::TerrainPush);
        sd.debug_name = "m19.8d2-surface-probe";
        surface = device.create_graphics_pipeline(sd);

        rhi::SamplerDesc smp{};
        smp.mag_filter = rhi::Filter::Nearest;
        smp.min_filter = rhi::Filter::Nearest;
        smp.address_mode = rhi::AddressMode::ClampToEdge;
        smp.debug_name = "m19.8d2-heights";
        sampler = device.create_sampler(smp);
    }

    ~Probe() {
        device.wait_idle();
        device.destroy(points);
        device.destroy(surface);
        device.destroy(sampler);
        device.destroy(fs_vertex);
        device.destroy(fs_height);
        device.destroy(vs);
    }

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
};

// One draw: a resident tile, the view it is drawn with, and its LOD parameters.
struct ProbeDraw {
    const render::TerrainTile* tile = nullptr;
    core::Mat4 view_proj;
    render::TerrainLodDraw lod{};
};

// Draw `draws` as points into a w × h pair of targets; returns (y words, xz words).
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>
probe_points(Probe& probe, const std::vector<ProbeDraw>& draws, std::uint32_t w, std::uint32_t h) {
    render::RenderGraph graph(probe.device);
    graph.reset();
    const render::RGTexture ys = graph.create_texture({{w, h}, rhi::Format::R32Uint, "probe-y"});
    const render::RGTexture xzs = graph.create_texture({{w, h}, rhi::Format::RG32Uint, "probe-xz"});
    graph.export_texture(ys);
    graph.export_texture(xzs);
    const render::RGColorAttachment colors[] = {
        {ys, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}},
        {xzs, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    std::vector<render::RGTexture> sampled;
    std::set<std::uint64_t> seen;
    for (const ProbeDraw& d : draws) {
        if (seen.insert((std::uint64_t{d.tile->heights.index} << 32) | d.tile->heights.generation)
                .second) {
            sampled.push_back(
                graph.import_texture(d.tile->heights, rhi::ResourceState::ShaderRead));
        }
    }
    render::RenderGraph::RasterPassDesc rpd{};
    rpd.colors = colors;
    rpd.sampled = sampled;
    graph.add_raster_pass("m19.8d2-vertex-probe", rpd, [&](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(probe.points);
        for (const ProbeDraw& d : draws) {
            const render::TerrainPush push =
                render::terrain_push(*d.tile, d.view_proj, {0.0f, 100.0f, 0.0f}, {}, d.lod);
            cmd.bind_texture(0, d.tile->heights, probe.sampler);
            cmd.push_constants(&push, sizeof(push));
            cmd.draw(d.tile->vertex_count);
        }
    });
    auto cmd = probe.device.begin_commands();
    graph.execute(*cmd);
    probe.device.submit_blocking(*cmd);
    return {read_back(probe.device, graph.physical(ys), w, h, 4),
            read_back(probe.device, graph.physical(xzs), w, h, 8)};
}

// Draw `draws` as triangles (the drawn surface) into a w × h target of y bits.
std::vector<std::uint8_t>
probe_surface(Probe& probe, const std::vector<ProbeDraw>& draws, std::uint32_t w, std::uint32_t h) {
    render::RenderGraph graph(probe.device);
    graph.reset();
    const render::RGTexture ys = graph.create_texture({{w, h}, rhi::Format::R32Uint, "surface-y"});
    graph.export_texture(ys);
    const render::RGColorAttachment colors[] = {
        {ys, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    std::vector<render::RGTexture> sampled;
    std::set<std::uint64_t> seen;
    for (const ProbeDraw& d : draws) {
        if (seen.insert((std::uint64_t{d.tile->heights.index} << 32) | d.tile->heights.generation)
                .second) {
            sampled.push_back(
                graph.import_texture(d.tile->heights, rhi::ResourceState::ShaderRead));
        }
    }
    render::RenderGraph::RasterPassDesc rpd{};
    rpd.colors = colors;
    rpd.sampled = sampled;
    graph.add_raster_pass("m19.8d2-surface-probe", rpd, [&](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(probe.surface);
        for (const ProbeDraw& d : draws) {
            const render::TerrainPush push =
                render::terrain_push(*d.tile, d.view_proj, {0.0f, 100.0f, 0.0f}, {}, d.lod);
            cmd.bind_texture(0, d.tile->heights, probe.sampler);
            cmd.bind_index_buffer(d.tile->indices, rhi::IndexType::Uint32);
            cmd.push_constants(&push, sizeof(push));
            cmd.draw_indexed(d.tile->index_count);
        }
    });
    auto cmd = probe.device.begin_commands();
    graph.execute(*cmd);
    probe.device.submit_blocking(*cmd);
    return read_back(probe.device, graph.physical(ys), w, h, 4);
}

// The LOD parameters the residency would push for `leaf` (draw_leaf's, reproduced from the same
// public inputs so the proof drives the shader exactly as the engine does).
render::TerrainLodDraw
lod_draw(const render::TerrainLodRanges& ranges, const TerrainLodLeaf& leaf, core::Vec3 camera) {
    render::TerrainLodDraw d{};
    d.enabled = true;
    d.grid_origin = kOrigin;
    d.base_x = leaf.key.coord.x * kCells;
    d.base_z = leaf.key.coord.z * kCells;
    d.level = leaf.key.level;
    d.coarser_edges = leaf.coarser_edges;
    d.coarser_corners = leaf.coarser_corners;
    d.camera = camera;
    d.morph_start = ranges.levels[leaf.key.level].morph_start;
    d.morph_end = ranges.levels[leaf.key.level].morph_end;
    return d;
}

// Every tile of the world uploaded once (they are 9×9: the whole chain is tiny).
struct Uploaded {
    render::TerrainPass pass;
    std::map<TerrainTileKey, render::TerrainTileId> ids;

    Uploaded(rhi::Device& device, const LodWorld& w) : pass(device) {
        for (const auto& [k, a] : w.tiles) {
            const render::TerrainTileId id = pass.upload(a);
            REQUIRE(id != render::kInvalidTerrainTile);
            ids.emplace(k, id);
        }
    }

    const render::TerrainTile& tile(TerrainTileKey k) const { return pass.tile(ids.at(k)); }
};

// A vertex read back from a points atlas.
struct GpuVertex {
    std::uint32_t xb = 0;
    std::uint32_t yb = 0;
    std::uint32_t zb = 0;
};

// Lay the leaves out as an atlas — leaf n in its own (kN+1)-pixel cell, one pixel per vertex — draw
// them as points and read every vertex back.
std::vector<std::vector<GpuVertex>> probe_leaves(Probe& probe,
                                                 const Uploaded& up,
                                                 const std::vector<ProbeDraw>& base_draws,
                                                 const std::vector<TerrainTileKey>& keys) {
    constexpr std::uint32_t kStride = kN + 1;
    const auto cols =
        static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(keys.size()))));
    const std::uint32_t rows = (static_cast<std::uint32_t>(keys.size()) + cols - 1) / cols;
    const std::uint32_t w = cols * kStride;
    const std::uint32_t h = rows * kStride;
    std::vector<ProbeDraw> draws = base_draws;
    std::vector<TopDown> views;
    for (std::size_t n = 0; n < keys.size(); ++n) {
        const TerrainTileKey k = keys[n];
        const float cell = static_cast<float>(1u << k.level);
        const std::uint32_t ax = static_cast<std::uint32_t>(n % cols) * kStride;
        const std::uint32_t ay = static_cast<std::uint32_t>(n / cols) * kStride;
        // Pixel (ax + i) holds local i: the target's left edge is half a cell before vertex 0.
        const float x0 = world_x(std::int64_t{k.coord.x} * kCells, k.level) -
                         (static_cast<float>(ax) + 0.5f) * cell;
        const float z0 = world_z(std::int64_t{k.coord.z} * kCells, k.level) -
                         (static_cast<float>(ay) + 0.5f) * cell;
        views.push_back(top_down(x0, z0, cell, w, h));
        draws[n].tile = &up.tile(k);
        draws[n].view_proj = views.back().view_proj;
    }
    const auto [ys, xzs] = probe_points(probe, draws, w, h);
    std::vector<std::vector<GpuVertex>> out(keys.size());
    for (std::size_t n = 0; n < keys.size(); ++n) {
        const TerrainTileKey k = keys[n];
        out[n].resize(std::size_t{kN} * kN);
        std::set<std::size_t> used;
        for (std::int32_t j = 0; j < kCells + 1; ++j) {
            for (std::int32_t i = 0; i < kCells + 1; ++i) {
                const float x = world_x(std::int64_t{k.coord.x} * kCells + i, k.level);
                const float z = world_z(std::int64_t{k.coord.z} * kCells + j, k.level);
                const auto px = pixel_of(views[n], x, 10.0f, z);
                REQUIRE(px.has_value());
                REQUIRE(used.insert(*px).second); // one vertex per pixel, within its own cell
                GpuVertex& v =
                    out[n][static_cast<std::size_t>(j) * kN + static_cast<std::size_t>(i)];
                v.yb = word_at(ys, *px);
                v.xb = word_at(xzs, *px * 2);
                v.zb = word_at(xzs, *px * 2 + 1);
                REQUIRE(v.yb != 0); // a point landed here
            }
        }
    }
    return out;
}

// The f32 spacing of numbers of magnitude `m` (one ULP).
float ulp(float m) {
    const float a = std::fabs(m);
    return std::nextafter(a, std::numeric_limits<float>::infinity()) - a;
}

} // namespace

// ── The ranges ──────────────────────────────────────────────────────────────────────────────────

TEST_CASE("m19.8d2: the derived ranges satisfy the screen-error and nesting inequalities") {
    const LodWorld w = make_lod_world(16, 16, 4);
    const render::TerrainLodView view = test_view();
    const render::TerrainLodRanges r = render::terrain_lod_ranges(w.world, view);
    REQUIRE(r.levels.size() == 4);
    CHECK(render::terrain_lod_ranges_valid(r, view));
    for (std::size_t l = 0; l < r.levels.size(); ++l) {
        const render::TerrainLodLevel& lv = r.levels[l];
        MESSAGE("level " << l << ": e=" << lv.error << " D=" << lv.diagonal << " morph ["
                         << lv.morph_start << ", " << lv.morph_end << "] range " << lv.range);
    }
    // Not vacuous: the chain's errors are real and grow up the chain (8d1 saturates them).
    CHECK(r.levels[0].error == 0.0f);
    CHECK(r.levels[1].error > 0.1f);
    CHECK(r.levels[2].error >= r.levels[1].error);
    CHECK(r.levels[3].error >= r.levels[2].error);
    // The nesting inequality is the binding one at the coarser levels, the screen error at level 0.
    CHECK(r.levels[0].morph_start == doctest::Approx(r.error_to_distance * r.levels[1].error));
    CHECK(r.levels[1].morph_start >= r.levels[0].range + r.levels[1].diagonal + view.step_margin);
    // A hand-broken set is recognised as broken (the falsification of (c) uses one).
    render::TerrainLodRanges broken = r;
    broken.levels[1].morph_start = broken.levels[0].range;
    CHECK_FALSE(render::terrain_lod_ranges_valid(broken, view));
}

// ── (c) 2:1 and (d) coverage, over the sweep, ideal and with forced fallback ────────────────────

TEST_CASE("m19.8d2: (c)(d) over a sweep, the leaves tile the world exactly and neighbours differ "
          "by at most one level — with and without forced fallback") {
    const LodWorld w = make_lod_world(16, 16, 4);
    const render::TerrainLodRanges ranges = render::terrain_lod_ranges(w.world, test_view());
    const std::uint32_t top = w.world.level_count() - 1;
    const std::vector<PathFrame> path = camera_path(w);
    std::uint64_t frame = 0;
    std::uint32_t levels_seen = 0;
    std::uint64_t ideal_collapses = 0;
    std::uint64_t fallback_collapses = 0;
    std::uint64_t fallback_leaves = 0;
    std::uint32_t worst_ideal_step = 0;
    std::uint32_t worst_fallback_step = 0;
    std::uint64_t coarser_bits_under_fallback = 0;
    for (const PathFrame& f : path) {
        ++frame;
        const render::TerrainLodSelection ideal = render::select_terrain_lod(
            w.world, ranges, f.eye, [&](TerrainTileKey k) { return w.world.find(k) != nullptr; });
        const Cover ci = measure_cover(w, ideal.leaves);
        REQUIRE(ci.exact);
        REQUIRE(ideal.uncovered == 0);
        CHECK(ideal.fallback_leaves == 0);
        ideal_collapses += ideal.balance_collapses;
        worst_ideal_step = std::max(worst_ideal_step, ci.max_step);
        std::uint32_t mask = 0;
        for (const TerrainLodLeaf& leaf : ideal.leaves) {
            mask |= 1u << leaf.key.level;
        }
        levels_seen =
            std::max<std::uint32_t>(levels_seen, static_cast<std::uint32_t>(std::popcount(mask)));

        const render::TerrainLodSelection fb =
            render::select_terrain_lod(w.world, ranges, f.eye, [&](TerrainTileKey k) {
                return forced_fallback_usable(k, top, frame);
            });
        const Cover cf = measure_cover(w, fb.leaves);
        REQUIRE(cf.exact);
        REQUIRE(fb.uncovered == 0);
        CHECK(cf.max_step <= 1);
        worst_fallback_step = std::max(worst_fallback_step, cf.max_step);
        fallback_collapses += fb.balance_collapses;
        fallback_leaves += fb.fallback_leaves;
        for (const TerrainLodLeaf& leaf : fb.leaves) {
            coarser_bits_under_fallback += leaf.coarser_edges != 0 ? 1u : 0u;
        }
    }
    // THE RANGES ALONE guarantee 2:1: not one collapse was needed on any ideal frame.
    CHECK(ideal_collapses == 0);
    CHECK(worst_ideal_step <= 1);
    CHECK(worst_fallback_step <= 1);
    // Not vacuous: three levels were on screen at once, fallback happened, and it broke 2:1 often
    // enough that the balance pass had to work.
    CHECK(levels_seen >= 3);
    CHECK(fallback_leaves > 0);
    CHECK(fallback_collapses > 0);
    CHECK(coarser_bits_under_fallback > 0);
    MESSAGE("m19.8d2 (c)(d): " << path.size() << " frames; up to " << levels_seen
                               << " levels on screen; forced fallback drew " << fallback_leaves
                               << " fallback leaves and needed " << fallback_collapses
                               << " balance collapses; the ideal selection needed "
                               << ideal_collapses);
}

// ── (f) determinism of the pure function ────────────────────────────────────────────────────────

TEST_CASE("m19.8d2: (f) selection is a pure function of camera, ranges and usability") {
    const LodWorld w = make_lod_world(16, 16, 4);
    const render::TerrainLodRanges ranges = render::terrain_lod_ranges(w.world, test_view());
    const std::uint32_t top = w.world.level_count() - 1;
    const std::vector<PathFrame> path = camera_path(w);
    // The same path walked forward twice — and the second time with the frames' selections
    // interleaved with unrelated ones (other cameras): no hidden state carries across calls.
    std::vector<render::TerrainLodSelection> first;
    std::uint64_t frame = 0;
    for (const PathFrame& f : path) {
        ++frame;
        first.push_back(render::select_terrain_lod(w.world, ranges, f.eye, [&](TerrainTileKey k) {
            return forced_fallback_usable(k, top, frame);
        }));
    }
    frame = 0;
    for (std::size_t n = 0; n < path.size(); ++n) {
        ++frame;
        (void)render::select_terrain_lod(
            w.world, ranges, path[path.size() - 1 - n].eye, [&](TerrainTileKey) { return true; });
        const render::TerrainLodSelection again =
            render::select_terrain_lod(w.world, ranges, path[n].eye, [&](TerrainTileKey k) {
                return forced_fallback_usable(k, top, frame);
            });
        REQUIRE(same_leaves(first[n].leaves, again.leaves));
        REQUIRE(first[n].balance_collapses == again.balance_collapses);
    }
}

// ── (a) morph endpoints, on the GPU ─────────────────────────────────────────────────────────────

namespace {

// The parent's triangulated surface, in WORLD metres, at fractional parent-sample position
// (pi, pj): ADR-0060's diagonal over the parent's own f32 vertex heights, evaluated in f64.
double parent_surface(const assets::HeightfieldAsset& p, double pi, double pj) {
    const auto ci = std::min<std::int32_t>(static_cast<std::int32_t>(std::floor(pi)), kCells - 1);
    const auto cj = std::min<std::int32_t>(static_cast<std::int32_t>(std::floor(pj)), kCells - 1);
    const double u = pi - ci;
    const double v = pj - cj;
    const double h00 = world_height(q_at(p, ci, cj));
    const double h10 = world_height(q_at(p, ci + 1, cj));
    const double h01 = world_height(q_at(p, ci, cj + 1));
    const double h11 = world_height(q_at(p, ci + 1, cj + 1));
    if (u >= v) {
        return h00 + u * (h10 - h00) + v * (h11 - h10);
    }
    return h00 + v * (h01 - h00) + u * (h11 - h01);
}

render::TerrainLodDraw fixed_morph(TerrainTileKey k, float morph) {
    render::TerrainLodDraw d{};
    d.enabled = true;
    d.grid_origin = kOrigin;
    d.base_x = k.coord.x * kCells;
    d.base_z = k.coord.z * kCells;
    d.level = k.level;
    d.camera = {kOrigin.x, 5000.0f, kOrigin.z}; // ~5 km away: d is far past any start below
    if (morph == 0.0f) {
        d.morph_start = 1.0e9f; // never reached
        d.morph_end = 2.0e9f;
    } else {
        d.morph_start = 0.0f; // long passed
        d.morph_end = 1.0f;
    }
    return d;
}

} // namespace

TEST_CASE(
    "m19.8d2: (a) at morph 0 a vertex is its own sample, bit for bit; at morph 1 it is on the "
    "parent's triangulated surface") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(4, 4, 3);
    const Uploaded up(*device, w);
    Probe probe(*device);

    for (const TerrainTileKey child : {TerrainTileKey{0, {3, 2}}, TerrainTileKey{1, {0, 1}}}) {
        const TerrainTileKey parent{child.level + 1, assets::terrain_parent_coord(child.coord)};
        const assets::HeightfieldAsset& ca = w.tiles.at(child);
        const assets::HeightfieldAsset& pa = w.tiles.at(parent);
        const auto m0 =
            probe_leaves(probe, up, {ProbeDraw{nullptr, {}, fixed_morph(child, 0.0f)}}, {child})[0];
        const auto m1 =
            probe_leaves(probe, up, {ProbeDraw{nullptr, {}, fixed_morph(child, 1.0f)}}, {child})[0];
        // The child's offset inside its parent, in PARENT samples.
        const std::int32_t off_i = (child.coord.x - 2 * parent.coord.x) * (kCells / 2);
        const std::int32_t off_j = (child.coord.z - 2 * parent.coord.z) * (kCells / 2);

        int bit_equal_m0 = 0;
        int even_bit_equal_m1 = 0;
        int odd_on_parent = 0;
        int odd_odd = 0;
        int wrong_diagonal_rejected = 0;
        float worst_m1 = 0.0f;
        float max_y = 0.0f;
        for (const GpuVertex& v : m0) {
            max_y = std::max(max_y, std::fabs(bits_float(v.yb)));
        }
        // DERIVED: an odd vertex at morph 1 is fl(0.5 · fl(ya + yb)) — one rounding of the exact
        // midpoint, ≤ ½ ULP of its magnitude — and the parent's surface there is that exact
        // midpoint (the vertex IS the midpoint of a parent edge or diagonal, in x and z exactly,
        // since this world's positions are exact). One ULP of the largest height is the bound.
        const float bound = ulp(max_y);
        for (std::int32_t j = 0; j < kCells + 1; ++j) {
            for (std::int32_t i = 0; i < kCells + 1; ++i) {
                const std::size_t n =
                    static_cast<std::size_t>(j) * kN + static_cast<std::size_t>(i);
                const std::int64_t gx = std::int64_t{child.coord.x} * kCells + i;
                const std::int64_t gz = std::int64_t{child.coord.z} * kCells + j;
                const std::uint32_t xb = float_bits(world_x(gx, child.level));
                const std::uint32_t zb = float_bits(world_z(gz, child.level));
                const float own = world_height(q_at(ca, i, j));
                // Morph 0: the vertex is the tile's own sample, BIT for bit, in all three axes.
                const bool exact0 = m0[n].xb == xb && m0[n].zb == zb && m0[n].yb == float_bits(own);
                bit_equal_m0 += exact0 ? 1 : 0;
                // Only y moves: x and z are the same bits at morph 1.
                CHECK(m1[n].xb == xb);
                CHECK(m1[n].zb == zb);
                const float y1 = bits_float(m1[n].yb);
                if (i % 2 == 0 && j % 2 == 0) {
                    // An even vertex IS a parent vertex: the parent's own sample, bit for bit.
                    const float parent_y = world_height(q_at(pa, off_i + i / 2, off_j + j / 2));
                    even_bit_equal_m1 += m1[n].yb == float_bits(parent_y) ? 1 : 0;
                    continue;
                }
                const double surf = parent_surface(pa, off_i + i / 2.0, off_j + j / 2.0);
                const double err = std::fabs(static_cast<double>(y1) - surf);
                worst_m1 = std::max(worst_m1, static_cast<float>(err));
                odd_on_parent += err <= bound ? 1 : 0;
                if (i % 2 == 1 && j % 2 == 1) {
                    // The built-in falsification: the OTHER diagonal's pair would put this vertex
                    // on a different surface, far outside the bound.
                    ++odd_odd;
                    const double wrong =
                        0.5 * (static_cast<double>(world_height(q_at(ca, i + 1, j - 1))) +
                               world_height(q_at(ca, i - 1, j + 1)));
                    wrong_diagonal_rejected += std::fabs(y1 - wrong) > 10.0 * bound ? 1 : 0;
                }
            }
        }
        CHECK(bit_equal_m0 == static_cast<int>(kN * kN));
        CHECK(even_bit_equal_m1 == static_cast<int>((kN + 1) / 2 * ((kN + 1) / 2)));
        CHECK(odd_on_parent == static_cast<int>(kN * kN - (kN + 1) / 2 * ((kN + 1) / 2)));
        CHECK(odd_odd == 16);
        CHECK(wrong_diagonal_rejected >= 14);
        MESSAGE("m19.8d2 (a) level " << child.level << ": morph 0 bit-exact at " << bit_equal_m0
                                     << "/81; morph 1 worst |y - parent surface| = " << worst_m1
                                     << " m (bound " << bound << " m); the other diagonal is "
                                     << "rejected at " << wrong_diagonal_rejected << "/" << odd_odd
                                     << " (odd, odd) vertices");
    }
}

// ── (b) shared edges, every orientation, with and without fallback ──────────────────────────────

TEST_CASE("m19.8d2: (b) along every shared edge both tiles' vertices lie on one polyline — every "
          "orientation, with and without fallback") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(16, 16, 4);
    const render::TerrainLodRanges ranges = render::terrain_lod_ranges(w.world, test_view());
    const std::uint32_t top = w.world.level_count() - 1;
    const Uploaded up(*device, w);
    Probe probe(*device);
    const std::vector<PathFrame> path = camera_path(w);

    // The classes required: side (4) × relation (same, coarser, finer) × mode (ideal, fallback).
    std::set<std::tuple<int, int, int>> classes;
    std::uint64_t same_level_vertices = 0;
    std::uint64_t partially_morphed_shared = 0; // witness for the per-vertex morph
    std::uint64_t t_junctions = 0;
    std::uint64_t forced_t_junctions = 0; // T-junctions whose DISTANCE morph was < 1: the edge bit
    double worst_t = 0.0;
    std::uint64_t frames_checked = 0;

    for (std::size_t fi = 0; fi < path.size(); fi += 23) {
        const core::Vec3 eye = path[fi].eye;
        for (int mode = 0; mode < 2; ++mode) {
            const render::TerrainLodSelection sel =
                mode == 0 ? render::select_terrain_lod(
                                w.world,
                                ranges,
                                eye,
                                [&](TerrainTileKey k) { return w.world.find(k) != nullptr; })
                          : render::select_terrain_lod(w.world, ranges, eye, [&](TerrainTileKey k) {
                                return forced_fallback_usable(k, top, fi);
                            });
            std::vector<TerrainTileKey> keys;
            std::vector<ProbeDraw> draws;
            for (const TerrainLodLeaf& leaf : sel.leaves) {
                keys.push_back(leaf.key);
                draws.push_back({nullptr, {}, lod_draw(ranges, leaf, eye)});
            }
            const auto verts = probe_leaves(probe, up, draws, keys);
            ++frames_checked;
            // level-0 tile → leaf index
            std::vector<int> owner(static_cast<std::size_t>(w.nx * w.nz), -1);
            for (std::size_t n = 0; n < keys.size(); ++n) {
                const std::int32_t s = 1 << keys[n].level;
                for (std::int32_t z = keys[n].coord.z * s; z < (keys[n].coord.z + 1) * s; ++z) {
                    for (std::int32_t x = keys[n].coord.x * s; x < (keys[n].coord.x + 1) * s; ++x) {
                        owner[static_cast<std::size_t>(z * w.nx + x)] = static_cast<int>(n);
                    }
                }
            }
            for (std::size_t a = 0; a < keys.size(); ++a) {
                const TerrainTileKey ka = keys[a];
                const std::int32_t s = 1 << ka.level;
                for (int side = 0; side < 4; ++side) {
                    // side: 0 = −x, 1 = +x, 2 = −z, 3 = +z (TerrainLodEdge order)
                    std::set<int> across;
                    for (std::int32_t t = 0; t < s; ++t) {
                        std::int32_t x = 0;
                        std::int32_t z = 0;
                        if (side < 2) {
                            x = side == 0 ? ka.coord.x * s - 1 : (ka.coord.x + 1) * s;
                            z = ka.coord.z * s + t;
                        } else {
                            z = side == 2 ? ka.coord.z * s - 1 : (ka.coord.z + 1) * s;
                            x = ka.coord.x * s + t;
                        }
                        if (x >= 0 && z >= 0 && x < w.nx && z < w.nz) {
                            across.insert(owner[static_cast<std::size_t>(z * w.nx + x)]);
                        }
                    }
                    for (const int b : across) {
                        const TerrainTileKey kb = keys[static_cast<std::size_t>(b)];
                        const int rel = kb.level == ka.level ? 0 : kb.level > ka.level ? 1 : 2;
                        classes.insert({side, rel, mode});
                        if (rel == 2) {
                            continue; // checked from the finer side
                        }
                        REQUIRE(kb.level <= ka.level + 1); // 2:1 — anything else is (c)'s failure
                        // A's edge vertices, by along-edge index t; B's facing edge.
                        const bool along_z = side < 2;
                        const std::int32_t ia = side == 0 ? 0 : side == 1 ? kCells : -1;
                        const std::int32_t ja = side == 2 ? 0 : side == 3 ? kCells : -1;
                        const std::int32_t ib = side == 0 ? kCells : side == 1 ? 0 : -1;
                        const std::int32_t jb = side == 2 ? kCells : side == 3 ? 0 : -1;
                        const std::int64_t base_a = along_z ? std::int64_t{ka.coord.z} * kCells
                                                            : std::int64_t{ka.coord.x} * kCells;
                        const std::int64_t base_b = along_z ? std::int64_t{kb.coord.z} * kCells
                                                            : std::int64_t{kb.coord.x} * kCells;
                        const auto vert = [&](std::size_t tile,
                                              std::int32_t i_fixed,
                                              std::int32_t j_fixed,
                                              std::int64_t t) -> const GpuVertex& {
                            const std::int64_t i = i_fixed >= 0 ? i_fixed : t;
                            const std::int64_t j = j_fixed >= 0 ? j_fixed : t;
                            return verts[tile][static_cast<std::size_t>(j * kN + i)];
                        };
                        for (std::int64_t t = 0; t <= kCells; ++t) {
                            const GpuVertex& va = vert(a, ia, ja, t);
                            const std::int64_t g = base_a + t; // global along-edge index, level A
                            const std::uint32_t perp_a = along_z ? va.xb : va.zb;
                            if (rel == 0) {
                                const GpuVertex& vb =
                                    vert(static_cast<std::size_t>(b), ib, jb, g - base_b);
                                // Same level: ONE vertex, drawn twice — the same bits.
                                REQUIRE(va.xb == vb.xb);
                                REQUIRE(va.yb == vb.yb);
                                REQUIRE(va.zb == vb.zb);
                                ++same_level_vertices;
                                if (t % 2 == 1) {
                                    const assets::HeightfieldAsset& ta = w.tiles.at(ka);
                                    const std::int32_t ti =
                                        ia >= 0 ? ia : static_cast<std::int32_t>(t);
                                    const std::int32_t tj =
                                        ja >= 0 ? ja : static_cast<std::int32_t>(t);
                                    const float own = world_height(q_at(ta, ti, tj));
                                    const float y = bits_float(va.yb);
                                    const float target =
                                        0.5f * (along_z ? world_height(q_at(ta, ti, tj - 1)) +
                                                              world_height(q_at(ta, ti, tj + 1))
                                                        : world_height(q_at(ta, ti - 1, tj)) +
                                                              world_height(q_at(ta, ti + 1, tj)));
                                    if (std::fabs(y - own) > 1e-4f &&
                                        std::fabs(y - target) > 1e-4f) {
                                        ++partially_morphed_shared;
                                    }
                                }
                                continue;
                            }
                            // B is one level coarser. g even: a coarse vertex — the same bits.
                            if (g % 2 == 0) {
                                const GpuVertex& vb =
                                    vert(static_cast<std::size_t>(b), ib, jb, g / 2 - base_b);
                                REQUIRE(va.xb == vb.xb);
                                REQUIRE(va.yb == vb.yb);
                                REQUIRE(va.zb == vb.zb);
                                continue;
                            }
                            // g odd: a T-junction on the coarse segment between g−1 and g+1.
                            const GpuVertex& v0 =
                                vert(static_cast<std::size_t>(b), ib, jb, (g - 1) / 2 - base_b);
                            const GpuVertex& v1 =
                                vert(static_cast<std::size_t>(b), ib, jb, (g + 1) / 2 - base_b);
                            REQUIRE(perp_a == (along_z ? v0.xb : v0.zb)); // on the edge's line
                            const double s0 = bits_float(along_z ? v0.zb : v0.xb);
                            const double s1 = bits_float(along_z ? v1.zb : v1.xb);
                            const double sa = bits_float(along_z ? va.zb : va.xb);
                            const double y0 = bits_float(v0.yb);
                            const double y1 = bits_float(v1.yb);
                            const double ya = bits_float(va.yb);
                            const double line = y0 + (y1 - y0) * (sa - s0) / (s1 - s0);
                            // DERIVED bound: the shader's average rounds once (½ ULP of y) and the
                            // vertex may sit off the segment's midpoint by the rounding of its
                            // along-edge position (½ ULP of s, times the segment's slope); both
                            // ends likewise. One ULP of each, with the slope, bounds the residual.
                            const double tol =
                                ulp(static_cast<float>(std::max(std::fabs(y0), std::fabs(y1)))) +
                                std::fabs(y1 - y0) / (s1 - s0) * 1.5 *
                                    ulp(static_cast<float>(std::max(std::fabs(s0), std::fabs(s1))));
                            const double err = std::fabs(ya - line);
                            worst_t = std::max(worst_t, err);
                            CHECK(err <= tol);
                            ++t_junctions;
                            // Would the DISTANCE morph alone have put it there? (The edge bit's
                            // witness: under fallback it often would not have.)
                            const float dx = bits_float(va.xb) - eye.x;
                            const float dy = bits_float(va.yb) - eye.y;
                            const float dz = bits_float(va.zb) - eye.z;
                            const render::TerrainLodLevel& lv = ranges.levels[ka.level];
                            if (std::sqrt(dx * dx + dy * dy + dz * dz) < lv.morph_end) {
                                ++forced_t_junctions;
                            }
                        }
                    }
                }
            }
        }
    }
    for (int side = 0; side < 4; ++side) {
        for (int rel = 0; rel < 3; ++rel) {
            for (int mode = 0; mode < 2; ++mode) {
                CHECK_MESSAGE(classes.contains({side, rel, mode}),
                              "orientation not exercised: side " << side << " relation " << rel
                                                                 << " mode " << mode);
            }
        }
    }
    CHECK(partially_morphed_shared > 0);
    CHECK(forced_t_junctions > 0);
    CHECK(t_junctions > 0);
    MESSAGE("m19.8d2 (b): " << frames_checked << " selections; " << classes.size()
                            << "/24 orientation classes; " << same_level_vertices
                            << " shared same-level vertices bit-identical ("
                            << partially_morphed_shared << " of them mid-morph); " << t_junctions
                            << " T-junctions, worst " << worst_t << " m off the coarse edge ("
                            << forced_t_junctions
                            << " held there by the edge bit, not by distance)");
}

// ── (e) the switch is pop-free ──────────────────────────────────────────────────────────────────

TEST_CASE("m19.8d2: (e) the frame a node switches level, the drawn surface does not move") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(16, 16, 4);
    const render::TerrainLodRanges ranges = render::terrain_lod_ranges(w.world, test_view());
    const Uploaded up(*device, w);
    Probe probe(*device);
    const std::vector<PathFrame> path = camera_path(w);

    // The whole world, 0.5 m per pixel.
    constexpr std::uint32_t kPx = 256;
    const TopDown view = top_down(kOrigin.x, kOrigin.z, 0.5f, kPx, kPx);
    const auto draws_for = [&](const render::TerrainLodSelection& sel, core::Vec3 eye, bool morph) {
        std::vector<ProbeDraw> d;
        for (const TerrainLodLeaf& leaf : sel.leaves) {
            render::TerrainLodDraw lod = lod_draw(ranges, leaf, eye);
            if (!morph) {
                lod.morph_start = std::numeric_limits<float>::infinity();
                lod.morph_end = std::numeric_limits<float>::infinity();
                lod.coarser_edges = 0;
            }
            d.push_back({&up.tile(leaf.key), view.view_proj, lod});
        }
        return d;
    };
    const auto all = [&](TerrainTileKey k) { return w.world.find(k) != nullptr; };
    // DERIVED as in m19.3 (terrain_height_test.cpp): the old and new surfaces are the SAME planes
    // in exact arithmetic (the children are fully morphed, the parent is not morphing), so they
    // differ only by f32 vertex rounding (~1 ULP of 25 m, 2e-6 m) and by the rasteriser's
    // barycentric interpolation, which Vulkan does not specify exactly — the m19.3 margin, 1 mm,
    // covers it with room. The no-morph control below shows the margin discriminates.
    constexpr float kPopBound = 1.0e-3f;
    render::TerrainLodSelection prev =
        render::select_terrain_lod(w.world, ranges, path[0].eye, all);
    int switches = 0;
    float worst = 0.0f;
    float worst_unmorphed = 0.0f;
    for (std::size_t f = 1; f < path.size() && switches < 24; ++f) {
        const render::TerrainLodSelection cur =
            render::select_terrain_lod(w.world, ranges, path[f].eye, all);
        const bool changed = !same_leaves(prev.leaves, cur.leaves);
        if (changed && path[f].slow) {
            ++switches;
            const core::Vec3 eye = path[f].eye;
            const auto before = probe_surface(probe, draws_for(prev, eye, true), kPx, kPx);
            const auto after = probe_surface(probe, draws_for(cur, eye, true), kPx, kPx);
            const auto before_flat = probe_surface(probe, draws_for(prev, eye, false), kPx, kPx);
            const auto after_flat = probe_surface(probe, draws_for(cur, eye, false), kPx, kPx);
            std::uint32_t covered = 0;
            for (std::size_t p = 0; p < std::size_t{kPx} * kPx; ++p) {
                const std::uint32_t a = word_at(before, p);
                const std::uint32_t b = word_at(after, p);
                if (a == 0 || b == 0) {
                    continue;
                }
                ++covered;
                worst = std::max(worst, std::fabs(bits_float(a) - bits_float(b)));
                worst_unmorphed = std::max(worst_unmorphed,
                                           std::fabs(bits_float(word_at(before_flat, p)) -
                                                     bits_float(word_at(after_flat, p))));
            }
            CHECK(covered == kPx * kPx);
        }
        prev = cur;
    }
    CHECK(switches >= 10);
    CHECK(worst <= kPopBound);
    // The margin DISCRIMINATES: without the morph the very same switches pop far past it.
    CHECK(worst_unmorphed > 10.0f * kPopBound);
    MESSAGE("m19.8d2 (e): " << switches
                            << " level switches on the slow path; worst per-pixel change " << worst
                            << " m (bound " << kPopBound << " m); without the morph "
                            << worst_unmorphed << " m");
}

// ── Through the residency: (g) pinned roots, (f) determinism, the 8d1 handoff, (h) m19.8a ───────

namespace {

// Write every tile of `w` into `dir` as a cooked file.
void write_lod_world(const fs::path& dir, const LodWorld& w) {
    for (const auto& [k, a] : w.tiles) {
        write_file(dir / tile_path(k), encode_heightfield(a));
    }
}

// Every load requested so far finished and promoted, so a frame sees the same state whatever
// order the workers completed in.
void settle(assets::AssetServer& server) {
    server.wait_for_pending_loads();
    server.pump();
}

render::TerrainLight flat_light() {
    render::TerrainLight l{};
    l.sun_irradiance = 0.0f;
    l.ambient = 1.0f;
    return l;
}

// One residency frame, drawn for real into a small target and ended blocking.
void lod_frame(rhi::Device& device,
               assets::AssetServer& server,
               render::TerrainResidency& residency,
               core::Vec3 eye) {
    settle(server);
    residency.begin_frame(eye);
    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr = graph.create_texture({{16, 16}, render::kHdrFormat, "lod-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{16, 16}, render::kDepthFormat, "lod-depth"});
    const render::RGColorAttachment clears[] = {
        {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}}};
    const render::RGDepthAttachment dclear{
        depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
    render::RenderGraph::RasterPassDesc cd{};
    cd.colors = clears;
    cd.depth = &dclear;
    graph.add_raster_pass("lod-clear", cd, [](rhi::CommandBuffer&) {});
    const core::Mat4 vp = top_down(kOrigin.x, kOrigin.z, 8.0f, 16, 16).view_proj;
    residency.add(graph, hdr, depth, vp, eye, flat_light());
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    residency.end_frame_blocking();
}

} // namespace

TEST_CASE("m19.8d2: (g) under maximum pressure the pinned roots stay resident and the world stays "
          "covered; a root cover over budget is refused") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("lod-pinned");
    const LodWorld w = make_lod_world(16, 8, 4); // two roots, side by side
    REQUIRE(w.world.tiles(3).size() == 2);
    write_lod_world(dir.path, w);
    const std::vector<PathFrame> path = camera_path(w);

    for (const std::uint32_t slots : {2u, 10u}) {
        core::JobSystem jobs(2);
        assets::AssetServer server(jobs);
        render::TerrainPass pass(*device);
        render::TerrainResidencyConfig cfg{};
        cfg.slots = slots;
        cfg.lod = test_view();
        render::TerrainResidency residency(*device, pass, server, w.world, dir.path, nullptr, cfg);
        REQUIRE(residency.lod());
        std::uint32_t warm = 0; // frames before the roots were all resident
        std::uint32_t worst_step = 0;
        std::uint32_t max_levels = 0;
        for (std::size_t f = 0; f < path.size(); ++f) {
            lod_frame(*device, server, residency, path[f].eye);
            const render::TerrainResidencyStats& s = residency.stats();
            REQUIRE(s.resident_slots + s.retiring_slots <= slots);
            if (s.pinned_roots < 2) {
                REQUIRE(f < 2); // requested on frame 1, resident by frame 2 — and never lost
                ++warm;
                continue;
            }
            const Cover c = measure_cover(w, residency.selection().leaves);
            REQUIRE(c.exact);
            REQUIRE(c.max_step <= 1);
            worst_step = std::max(worst_step, c.max_step);
            std::set<std::uint32_t> lv;
            for (const TerrainLodLeaf& leaf : residency.selection().leaves) {
                lv.insert(leaf.key.level);
            }
            max_levels = std::max(max_levels, static_cast<std::uint32_t>(lv.size()));
        }
        const render::TerrainResidencyStats& s = residency.stats();
        CHECK(s.pinned_evictions == 0);
        CHECK(s.pinned_roots == 2);
        CHECK(s.uncovered_draws == 2u * warm); // only before the roots arrived
        CHECK(s.fallback_draws > 0);           // pressure lowered detail...
        CHECK(s.fallback_appearance_draws > 0);
        CHECK(s.lod_draws == s.draws);
        CHECK(s.stale_draws == 0);
        CHECK(s.refused_parents == 0);
        CHECK(s.coincidence_checks > 0);
        CHECK(residency.refusals().coincidence_mismatches == 0);
        if (slots == 10) {
            CHECK(max_levels >= 2); // ...but where it could, detail came through
        }
        MESSAGE("m19.8d2 (g) " << slots << " slots: " << path.size()
                               << " frames, roots resident from frame " << warm + 1 << ", "
                               << s.fallback_draws << " fallback tile-frames, " << s.uploads
                               << " uploads, " << s.evictions << " evictions ("
                               << s.pinned_evictions << " of a root), up to " << max_levels
                               << " levels drawn, " << s.coincidence_checks
                               << " parent/child checks");
    }

    // A budget smaller than the root cover cannot promise coverage: refused at construction.
    core::JobSystem jobs(1);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 1;
    cfg.lod = test_view();
    render::TerrainResidency refused(*device, pass, server, w.world, dir.path, nullptr, cfg);
    CHECK(refused.stats().root_cover_refusals == 1);
    for (int f = 0; f < 3; ++f) {
        lod_frame(*device, server, refused, path[0].eye);
    }
    CHECK(refused.selection().leaves.empty());
    CHECK(refused.stats().draws == 0);
    CHECK(refused.stats().heightfield_requests == 0);
}

TEST_CASE("m19.8d2: (f) the same camera path draws the same selections whatever order the loads "
          "complete in") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("lod-determinism");
    const LodWorld w = make_lod_world(16, 16, 4);
    write_lod_world(dir.path, w);
    const std::vector<PathFrame> path = camera_path(w);

    std::vector<std::vector<TerrainLodLeaf>> runs[2];
    std::uint64_t fallback[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        // One worker completes loads in request order; eight complete them in whatever order.
        core::JobSystem jobs(run == 0 ? 1u : 8u);
        assets::AssetServer server(jobs);
        render::TerrainPass pass(*device);
        render::TerrainResidencyConfig cfg{};
        cfg.slots = 24; // pressure: the ideal selection wants more than this on most frames
        cfg.lod = test_view();
        render::TerrainResidency residency(*device, pass, server, w.world, dir.path, nullptr, cfg);
        for (const PathFrame& f : path) {
            lod_frame(*device, server, residency, f.eye);
            runs[run].push_back(residency.selection().leaves);
        }
        fallback[run] = residency.stats().fallback_draws;
    }
    REQUIRE(runs[0].size() == runs[1].size());
    std::size_t identical = 0;
    for (std::size_t f = 0; f < runs[0].size(); ++f) {
        identical += same_leaves(runs[0][f], runs[1][f]) ? 1 : 0;
    }
    CHECK(identical == runs[0].size());
    CHECK(fallback[0] == fallback[1]);
    CHECK(fallback[0] > 0); // not vacuous: pressure made residency decide what was drawn
    MESSAGE("m19.8d2 (f): " << identical << "/" << runs[0].size()
                            << " frames identical across 1 and 8 load workers; " << fallback[0]
                            << " fallback tile-frames in each");
}

TEST_CASE("m19.8d2: a parent whose samples disagree with its child is refused, counted, never "
          "drawn — and its area stays covered") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("lod-coincide");
    LodWorld w = make_lod_world(4, 4, 3);
    // Corrupt a MID-LEVEL tile: level-1 (0, 0)'s sample (2, 2). It lies over level-0 child (0, 0)'s
    // sample (4, 4) — and it is also a sample the ROOT holds (root sample (1, 1)). So the first
    // pair to disagree is (root, bad), and it blames the innocent root; the level-0 child then
    // convicts `bad`, and the root's refusal must be retracted.
    const TerrainTileKey bad{1, {0, 0}};
    const TerrainTileKey root{2, {0, 0}};
    w.tiles.at(bad).samples[2 + 2 * kN] += 7;
    write_lod_world(dir.path, w);

    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 21; // everything fits: the refusal, not pressure, decides
    cfg.lod = test_view();
    render::TerrainResidency residency(*device, pass, server, w.world, dir.path, nullptr, cfg);
    const core::Vec3 eye{kOrigin.x + 3.0f, 27.0f, kOrigin.z + 3.0f}; // over the doctored corner
    constexpr int kFrames = 10;
    int covered_frames = 0;
    for (int f = 0; f < kFrames; ++f) {
        lod_frame(*device, server, residency, eye);
        for (const TerrainLodLeaf& leaf : residency.selection().leaves) {
            CHECK_FALSE(leaf.key == bad); // never drawn — not even before it was convicted
        }
        covered_frames += measure_cover(w, residency.selection().leaves).exact ? 1 : 0;
    }
    const render::TerrainResidencyStats& s = residency.stats();
    CHECK(residency.refusals().coincidence_mismatches == 2); // (root, bad), then (bad, level 0)
    CHECK(s.refused_parents == 2);
    CHECK(s.refusals_retracted == 1); // the root's
    CHECK_FALSE(residency.resident(bad).is_valid());
    CHECK(residency.resident(root).is_valid());
    // The last frames are covered by the root: its children cannot all be drawn, so it falls back.
    CHECK(measure_cover(w, residency.selection().leaves).exact);
    CHECK(s.fallback_draws > 0);
    MESSAGE("m19.8d2 handoff: " << s.coincidence_checks << " pairs checked, "
                                << residency.refusals().coincidence_mismatches << " mismatches, "
                                << s.refusals_retracted << " retracted; covered " << covered_frames
                                << "/" << kFrames << " frames, " << s.uncovered_draws
                                << " uncovered root-frames while the root was reloaded");
}

TEST_CASE("m19.8d2: (h) a world without a chain is driven exactly as m19.8a drives it") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("lod-level0");
    const LodWorld chain = make_lod_world(4, 4, 1);
    write_lod_world(dir.path, chain);
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 4;
    cfg.activation_radius = 4.0f;
    cfg.retention_radius = 8.0f;
    cfg.lod = test_view();
    render::TerrainResidency residency(*device, pass, server, chain.world, dir.path, nullptr, cfg);
    CHECK_FALSE(residency.lod());
    CHECK(residency.ranges().levels.empty());
    for (const PathFrame& f : camera_path(chain)) {
        lod_frame(*device, server, residency, f.eye);
        CHECK(residency.stats().resident_slots <= 4);
    }
    const render::TerrainResidencyStats& s = residency.stats();
    CHECK(s.draws > 0);
    CHECK(s.lod_draws == 0);
    CHECK(s.fallback_draws == 0);
    CHECK(s.fallback_appearance_draws == 0);
    CHECK(s.pinned_roots == 0);
    CHECK(s.coincidence_checks == 0);
    CHECK(s.root_cover_refusals == 0);
    CHECK(s.missing.total() > 0); // m19.8a's holes, counted as before
    CHECK(residency.selection().leaves.empty());
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// m19.8d3 (ADR-0072): BAKED APPEARANCE for coarse tiles, faded by the geometry's morph factor.
//
// The bake's CONTENT is proven where it is made (tools/asset-pipeline/src/terrain_bake.rs: byte-
// equal to a brute force; shared edges byte-identical). These proofs are about what the renderer
// does with a bake, so they use a SYNTHETIC one — hashed bytes that are a function of (level,
// global sample), which is exactly the property the cook guarantees: a texel two tiles share is
// the same bytes in both. Hashed bytes are the hardest case for every claim here: neighbouring
// texels differ by up to the full range, so any sampling slip shows at full contrast.
//
// Everything is drawn through TerrainPass::add into the HDR target with the sun off and ambient
// 1. With no sky that leaves  radiance = (1 − metallic)·base + mix(0.04, base, metallic)
//                                      = base + 0.04·(1 − metallic),
// so a pixel's "shaded base colour" is read straight off the frame.
// ════════════════════════════════════════════════════════════════════════════════════════════════

namespace {

std::uint8_t bake_byte(std::uint32_t level, std::int64_t gx, std::int64_t gz, std::uint32_t c) {
    std::uint64_t h = (static_cast<std::uint64_t>(level) + 1) * 0xD6E8FEB86659FD93ull ^
                      static_cast<std::uint64_t>(gx) * 0x9E3779B97F4A7C15ull ^
                      static_cast<std::uint64_t>(gz) * 0xC2B2AE3D27D4EB4Full ^
                      (static_cast<std::uint64_t>(c) + 1) * 0x165667B19E3779F9ull;
    h ^= h >> 32;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return static_cast<std::uint8_t>(h & 0xFF);
}

struct Bake {
    std::vector<std::byte> color;    // sRGB bytes, A = 255
    std::vector<std::byte> material; // R = metallic, G = roughness
};

// The bake of tile `k`: texel (i, j) from the tile's GLOBAL level-L sample index, so same-level
// neighbours agree on a shared edge by construction (the cook's guarantee).
Bake make_bake(TerrainTileKey k) {
    Bake b;
    for (std::int32_t j = 0; j <= kCells; ++j) {
        for (std::int32_t i = 0; i <= kCells; ++i) {
            const std::int64_t gx = std::int64_t{k.coord.x} * kCells + i;
            const std::int64_t gz = std::int64_t{k.coord.z} * kCells + j;
            for (std::uint32_t c = 0; c < 3; ++c) {
                b.color.push_back(static_cast<std::byte>(bake_byte(k.level, gx, gz, c)));
            }
            b.color.push_back(std::byte{255});
            b.material.push_back(static_cast<std::byte>(bake_byte(k.level, gx, gz, 3)));
            b.material.push_back(static_cast<std::byte>(bake_byte(k.level, gx, gz, 4)));
            b.material.push_back(std::byte{0});
            b.material.push_back(std::byte{255});
        }
    }
    return b;
}

render::TerrainBakeTexels texels_of(const Bake& b) {
    return {kN, kN, b.color, b.material};
}

// sRGB → linear, IEC 61966-2-1: what an *_SRGB format applies on a fetch.
double bake_srgb(std::uint8_t byte) {
    const double c = byte / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// How far a GPU's sRGB decode of `byte` may sit from the IEC curve: HALF AN 8-BIT CODE, measured
// in the ENCODED space — the tolerance the format conversion rules give (D3D's is stated exactly
// so; Vulkan defers to the Khronos Data Format spec, which hardware built for both satisfies the
// same way). In linear light that is the curve's slope times half a code, so it is NOT one
// number: 1.5e-4 near black, 4.3e-3 near white. A flat bound would be either too loose for the
// dark texels to mean anything or too tight for the bright ones to pass.
double bake_srgb_tolerance(std::uint8_t byte) {
    const auto at = [](double code) {
        const double c = std::clamp(code, 0.0, 255.0) / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return std::max(at(byte + 0.5) - at(byte), at(byte) - at(byte - 0.5));
}

// The frame value terrain.frag should produce from a bake at sample position (si, sj) of tile `k`
// — the shader's hand-written bilinear, in double — channel c of RGB. `decode_tolerance`, when
// given, receives the same blend of the texels' sRGB decode tolerances.
double bake_radiance(TerrainTileKey k,
                     double si,
                     double sj,
                     std::uint32_t c,
                     double* decode_tolerance = nullptr) {
    const auto i0 = static_cast<std::int32_t>(std::floor(si));
    const auto j0 = static_cast<std::int32_t>(std::floor(sj));
    const std::int32_t i1 = std::min(i0 + 1, kCells);
    const std::int32_t j1 = std::min(j0 + 1, kCells);
    const double fx = si - i0;
    const double fz = sj - j0;
    const auto at = [&](std::int32_t i, std::int32_t j, std::uint32_t ch) {
        const std::uint8_t byte = bake_byte(k.level,
                                            std::int64_t{k.coord.x} * kCells + i,
                                            std::int64_t{k.coord.z} * kCells + j,
                                            ch);
        return ch < 3 ? bake_srgb(byte) : byte / 255.0;
    };
    const auto lerp = [&](std::uint32_t ch) {
        return (1.0 - fz) * ((1.0 - fx) * at(i0, j0, ch) + fx * at(i1, j0, ch)) +
               fz * ((1.0 - fx) * at(i0, j1, ch) + fx * at(i1, j1, ch));
    };
    if (decode_tolerance != nullptr) {
        const auto tol = [&](std::int32_t i, std::int32_t j) {
            return bake_srgb_tolerance(bake_byte(k.level,
                                                 std::int64_t{k.coord.x} * kCells + i,
                                                 std::int64_t{k.coord.z} * kCells + j,
                                                 c));
        };
        *decode_tolerance = (1.0 - fz) * ((1.0 - fx) * tol(i0, j0) + fx * tol(i1, j0)) +
                            fz * ((1.0 - fx) * tol(i0, j1) + fx * tol(i1, j1));
    }
    return lerp(c) + 0.04 * (1.0 - lerp(3));
}

// The chain uploaded, every parent with its bake.
struct BakedUpload {
    render::TerrainPass pass;
    std::map<TerrainTileKey, render::TerrainTileId> ids;

    BakedUpload(rhi::Device& device, const LodWorld& w) : pass(device) {
        for (const auto& [k, a] : w.tiles) {
            const render::TerrainTileId id = pass.upload(a);
            REQUIRE(id != render::kInvalidTerrainTile);
            ids.emplace(k, id);
            if (k.level > 0) {
                const Bake b = make_bake(k);
                REQUIRE(pass.set_bake(id, texels_of(b)));
            }
        }
    }

    // `lod` with the tile's parent filled in, as TerrainResidency::draw_leaf does.
    render::TerrainLodDraw with_parent(TerrainTileKey k, render::TerrainLodDraw lod) const {
        const TerrainTileKey p{k.level + 1, assets::terrain_parent_coord(k.coord)};
        const auto it = ids.find(p);
        if (it != ids.end()) {
            lod.parent = it->second;
            lod.parent_quadrant = static_cast<std::uint32_t>(k.coord.x & 1) |
                                  (static_cast<std::uint32_t>(k.coord.z & 1) << 1);
        }
        return lod;
    }
};

float half_float(std::uint16_t h) {
    const int exp = (h >> 10) & 0x1F;
    const int mant = h & 0x3FF;
    const float sign = (h & 0x8000) != 0 ? -1.0f : 1.0f;
    if (exp == 0) {
        return sign * std::ldexp(static_cast<float>(mant), -24);
    }
    if (exp == 31) {
        return mant == 0 ? sign * std::numeric_limits<float>::infinity()
                         : std::numeric_limits<float>::quiet_NaN();
    }
    return sign * std::ldexp(static_cast<float>(mant | 0x400), exp - 25);
}

// One f16 ULP at magnitude m: what the RGBA16F target rounds a value of that size to.
double ulp16(double m) {
    int e = 0;
    std::frexp(std::max(std::fabs(m), 6.2e-5), &e); // below 2^-14 halves are subnormal
    return std::ldexp(1.0, e - 11);
}

// A top-down view like `top_down`, optionally turned 180° about the vertical. Vulkan's top-left
// rule gives a pixel centre lying EXACTLY on a tile's border to only one side of it, so a tile
// drawn alone covers its border pixels along two of its four edges; turned half a turn it covers
// the other two. Pixel centres map to pixel centres, so nothing else changes.
TopDown top_down_turn(float x0, float z0, float cell, std::uint32_t w, std::uint32_t h, bool turn) {
    const float hx = 0.5f * cell * static_cast<float>(w);
    const float hz = 0.5f * cell * static_cast<float>(h);
    const core::Vec3 eye{x0 + hx, 400.0f, z0 + hz};
    return {core::ortho(-hx, hx, -hz, hz, 0.0f, 800.0f) *
                core::look_at(eye, {eye.x, 0.0f, eye.z}, {0.0f, 0.0f, turn ? 1.0f : -1.0f}),
            w,
            h};
}

struct Shade {
    render::TerrainTileId id = render::kInvalidTerrainTile;
    core::Mat4 view_proj;
    render::TerrainLodDraw lod{};
};

// Draw through the ENGINE's pass into a w × h HDR target cleared to alpha 0; returns RGBA floats
// (alpha 1 = a terrain pixel).
std::vector<float> shade(rhi::Device& device,
                         render::TerrainPass& pass,
                         const std::vector<Shade>& draws,
                         std::uint32_t w,
                         std::uint32_t h,
                         const render::TerrainLight& light) {
    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr = graph.create_texture({{w, h}, render::kHdrFormat, "bake-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{w, h}, render::kDepthFormat, "bake-depth"});
    graph.export_texture(hdr);
    const render::RGColorAttachment clears[] = {
        {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    const render::RGDepthAttachment dclear{
        depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
    render::RenderGraph::RasterPassDesc cd{};
    cd.colors = clears;
    cd.depth = &dclear;
    graph.add_raster_pass("bake-clear", cd, [](rhi::CommandBuffer&) {});
    for (const Shade& d : draws) {
        pass.add(graph, hdr, depth, d.id, d.view_proj, {0.0f, 500.0f, 0.0f}, light, {}, d.lod);
    }
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    const std::vector<std::uint8_t> raw = read_back(device, graph.physical(hdr), w, h, 8);
    std::vector<float> out(std::size_t{w} * h * 4);
    for (std::size_t n = 0; n < out.size(); ++n) {
        std::uint16_t half = 0;
        std::memcpy(&half, &raw[n * 2], sizeof(half));
        out[n] = half_float(half);
    }
    return out;
}

// A view with ONE PIXEL PER VERTEX of tile `k`, pixel centres on the vertices, `margin` pixels
// around it.
TopDown vertex_view(TerrainTileKey k, std::uint32_t margin, bool turn) {
    const float cell = static_cast<float>(1u << k.level);
    const float x0 = world_x(std::int64_t{k.coord.x} * kCells, k.level) -
                     (static_cast<float>(margin) + 0.5f) * cell;
    const float z0 = world_z(std::int64_t{k.coord.z} * kCells, k.level) -
                     (static_cast<float>(margin) + 0.5f) * cell;
    return top_down_turn(x0, z0, cell, kN + 2 * margin, kN + 2 * margin, turn);
}

// The pixel of tile `k`'s vertex (i, j) in `v`.
std::size_t vertex_pixel(const TopDown& v, TerrainTileKey k, std::int32_t i, std::int32_t j) {
    const auto px = pixel_of(v,
                             world_x(std::int64_t{k.coord.x} * kCells + i, k.level),
                             10.0f,
                             world_z(std::int64_t{k.coord.z} * kCells + j, k.level));
    REQUIRE(px.has_value());
    return *px;
}

// A pixel centre sits ON its vertex in exact arithmetic; the rasteriser's f32 interpolation puts
// the sample coordinate within ~2^-20 of it, times at most the full texel range. 1e-4 covers that
// with two orders to spare and is below one f16 ULP of any value above 0.1.
constexpr double kInterpolationSlack = 1.0e-4;

} // namespace

TEST_CASE("m19.8d3: set_bake takes one texel per sample, owns it, and refuses anything else") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(2, 2, 2);
    render::TerrainPass pass(*device);
    const TerrainTileKey root{1, {0, 0}};
    const render::TerrainTileId id = pass.upload(w.tiles.at(root));
    REQUIRE(id != render::kInvalidTerrainTile);
    const std::uint64_t before = pass.tile_bytes(id);
    const Bake b = make_bake(root);

    render::TerrainBakeTexels wrong = texels_of(b);
    wrong.columns = kN - 1; // not this tile's grid
    CHECK_FALSE(pass.set_bake(id, wrong));
    wrong = texels_of(b);
    wrong.material = wrong.material.first(wrong.material.size() - 4); // a short span
    CHECK_FALSE(pass.set_bake(id, wrong));
    CHECK_FALSE(pass.set_bake(render::kInvalidTerrainTile, texels_of(b)));
    CHECK(pass.bakes_refused() == 3);
    CHECK(pass.tile_bytes(id) == before);
    CHECK_FALSE(pass.tile(id).bake_color.is_valid());

    REQUIRE(pass.set_bake(id, texels_of(b)));
    CHECK(pass.tile_bytes(id) == before + 2u * kN * kN * 4u);
    CHECK_FALSE(pass.set_bake(id, texels_of(b))); // already has one
    CHECK(pass.bakes_refused() == 4);
    CHECK(
        pass.release(id)); // destroys the bake with the tile (ASan and the validation layer watch)
    CHECK(pass.tile_bytes(id) == 0);
}

TEST_CASE("m19.8d3: (a) a parent shades each vertex with exactly its bake texel, and a fully "
          "morphed child with the parent's") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(4, 4, 3);
    BakedUpload up(*device, w);
    const render::TerrainLight light = flat_light();

    double worst = 0.0;       // |frame − prediction|
    double worst_share = 0.0; // the largest fraction of its own bound any value used
    double worst_dark = 0.0;  // |frame − prediction| over predictions below 0.1, in f16 ULPs
    std::uint32_t checked = 0;
    std::uint32_t wrong_texel_matches = 0;
    const auto compare = [&](const std::vector<float>& img,
                             std::size_t px,
                             TerrainTileKey bake_of,
                             double si,
                             double sj) {
        REQUIRE(img[px * 4 + 3] == 1.0f); // a terrain pixel
        for (std::uint32_t c = 0; c < 3; ++c) {
            double decode = 0.0;
            const double want = bake_radiance(bake_of, si, sj, c, &decode);
            const double got = img[px * 4 + c];
            // One f16 ULP (the target's rounding) + the sRGB decode bound + the interpolation.
            const double bound = ulp16(want) + decode + kInterpolationSlack;
            CHECK(std::fabs(got - want) <= bound);
            worst = std::max(worst, std::fabs(got - want));
            worst_share = std::max(worst_share, std::fabs(got - want) / bound);
            if (want < 0.1) {
                worst_dark = std::max(worst_dark, std::fabs(got - want) / ulp16(want));
            }
            ++checked;
            // The bound DISCRIMINATES: the texel one sample over would almost never pass it.
            const double other = bake_radiance(bake_of, si < kCells ? si + 1.0 : si - 1.0, sj, c);
            wrong_texel_matches += std::fabs(got - other) <= bound ? 1u : 0u;
        }
    };

    // Parents at two levels, unmorphed: their own bake, vertex by vertex. Both turns of the view,
    // so every border vertex is covered in one of them (the top-left rule).
    for (const TerrainTileKey k :
         {TerrainTileKey{1, {1, 0}}, TerrainTileKey{1, {0, 1}}, TerrainTileKey{2, {0, 0}}}) {
        std::set<std::pair<std::int32_t, std::int32_t>> seen;
        for (const bool turn : {false, true}) {
            const TopDown v = vertex_view(k, 1, turn);
            const auto img = shade(*device,
                                   up.pass,
                                   {{up.ids.at(k), v.view_proj, fixed_morph(k, 0.0f)}},
                                   v.w,
                                   v.h,
                                   light);
            for (std::int32_t j = 0; j <= kCells; ++j) {
                for (std::int32_t i = 0; i <= kCells; ++i) {
                    const std::size_t px = vertex_pixel(v, k, i, j);
                    if (img[px * 4 + 3] == 1.0f && seen.insert({i, j}).second) {
                        compare(img, px, k, i, j);
                    }
                }
            }
        }
        // Each turn covers the interior and two borders; the two corners where a covered border
        // meets an uncovered one belong to neither.
        CHECK(seen.size() >= kN * kN - 2);
    }
    const std::uint32_t parent_checks = checked;

    // A level-0 child at morph 1 (each quadrant): every vertex shades as the PARENT's bake at
    // ((offset + i) / 2, (offset + j) / 2) — a parent texel at even vertices, the midpoint of two
    // (or four) at odd ones.
    const TerrainTileKey parent{1, {1, 0}};
    for (const TerrainTileKey k : {TerrainTileKey{0, {2, 0}},
                                   TerrainTileKey{0, {3, 0}},
                                   TerrainTileKey{0, {2, 1}},
                                   TerrainTileKey{0, {3, 1}}}) {
        const TopDown v = vertex_view(k, 1, false);
        const auto img =
            shade(*device,
                  up.pass,
                  {{up.ids.at(k), v.view_proj, up.with_parent(k, fixed_morph(k, 1.0f))}},
                  v.w,
                  v.h,
                  light);
        for (std::int32_t j = 0; j <= kCells; ++j) {
            for (std::int32_t i = 0; i <= kCells; ++i) {
                const std::size_t px = vertex_pixel(v, k, i, j);
                if (img[px * 4 + 3] == 1.0f) {
                    compare(img,
                            px,
                            parent,
                            0.5 * ((k.coord.x & 1) * kCells + i),
                            0.5 * ((k.coord.z & 1) * kCells + j));
                }
            }
        }
    }
    CHECK(checked >= parent_checks + 4u * 3u * (kN - 1) * (kN - 1));
    CHECK(wrong_texel_matches * 20 < checked); // < 5 %: hashed neighbours rarely look alike

    // At morph 0 the parent's bake is bound and NOT read: bit-identical to a draw without it.
    {
        const TerrainTileKey k{0, {2, 1}};
        const TopDown v = vertex_view(k, 1, false);
        const auto bound =
            shade(*device,
                  up.pass,
                  {{up.ids.at(k), v.view_proj, up.with_parent(k, fixed_morph(k, 0.0f))}},
                  v.w,
                  v.h,
                  light);
        const auto plain = shade(
            *device, up.pass, {{up.ids.at(k), v.view_proj, fixed_morph(k, 0.0f)}}, v.w, v.h, light);
        CHECK(std::memcmp(bound.data(), plain.data(), bound.size() * sizeof(float)) == 0);
        // …and it is the flat material, not the bake: the witness that level 0 has its own look.
        const std::size_t px = vertex_pixel(v, k, 4, 4);
        CHECK(std::fabs(plain[px * 4] - (light.albedo.x + 0.04f)) < 1.0e-3f);
    }

    // ROUGHNESS reaches the BRDF too (the ambient-only frames above cannot see it): the same
    // parent under a sun, with only the bake's roughness channel changed, shades differently.
    {
        const TerrainTileKey k{1, {0, 0}};
        render::TerrainPass other(*device);
        const render::TerrainTileId id = other.upload(w.tiles.at(k));
        Bake b = make_bake(k);
        for (std::size_t t = 0; t < b.material.size(); t += 4) {
            b.material[t + 1] = static_cast<std::byte>(255 - static_cast<int>(b.material[t + 1]));
        }
        REQUIRE(other.set_bake(id, texels_of(b)));
        render::TerrainLight sun{};
        sun.sun_direction = {0.3f, -1.0f, 0.2f};
        const TopDown v = vertex_view(k, 1, false);
        const auto a = shade(
            *device, up.pass, {{up.ids.at(k), v.view_proj, fixed_morph(k, 0.0f)}}, v.w, v.h, sun);
        const auto c =
            shade(*device, other, {{id, v.view_proj, fixed_morph(k, 0.0f)}}, v.w, v.h, sun);
        std::uint32_t differ = 0;
        for (std::size_t n = 0; n < a.size(); ++n) {
            differ += a[n] != c[n] ? 1u : 0u;
        }
        CHECK(differ > kN * kN);
    }
    CHECK(up.pass.bake_draws() >= 6);
    CHECK(up.pass.parent_bake_draws() >= 5);
    MESSAGE("m19.8d3 (a): " << checked << " channel values; worst |frame - bake| " << worst
                            << ", at most " << worst_share
                            << " of its bound (1 f16 ULP + half an sRGB code + "
                            << kInterpolationSlack << "); dark values (< 0.1) within " << worst_dark
                            << " f16 ULP; " << wrong_texel_matches
                            << " would also match the neighbouring texel");
}

TEST_CASE("m19.8d3: (c) along a shared edge both tiles shade the same colour — same-level "
          "parents, a fine tile against a coarser one, and two fading children") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(4, 4, 3);
    BakedUpload up(*device, w);
    const render::TerrainLight light = flat_light();

    // Draw `k` alone with `lod` and return the frame's RGB at world sample positions along the
    // line x = `gx` (level-0 global samples), z from gz0 to gz1 — whichever turn covers each.
    struct EdgeDraw {
        TerrainTileKey key;
        render::TerrainLodDraw lod;
    };

    const auto along = [&](const EdgeDraw& d, std::int64_t gx, std::int64_t gz0, std::int64_t gz1) {
        std::map<std::int64_t, std::array<float, 3>> out;
        // One pixel per LEVEL-0 sample, so a coarse tile is also read between its vertices.
        const float x0 = world_x(gx, 0) - 8.5f;
        const float z0 = world_z(gz0, 0) - 1.5f;
        const auto h = static_cast<std::uint32_t>(gz1 - gz0 + 4);
        for (const bool turn : {false, true}) {
            const TopDown v = top_down_turn(x0, z0, 1.0f, 17, h, turn);
            const auto img =
                shade(*device, up.pass, {{up.ids.at(d.key), v.view_proj, d.lod}}, v.w, v.h, light);
            for (std::int64_t gz = gz0; gz <= gz1; ++gz) {
                const auto px = pixel_of(v, world_x(gx, 0), 10.0f, world_z(gz, 0));
                REQUIRE(px.has_value());
                if (img[*px * 4 + 3] == 1.0f) {
                    out[gz] = {img[*px * 4], img[*px * 4 + 1], img[*px * 4 + 2]};
                }
            }
        }
        return out;
    };
    std::uint32_t compared = 0;
    std::uint32_t bit_equal = 0;
    double worst_ulps = 0.0;
    const auto same = [&](const EdgeDraw& a,
                          const EdgeDraw& b,
                          std::int64_t gx,
                          std::int64_t gz0,
                          std::int64_t gz1) {
        const auto ea = along(a, gx, gz0, gz1);
        const auto eb = along(b, gx, gz0, gz1);
        std::uint32_t n = 0;
        for (const auto& [gz, ca] : ea) {
            const auto it = eb.find(gz);
            if (it == eb.end()) {
                continue;
            }
            ++n;
            for (std::size_t c = 0; c < 3; ++c) {
                const double ulps = std::fabs(ca[c] - it->second[c]) / ulp16(ca[c]);
                CHECK(ulps <= 1.0);
                worst_ulps = std::max(worst_ulps, ulps);
                ++compared;
                bit_equal += ca[c] == it->second[c] ? 1u : 0u;
            }
        }
        // Every sample on the edge except, at most, its two end corners.
        CHECK(n >= static_cast<std::uint32_t>(gz1 - gz0 - 1));
        // Not vacuous: the colour varies along the edge.
        CHECK(ea.begin()->second != std::prev(ea.end())->second);
    };

    // 1. Two level-1 parents, unmorphed: each reads its OWN bake's edge column.
    const TerrainTileKey a1{1, {0, 0}};
    const TerrainTileKey b1{1, {1, 0}};
    same({a1, fixed_morph(a1, 0.0f)}, {b1, fixed_morph(b1, 0.0f)}, 2 * kCells, 0, 2 * kCells);

    // 2. A FINE tile against a COARSER neighbour: level-0 (1, 0), whose +x neighbour is drawn at
    //    level 1. The edge bit forces its border to morph 1, so there it shades its PARENT's edge
    //    column — the same bytes as the coarse neighbour's own. The distance morph is off (m = 0
    //    inside the tile), so only the edge bit can be doing this.
    const TerrainTileKey fine{0, {1, 0}};
    render::TerrainLodDraw fine_lod = up.with_parent(fine, fixed_morph(fine, 0.0f));
    fine_lod.coarser_edges = render::kTerrainEdgePosX;
    same({fine, fine_lod}, {b1, fixed_morph(b1, 0.0f)}, 2 * kCells, 0, kCells);

    // 3. Two level-0 tiles with DIFFERENT parents, both fully morphed: each shades its own
    //    parent's edge column.
    const TerrainTileKey c0{0, {1, 1}};
    const TerrainTileKey d0{0, {2, 1}};
    same({c0, up.with_parent(c0, fixed_morph(c0, 1.0f))},
         {d0, up.with_parent(d0, fixed_morph(d0, 1.0f))},
         2 * kCells,
         kCells,
         2 * kCells);

    // The control: one sample INSIDE each parent the two do differ (their bakes are different
    // textures; only the shared column agrees).
    {
        const auto ia = along({a1, fixed_morph(a1, 0.0f)}, 2 * kCells - 2, 2, 2 * kCells - 2);
        const auto ib = along({b1, fixed_morph(b1, 0.0f)}, 2 * kCells + 2, 2, 2 * kCells - 2);
        std::uint32_t differ = 0;
        for (const auto& [gz, ca] : ia) {
            differ += ib.contains(gz) && ib.at(gz) != ca ? 1u : 0u;
        }
        CHECK(differ > kCells);
    }
    CHECK(compared >= 3u * (2u * kCells + 2u * kCells - 4u));
    MESSAGE("m19.8d3 (c): " << compared << " channel values on shared edges, " << bit_equal
                            << " bit-identical; worst " << worst_ulps << " f16 ULP");
}

namespace {

// Every leaf's per-vertex morph factor, read back from the GPU (points, one pixel per vertex).
struct MorphProbe {
    rhi::Device& device;
    rhi::ShaderHandle vs{};
    rhi::ShaderHandle fs{};
    rhi::PipelineHandle points{};
    rhi::SamplerHandle sampler{};

    explicit MorphProbe(rhi::Device& d) : device(d) {
        rhi::ShaderDesc sd{};
        sd.stage = rhi::ShaderStage::Vertex;
        sd.spirv = terrain_vert_spv;
        sd.spirv_size_bytes = sizeof(terrain_vert_spv);
        sd.debug_name = "terrain.vert";
        vs = device.create_shader(sd);
        sd.stage = rhi::ShaderStage::Fragment;
        sd.spirv = terrain_lod_probe_frag_spv;
        sd.spirv_size_bytes = sizeof(terrain_lod_probe_frag_spv);
        sd.debug_name = "terrain_lod_probe.frag";
        fs = device.create_shader(sd);
        static const rhi::BindingDesc bindings[] = {
            {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Vertex},
        };
        rhi::GraphicsPipelineDesc pd{};
        pd.vertex_shader = vs;
        pd.fragment_shader = fs;
        pd.color_format = rhi::Format::R32Uint;
        pd.topology = rhi::PrimitiveTopology::PointList;
        pd.cull = rhi::CullMode::None;
        pd.bindings = bindings;
        pd.push_constant_size = sizeof(render::TerrainPush);
        pd.debug_name = "m19.8d3-morph-probe";
        points = device.create_graphics_pipeline(pd);
        rhi::SamplerDesc smp{};
        smp.mag_filter = rhi::Filter::Nearest;
        smp.min_filter = rhi::Filter::Nearest;
        smp.address_mode = rhi::AddressMode::ClampToEdge;
        sampler = device.create_sampler(smp);
    }

    ~MorphProbe() {
        device.wait_idle();
        device.destroy(points);
        device.destroy(sampler);
        device.destroy(fs);
        device.destroy(vs);
    }

    MorphProbe(const MorphProbe&) = delete;
    MorphProbe& operator=(const MorphProbe&) = delete;

    // morphs[n][i + kN·j] for leaf n.
    std::vector<std::vector<float>> run(const Uploaded& up,
                                        const std::vector<TerrainLodLeaf>& leaves,
                                        const render::TerrainLodRanges& ranges,
                                        core::Vec3 eye,
                                        bool corner_bits = true) {
        constexpr std::uint32_t kStride = kN + 1;
        const auto cols =
            static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(leaves.size()))));
        const std::uint32_t rows = (static_cast<std::uint32_t>(leaves.size()) + cols - 1) / cols;
        const std::uint32_t w = cols * kStride;
        const std::uint32_t h = rows * kStride;
        std::vector<TopDown> views;
        for (std::size_t n = 0; n < leaves.size(); ++n) {
            const TerrainTileKey k = leaves[n].key;
            const float cell = static_cast<float>(1u << k.level);
            const float x0 = world_x(std::int64_t{k.coord.x} * kCells, k.level) -
                             (static_cast<float>(n % cols * kStride) + 0.5f) * cell;
            const float z0 = world_z(std::int64_t{k.coord.z} * kCells, k.level) -
                             (static_cast<float>(n / cols * kStride) + 0.5f) * cell;
            views.push_back(top_down(x0, z0, cell, w, h));
        }
        render::RenderGraph graph(device);
        graph.reset();
        const render::RGTexture target =
            graph.create_texture({{w, h}, rhi::Format::R32Uint, "morph-probe"});
        graph.export_texture(target);
        const render::RGColorAttachment colors[] = {
            {target, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
        std::vector<render::RGTexture> sampled;
        for (const TerrainLodLeaf& leaf : leaves) {
            sampled.push_back(
                graph.import_texture(up.tile(leaf.key).heights, rhi::ResourceState::ShaderRead));
        }
        render::RenderGraph::RasterPassDesc rpd{};
        rpd.colors = colors;
        rpd.sampled = sampled;
        graph.add_raster_pass("m19.8d3-morph-probe", rpd, [&](rhi::CommandBuffer& cmd) {
            cmd.bind_pipeline(points);
            for (std::size_t n = 0; n < leaves.size(); ++n) {
                const render::TerrainTile& tile = up.tile(leaves[n].key);
                render::TerrainLodDraw lod = lod_draw(ranges, leaves[n], eye);
                if (!corner_bits) {
                    lod.coarser_corners = 0; // the counterfactual: the vertex stage without them
                }
                const render::TerrainPush push =
                    render::terrain_push(tile, views[n].view_proj, eye, {}, lod);
                cmd.bind_texture(0, tile.heights, sampler);
                cmd.push_constants(&push, sizeof(push));
                cmd.draw(tile.vertex_count);
            }
        });
        auto cmd = device.begin_commands();
        graph.execute(*cmd);
        device.submit_blocking(*cmd);
        const auto words = read_back(device, graph.physical(target), w, h, 4);
        std::vector<std::vector<float>> out(leaves.size());
        for (std::size_t n = 0; n < leaves.size(); ++n) {
            const TerrainTileKey k = leaves[n].key;
            for (std::int32_t j = 0; j <= kCells; ++j) {
                for (std::int32_t i = 0; i <= kCells; ++i) {
                    const auto px =
                        pixel_of(views[n],
                                 world_x(std::int64_t{k.coord.x} * kCells + i, k.level),
                                 10.0f,
                                 world_z(std::int64_t{k.coord.z} * kCells + j, k.level));
                    REQUIRE(px.has_value());
                    const std::uint32_t word = word_at(words, *px);
                    REQUIRE(word != 0); // a point landed here (the probe sets the sign bit)
                    out[n].push_back(bits_float(word ^ 0x80000000u));
                }
            }
        }
        return out;
    }
};

} // namespace

TEST_CASE("m19.8d3: a vertex gets ONE morph factor whichever tile draws it, and 1 wherever it "
          "touches a coarser tile — edges and corners") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(16, 16, 4);
    const render::TerrainLodRanges ranges = render::terrain_lod_ranges(w.world, test_view());
    const Uploaded up(*device, w);
    MorphProbe probe(*device);
    const std::vector<PathFrame> path = camera_path(w);
    const std::uint32_t top = w.world.level_count() - 1;

    struct Seen {
        std::uint32_t level;
        float m;
    };

    std::uint64_t shared = 0;          // vertex positions drawn by more than one leaf
    std::uint64_t same_level = 0;      // …by two leaves of one level
    std::uint64_t mid_morph = 0;       // …of which strictly between 0 and 1
    std::uint64_t cross_level = 0;     // …by leaves of different levels
    std::uint64_t coarse_morphing = 0; // the COARSER tile is itself morphing at such a vertex
    std::uint64_t coarse_morphing_ideal = 0;
    std::uint32_t frames = 0;
    for (std::size_t f = 0; f < path.size(); f += 7) {
        for (const bool forced : {false, true}) {
            const render::TerrainLodSelection sel =
                render::select_terrain_lod(w.world, ranges, path[f].eye, [&](TerrainTileKey k) {
                    return w.world.find(k) != nullptr &&
                           (!forced || forced_fallback_usable(k, top, f));
                });
            const auto morphs = probe.run(up, sel.leaves, ranges, path[f].eye);
            ++frames;
            std::map<std::pair<std::int64_t, std::int64_t>, std::vector<Seen>> at;
            for (std::size_t n = 0; n < sel.leaves.size(); ++n) {
                const TerrainTileKey k = sel.leaves[n].key;
                for (std::int32_t j = 0; j <= kCells; ++j) {
                    for (std::int32_t i = 0; i <= kCells; ++i) {
                        const std::int64_t gx = (std::int64_t{k.coord.x} * kCells + i) << k.level;
                        const std::int64_t gz = (std::int64_t{k.coord.z} * kCells + j) << k.level;
                        at[{gx, gz}].push_back(
                            {k.level, morphs[n][static_cast<std::size_t>(i + kN * j)]});
                    }
                }
            }
            for (const auto& [pos, seen] : at) {
                if (seen.size() < 2) {
                    continue;
                }
                ++shared;
                std::uint32_t finest = seen[0].level;
                std::uint32_t coarsest = seen[0].level;
                for (const Seen& s : seen) {
                    finest = std::min(finest, s.level);
                    coarsest = std::max(coarsest, s.level);
                }
                for (const Seen& s : seen) {
                    for (const Seen& o : seen) {
                        if (s.level == o.level) {
                            // THE CLAIM: bit for bit the same factor.
                            CHECK(float_bits(s.m) == float_bits(o.m));
                        }
                    }
                }
                if (finest == coarsest) {
                    ++same_level;
                    mid_morph += seen[0].m > 0.0f && seen[0].m < 1.0f ? 1u : 0u;
                    continue;
                }
                ++cross_level;
                for (const Seen& s : seen) {
                    if (s.level < coarsest) {
                        // Finer than something it touches: fully on the parent's appearance.
                        CHECK(s.m == 1.0f);
                    } else if (s.m != 0.0f) {
                        ++coarse_morphing;
                        coarse_morphing_ideal += forced ? 0u : 1u;
                    }
                }
            }
        }
    }
    CHECK(frames >= 40);
    CHECK(same_level > 10000);
    CHECK(mid_morph > 500);
    CHECK(cross_level > 1000);
    // With every tile available (the ideal selection) the ranges NEST: a coarser tile is not
    // morphing where a finer one touches it, so the fine side's "parent bake" and the coarse
    // side's own bake are the same texels and the appearance is continuous across the level
    // boundary. Under forced fallback that nesting is not guaranteed; it is counted, not asserted.
    CHECK(coarse_morphing_ideal == 0);
    MESSAGE("m19.8d3 morph agreement: " << frames << " selections, " << shared
                                        << " shared vertices — " << same_level << " same-level ("
                                        << mid_morph << " mid-morph), " << cross_level
                                        << " across levels; coarser side morphing at "
                                        << coarse_morphing << " of them under forced fallback, "
                                        << coarse_morphing_ideal << " in ideal selections");

    // ── THE CORNER, BUILT ON PURPOSE ────────────────────────────────────────────────────────
    //
    // The sweep above passes with the corner bits removed (measured): wherever a tile is coarser
    // because it is FAR, every vertex touching it is beyond the finer level's morph range anyway.
    // The bits matter when a tile is coarser because its children are NOT RESIDENT while the
    // camera is close — so that case is constructed: three level-1 nodes around a point split to
    // level 0, the fourth (diagonal) one cannot, and the camera hovers over the point. The
    // level-0 tile diagonal to the coarse node touches it at ONE vertex; its two neighbours have
    // the coarse node across an edge. Without the corner bit that tile fades the vertex by
    // distance (m = 0 here) while its neighbours hold it at 1.
    {
        const core::Vec3 eye{world_x(2 * kCells, 0), 27.0f, world_z(2 * kCells, 0)};
        const TerrainTileKey coarse{1, {1, 1}};
        const render::TerrainLodSelection sel =
            render::select_terrain_lod(w.world, ranges, eye, [&](TerrainTileKey k) {
                return w.world.find(k) != nullptr &&
                       !(k.level == 0 && assets::terrain_parent_coord(k.coord) == coarse.coord);
            });
        const TerrainLodLeaf* diagonal = nullptr;
        bool coarse_is_leaf = false;
        for (const TerrainLodLeaf& leaf : sel.leaves) {
            if (leaf.key == TerrainTileKey{0, {1, 1}}) {
                diagonal = &leaf;
            }
            coarse_is_leaf = coarse_is_leaf || leaf.key == coarse;
        }
        REQUIRE(diagonal != nullptr); // the three neighbours did split to level 0
        REQUIRE(coarse_is_leaf);
        CHECK(diagonal->coarser_edges == 0);   // it has no coarser EDGE neighbour…
        CHECK(diagonal->coarser_corners == 8); // …only the (last, last) corner
        const auto disagreements = [&](bool corner_bits) {
            const auto morphs = probe.run(up, sel.leaves, ranges, eye, corner_bits);
            std::map<std::tuple<std::uint32_t, std::int64_t, std::int64_t>, std::uint32_t> first;
            std::uint32_t n_bad = 0;
            for (std::size_t n = 0; n < sel.leaves.size(); ++n) {
                const TerrainTileKey k = sel.leaves[n].key;
                for (std::int32_t j = 0; j <= kCells; ++j) {
                    for (std::int32_t i = 0; i <= kCells; ++i) {
                        const std::uint32_t bits =
                            float_bits(morphs[n][static_cast<std::size_t>(i + kN * j)]);
                        const auto [it, fresh] =
                            first.try_emplace({k.level,
                                               std::int64_t{k.coord.x} * kCells + i,
                                               std::int64_t{k.coord.z} * kCells + j},
                                              bits);
                        n_bad += !fresh && it->second != bits ? 1u : 0u;
                    }
                }
            }
            return n_bad;
        };
        CHECK(disagreements(true) == 0);
        const std::uint32_t without = disagreements(false);
        CHECK(without > 0); // the bit is what holds it: its absence is visible
        MESSAGE("m19.8d3 corner: a tile touching a coarser one at a corner only — 0 shared "
                "vertices disagree with the corner bit, "
                << without << " without it");
    }
}

TEST_CASE("m19.8d3: (b) the frame a node switches level, no pixel's colour changes") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(16, 16, 4);
    const render::TerrainLodRanges ranges = render::terrain_lod_ranges(w.world, test_view());
    BakedUpload up(*device, w);
    const std::vector<PathFrame> path = camera_path(w);
    const render::TerrainLight light = flat_light();

    constexpr std::uint32_t kPx = 256; // the whole world, 0.5 m per pixel
    const TopDown view = top_down(kOrigin.x, kOrigin.z, 0.5f, kPx, kPx);
    const auto frame = [&](const render::TerrainLodSelection& sel, core::Vec3 eye, bool morph) {
        std::vector<Shade> d;
        for (const TerrainLodLeaf& leaf : sel.leaves) {
            render::TerrainLodDraw lod = up.with_parent(leaf.key, lod_draw(ranges, leaf, eye));
            if (!morph) {
                // The control: geometry and appearance both unmorphed (m = 0 everywhere).
                lod.morph_start = std::numeric_limits<float>::infinity();
                lod.morph_end = std::numeric_limits<float>::infinity();
                lod.coarser_edges = 0;
                lod.coarser_corners = 0;
            }
            d.push_back({up.ids.at(leaf.key), view.view_proj, lod});
        }
        return shade(*device, up.pass, d, kPx, kPx, light);
    };
    const auto all = [&](TerrainTileKey k) { return w.world.find(k) != nullptr; };
    // DERIVED. At a switch a node is replaced by its four children, which arrive at morph 1 while
    // the node itself is not morphing (the nesting, ADR-0071 §1). So before, a pixel shows the
    // node's bake at sample s; after, a child shows its PARENT's bake — the same texture — at
    // (offset + s_child) / 2, which is s in exact arithmetic. What differs is the f32
    // interpolation of the sample coordinate in two different triangulations (kInterpolationSlack,
    // times a texel range of at most 1) and the two frames' independent f16 rounding: one ULP
    // each, of a value that is at most 1.04.
    const double kPopBound = 2.0 * ulp16(1.04) + kInterpolationSlack;
    render::TerrainLodSelection prev =
        render::select_terrain_lod(w.world, ranges, path[0].eye, all);
    int switches = 0;
    double worst = 0.0;
    double worst_unmorphed = 0.0;
    std::uint64_t unmorphed_pixels = 0;
    for (std::size_t f = 1; f < path.size() && switches < 24; ++f) {
        const render::TerrainLodSelection cur =
            render::select_terrain_lod(w.world, ranges, path[f].eye, all);
        if (!same_leaves(prev.leaves, cur.leaves) && path[f].slow) {
            ++switches;
            const core::Vec3 eye = path[f].eye;
            const auto before = frame(prev, eye, true);
            const auto after = frame(cur, eye, true);
            const auto before_flat = frame(prev, eye, false);
            const auto after_flat = frame(cur, eye, false);
            std::uint32_t covered = 0;
            for (std::size_t p = 0; p < std::size_t{kPx} * kPx; ++p) {
                if (before[p * 4 + 3] != 1.0f || after[p * 4 + 3] != 1.0f) {
                    continue;
                }
                ++covered;
                bool jumped = false;
                for (std::size_t c = 0; c < 3; ++c) {
                    worst = std::max(
                        worst, std::fabs(double{before[p * 4 + c]} - double{after[p * 4 + c]}));
                    const double flat =
                        std::fabs(double{before_flat[p * 4 + c]} - double{after_flat[p * 4 + c]});
                    worst_unmorphed = std::max(worst_unmorphed, flat);
                    jumped = jumped || flat > kPopBound;
                }
                unmorphed_pixels += jumped ? 1u : 0u;
            }
            CHECK(covered == kPx * kPx);
        }
        prev = cur;
    }
    CHECK(switches >= 10);
    CHECK(worst <= kPopBound);
    // The bound DISCRIMINATES: without the morph the same switches jump two orders past it.
    CHECK(worst_unmorphed > 100.0 * kPopBound);
    CHECK(unmorphed_pixels > 1000);
    CHECK(up.pass.parent_bake_missing_draws() == 0);
    MESSAGE("m19.8d3 (b): " << switches << " level switches; worst per-pixel colour change "
                            << worst << " (bound " << kPopBound << "); without the morph "
                            << worst_unmorphed << ", over " << unmorphed_pixels << " pixels");
}

// ── Through the residency: bakes loaded from files, bound, counted ──────────────────────────────

namespace {

std::string bake_path(TerrainTileKey k, const char* what) {
    return "lod_L" + std::to_string(k.level) + "_" + std::to_string(k.coord.x) + "_" +
           std::to_string(k.coord.z) + "_bake_" + what + ".rtex";
}

// A single-level RGBA8 texture file, as `rime terrain-world` cooks a bake (texture.rs,
// cook_single_level).
std::vector<std::byte> encode_single_level(std::uint32_t w,
                                           std::uint32_t h,
                                           assets::TextureFormat format,
                                           const std::vector<std::byte>& pixels) {
    Writer p;
    p.u32(w);
    p.u32(h);
    p.u32(static_cast<std::uint32_t>(format));
    p.u32(1);
    p.u32(w);
    p.u32(h);
    p.u32(0);
    p.u32(static_cast<std::uint32_t>(pixels.size()));
    p.b.insert(p.b.end(), pixels.begin(), pixels.end());
    return rma1(assets::AssetKind::Texture, assets::texture_schema_hash(), p.b);
}

// `w`'s manifest with every parent naming its bake, and the bake files written into `dir`.
assets::TerrainWorld write_baked_world(const fs::path& dir, const LodWorld& w) {
    write_lod_world(dir, w);
    auto out = assets::TerrainWorld::make(lod_grid());
    REQUIRE(out.has_value());
    for (std::uint32_t l = 0; l < w.world.level_count(); ++l) {
        for (assets::TerrainWorldTile t : w.world.tiles(l)) {
            if (l > 0) {
                const Bake b = make_bake(t.key());
                t.bake_color_path = bake_path(t.key(), "color");
                t.bake_material_path = bake_path(t.key(), "material");
                write_file(dir / t.bake_color_path,
                           encode_single_level(kN, kN, assets::TextureFormat::Rgba8Srgb, b.color));
                write_file(
                    dir / t.bake_material_path,
                    encode_single_level(kN, kN, assets::TextureFormat::Rgba8Unorm, b.material));
            }
            REQUIRE(out->add_tile(std::move(t)));
        }
    }
    REQUIRE(out->validate_levels());
    return *out;
}

struct BakeRun {
    render::TerrainResidencyStats stats;
    std::uint64_t top_level_draws = 0;
    std::uint64_t parent_draws = 0;
    std::uint64_t pass_bake_draws = 0;
    std::uint64_t pass_parent_draws = 0;
    std::uint64_t pass_refused = 0;
    bool always_covered = true; // from the frame the roots were resident
};

BakeRun run_sweep(rhi::Device& device,
                  const LodWorld& w,
                  const assets::TerrainWorld& world,
                  const fs::path& dir,
                  std::uint32_t slots) {
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = slots;
    cfg.lod = test_view();
    render::TerrainResidency residency(device, pass, server, world, dir, nullptr, cfg);
    REQUIRE(residency.lod());
    const std::uint32_t top = world.level_count() - 1;
    const std::size_t roots = world.tiles(top).size();
    BakeRun out;
    for (const PathFrame& f : camera_path(w)) {
        lod_frame(device, server, residency, f.eye);
        for (const TerrainLodLeaf& leaf : residency.selection().leaves) {
            out.top_level_draws += leaf.key.level == top ? 1u : 0u;
            out.parent_draws += leaf.key.level > 0 ? 1u : 0u;
        }
        if (residency.stats().pinned_roots == roots) {
            out.always_covered =
                out.always_covered && measure_cover(w, residency.selection().leaves).exact;
        }
    }
    out.stats = residency.stats();
    out.pass_bake_draws = pass.bake_draws();
    out.pass_parent_draws = pass.parent_bake_draws();
    out.pass_refused = pass.bakes_refused();
    return out;
}

} // namespace

TEST_CASE("m19.8d3: (d) a fully cooked world draws every parent from its bake and fades every "
          "tile toward its parent's — no placeholder, at any slot budget") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("lod-bake");
    const LodWorld w = make_lod_world(16, 16, 4);
    const assets::TerrainWorld baked = write_baked_world(dir.path, w);

    // The manifest round trip: a 14-field line for a parent, the 10-field line for level 0.
    {
        const std::string text =
            "grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n"
            "tile\t0\t0\t0\t0\t1\t2\t0\tabc\ta.rhf\n"
            "tile\t0\t1\t0\t0\t1\t2\t0\tabd\tb.rhf\n"
            "tile\t0\t0\t1\t0\t1\t2\t0\tabe\tc.rhf\n"
            "tile\t0\t1\t1\t0\t1\t2\t0\tabf\td.rhf\n"
            "tile\t1\t0\t0\t0\t1\t2\t0.5\tac0\tp.rhf\t111\tp_color.rtex\t222\tp_material.rtex\n";
        const auto parsed = assets::TerrainWorld::parse(text);
        REQUIRE(parsed.has_value());
        const assets::TerrainWorldTile* p = parsed->find(TerrainTileKey{1, {0, 0}});
        REQUIRE(p != nullptr);
        CHECK(p->has_bake());
        CHECK(p->bake_color_path == "p_color.rtex");
        CHECK(p->bake_material_path == "p_material.rtex");
        CHECK(p->bake_color_id == assets::AssetId{0x111});
        CHECK(p->bake_material_id == assets::AssetId{0x222});
        CHECK_FALSE(parsed->find(TerrainTileKey{0, {1, 1}})->has_bake());
        // A level-0 line may not carry a bake, and a half-named bake is malformed.
        CHECK_FALSE(assets::TerrainWorld::parse(
                        "grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n"
                        "tile\t0\t0\t0\t0\t1\t2\t0\tabc\ta.rhf\t1\tc.rtex\t2\tm.rtex\n")
                        .has_value());
        CHECK_FALSE(
            assets::TerrainWorld::parse("grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n"
                                        "tile\t1\t0\t0\t0\t1\t2\t0\tabc\ta.rhf\t1\t\t2\tm.rtex\n")
                .has_value());
    }

    for (const std::uint32_t slots : {160u, 12u}) {
        const BakeRun r = run_sweep(*device, w, baked, dir.path, slots);
        const render::TerrainResidencyStats& s = r.stats;
        CHECK(s.fallback_appearance_draws == 0); // THE CLAIM
        CHECK(s.parent_bake_missing_draws == 0);
        CHECK(s.bake_load_failures == 0);
        CHECK(s.bake_refusals == 0);
        CHECK(r.pass_refused == 0);
        CHECK(r.always_covered);
        // Not vacuous, and every draw accounted for: each parent drawn was drawn from its bake,
        // each tile below the top level had its parent's bound — in the residency's counters and
        // in the pass's own.
        CHECK(s.bake_requests > 4);
        CHECK(r.parent_draws > 500);
        CHECK(s.baked_appearance_draws == r.parent_draws);
        CHECK(s.appearance_morph_draws == s.lod_draws - r.top_level_draws);
        CHECK(s.appearance_morph_draws > 1000);
        CHECK(r.pass_bake_draws == s.baked_appearance_draws);
        CHECK(r.pass_parent_draws == s.appearance_morph_draws);
        if (slots == 12u) {
            CHECK(s.fallback_draws > 0); // pressure really did lower detail
            CHECK(s.evictions > 0);
        }
        MESSAGE("m19.8d3 (d), " << slots << " slots: " << s.lod_draws << " leaf draws, "
                                << s.baked_appearance_draws << " parents from their bake, "
                                << s.appearance_morph_draws << " fading toward a parent's, "
                                << s.fallback_appearance_draws << " placeholder, "
                                << s.parent_bake_missing_draws << " without a parent bake; "
                                << s.bake_waits << " tile-frames waited for a bake, "
                                << s.fallback_draws << " fallback tile-frames");
    }
}

TEST_CASE("m19.8d3: a world without bakes, a bake of the wrong size and a missing bake file all "
          "still draw — placeholder, counted, coverage intact") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const LodWorld w = make_lod_world(8, 8, 3);

    // Cooked WITHOUT bakes: m19.8d2's picture, and its counter.
    {
        TempDir dir("lod-nobake");
        write_lod_world(dir.path, w);
        const BakeRun r = run_sweep(*device, w, w.world, dir.path, 96);
        CHECK(r.always_covered);
        CHECK(r.stats.bake_requests == 0);
        CHECK(r.stats.baked_appearance_draws == 0);
        CHECK(r.stats.appearance_morph_draws == 0);
        CHECK(r.stats.fallback_appearance_draws == r.parent_draws);
        CHECK(r.stats.fallback_appearance_draws > 0);
        CHECK(r.stats.parent_bake_missing_draws == r.stats.lod_draws - r.top_level_draws);
        CHECK(r.pass_bake_draws == 0);
    }
    // One root's bake is 5×5 (a valid texture, not this tile's), another root's colour file is
    // missing. Both roots still become resident and are drawn — with the placeholder.
    {
        TempDir dir("lod-badbake");
        const assets::TerrainWorld baked = write_baked_world(dir.path, w);
        REQUIRE(baked.tiles(2).size() == 4);
        const TerrainTileKey small{2, {0, 0}};
        const TerrainTileKey gone{2, {1, 0}};
        const std::vector<std::byte> tiny(5 * 5 * 4, std::byte{128});
        write_file(dir.path / bake_path(small, "color"),
                   encode_single_level(5, 5, assets::TextureFormat::Rgba8Srgb, tiny));
        write_file(dir.path / bake_path(small, "material"),
                   encode_single_level(5, 5, assets::TextureFormat::Rgba8Unorm, tiny));
        fs::remove(dir.path / bake_path(gone, "color"));
        const BakeRun r = run_sweep(*device, w, baked, dir.path, 96);
        CHECK(r.always_covered);
        CHECK(r.stats.pinned_roots == 4);
        CHECK(r.stats.bake_refusals == 1);
        CHECK(r.pass_refused == 1);
        CHECK(r.stats.bake_load_failures == 1);
        CHECK(r.stats.refused_loads == 0); // the HEIGHTS loaded: appearance never costs coverage
        // Every placeholder draw is one of those two roots, or a child fading toward one.
        CHECK(r.stats.fallback_appearance_draws + r.stats.baked_appearance_draws == r.parent_draws);
        CHECK(r.stats.baked_appearance_draws > 0);
        CHECK(r.stats.appearance_morph_draws + r.stats.parent_bake_missing_draws ==
              r.stats.lod_draws - r.top_level_draws);
        CHECK(r.stats.parent_bake_missing_draws > 0);
        CHECK(r.stats.appearance_morph_draws > 0);
    }
}
