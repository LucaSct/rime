// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m19.8a — terrain render residency and the terrain builder (ADR-0069).
//
// What is proven here, and how:
//
//   (a) PLATEAU — a long scripted traversal over a 4x4 tile world with a 4-slot budget: slots and
//       tile bytes never exceed the budget, the pass's own tile store stays bounded, and every
//       upload is accounted for as resident-at-the-end or evicted.
//   (b) FENCE SAFETY — the slot policy driven with FAKE frames on the CPU (deterministic), plus a
//       GPU smoke on the device's real submit/fence path in which the test keeps its own copy of
//       every frame's ticket and checks, independently of the residency, that no slot was ever
//       recycled while the frame that last read it was still running.
//   (c) STALE IDS — drawing an evicted tile's old id is byte-identical to not drawing at all.
//   (d) THE BUILDER — a TerrainLayer palette renders bit-identically to the same palette built by
//       hand; a Material palette matches today's scalar path; shared layer textures are uploaded
//       once and destroyed exactly once, with the last palette — directly and through residency.
//   (e) SEAMS — two adjacent resident tiles with matching borders cover every pixel across the
//       seam.
//   (f) REFUSALS — duplicate coordinate, spacing mismatch, border mismatch and a tile TerrainPass
//       will not upload are each refused and counted under their own reason.
//
// The worlds are COOKED FILES written by this test (RMA1 heightfield v1/v2, material and terrain
// layer records — the formats cooked_reader.cpp decodes) into a temporary directory, because the
// residency's whole job is to go through the AssetServer's streamed handles, and a proof that
// handed it in-memory assets would skip the part under test. The encoders are checked against the
// engine's own readers before anything relies on them.
//
// Rendered on the default Vulkan device; structural checks and memcmp only, no golden images.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
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
#include "rime/render/gpu_asset_bridge.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/terrain_builder.hpp"
#include "rime/render/terrain_pass.hpp"
#include "rime/render/terrain_residency.hpp"
#include "rime/rhi/device.hpp"
#include "terrain_test_files.hpp"

#ifndef RIME_ASSETS_FIXTURE_DIR
#error "RIME_ASSETS_FIXTURE_DIR must be defined by the build"
#endif

namespace {

using namespace rime;
namespace fs = std::filesystem;
using namespace rime::terrain_test;
using assets::AssetId;
using assets::TerrainTileCoord;

// ── Cooked-file encoders: Writer, rma1, encode_heightfield, write_file and TempDir are shared with
// the m19.8d2 proofs (terrain_test_files.hpp); the material and layer encoders are this file's. ──

struct MaterialSpec {
    float rgb[3] = {0.5f, 0.5f, 0.5f};
    float metallic = 0.0f;
    float roughness = 1.0f;
};

std::vector<std::byte> encode_material(const MaterialSpec& m) {
    Writer w;
    w.f32(m.rgb[0]);
    w.f32(m.rgb[1]);
    w.f32(m.rgb[2]);
    w.f32(1.0f); // base alpha
    for (int i = 0; i < 3; ++i) {
        w.f32(0.0f); // emissive
    }
    w.f32(m.metallic);
    w.f32(m.roughness);
    w.f32(1.0f); // normal_scale
    w.f32(1.0f); // occlusion_strength
    w.f32(0.5f); // alpha_cutoff
    w.u32(0);    // Opaque
    for (int i = 0; i < 5; ++i) {
        w.u64(0); // no textures
    }
    w.u32(0);
    w.u32(0);
    return rma1(assets::AssetKind::Material, assets::material_schema_hash(), w.b);
}

std::vector<std::byte>
encode_terrain_layer(AssetId material, AssetId texture, float uv_x, float uv_z, float contrast) {
    Writer w;
    w.u64(material.value);
    w.u64(texture.value);
    w.f32(uv_x);
    w.f32(uv_z);
    w.f32(contrast);
    return rma1(assets::AssetKind::TerrainLayer, assets::terrain_layer_schema_hash(), w.b);
}

// ── The world ────────────────────────────────────────────────────────────────────────────────────

constexpr std::uint32_t kSamples = 9; // 8 cells of 1 m: a tile is 8 m square
constexpr float kPitch = 8.0f;
constexpr float kScale = 0.01f; // 1 cm per step

assets::TerrainWorldGrid world_grid() {
    assets::TerrainWorldGrid g{};
    g.samples = kSamples;
    g.cell_size_x = 1.0f;
    g.cell_size_z = 1.0f;
    g.height_scale = kScale;
    g.height_offset = 0.0f;
    g.origin = {0.0f, 0.0f, 0.0f};
    return g;
}

// One GLOBAL integer field the tiles are cut from: two tiles agree on a shared edge by
// construction. ~10 m high, gently non-planar.
std::uint16_t field(std::int32_t gx, std::int32_t gz) {
    return static_cast<std::uint16_t>(1000 + 3 * gx + 5 * gz + ((gx * gz) & 7));
}

assets::HeightfieldAsset world_tile(TerrainTileCoord c) {
    const assets::TerrainWorldGrid g = world_grid();
    assets::HeightfieldAsset a{};
    a.columns = kSamples;
    a.rows = kSamples;
    a.cell_size_x = g.cell_size_x;
    a.cell_size_z = g.cell_size_z;
    a.height_scale = g.height_scale;
    a.height_offset = g.height_offset;
    a.origin = g.tile_origin(c);
    const std::int32_t n = static_cast<std::int32_t>(kSamples) - 1;
    for (std::uint32_t j = 0; j < kSamples; ++j) {
        for (std::uint32_t i = 0; i < kSamples; ++i) {
            a.samples.push_back(field(c.x * n + static_cast<std::int32_t>(i),
                                      c.z * n + static_cast<std::int32_t>(j)));
        }
    }
    return a;
}

// A splat tile: a 2x2 weight map, every texel `texel`, palette `layers`.
assets::HeightfieldAsset
splat_tile(TerrainTileCoord c, std::array<AssetId, 4> layers, std::array<std::uint8_t, 4> texel) {
    assets::HeightfieldAsset a = world_tile(c);
    a.weight_columns = 2;
    a.weight_rows = 2;
    for (std::size_t k = 0; k < 4; ++k) {
        a.layers[k] = layers[k];
    }
    for (int t = 0; t < 4; ++t) {
        a.weights.insert(a.weights.end(), texel.begin(), texel.end());
    }
    return a;
}

std::string tile_file(TerrainTileCoord c) {
    return "tile_" + std::to_string(c.x) + "_" + std::to_string(c.z) + ".rhf";
}

// Write the tiles' files and return the world (every listed tile; `assets` overrides content).
assets::TerrainWorld
write_world(const fs::path& dir,
            const std::vector<std::pair<TerrainTileCoord, assets::HeightfieldAsset>>& tiles) {
    auto world = assets::TerrainWorld::make(world_grid());
    REQUIRE(world.has_value());
    for (const auto& [c, a] : tiles) {
        write_file(dir / tile_file(c), encode_heightfield(a));
        REQUIRE(world->add_tile({c, 1, 0.0f, 20.0f, {}, tile_file(c)}));
    }
    return *world;
}

// ── Rendering ────────────────────────────────────────────────────────────────────────────────────

constexpr std::uint32_t kSize = 64;

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

std::unique_ptr<rhi::Device> make_device() {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the m19.8a GPU proof");
    }
    return device;
}

