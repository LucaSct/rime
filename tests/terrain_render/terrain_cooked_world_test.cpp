// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m19.8e (ADR-0073), proof (d): a world COOKED BY THE REAL TOOL, drawn through the whole stack.
//
// Every m19.8d proof drew a world the C++ test wrote itself — heightfields encoded here, and in
// 8d3 a synthetic bake ("GPU proofs used a synthetic bake" is 8d3's own stated gap). This one
// loads tests/assets/fixtures/splat_world/, every byte of which `rime` cooked (regen.sh): a
// material by `rime cook`, two terrain layers by `rime terrain-layer`, and sixteen splat tiles,
// their LOD chain and five parents' appearance bakes by `rime terrain-world`. Nothing in it was
// written by C++. Through it:
//
//   * the residency streams the world under a moving camera — splat tiles resolved by the
//     engine's TerrainLayerBuilder through the palette manifest, parents with their cooked bakes —
//     and coverage is exact every frame, with no placeholder anywhere;
//   * the same, with m19.8e's byte budget, upload cap and culling all on;
//   * at every level switch on a slow fly, the frame before (the parent, from its cooked bake) and
//     the frame after (its four splat children, height-blended, at morph 1) shade every pixel the
//     same to within 8d3's bound — and without the morph they visibly do not.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/heightfield_asset.hpp"
#include "rime/assets/manifest.hpp"
#include "rime/assets/terrain_world.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/terrain_builder.hpp"
#include "rime/render/terrain_lod.hpp"
#include "rime/render/terrain_pass.hpp"
#include "rime/render/terrain_residency.hpp"
#include "rime/rhi/device.hpp"

namespace {

using namespace rime;
namespace fs = std::filesystem;
using assets::TerrainTileKey;
using render::TerrainLodLeaf;

const fs::path kFixture = fs::path(RIME_ASSETS_FIXTURE_DIR) / "splat_world";
const fs::path kWorldDir = kFixture / "world";
const fs::path kPaletteDir = kFixture / "palette";
constexpr std::int32_t kTiles = 4; // level-0 tiles per axis

std::unique_ptr<rhi::Device> make_device() {
    auto device = rhi::create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the m19.8e cooked-world proof");
    }
    return device;
}

std::string read_text(const fs::path& p) {
    const auto bytes = platform::read_file(p);
    REQUIRE_MESSAGE(bytes.has_value(), "cannot read ", p.string());
    return {reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

assets::TerrainWorld load_world() {
    auto w = assets::TerrainWorld::parse(read_text(kWorldDir / "splat_world.terrainworld"));
    REQUIRE(w.has_value());
    REQUIRE(w->validate_levels());
    return *w;
}

assets::Manifest load_palette_manifest() {
    auto m = assets::Manifest::parse(read_text(kPaletteDir / "manifest.txt"));
    REQUIRE(m.has_value());
    return *m;
}

// The view 8d2/8d3's proofs use: LOD switches inside a world of tens of metres.
render::TerrainLodView test_view() {
    render::TerrainLodView v{};
    v.viewport_height_px = 64.0f;
    v.vertical_fov = 1.0471976f;
    v.pixel_error = 8.0f;
    v.step_margin = 1.0f;
    v.morph_fraction = 0.25f;
    return v;
}

render::TerrainLight flat_light() {
    render::TerrainLight l{};
    l.sun_irradiance = 0.0f;
    l.ambient = 1.0f;
    return l;
}

// Exactly once over the 4×4 level-0 grid.
bool covers_exactly(const std::vector<TerrainLodLeaf>& leaves) {
    std::vector<int> n(kTiles * kTiles, 0);
    for (const TerrainLodLeaf& leaf : leaves) {
        const std::int32_t s = 1 << leaf.key.level;
        for (std::int32_t z = leaf.key.coord.z * s; z < (leaf.key.coord.z + 1) * s; ++z) {
            for (std::int32_t x = leaf.key.coord.x * s; x < (leaf.key.coord.x + 1) * s; ++x) {
                if (x < 0 || z < 0 || x >= kTiles || z >= kTiles) {
                    return false;
                }
                ++n[static_cast<std::size_t>(z * kTiles + x)];
            }
        }
    }
    return std::all_of(n.begin(), n.end(), [](int c) { return c == 1; });
}

bool same_leaves(const std::vector<TerrainLodLeaf>& a, const std::vector<TerrainLodLeaf>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!(a[i].key == b[i].key) || a[i].coarser_edges != b[i].coarser_edges) {
            return false;
        }
    }
    return true;
}

