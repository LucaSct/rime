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