struct View {
    core::Mat4 view_proj;
    core::Vec3 eye;
};

// Top-down orthographic view of a square of side 2*half centred on (cx, cz): pixel (px, py) is
// world x = cx - half + (px + 0.5) * 2*half/kSize, z likewise (the m19.7b top_down_centred map).
View top_down(float cx, float cz, float half) {
    const core::Vec3 eye{cx, 200.0f, cz};
    return {core::ortho(-half, half, -half, half, 0.0f, 400.0f) *
                core::look_at(eye, {cx, 0.0f, cz}, {0.0f, 0.0f, -1.0f}),
            eye};
}

render::TerrainLight ambient_light() {
    render::TerrainLight l{};
    l.sun_irradiance = 0.0f;
    l.ambient = 1.0f;
    l.albedo = {0.5f, 0.5f, 0.5f};
    return l;
}

std::vector<std::uint8_t> read_texture(rhi::Device& device, rhi::TextureHandle texture) {
    const std::uint64_t bytes = std::uint64_t{kSize} * kSize * 8; // RGBA16F
    rhi::BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = rhi::BufferUsage::TransferDst;
    rbd.memory = rhi::MemoryUsage::GpuToCpu;
    rbd.debug_name = "m19.8a-readback";
    const rhi::BufferHandle rb = device.create_buffer(rbd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(texture, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

using Declare = std::function<void(render::RenderGraph&, render::RGTexture, render::RGTexture)>;

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

// One cleared frame, `declare` adds the draws, submitted BLOCKING and read back.
std::vector<std::uint8_t> render_frame(rhi::Device& device, const Declare& declare) {
    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr =
        graph.create_texture({{kSize, kSize}, render::kHdrFormat, "m19.8a-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "m19.8a-depth"});
    graph.export_texture(hdr);
    declare_clear(graph, hdr, depth);
    declare(graph, hdr, depth);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return read_texture(device, graph.physical(hdr));
}

// A pixel is covered when terrain wrote it: the clear is exactly zero in R, G and B, and an
// ambient-lit tile of a non-black albedo is not.
bool covered(const std::vector<std::uint8_t>& img, std::uint32_t px, std::uint32_t py) {
    const std::size_t o = (std::size_t{py} * kSize + px) * 8;
    return (img[o] | img[o + 1] | img[o + 2] | img[o + 3] | img[o + 4] | img[o + 5]) != 0;
}

std::uint32_t covered_count(const std::vector<std::uint8_t>& img) {
    std::uint32_t n = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            n += covered(img, x, y) ? 1 : 0;
        }
    }
    return n;
}

// Settle the asset server: every load requested so far is finished and promoted. Called before
// each begin_frame, so a tile requested in frame N is Ready in frame N+1 — deterministic.
void settle(assets::AssetServer& server) {
    server.wait_for_pending_loads();
    server.pump();
}

// A residency frame drawn with the blocking path.
void residency_frame(rhi::Device& device,
                     assets::AssetServer& server,
                     render::TerrainResidency& residency,
                     core::Vec3 eye,
                     const View& view) {
    settle(server);
    residency.begin_frame(eye);
    render::RenderGraph graph(device);
    graph.reset();
    const render::RGTexture hdr =
        graph.create_texture({{kSize, kSize}, render::kHdrFormat, "m19.8a-hdr"});
    const render::RGTexture depth =
        graph.create_texture({{kSize, kSize}, render::kDepthFormat, "m19.8a-depth"});
    declare_clear(graph, hdr, depth);
    residency.add(graph, hdr, depth, view.view_proj, view.eye, ambient_light());
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    residency.end_frame_blocking();
}

// The camera path for the traversals: a boustrophedon over the 4x4 world (rows z = 0..3, each
// swept in x, alternating direction), a teleport back to the start, then a diagonal return.
std::vector<core::Vec3> traversal_path() {
    std::vector<core::Vec3> path;
    constexpr int kStepsPerRow = 64; // 0.5 m per frame across 32 m
    for (int row = 0; row < 4; ++row) {
        const float z = kPitch * (static_cast<float>(row) + 0.5f);
        for (int s = 0; s <= kStepsPerRow; ++s) {
            const float t = static_cast<float>(s) / kStepsPerRow;
            const float x = (row % 2 == 0 ? t : 1.0f - t) * 4.0f * kPitch;
            path.push_back({x, 100.0f, z});
        }
    }
    for (int s = 0; s < 8; ++s) {
        path.push_back({1.0f, 100.0f, 1.0f}); // teleport, and dwell
    }
    for (int s = 0; s <= 64; ++s) {
        const float t = static_cast<float>(s) / 64.0f;
        path.push_back({t * 31.0f, 100.0f, t * 31.0f});
    }
    return path;
}

std::vector<std::pair<TerrainTileCoord, assets::HeightfieldAsset>> grid_tiles(int nx, int nz) {
    std::vector<std::pair<TerrainTileCoord, assets::HeightfieldAsset>> v;
    for (std::int32_t z = 0; z < nz; ++z) {
        for (std::int32_t x = 0; x < nx; ++x) {
            v.emplace_back(TerrainTileCoord{x, z}, world_tile({x, z}));
        }
    }
    return v;
}

} // namespace

// ── The encoders, checked against the engine's own readers ─────────────────────────────────────

TEST_CASE("m19.8a: the test's cooked-file encoders round-trip through the engine's readers") {
    assets::AssetError err{};
    const auto v1 = assets::read_heightfield(encode_heightfield(world_tile({2, 1})), err);
    REQUIRE(v1.has_value());
    CHECK(v1->samples == world_tile({2, 1}).samples);
    CHECK(v1->origin.x == 16.0f);
    CHECK(v1->origin.z == 8.0f);
    CHECK_FALSE(v1->has_splat());

    const auto tile = splat_tile({0, 0}, {AssetId{7}, AssetId{9}, {}, {}}, {128, 127, 0, 0});
    const auto v2 = assets::read_heightfield(encode_heightfield(tile), err);
    REQUIRE(v2.has_value());
    CHECK(v2->has_splat());
    CHECK(v2->layers[1].value == 9);
    CHECK(v2->weights == tile.weights);

    const auto m = assets::read_material(encode_material({{0.25f, 0.5f, 0.75f}, 0.5f, 0.3f}), err);
    REQUIRE(m.has_value());
    CHECK(m->base_color[2] == 0.75f);
    CHECK(m->roughness == 0.3f);

    const auto l = assets::read_terrain_layer(
        encode_terrain_layer(AssetId{1}, AssetId{2}, 2.0f, 3.0f, 0.5f), err);
    REQUIRE(l.has_value());
    CHECK(l->albedo_height.value == 2);
    CHECK(l->uv_scale[1] == 3.0f);
    CHECK(l->height_contrast == 0.5f);
}

// ── (b) the fence rule, on the CPU, with fake frames ────────────────────────────────────────────

TEST_CASE("m19.8a: a slot is never reused while a frame that read it is unretired (fake fences)") {
    render::TerrainSlotTable t(2);
    const auto a = t.acquire();
    const auto b = t.acquire();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->slot == 0);
    CHECK(b->slot == 1);
    CHECK_FALSE(t.acquire().has_value()); // the budget

    // Frames 3 and 7 read slot 0; frame 5 reads slot 1.
    CHECK(t.mark_read(*a, 3));
    CHECK(t.mark_read(*a, 7));
    CHECK(t.mark_read(*b, 5));
    CHECK(t.last_read(0) == 7);

    REQUIRE(t.evict(*a));
    CHECK(t.state(0) == render::TerrainSlotTable::State::Retiring);
    // Evicted is not free: the slot is still being read by frame 7 for all we know.
    CHECK_FALSE(t.acquire().has_value());

    std::vector<std::uint32_t> freed;
    // Watermark 6: frames up to 6 are done, frame 7 is not. Slot 0 waits.
    CHECK(t.reclaim(6, freed) == 1);
    CHECK(freed.empty());
    CHECK_FALSE(t.acquire().has_value());
    CHECK(t.state(0) == render::TerrainSlotTable::State::Retiring);
    // Watermark 7: the last reader has retired. Now — and only now — the slot comes back.
    CHECK(t.reclaim(7, freed) == 0);
    REQUIRE(freed == std::vector<std::uint32_t>{0});
    const auto c = t.acquire();
    REQUIRE(c.has_value());
    CHECK(c->slot == 0);
    CHECK(c->generation == a->generation + 1);

    // A slot evicted without ever being read is reclaimable at once, at any watermark.
    REQUIRE(t.evict(*c));
    freed.clear();
    CHECK(t.reclaim(0, freed) == 0);
    CHECK(freed == std::vector<std::uint32_t>{0});

    // abandon() is only for a slot no frame has read: a read slot must go through evict.
    CHECK_FALSE(t.abandon(*b));
    const auto d = t.acquire();
    REQUIRE(d.has_value());
    CHECK(t.abandon(*d));
    CHECK(t.state(d->slot) == render::TerrainSlotTable::State::Free);
}

// ── (c) stale ids, on the CPU ───────────────────────────────────────────────────────────────────

TEST_CASE("m19.8a: an evicted id is stale at once and stays stale after its slot is reused") {
    render::TerrainSlotTable t(1);
    const auto a = t.acquire();
    REQUIRE(a.has_value());
    CHECK(t.resolve(*a));
    const render::TerrainResidentId copy = *a; // a second holder of the same id
    REQUIRE(t.evict(*a));
    // Stale from the instant of eviction — before any reuse — through every copy.
    CHECK_FALSE(t.resolve(copy));
    CHECK_FALSE(t.mark_read(copy, 9));
    CHECK_FALSE(t.evict(copy));
    std::vector<std::uint32_t> freed;
    t.reclaim(0, freed);
    const auto b = t.acquire();
    REQUIRE(b.has_value());
    CHECK(b->slot == a->slot); // the same index, a new tenant…
    CHECK(t.resolve(*b));
    CHECK_FALSE(t.resolve(copy)); // …which the old id cannot reach
    CHECK_FALSE(t.mark_read(copy, 10));
    CHECK(t.last_read(b->slot) == 0);
}

// ── (a) the plateau ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("m19.8a: a long traversal over a 4x4 world never exceeds the 4-slot budget, and tile "
          "bytes plateau") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("plateau");
    const assets::TerrainWorld world = write_world(dir.path, grid_tiles(4, 4));
    REQUIRE(world.tiles().size() == 16);
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);

    render::TerrainResidencyConfig cfg{};
    cfg.slots = 4;
    cfg.activation_radius = 0.25f * kPitch; // < pitch/2: at most a 2x2 block is ever wanted
    cfg.retention_radius = 0.5f * kPitch;
    render::TerrainResidency residency(*device, pass, server, world, dir.path, nullptr, cfg);

    // Every tile has the same shape, so every tile has the same byte count.
    const std::uint64_t tile_bytes =
        std::uint64_t{kSamples} * kSamples * 2 + std::uint64_t{8} * 8 * 6 * 4 + 4 + 160;
    const View view = top_down(16.0f, 16.0f, 16.0f);
    const std::vector<core::Vec3> path = traversal_path();
    std::uint64_t max_bytes = 0;
    std::uint32_t max_pass_tiles = 0;
    std::uint64_t frames_at_cap = 0;
    for (const core::Vec3& eye : path) {
        residency_frame(*device, server, residency, eye, view);
        const render::TerrainResidencyStats& s = residency.stats();
        REQUIRE(s.resident_slots + s.retiring_slots <= cfg.slots);
        REQUIRE(s.resident_bytes <= cfg.slots * tile_bytes);
        REQUIRE(s.resident_bytes ==
                std::uint64_t{s.resident_slots + s.retiring_slots} * tile_bytes);
        // The pass's own tile store is bounded too: ids are recycled, not appended forever.
        REQUIRE(pass.tile_count() == s.resident_slots + s.retiring_slots);
        // The CPU side is bounded as well: no more heightfields live than records in flight.
        REQUIRE(server.live_heightfield_slots() <= 16);
        max_bytes = std::max(max_bytes, s.resident_bytes);
        max_pass_tiles =
            std::max<std::uint32_t>(max_pass_tiles, static_cast<std::uint32_t>(pass.tile_count()));
        frames_at_cap += s.resident_bytes == cfg.slots * tile_bytes ? 1 : 0;
    }
    const render::TerrainResidencyStats& s = residency.stats();
    // Churn happened — far more uploads than slots — while the bytes held stayed at the plateau.
    CHECK(s.uploads >= 16);
    CHECK(max_bytes == cfg.slots * tile_bytes);
    CHECK(s.peak_resident_bytes == max_bytes);
    CHECK(s.peak_occupied_slots == cfg.slots);
    CHECK(max_pass_tiles == cfg.slots);
    CHECK(frames_at_cap > 0);
    // Every upload is accounted for: still resident, or evicted (and every eviction reclaimed,
    // except those still retiring).
    CHECK(s.uploads == s.evictions + s.resident_slots);
    CHECK(s.reclaims == s.evictions - s.retiring_slots);
    CHECK(s.upload_failures == 0);
    CHECK(s.refused_loads + s.refused_world + s.refused_palettes == 0);
    CHECK(s.stale_draws == 0);
    CHECK(s.unclosed_frames == 0);
    CHECK(s.missing.refused == 0);
    CHECK(s.missing.upload_failed == 0);
    CHECK(s.missing.not_loaded > 0); // a request is Ready one frame later, by construction
    // CPU heightfields: every request was released (uploaded or cancelled) except those in flight.
    const assets::StreamCounters sc = server.stream_counters();
    CHECK(sc.requests == s.heightfield_requests);
    CHECK(s.heightfield_requests - s.heightfield_releases == server.live_heightfield_slots());
    CHECK(sc.stale_handle_resolutions == 0);
    MESSAGE("m19.8a plateau: " << path.size() << " frames, " << s.uploads << " uploads, "
                               << s.evictions << " evictions, " << s.reclaims << " reclaims, "
                               << s.cancelled_loads << " cancelled loads; tile bytes plateau "
                               << max_bytes << " (" << frames_at_cap
                               << " frames at the cap); missing tile-frames not_loaded="
                               << s.missing.not_loaded
                               << " no_free_slot=" << s.missing.no_free_slot);
}