// A slow fly (0.25 m steps, inside the 1 m step margin) corner to corner and back, low over the
// ground, so every level switches on the way.
std::vector<core::Vec3> camera_path() {
    std::vector<core::Vec3> out;
    const auto fly = [&](core::Vec3 a, core::Vec3 b) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dz = b.z - a.z;
        const int n = static_cast<int>(std::ceil(std::sqrt(dx * dx + dy * dy + dz * dz) / 0.25f));
        for (int s = 0; s <= n; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(n);
            out.push_back({a.x + t * dx, a.y + t * dy, a.z + t * dz});
        }
    };
    fly({1.0f, 11.0f, 2.0f}, {30.0f, 40.0f, 29.0f});
    fly({30.0f, 40.0f, 29.0f}, {3.0f, 11.5f, 27.0f});
    fly({3.0f, 11.5f, 27.0f}, {29.0f, 24.0f, 3.0f});
    return out;
}

struct TopDown {
    core::Mat4 view_proj;
};

// The whole 32 m world from above, `px` pixels square.
TopDown whole_world(std::uint32_t px) {
    (void)px;
    const core::Vec3 eye{16.0f, 400.0f, 16.0f};
    return {core::ortho(-16.0f, 16.0f, -16.0f, 16.0f, 0.0f, 800.0f) *
            core::look_at(eye, {16.0f, 0.0f, 16.0f}, {0.0f, 0.0f, -1.0f})};
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
    rbd.debug_name = "m19.8e-readback";
    const rhi::BufferHandle rb = device.create_buffer(rbd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(texture, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

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

double ulp16(double m) {
    int e = 0;
    std::frexp(std::max(std::fabs(m), 6.2e-5), &e);
    return std::ldexp(1.0, e - 11);
}

void settle(assets::AssetServer& server) {
    server.wait_for_pending_loads();
    server.pump();
}

// Every tile of the cooked world on one pass, as the residency would upload it: splat tiles with
// the palette the ENGINE's builder resolves, parents with the bake read from the cooked file.
struct Loaded {
    render::TerrainPass pass;
    std::map<TerrainTileKey, render::TerrainTileId> ids;
    std::vector<render::TerrainPaletteHandle> palettes;
    std::uint32_t splat_tiles = 0;
    std::uint32_t baked = 0;

    Loaded(rhi::Device& device,
           const assets::TerrainWorld& world,
           assets::AssetServer& server,
           render::TerrainLayerBuilder& builder)
        : pass(device) {
        for (std::uint32_t l = 0; l < world.level_count(); ++l) {
            for (const assets::TerrainWorldTile& t : world.tiles(l)) {
                const auto bytes = platform::read_file(kWorldDir / t.path);
                REQUIRE(bytes.has_value());
                assets::AssetError err{};
                const auto hf = assets::read_heightfield(*bytes, err);
                REQUIRE(hf.has_value());
                render::TerrainTileId id = render::kInvalidTerrainTile;
                if (hf->has_splat()) {
                    std::array<assets::AssetId, 4> layers{};
                    std::copy(std::begin(hf->layers), std::end(hf->layers), layers.begin());
                    const render::TerrainPaletteHandle h = builder.request(layers);
                    render::TerrainPaletteState s = render::TerrainPaletteState::Pending;
                    for (int i = 0; i < 16 && s == render::TerrainPaletteState::Pending; ++i) {
                        settle(server);
                        s = builder.update(h);
                    }
                    REQUIRE(s == render::TerrainPaletteState::Ready);
                    palettes.push_back(h);
                    id = pass.upload(*hf, *builder.palette(h));
                    ++splat_tiles;
                } else {
                    id = pass.upload(*hf);
                }
                REQUIRE(id != render::kInvalidTerrainTile);
                CHECK(pass.tile_bytes(id) == render::TerrainPass::predicted_tile_bytes(*hf, false));
                if (t.has_bake()) {
                    const auto read_tex = [](const fs::path& p) {
                        const auto b = platform::read_file(p);
                        REQUIRE(b.has_value());
                        assets::AssetError e{};
                        auto tex = assets::read_texture(*b, e);
                        REQUIRE(tex.has_value());
                        return *tex;
                    };
                    const assets::TextureAsset c = read_tex(kWorldDir / t.bake_color_path);
                    const assets::TextureAsset m = read_tex(kWorldDir / t.bake_material_path);
                    render::TerrainBakeTexels texels{};
                    texels.columns = c.width;
                    texels.rows = c.height;
                    texels.color = std::span<const std::byte>(c.pixels.data(), c.mips[0].size);
                    texels.material = std::span<const std::byte>(m.pixels.data(), m.mips[0].size);
                    REQUIRE(pass.set_bake(id, texels));
                    CHECK(pass.tile_bytes(id) ==
                          render::TerrainPass::predicted_tile_bytes(*hf, true));
                    ++baked;
                }
                ids.emplace(t.key(), id);
            }
        }
    }

    // What TerrainResidency::draw_leaf pushes for `leaf`, from the same public inputs.
    render::TerrainLodDraw draw(const assets::TerrainWorld& world,
                                const render::TerrainLodRanges& ranges,
                                const TerrainLodLeaf& leaf,
                                core::Vec3 camera) const {
        const assets::TerrainWorldGrid& g = world.grid();
        const auto n = static_cast<std::int32_t>(g.samples - 1);
        render::TerrainLodDraw d{};
        d.enabled = true;
        d.grid_origin = g.origin;
        d.base_x = leaf.key.coord.x * n;
        d.base_z = leaf.key.coord.z * n;
        d.level = leaf.key.level;
        d.coarser_edges = leaf.coarser_edges;
        d.coarser_corners = leaf.coarser_corners;
        d.camera = camera;
        d.morph_start = ranges.levels[leaf.key.level].morph_start;
        d.morph_end = ranges.levels[leaf.key.level].morph_end;
        const TerrainTileKey p{leaf.key.level + 1, assets::terrain_parent_coord(leaf.key.coord)};
        if (const auto it = ids.find(p); it != ids.end()) {
            d.parent = it->second;
            d.parent_quadrant = static_cast<std::uint32_t>(leaf.key.coord.x & 1) |
                                (static_cast<std::uint32_t>(leaf.key.coord.z & 1) << 1);
        }
        return d;
    }
};

// Draw `draws` through the engine's pass into a px × px RGBA16F target cleared to alpha 0.
std::vector<float>
shade(rhi::Device& device,
      render::TerrainPass& pass,
      const std::vector<std::pair<render::TerrainTileId, render::TerrainLodDraw>>& draws,
      const core::Mat4& vp,
      std::uint32_t px) {
    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr = graph.create_texture({{px, px}, render::kHdrFormat, "cw-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{px, px}, render::kDepthFormat, "cw-depth"});
    graph.export_texture(hdr);
    const render::RGColorAttachment clears[] = {
        {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    const render::RGDepthAttachment dclear{
        depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
    render::RenderGraph::RasterPassDesc cd{};
    cd.colors = clears;
    cd.depth = &dclear;
    graph.add_raster_pass("cw-clear", cd, [](rhi::CommandBuffer&) {});
    for (const auto& [id, lod] : draws) {
        pass.add(graph, hdr, depth, id, vp, {16.0f, 500.0f, 16.0f}, flat_light(), {}, lod);
    }
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    const std::vector<std::uint8_t> raw = read_back(device, graph.physical(hdr), px, px, 8);
    std::vector<float> out(std::size_t{px} * px * 4);
    for (std::size_t n = 0; n < out.size(); ++n) {
        std::uint16_t half = 0;
        std::memcpy(&half, &raw[n * 2], sizeof(half));
        out[n] = half_float(half);
    }
    return out;
}

} // namespace

TEST_CASE("m19.8e: (d) a world cooked by `rime terrain-world` streams with exact coverage and no "
          "placeholder — splat tiles through the builder, parents from their cooked bakes") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const assets::TerrainWorld world = load_world();
    REQUIRE(world.level_count() == 3);
    const assets::Manifest manifest = load_palette_manifest();
    const std::vector<core::Vec3> path = camera_path();
    // A camera looking ahead and down along the fly: part of the world is behind it.
    const auto view_of = [&](std::size_t f) {
        const core::Vec3 a = path[f];
        const core::Vec3 b = path[std::min(f + 1, path.size() - 1)];
        const float dx = f + 1 < path.size() ? b.x - a.x : 1.0f;
        const float dz = f + 1 < path.size() ? b.z - a.z : 1.0f;
        const float len = std::max(std::sqrt(dx * dx + dz * dz), 1e-3f);
        const core::Vec3 at{a.x + dx / len, a.y - 0.5f, a.z + dz / len};
        return core::perspective(1.0f, 1.0f, 0.1f, 200.0f) *
               core::look_at(a, at, {0.0f, 1.0f, 0.0f});
    };

    for (const bool budgets : {false, true}) {
        core::JobSystem jobs(2);
        assets::AssetServer server(jobs);
        render::TerrainPass pass(*device);
        render::TerrainLayerBuilder builder(*device, server, manifest, kPaletteDir);
        render::TerrainResidencyConfig cfg{};
        cfg.slots = 64;
        cfg.lod = test_view();
        if (budgets) {
            // m19.8e all on: room for the root, its four parents and one splat quad with its
            // layer textures (~21 KB) and little more; a few tiles per frame; culling.
            cfg.byte_budget = 28 * 1024;
            cfg.upload_cap = 6 * 1024;
            cfg.frustum_cull = true;
        }
        render::TerrainResidency residency(*device, pass, server, world, kWorldDir, &builder, cfg);
        REQUIRE(residency.lod());
        std::uint32_t covered = 0;
        std::uint64_t level0_draws = 0;
        std::uint64_t parent_draws = 0;
        for (std::size_t f = 0; f < path.size(); ++f) {
            const core::Vec3 eye = path[f];
            const core::Mat4 vp = view_of(f);
            settle(server);
            residency.begin_frame(eye);
            render::RenderGraph graph(*device);
            graph.reset();
            const render::RGTexture hdr =
                graph.create_texture({{64, 64}, render::kHdrFormat, "cw-hdr"});
            const render::RGTexture depth =
                graph.create_texture({{64, 64}, render::kDepthFormat, "cw-d"});
            const render::RGColorAttachment clears[] = {
                {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}}};
            const render::RGDepthAttachment dclear{
                depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f, 0, false, 0};
            render::RenderGraph::RasterPassDesc cd{};
            cd.colors = clears;
            cd.depth = &dclear;
            graph.add_raster_pass("cw-clear", cd, [](rhi::CommandBuffer&) {});
            residency.add(graph, hdr, depth, vp, eye, flat_light());
            auto cmd = device->begin_commands();
            graph.execute(*cmd);
            device->submit_blocking(*cmd);
            residency.end_frame_blocking();

            const render::TerrainResidencyStats& s = residency.stats();
            if (s.pinned_roots == 1) {
                REQUIRE(covers_exactly(residency.selection().leaves));
                ++covered;
            }
            if (budgets) {
                REQUIRE(s.budget_bytes <= cfg.byte_budget);
                REQUIRE(s.upload_bytes_this_frame <= cfg.upload_cap);
            }
            for (const TerrainLodLeaf& leaf : residency.selection().leaves) {
                ++(leaf.key.level == 0 ? level0_draws : parent_draws);
            }
        }
        const render::TerrainResidencyStats& s = residency.stats();
        CHECK(covered + 2 >= path.size());
        // THE CLAIM: nothing drawn with the placeholder, nothing refused, every load good.
        CHECK(s.fallback_appearance_draws == 0);
        CHECK(s.parent_bake_missing_draws == 0);
        CHECK(s.bake_load_failures == 0);
        CHECK(s.bake_refusals == 0);
        CHECK(s.refused_palettes == 0);
        CHECK(s.refused_loads == 0);
        CHECK(s.refused_world == 0);
        CHECK(s.uncovered_draws <= 2);
        CHECK(pass.splat_refused() == 0);
        CHECK(builder.counters().palettes_failed == 0);
        CHECK(builder.counters().unresolved_ids == 0);
        CHECK(builder.counters().textures_uploaded >= 2); // grass and rock, each once at a time
        // Not vacuous: splat tiles and baked parents were both drawn, in quantity.
        CHECK(level0_draws > 500);
        CHECK(parent_draws > 200);
        CHECK(s.baked_appearance_draws > 0);
        CHECK(s.appearance_morph_draws > 0);
        CHECK(s.resident_bake_holds == 0);
        if (budgets) {
            CHECK(s.peak_budget_bytes <= cfg.byte_budget);
            CHECK(s.peak_upload_bytes_frame <= cfg.upload_cap);
            CHECK(s.culled_draws + s.lod_draws == level0_draws + parent_draws);
            CHECK(s.layer_texture_bytes > 0);
            CHECK(s.culled_draws > 0);
            CHECK(s.byte_budget_waits + s.byte_budget_evictions + s.upload_cap_waits > 0);
        }
        const std::string label = budgets ? "budgets on" : "budgets off";
        MESSAGE("m19.8e (d) cooked world, "
                << label << ": " << path.size() << " frames, " << level0_draws
                << " splat leaf-frames, " << parent_draws << " parent leaf-frames, "
                << s.baked_appearance_draws << " parents drawn from "
                << "cooked bakes, " << s.fallback_appearance_draws << " placeholder; peak "
                << s.peak_budget_bytes << " GPU bytes, " << s.culled_draws << " culled");
    }
}

TEST_CASE("m19.8e: (d) at every level switch of the cooked world, the parent's cooked bake and its "
          "splat children shade the same pixels within 8d3's bound") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const assets::TerrainWorld world = load_world();
    const assets::Manifest manifest = load_palette_manifest();
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainLayerBuilder builder(*device, server, manifest, kPaletteDir);
    Loaded up(*device, world, server, builder);
    REQUIRE(up.splat_tiles == 16);
    REQUIRE(up.baked == 5);
    const render::TerrainLodRanges ranges = render::terrain_lod_ranges(world, test_view());
    constexpr std::uint32_t kPx = 128; // 0.25 m per pixel
    const core::Mat4 vp = whole_world(kPx).view_proj;
    const auto all = [&](TerrainTileKey k) { return world.find(k) != nullptr; };
    const auto frame = [&](const render::TerrainLodSelection& sel, core::Vec3 eye, bool morph) {
        std::vector<std::pair<render::TerrainTileId, render::TerrainLodDraw>> d;
        for (const TerrainLodLeaf& leaf : sel.leaves) {
            render::TerrainLodDraw lod = up.draw(world, ranges, leaf, eye);
            if (!morph) {
                lod.morph_start = std::numeric_limits<float>::infinity();
                lod.morph_end = std::numeric_limits<float>::infinity();
                lod.coarser_edges = 0;
                lod.coarser_corners = 0;
            }
            d.emplace_back(up.ids.at(leaf.key), lod);
        }
        return shade(*device, up.pass, d, vp, kPx);
    };
    // 8d3's bound (ADR-0072 (b)): two f16 ULPs of a value ≤ 1.04, plus the f32 interpolation of the
    // sample coordinate in two triangulations.
    const double kPopBound = 2.0 * ulp16(1.04) + 1.0e-4;
    const std::vector<core::Vec3> path = camera_path();
    render::TerrainLodSelection prev = render::select_terrain_lod(world, ranges, path[0], all);
    int switches = 0;
    int to_splat = 0; // switches where a level-0 (splat) tile appears or disappears
    double worst = 0.0;
    double worst_unmorphed = 0.0;
    for (std::size_t f = 1; f < path.size(); ++f) {
        const render::TerrainLodSelection cur =
            render::select_terrain_lod(world, ranges, path[f], all);
        if (!same_leaves(prev.leaves, cur.leaves)) {
            ++switches;
            const auto has0 = [](const render::TerrainLodSelection& s) {
                return std::count_if(s.leaves.begin(), s.leaves.end(), [](const TerrainLodLeaf& l) {
                    return l.key.level == 0;
                });
            };
            to_splat += has0(prev) != has0(cur) ? 1 : 0;
            const auto before = frame(prev, path[f], true);
            const auto after = frame(cur, path[f], true);
            const auto before_flat = frame(prev, path[f], false);
            const auto after_flat = frame(cur, path[f], false);
            std::uint32_t covered = 0;
            for (std::size_t p = 0; p < std::size_t{kPx} * kPx; ++p) {
                if (before[p * 4 + 3] != 1.0f || after[p * 4 + 3] != 1.0f) {
                    continue;
                }
                ++covered;
                for (std::size_t c = 0; c < 3; ++c) {
                    worst = std::max(
                        worst, std::fabs(double{before[p * 4 + c]} - double{after[p * 4 + c]}));
                    worst_unmorphed = std::max(
                        worst_unmorphed,
                        std::fabs(double{before_flat[p * 4 + c]} - double{after_flat[p * 4 + c]}));
                }
            }
            CHECK(covered == kPx * kPx);
        }
        prev = cur;
    }
    CHECK(switches >= 6);
    CHECK(to_splat >= 2);
    CHECK(worst <= kPopBound); // THE CLAIM
    // The bound discriminates on REAL data: without the morph, splat detail against the bake.
    CHECK(worst_unmorphed > 10.0 * kPopBound);
    CHECK(up.pass.parent_bake_missing_draws() == 0);
    for (const render::TerrainPaletteHandle h : up.palettes) {
        builder.release(h);
    }
    MESSAGE("m19.8e (d) switches on the cooked world: "
            << switches << " (" << to_splat << " to or from splat tiles); worst "
            << "per-pixel colour change " << worst << " (bound " << kPopBound
            << "); without the morph " << worst_unmorphed);
}