// ── (b) the GPU smoke: the device's real submit / fence path ────────────────────────────────────

TEST_CASE("m19.8a: on the real fence path, no slot is recycled before its last reader retires") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("fence");
    const assets::TerrainWorld world = write_world(dir.path, grid_tiles(4, 4));
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 4;
    cfg.activation_radius = 0.25f * kPitch;
    cfg.retention_radius = 0.5f * kPitch;
    render::TerrainResidency residency(*device, pass, server, world, dir.path, nullptr, cfg);

    // Three frames in flight: each ring entry owns a render graph (its transient targets must
    // outlive the GPU work) and the ticket it was submitted with. The test keeps its OWN map of
    // frame -> ticket, so its check below does not lean on the residency's bookkeeping.
    struct Frame {
        std::unique_ptr<render::RenderGraph> graph;
        rhi::SubmitTicket ticket{};
    };

    std::array<Frame, 3> ring;
    std::vector<rhi::SubmitTicket> ticket_of(1, rhi::SubmitTicket{}); // index = frame number
    const View view = top_down(16.0f, 16.0f, 16.0f);
    std::uint64_t recycled = 0;
    std::uint64_t violations = 0;
    std::uint64_t retiring_seen = 0;
    for (const core::Vec3& eye : traversal_path()) {
        Frame& f = ring[residency.frame() % ring.size()];
        if (f.ticket.is_valid()) {
            device->wait(f.ticket); // the ring entry's previous frame — the others stay in flight
            f.ticket = {};
        }
        f.graph = std::make_unique<render::RenderGraph>(*device);

        // What each slot held, and which frame last read it, BEFORE this frame's reclaim.
        std::array<std::uint32_t, 4> gen{};
        std::array<render::TerrainFrame, 4> last{};
        std::array<render::TerrainSlotTable::State, 4> state{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            gen[i] = residency.slots().generation(i);
            last[i] = residency.slots().last_read(i);
            state[i] = residency.slots().state(i);
        }
        settle(server);
        residency.begin_frame(eye);
        for (std::uint32_t i = 0; i < 4; ++i) {
            retiring_seen +=
                residency.slots().state(i) == render::TerrainSlotTable::State::Retiring;
            // A slot has been RECYCLED when it was Retiring and no longer is (reclaimed, perhaps
            // re-acquired in the same breath), or was Occupied and now holds a new generation
            // without being Retiring (evicted, reclaimed and re-acquired inside one begin_frame).
            // If a frame had read it, that frame's fence must have signalled — asked of the
            // DEVICE, through the test's own ticket, not of the residency.
            using St = render::TerrainSlotTable::State;
            const St now = residency.slots().state(i);
            const bool changed = (state[i] == St::Retiring && now != St::Retiring) ||
                                 (state[i] == St::Occupied &&
                                  residency.slots().generation(i) != gen[i] && now != St::Retiring);
            if (changed && last[i] != 0) {
                ++recycled;
                violations += device->is_complete(ticket_of[last[i]]) ? 0 : 1;
            }
        }
        render::RenderGraph& graph = *f.graph;
        graph.reset();
        // HEAVY frames on purpose: a 64² frame finishes before the CPU reaches the next
        // begin_frame, and then every fence has signalled and the rule is never exercised. A
        // 1024² target drawn several times over keeps frames genuinely in flight.
        constexpr std::uint32_t kSmoke = 1024;
        const render::RGTexture hdr =
            graph.create_texture({{kSmoke, kSmoke}, render::kHdrFormat, "m19.8a-hdr"});
        const render::RGTexture depth =
            graph.create_texture({{kSmoke, kSmoke}, render::kDepthFormat, "m19.8a-depth"});
        declare_clear(graph, hdr, depth);
        for (int k = 0; k < 8; ++k) {
            residency.add(graph, hdr, depth, view.view_proj, view.eye, ambient_light());
        }
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        f.ticket = device->submit(std::move(cmd));
        REQUIRE(f.ticket.is_valid());
        ticket_of.push_back(f.ticket);
        REQUIRE(ticket_of.size() == residency.frame() + 1);
        residency.end_frame(f.ticket);
    }
    for (Frame& f : ring) {
        device->wait(f.ticket);
    }
    const render::TerrainResidencyStats& s = residency.stats();
    CHECK(violations == 0);
    CHECK(recycled > 0); // the check was not vacuous: slots really were recycled
    CHECK(s.evictions > 0);
    CHECK(s.frames_retired > 0);
    CHECK(s.unclosed_frames == 0);
    MESSAGE("m19.8a fence smoke: " << s.evictions << " evictions, " << recycled
                                   << " recycled slots checked against the device, "
                                   << s.reclaim_waits << " reclaim waits on an unretired reader, "
                                   << retiring_seen << " slot-frames seen Retiring");
}

// ── p1: the fly-through's timed frame loop hands back every submission it makes ────────────────

// The Release terrain_flythrough crash (p1). The sample's frame was: begin_frame (which uploads
// through submit_blocking), draw, submit, wait_and_borrow to read the pass timings, end_frame —
// and never release. Every frame leaked one fence and one command buffer; on NVIDIA each pinned a
// device fd, so under a 1024-fd limit vkCreateFence answered VK_ERROR_OUT_OF_HOST_MEMORY a few
// thousand frames in, inside an upload, and the driver segfaulted on the null fence that followed.
//
// This is that loop, over the traversal that uploads, evicts and re-uploads tiles, timed through
// `RenderGraph::submit_and_time` exactly as the sample now is. After EVERY frame the device's live
// fences, command buffers and in-flight submissions must be back at the baseline: a leak of one
// per frame is a red frame count here, three hundred frames before it could ever be a crash.
TEST_CASE("p1: a timed terrain frame loop with uploads holds the device's live fences and command "
          "buffers flat") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("p1-submissions");
    const assets::TerrainWorld world = write_world(dir.path, grid_tiles(4, 4));
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 4;
    cfg.activation_radius = 0.25f * kPitch;
    cfg.retention_radius = 0.5f * kPitch;
    render::TerrainResidency residency(*device, pass, server, world, dir.path, nullptr, cfg);

    const View view = top_down(16.0f, 16.0f, 16.0f);
    const rhi::SubmissionCounters base = device->submission_counters();
    std::uint64_t frames = 0;
    std::uint64_t drifted = 0; // frames that ended with more live submission objects than baseline
    rhi::SubmissionCounters worst = base;
    for (const core::Vec3& eye : traversal_path()) {
        settle(server);
        residency.begin_frame(eye);
        render::RenderGraph graph(*device);
        graph.reset();
        const render::RGTexture hdr =
            graph.create_texture({{kSize, kSize}, render::kHdrFormat, "p1-hdr"});
        const render::RGTexture depth =
            graph.create_texture({{kSize, kSize}, render::kDepthFormat, "p1-depth"});
        declare_clear(graph, hdr, depth);
        residency.add(graph, hdr, depth, view.view_proj, view.eye, ambient_light());
        auto cmd = device->begin_commands();
        REQUIRE(cmd != nullptr);
        graph.execute(*cmd);
        (void)graph.submit_and_time(*device, std::move(cmd));
        residency.end_frame_blocking();
        ++frames;

        const rhi::SubmissionCounters c = device->submission_counters();
        const bool flat = c.live_fences == base.live_fences &&
                          c.live_command_buffers == base.live_command_buffers &&
                          c.in_flight_submissions == base.in_flight_submissions;
        drifted += flat ? 0 : 1;
        worst.live_fences = std::max(worst.live_fences, c.live_fences);
        worst.live_command_buffers = std::max(worst.live_command_buffers, c.live_command_buffers);
    }
    const rhi::SubmissionCounters end = device->submission_counters();
    CHECK(frames > 300);
    CHECK(residency.stats().uploads > 0); // the loop really did upload (submit_blocking) tiles
    CHECK(residency.stats().evictions > 0);
    CHECK(drifted == 0);
    CHECK(end.live_fences == base.live_fences);
    CHECK(end.live_command_buffers == base.live_command_buffers);
    CHECK(end.in_flight_submissions == base.in_flight_submissions);
    CHECK(end.failed_submissions == 0);
    MESSAGE("p1: " << frames << " frames, " << residency.stats().uploads << " uploads; peak live "
                   << worst.live_fences << " fences / " << worst.live_command_buffers
                   << " command buffers (baseline " << base.live_fences << " / "
                   << base.live_command_buffers << ")");
}

// ── (c) a stale id draws nothing ────────────────────────────────────────────────────────────────

TEST_CASE("m19.8a: drawing an evicted tile's old id is byte-identical to not drawing it, and is "
          "counted") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("stale");
    const assets::TerrainWorld world = write_world(dir.path, grid_tiles(2, 1));
    core::JobSystem jobs(1);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 1; // one slot: B can only arrive by evicting A into A's own slot
    cfg.activation_radius = 1.0f;
    cfg.retention_radius = 2.0f;
    render::TerrainResidency residency(*device, pass, server, world, dir.path, nullptr, cfg);
    const View view = top_down(8.0f, 4.0f, 8.0f); // sees both tiles

    const core::Vec3 on_a{3.0f, 100.0f, 4.0f};
    const core::Vec3 on_b{13.0f, 100.0f, 4.0f};
    for (int i = 0; i < 3; ++i) {
        residency_frame(*device, server, residency, on_a, view);
    }
    const render::TerrainResidentId id_a = residency.resident({0, 0});
    REQUIRE(id_a.is_valid());
    // Hysteresis first: just over the seam, B is wanted but A is still KEPT (1.5 m away, inside
    // the 2 m retention), so the one slot is not taken from A — B waits, counted as no_free_slot.
    for (int i = 0; i < 3; ++i) {
        residency_frame(*device, server, residency, {9.5f, 100.0f, 4.0f}, view);
    }
    CHECK(residency.resident({0, 0}) == id_a);
    CHECK(residency.stats().missing_this_frame.no_free_slot == 1);
    CHECK(residency.stats().evictions == 0);
    for (int i = 0; i < 3; ++i) {
        residency_frame(*device, server, residency, on_b, view);
    }
    const render::TerrainResidentId id_b = residency.resident({1, 0});
    REQUIRE(id_b.is_valid());
    REQUIRE_FALSE(residency.resident({0, 0}).is_valid());
    REQUIRE(id_b.slot == id_a.slot); // B lives in A's old slot: the case a bare index gets wrong
    REQUIRE(residency.stats().evictions == 1);

    settle(server);
    residency.begin_frame(on_b);
    const std::uint64_t stale_before = residency.stats().stale_draws;
    bool drew_stale = true;
    const auto with_stale =
        render_frame(*device, [&](render::RenderGraph& g, auto hdr, auto depth) {
            drew_stale =
                residency.add_tile(g, id_a, hdr, depth, view.view_proj, view.eye, ambient_light());
        });
    const auto nothing = render_frame(*device, [](render::RenderGraph&, auto, auto) {});
    const auto live_b = render_frame(*device, [&](render::RenderGraph& g, auto hdr, auto depth) {
        CHECK(residency.add_tile(g, id_b, hdr, depth, view.view_proj, view.eye, ambient_light()));
    });
    residency.end_frame_blocking();

    CHECK_FALSE(drew_stale);
    CHECK(residency.stats().stale_draws == stale_before + 1);
    CHECK(with_stale.size() == nothing.size());
    CHECK(std::memcmp(with_stale.data(), nothing.data(), nothing.size()) == 0);
    // Not vacuous: the slot's live tenant IS in view, so a stale id that resolved to it would
    // have changed the picture.
    CHECK(covered_count(live_b) > 0);
    CHECK(std::memcmp(live_b.data(), nothing.data(), nothing.size()) != 0);
}

// ── (d) the builder ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr AssetId kMatA{0x1001};
constexpr AssetId kMatB{0x1002};
constexpr AssetId kTex{0x2001};
constexpr AssetId kLayer1{0x3001};
constexpr AssetId kLayer2{0x3002};
constexpr AssetId kTexAsPalette{0x2001}; // a texture id put where a layer belongs

const MaterialSpec kMatASpec{{0.6f, 0.5f, 0.3f}, 0.0f, 0.8f};
const MaterialSpec kMatBSpec{{0.2f, 0.4f, 0.7f}, 0.25f, 0.5f};

fs::path fixture_texture() {
    return fs::path(RIME_ASSETS_FIXTURE_DIR) / "terrain_layer_albedo_height.rtex";
}

// Cook the layer set into `dir` and return its manifest: two materials, the m19.7a fixture's packed
// albedo+height texture, and two TerrainLayers that SHARE that texture.
assets::Manifest write_layers(const fs::path& dir) {
    write_file(dir / "mat_a.rmat", encode_material(kMatASpec));
    write_file(dir / "mat_b.rmat", encode_material(kMatBSpec));
    write_file(dir / "layer_1.rtl", encode_terrain_layer(kMatA, kTex, 2.0f, 3.0f, 0.5f));
    write_file(dir / "layer_2.rtl", encode_terrain_layer(kMatB, kTex, 1.5f, 1.5f, 0.5f));
    const std::string text = "# rime-manifest m19.8a test\n"
                             "a.mat\tmaterial\t0000000000001001\tmat_a.rmat\n"
                             "b.mat\tmaterial\t0000000000001002\tmat_b.rmat\n"
                             "t.png\ttexture\t0000000000002001\t" +
                             fixture_texture().string() +
                             "\n"
                             "l1.toml\tterrain_layer\t0000000000003001\tlayer_1.rtl\n"
                             "l2.toml\tterrain_layer\t0000000000003002\tlayer_2.rtl\n";
    auto m = assets::Manifest::parse(text);
    REQUIRE(m.has_value());
    return *m;
}

render::TerrainPaletteState resolve(assets::AssetServer& server,
                                    render::TerrainLayerBuilder& builder,
                                    render::TerrainPaletteHandle h) {
    render::TerrainPaletteState s = render::TerrainPaletteState::Pending;
    for (int i = 0; i < 8 && s == render::TerrainPaletteState::Pending; ++i) {
        settle(server);
        s = builder.update(h);
    }
    return s;
}

render::TerrainLayer by_hand(const MaterialSpec& m) {
    render::TerrainLayer l{};
    l.base_color = {m.rgb[0], m.rgb[1], m.rgb[2]};
    l.metallic = m.metallic;
    l.roughness = m.roughness;
    return l;
}

} // namespace

TEST_CASE("m19.8a: a TerrainLayer palette renders BIT-IDENTICALLY to the same palette built by "
          "hand") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("builder-layer");
    const assets::Manifest manifest = write_layers(dir.path);
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainLayerBuilder builder(*device, server, manifest, dir.path);

    const std::array<AssetId, 4> ids{kLayer1, kLayer2, {}, {}};
    const render::TerrainPaletteHandle h = builder.request(ids);
    REQUIRE(resolve(server, builder, h) == render::TerrainPaletteState::Ready);
    const render::TerrainPalette* built = builder.palette(h);
    REQUIRE(built != nullptr);

    // BY HAND: the scalars typed in from the material specs, the layer fields from the records,
    // and the texture through the OTHER path every material texture takes — GpuAssetBridge.
    render::GpuAssetBridge bridge(*device, server);
    const auto tex = bridge.request_texture(fixture_texture());
    settle(server);
    REQUIRE(bridge.drain() == 1);
    render::TerrainPalette hand{};
    hand[0] = by_hand(kMatASpec);
    hand[0].albedo_height = bridge.texture_or_placeholder(tex);
    hand[0].uv_scale[0] = 2.0f;
    hand[0].uv_scale[1] = 3.0f;
    hand[0].height_contrast = 0.5f;
    hand[1] = by_hand(kMatBSpec);
    hand[1].albedo_height = bridge.texture_or_placeholder(tex);
    hand[1].uv_scale[0] = 1.5f;
    hand[1].uv_scale[1] = 1.5f;
    hand[1].height_contrast = 0.5f;
    // The fields match exactly (the texture handle differs: two uploads of one file).
    for (std::size_t k = 0; k < 2; ++k) {
        CHECK((*built)[k].base_color.x == hand[k].base_color.x);
        CHECK((*built)[k].base_color.z == hand[k].base_color.z);
        CHECK((*built)[k].metallic == hand[k].metallic);
        CHECK((*built)[k].roughness == hand[k].roughness);
        CHECK((*built)[k].uv_scale[1] == hand[k].uv_scale[1]);
        CHECK((*built)[k].height_contrast == hand[k].height_contrast);
        CHECK((*built)[k].albedo_height.is_valid());
    }
    CHECK((*built)[0].albedo_height == (*built)[1].albedo_height); // shared: ONE upload

    const assets::HeightfieldAsset tile = splat_tile({0, 0}, ids, {128, 127, 0, 0});
    const render::TerrainTileId built_id = pass.upload(tile, *built);
    const render::TerrainTileId hand_id = pass.upload(tile, hand);
    render::TerrainPalette untextured = hand;
    untextured[0].albedo_height = {};
    untextured[1].albedo_height = {};
    const render::TerrainTileId plain_id = pass.upload(tile, untextured);
    REQUIRE(built_id != render::kInvalidTerrainTile);
    REQUIRE(hand_id != render::kInvalidTerrainTile);
    REQUIRE(plain_id != render::kInvalidTerrainTile);

    const View view = top_down(4.0f, 4.0f, 4.0f);
    const auto draw = [&](render::TerrainTileId id) {
        return render_frame(*device, [&](render::RenderGraph& g, auto hdr, auto depth) {
            pass.add(g, hdr, depth, id, view.view_proj, view.eye, ambient_light());
        });
    };
    const auto a = draw(built_id);
    const auto b = draw(hand_id);
    const auto c = draw(plain_id);
    CHECK(covered_count(a) == kSize * kSize);
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
    // Not vacuous: the texture reaches the picture.
    CHECK(std::memcmp(a.data(), c.data(), a.size()) != 0);
    CHECK(builder.release(h));
}

TEST_CASE("m19.8a: a Material palette resolves to today's scalar path, bit for bit") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("builder-material");
    const assets::Manifest manifest = write_layers(dir.path);
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainLayerBuilder builder(*device, server, manifest, dir.path);

    const std::array<AssetId, 4> ids{kMatA, kMatB, {}, {}};
    const render::TerrainPaletteHandle h = builder.request(ids);
    REQUIRE(resolve(server, builder, h) == render::TerrainPaletteState::Ready);
    const render::TerrainPalette* built = builder.palette(h);
    REQUIRE(built != nullptr);
    CHECK_FALSE((*built)[0].albedo_height.is_valid()); // a Material has no layer texture
    CHECK((*built)[0].uv_scale[0] == 1.0f);
    CHECK((*built)[0].height_contrast == 0.0f);
    CHECK(builder.live_textures() == 0);

    const render::TerrainPalette hand{by_hand(kMatASpec), by_hand(kMatBSpec), {}, {}};
    render::TerrainPalette swapped{by_hand(kMatBSpec), by_hand(kMatASpec), {}, {}};
    const assets::HeightfieldAsset tile = splat_tile({0, 0}, ids, {200, 55, 0, 0});
    const auto built_id = pass.upload(tile, *built);
    const auto hand_id = pass.upload(tile, hand);
    const auto swapped_id = pass.upload(tile, swapped);
    REQUIRE(built_id != render::kInvalidTerrainTile);
    REQUIRE(hand_id != render::kInvalidTerrainTile);
    REQUIRE(swapped_id != render::kInvalidTerrainTile);
    const View view = top_down(4.0f, 4.0f, 4.0f);
    const auto draw = [&](render::TerrainTileId id) {
        return render_frame(*device, [&](render::RenderGraph& g, auto hdr, auto depth) {
            pass.add(g, hdr, depth, id, view.view_proj, view.eye, ambient_light());
        });
    };
    const auto a = draw(built_id);
    const auto b = draw(hand_id);
    const auto c = draw(swapped_id);
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
    CHECK(std::memcmp(a.data(), c.data(), a.size()) != 0); // not vacuous: slot order matters
    CHECK(builder.release(h));
}

TEST_CASE("m19.8a: the builder refuses ids the manifest does not list and kinds a palette may not "
          "name, and counts each") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("builder-refuse");
    const assets::Manifest manifest = write_layers(dir.path);
    core::JobSystem jobs(1);
    assets::AssetServer server(jobs);
    render::TerrainLayerBuilder builder(*device, server, manifest, dir.path);

    const auto unknown = builder.request({AssetId{0xDEAD}, {}, {}, {}});
    CHECK(resolve(server, builder, unknown) == render::TerrainPaletteState::Failed);
    CHECK(builder.counters().unresolved_ids == 1);
    const auto texture = builder.request({kMatA, kTexAsPalette, {}, {}});
    CHECK(resolve(server, builder, texture) == render::TerrainPaletteState::Failed);
    CHECK(builder.counters().wrong_kind == 1);
    CHECK(builder.palette(texture) == nullptr);
    CHECK(builder.counters().palettes_failed == 2);
    CHECK(builder.release(unknown));
    CHECK(builder.release(texture));
    CHECK_FALSE(builder.release(texture)); // twice: refused, nothing changes
    CHECK(builder.counters().palettes_released == 2);
    CHECK(builder.live_textures() == 0);
}

TEST_CASE("m19.8a: a shared layer texture is uploaded once and destroyed exactly once, with the "
          "last palette") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("builder-shared");
    const assets::Manifest manifest = write_layers(dir.path);
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainLayerBuilder builder(*device, server, manifest, dir.path);

    // p1 names the texture twice (two layers share it), p2 once.
    const auto p1 = builder.request({kLayer1, kLayer2, {}, {}});
    const auto p2 = builder.request({kLayer1, kMatA, {}, {}});
    REQUIRE(resolve(server, builder, p1) == render::TerrainPaletteState::Ready);
    REQUIRE(resolve(server, builder, p2) == render::TerrainPaletteState::Ready);
    CHECK(builder.counters().textures_uploaded == 1);
    CHECK(builder.live_textures() == 1);
    CHECK(builder.texture_references() == 3);
    CHECK(builder.texture_bytes() > 0);
    CHECK(server.live_terrain_layer_slots() == 0); // the records were handed back once copied

    CHECK(builder.release(p1));
    CHECK(builder.live_textures() == 1); // p2 still draws with it
    CHECK(builder.texture_references() == 1);
    CHECK(builder.counters().textures_destroyed == 0);
    CHECK_FALSE(builder.release(p1)); // a second release cannot spend p2's reference
    CHECK(builder.texture_references() == 1);

    CHECK(builder.release(p2));
    CHECK(builder.live_textures() == 0);
    CHECK(builder.texture_references() == 0);
    CHECK(builder.counters().textures_destroyed == 1);
    CHECK(builder.counters().textures_uploaded == 1);

    // Coming back re-uploads (from the CPU copy the server retains) — and is released once again.
    const auto p3 = builder.request({kLayer2, {}, {}, {}});
    REQUIRE(resolve(server, builder, p3) == render::TerrainPaletteState::Ready);
    CHECK(builder.counters().textures_uploaded == 2);
    CHECK(builder.release(p3));
    CHECK(builder.counters().textures_destroyed == 2);
    CHECK(builder.live_palettes() == 0);
}

TEST_CASE("m19.8a: through the residency, splat tiles sharing a layer release its texture once, "
          "after the last of them retires") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("builder-residency");
    const assets::Manifest manifest = write_layers(dir.path);
    std::vector<std::pair<TerrainTileCoord, assets::HeightfieldAsset>> tiles;
    for (std::int32_t x = 0; x < 3; ++x) {
        tiles.emplace_back(TerrainTileCoord{x, 0},
                           splat_tile({x, 0}, {kLayer1, kLayer2, {}, {}}, {128, 127, 0, 0}));
    }
    const assets::TerrainWorld world = write_world(dir.path, tiles);
    core::JobSystem jobs(2);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainLayerBuilder builder(*device, server, manifest, dir.path);
    {
        render::TerrainResidencyConfig cfg{};
        cfg.slots = 2;
        cfg.activation_radius = 1.0f;
        cfg.retention_radius = 2.0f;
        render::TerrainResidency residency(*device, pass, server, world, dir.path, &builder, cfg);
        const View view = top_down(12.0f, 4.0f, 12.0f);
        // Stand on the (0,0)|(1,0) seam, then walk to (2,0): (0,0) is evicted for it.
        for (int i = 0; i < 6; ++i) {
            residency_frame(*device, server, residency, {8.0f, 100.0f, 4.0f}, view);
        }
        CHECK(residency.resident_tiles().size() == 2);
        CHECK(builder.live_palettes() == 2);
        for (int i = 0; i < 6; ++i) {
            residency_frame(*device, server, residency, {20.0f, 100.0f, 4.0f}, view);
        }
        CHECK(residency.resident({2, 0}).is_valid());
        CHECK(residency.stats().evictions >= 1);
        CHECK(residency.stats().refused_palettes == 0);
        // The texture stayed alive through the eviction — another resident tile still samples it.
        CHECK(builder.counters().textures_uploaded == 1);
        CHECK(builder.counters().textures_destroyed == 0);
        CHECK(builder.live_textures() == 1);
        CHECK(builder.counters().palettes_released == residency.stats().reclaims);
    }
    // The residency is gone, so every palette was released — and the texture with the last one.
    CHECK(builder.live_palettes() == 0);
    CHECK(builder.counters().textures_uploaded == 1);
    CHECK(builder.counters().textures_destroyed == 1);
    CHECK(builder.live_textures() == 0);
}

// ── (e) seams ───────────────────────────────────────────────────────────────────────────────────

TEST_CASE("m19.8a: two adjacent resident tiles with matching borders leave no gap at the seam") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("seam");
    const assets::TerrainWorld world = write_world(dir.path, grid_tiles(2, 1));
    core::JobSystem jobs(1);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 2;
    cfg.activation_radius = 2.0f;
    cfg.retention_radius = 4.0f;
    render::TerrainResidency residency(*device, pass, server, world, dir.path, nullptr, cfg);

    // The camera stands ON the seam x = 8; the view is the 8 m square x in [4, 12], z in [0, 8]:
    // the right half of (0,0) and the left half of (1,0), seam down the middle column pair.
    const core::Vec3 on_seam{8.0f, 100.0f, 4.0f};
    const View view = top_down(8.0f, 4.0f, 4.0f);
    for (int i = 0; i < 3; ++i) {
        residency_frame(*device, server, residency, on_seam, view);
    }
    REQUIRE(residency.resident({0, 0}).is_valid());
    REQUIRE(residency.resident({1, 0}).is_valid());
    CHECK(residency.refusals().border_mismatches == 0);

    settle(server);
    residency.begin_frame(on_seam);
    const auto both = render_frame(*device, [&](render::RenderGraph& g, auto hdr, auto depth) {
        residency.add(g, hdr, depth, view.view_proj, view.eye, ambient_light());
    });
    const auto left = render_frame(*device, [&](render::RenderGraph& g, auto hdr, auto depth) {
        residency.add_tile(
            g, residency.resident({0, 0}), hdr, depth, view.view_proj, view.eye, ambient_light());
    });
    residency.end_frame_blocking();

    // Every pixel row crosses the seam (pixels 31 | 32 straddle x = 8); every pixel of every row
    // is covered — no column on either side of the seam is open.
    std::uint32_t uncovered = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            uncovered += covered(both, x, y) ? 0 : 1;
        }
    }
    CHECK(uncovered == 0);
    // Not vacuous: with only the left tile, the right half of the view is open.
    CHECK(covered_count(left) == kSize * kSize / 2);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        CHECK(covered(left, 31, y));
        CHECK_FALSE(covered(left, 32, y));
    }
}

// ── (f) refusals, through the residency ─────────────────────────────────────────────────────────

TEST_CASE("m19.8a: duplicate coordinate, spacing and border mismatches are refused and counted, "
          "and so is a tile the pass will not upload") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("refuse");
    // (0,0) good; (1,0) cooked at the wrong spacing; (2,0) good; (3,0)'s west edge one step off
    // from (2,0)'s east edge.
    assets::HeightfieldAsset wrong_spacing = world_tile({1, 0});
    wrong_spacing.cell_size_x = 1.0001f;
    assets::HeightfieldAsset cracked = world_tile({3, 0});
    cracked.samples[cracked.index(0, 4)] += 1;
    const std::vector<std::pair<TerrainTileCoord, assets::HeightfieldAsset>> tiles = {
        {{0, 0}, world_tile({0, 0})},
        {{1, 0}, wrong_spacing},
        {{2, 0}, world_tile({2, 0})},
        {{3, 0}, cracked},
    };
    for (const auto& [c, a] : tiles) {
        write_file(dir.path / tile_file(c), encode_heightfield(a));
    }
    // The world FILE, with a duplicate (2,0) line pointing at the good tile's twin.
    std::string text = "grid\t9\t1\t1\t0.01\t0\t0\t0\t0\n";
    for (const auto& [c, a] : tiles) {
        text += "tile\t" + std::to_string(c.x) + "\t" + std::to_string(c.z) + "\t1\t0\t20\t0\t" +
                tile_file(c) + "\n";
    }
    text += "tile\t2\t0\t9\t0\t20\t0\ttile_0_0.rhf\n";
    const auto world = assets::TerrainWorld::parse(text);
    REQUIRE(world.has_value());
    CHECK(world->refusals().duplicate_coords == 1);
    CHECK(world->find({2, 0})->path == tile_file({2, 0}));

    core::JobSystem jobs(1);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 4;
    cfg.activation_radius = 1.0f;
    cfg.retention_radius = 2.0f;
    render::TerrainResidency residency(*device, pass, server, *world, dir.path, nullptr, cfg);
    const View view = top_down(16.0f, 4.0f, 16.0f);
    for (const float x : {4.0f, 8.0f, 12.0f, 20.0f, 24.0f, 28.0f}) {
        for (int i = 0; i < 3; ++i) {
            residency_frame(*device, server, residency, {x, 100.0f, 4.0f}, view);
        }
    }
    const auto& r = residency.refusals();
    CHECK(r.spacing_mismatches == 1);
    CHECK(r.border_mismatches == 1);
    CHECK(residency.stats().refused_world == 2);
    CHECK(residency.stats().missing.refused > 0);
    CHECK(residency.stats().upload_failures == 0);
    CHECK_FALSE(residency.resident({1, 0}).is_valid());
    CHECK_FALSE(residency.resident({3, 0}).is_valid());
    CHECK(residency.resident({2, 0}).is_valid());
}

TEST_CASE("m19.8a: a tile TerrainPass refuses is missing as upload_failed, not as not_loaded") {
    auto device = make_device();
    if (!device) {
        return;
    }
    TempDir dir("upload-failed");
    // A world of 1025-sample tiles: valid for the asset reader (ADR-0060 allows 32768) but past
    // the pass's kMaxTileSamplesPerAxis, so the upload — and only the upload — refuses.
    assets::TerrainWorldGrid g = world_grid();
    g.samples = render::kMaxTileSamplesPerAxis + 1;
    auto world = assets::TerrainWorld::make(g);
    REQUIRE(world.has_value());
    assets::HeightfieldAsset big{};
    big.columns = g.samples;
    big.rows = g.samples;
    big.cell_size_x = 1.0f;
    big.cell_size_z = 1.0f;
    big.height_scale = kScale;
    big.samples.assign(big.sample_count(), 1000);
    write_file(dir.path / "big.rhf", encode_heightfield(big));
    REQUIRE(world->add_tile({{0, 0}, 1, 0.0f, 20.0f, {}, "big.rhf"}));

    core::JobSystem jobs(1);
    assets::AssetServer server(jobs);
    render::TerrainPass pass(*device);
    render::TerrainResidencyConfig cfg{};
    cfg.slots = 1;
    cfg.activation_radius = 1.0f;
    cfg.retention_radius = 2.0f;
    render::TerrainResidency residency(*device, pass, server, *world, dir.path, nullptr, cfg);
    const View view = top_down(4.0f, 4.0f, 4.0f);
    residency_frame(*device, server, residency, {4.0f, 100.0f, 4.0f}, view);
    CHECK(residency.stats().missing_this_frame.not_loaded == 1);
    residency_frame(*device, server, residency, {4.0f, 100.0f, 4.0f}, view);
    CHECK(residency.stats().missing_this_frame.upload_failed == 1);
    CHECK(residency.stats().upload_failures == 1);
    CHECK(pass.tiles_refused() == 1);
    // Sticky while kept: not retried (and re-refused) every frame.
    residency_frame(*device, server, residency, {4.0f, 100.0f, 4.0f}, view);
    CHECK(residency.stats().upload_failures == 1);
    CHECK(residency.stats().missing.upload_failed == 2);
    // The failed upload gave its slot straight back.
    CHECK(residency.slots().count(render::TerrainSlotTable::State::Free) == 1);
    CHECK(server.live_heightfield_slots() == 0);
}
