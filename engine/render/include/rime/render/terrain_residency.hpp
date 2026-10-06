// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/terrain_world.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/terrain_builder.hpp" // TerrainPaletteHandle
#include "rime/render/terrain_pass.hpp"

// TERRAIN RENDER RESIDENCY (m19.8a, ADR-0069): a world of terrain tiles drawn from a FIXED budget
// of GPU tile slots, chosen around the camera every frame.
//
// m19.3–m19.7 drew a terrain whose every tile was uploaded once and stayed until the pass died.
// That cannot draw a world larger than GPU memory. m19.8b (ADR-0067) made the CPU side releasable;
// this is the GPU side: a constructor-fixed number of slots, filled with the tiles nearest the
// camera and refilled as it moves.
//
// ── THE FRAME ───────────────────────────────────────────────────────────────────────────────────
//
//   server.pump();                       // the app's frame point: CPU loads become Ready
//   residency.begin_frame(eye);          // fences → reclaim; want/keep; request; upload
//   residency.add(graph, hdr, depth, …); // draw every resident tile (each read is recorded)
//   … graph.execute(*cmd); ticket = device.submit(std::move(cmd));
//   residency.end_frame(ticket);         // or end_frame_blocking() after submit_blocking
//
// ── WANT, KEEP, EVICT (hysteresis) ──────────────────────────────────────────────────────────────
//
// A tile is WANTED when its footprint is within `activation_radius` of the camera in XZ, and KEPT
// while within `retention_radius` (>= activation). A tile between the two is neither requested nor
// thrown away, so a camera pacing back and forth across one boundary does not upload and evict the
// same tile every other frame — the classic streaming thrash. A resident tile beyond retention is
// only a CANDIDATE: it is evicted (farthest first) when a wanted tile needs its slot, so a tile
// that is merely out of range but not in the way stays cached.
//
// ── FENCE-SAFE EVICTION: the m18.5 rule, not the m18.5 code ────────────────────────────────────
//
// The RHI destroys a resource at once. Destroying a tile that a submitted-but-unfinished frame is
// still drawing is a use-after-free on the GPU — invisible on a fast GPU, a crash or garbage on a
// slow one. The virtual-geometry page pool (m18.5, virtual_geometry_page_pool.cpp) solved the same
// problem and its RULES are reused here verbatim, its geometry ABI not at all:
//
//   * every frame that draws records, per slot it read, "frame F read this slot";
//   * each submitted frame's SubmitTicket is kept IN ORDER; the RETIREMENT WATERMARK is the newest
//     frame such that it and every frame before it have a signalled fence (`Device::is_complete`).
//     Completion is taken strictly in order — the watermark means "everything up to here is done";
//   * EVICTION and REUSE are two different moments. Evicting a tile (`TerrainSlotTable::evict`)
//     bumps the slot's generation at once — so no new draw can name it — and parks the slot as
//     RETIRING. Only when the watermark reaches the last frame that read it is the slot RECLAIMED:
//     its GPU resources destroyed and the slot free for the next tile.
//   * "We submitted it" is never taken as "it finished" — except after `submit_blocking`, where it
//     is literally true (`end_frame_blocking`). A frame ended with an INVALID ticket never ran, so
//     it read nothing and retires at once (the page pool's `end_frame` rule).
//
// ── GENERATION-CHECKED IDS ──────────────────────────────────────────────────────────────────────
//
// `TerrainResidentId` is a slot plus that slot's generation. A slot is reused for a different tile,
// so an id held across an eviction would — with a bare index — draw the NEW tenant: the wrong
// ground, in the right place, with nothing to say so. `add_tile` checks the generation and draws
// NOTHING for a stale id (no pass declared, so the frame is byte-identical to one without the
// call), and counts it in `stale_draws`.
//
// ── MISSING TILES ARE ALLOWED, AND COUNTED (no coarse cover until m19.8d) ──────────────────────
//
// A wanted tile that is not resident is a HOLE this brick does not cover. Every frame, every wanted
// tile that is not resident is counted under exactly one reason:
//
//   not_loaded     its heightfield (or its palette) is still loading
//   no_free_slot   ready to upload, but every slot holds a kept tile or is still retiring
//   upload_failed  TerrainPass::upload refused it (counted there too, by cause)
//   refused        the load failed, its palette failed, or it does not fit the world (check_tile /
//                  edges_match — counted by cause in `refusals()`)
//
// A refused or failed tile stays so while it is kept, rather than being retried every frame;
// leaving retention forgets it.
//
// ── WHAT IS NOT HERE ────────────────────────────────────────────────────────────────────────────
//
//   * no LOD and no coarse cover: a missing tile is a hole (brick 8d);
//   * no byte budget: the budget is a slot COUNT; bytes are measured, not enforced (brick 8e);
//   * collision is not driven from here and never will be — camera residency must not decide what
//     the simulation stands on (ADR-0067's plan, brick 8c);
//   * uploads are synchronous (TerrainPass::upload); the asynchrony is the AssetServer's;
//   * a border mismatch refuses whichever of the two tiles arrives SECOND, which depends on travel
//     order. A world whose borders do not match is a cook bug; the counter is how it is found.
//
// Deleting this file, terrain_builder.*, and their CMake lines leaves the engine building: nothing
// else in rime_render includes them (the modularity guardrail, as terrain_pass.hpp keeps it).
namespace rime::render {

// Frames are numbered by the residency from 1; 0 means "never read".
using TerrainFrame = std::uint64_t;

struct TerrainResidentId {
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
    std::uint32_t slot = kNoSlot;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return slot != kNoSlot; }

    friend constexpr bool operator==(TerrainResidentId a, TerrainResidentId b) noexcept {
        return a.slot == b.slot && a.generation == b.generation;
    }
};

// The slot POLICY, with no GPU and no assets in it — so the fence rule can be driven with fake
// frames on the CPU (the m18.5 split: VirtualGeometryPageCache plans, the pool executes).
//
//   Free ──acquire──▶ Occupied ──evict (generation++)──▶ Retiring ──reclaim(watermark)──▶ Free
//                         └──────────abandon (never read)──────────────────────────────▶ Free
class TerrainSlotTable {
public:
    enum class State : std::uint8_t { Free, Occupied, Retiring };

    explicit TerrainSlotTable(std::uint32_t slots);

    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(slots_.size());
    }

    // Take the lowest-numbered FREE slot (deterministic), or nullopt. A Retiring slot is never
    // handed out: that is the whole rule.
    [[nodiscard]] std::optional<TerrainResidentId> acquire();

    // True for an Occupied slot whose generation matches.
    [[nodiscard]] bool resolve(TerrainResidentId id) const noexcept;

    // Record that frame `frame` reads this slot. False (and nothing recorded) for a stale id.
    bool mark_read(TerrainResidentId id, TerrainFrame frame) noexcept;

    // Occupied → Retiring; the generation is bumped NOW, so `id` and every copy of it go stale at
    // once. False for a stale id.
    bool evict(TerrainResidentId id) noexcept;

    // Occupied → Free for a slot that was never read (an upload that failed after acquire). False
    // for a stale id or a slot some frame has read — that one must go through evict.
    bool abandon(TerrainResidentId id) noexcept;

    // Free every Retiring slot whose last reader is at or below `retired` — the confirmed
    // watermark — appending each to `out` so the caller destroys its resources first. Returns how
    // many Retiring slots are still waiting on an unretired reader.
    std::uint32_t reclaim(TerrainFrame retired, std::vector<std::uint32_t>& out);

    [[nodiscard]] State state(std::uint32_t slot) const noexcept { return slots_[slot].state; }

    [[nodiscard]] TerrainFrame last_read(std::uint32_t slot) const noexcept {
        return slots_[slot].last_read;
    }

    [[nodiscard]] std::uint32_t generation(std::uint32_t slot) const noexcept {
        return slots_[slot].generation;
    }

    [[nodiscard]] std::uint32_t count(State s) const noexcept;

private:
    struct Slot {
        State state = State::Free;
        std::uint32_t generation = 0;
        TerrainFrame last_read = 0;
    };

    std::vector<Slot> slots_;
};

struct TerrainResidencyConfig {
    std::uint32_t slots = 4;        // the GPU tile budget (>= 1)
    float activation_radius = 0.0f; // request tiles within this (XZ, from the tile footprint)
    float retention_radius = 0.0f;  // keep tiles within this; clamped up to activation
};

// Per-reason counts of wanted-but-not-resident tiles (tile-frames).
struct TerrainMissCounts {
    std::uint64_t not_loaded = 0;
    std::uint64_t no_free_slot = 0;
    std::uint64_t upload_failed = 0;
    std::uint64_t refused = 0;

    [[nodiscard]] std::uint64_t total() const noexcept {
        return not_loaded + no_free_slot + upload_failed + refused;
    }
};

struct TerrainResidencyStats {
    std::uint64_t frames_begun = 0;
    std::uint64_t frames_retired = 0;
    std::uint64_t unclosed_frames = 0; // begin_frame with the previous frame never ended (waited)
    std::uint64_t heightfield_requests = 0;
    std::uint64_t heightfield_releases = 0;
    std::uint64_t cancelled_loads = 0; // left retention before it became resident
    std::uint64_t uploads = 0;
    std::uint64_t upload_failures = 0;
    std::uint64_t refused_loads = 0;    // the heightfield load failed
    std::uint64_t refused_palettes = 0; // the builder could not resolve the palette (or no builder)
    std::uint64_t refused_world = 0;    // check_tile / edges_match said no (by cause: refusals())
    std::uint64_t evictions = 0;
    std::uint64_t reclaims = 0;
    std::uint64_t reclaim_waits = 0; // slot-frames a Retiring slot waited on an unretired reader
    std::uint64_t draws = 0;
    std::uint64_t stale_draws = 0;
    std::uint64_t draws_outside_frame = 0;
    // Gauges, and their peaks since construction.
    std::uint32_t resident_slots = 0;
    std::uint32_t retiring_slots = 0;
    std::uint32_t peak_occupied_slots = 0; // resident + retiring — what the GPU actually holds
    std::uint64_t resident_bytes = 0;      // tile bytes held (resident + retiring), not textures
    std::uint64_t peak_resident_bytes = 0;
    std::uint32_t frames_in_flight = 0;
    TerrainMissCounts missing{};            // cumulative
    TerrainMissCounts missing_this_frame{}; // the last begin_frame's
};

class TerrainResidency {
public:
    // Borrows everything. `world_dir` resolves the world's relative tile paths. `builder` may be
    // null for a world of v1 (no splat) tiles; a splat tile is then refused and counted.
    TerrainResidency(rhi::Device& device,
                     TerrainPass& pass,
                     assets::AssetServer& server,
                     const assets::TerrainWorld& world,
                     std::filesystem::path world_dir,
                     TerrainLayerBuilder* builder,
                     const TerrainResidencyConfig& config);
    // Waits for every frame it was told about, then releases every tile, palette and heightfield.
    ~TerrainResidency();

    TerrainResidency(const TerrainResidency&) = delete;
    TerrainResidency& operator=(const TerrainResidency&) = delete;

    // Open frame N+1 around camera position `eye` (main thread, after AssetServer::pump()).
    void begin_frame(const core::Vec3& eye);

    // Draw every resident tile into the open frame. Outside a frame: nothing, counted.
    void add(RenderGraph& graph,
             RGTexture hdr,
             RGTexture depth,
             const core::Mat4& view_proj,
             const core::Vec3& eye,
             const TerrainLight& light,
             const SkyLightBinding& sky = {});

    // Draw ONE tile by id. A stale id (or one outside a frame) declares nothing, is counted, and
    // returns false.
    bool add_tile(RenderGraph& graph,
                  TerrainResidentId id,
                  RGTexture hdr,
                  RGTexture depth,
                  const core::Mat4& view_proj,
                  const core::Vec3& eye,
                  const TerrainLight& light,
                  const SkyLightBinding& sky = {});

    // Close the open frame with its submission's ticket (invalid = it never ran).
    void end_frame(rhi::SubmitTicket ticket);
    // Close the open frame after submit_blocking returned: it has retired.
    void end_frame_blocking();

    // The id of the tile resident at `c`, or an invalid id.
    [[nodiscard]] TerrainResidentId resident(assets::TerrainTileCoord c) const;
    // The coordinates resident right now, in coordinate order.
    [[nodiscard]] std::vector<assets::TerrainTileCoord> resident_tiles() const;

    [[nodiscard]] const TerrainResidencyStats& stats() const noexcept { return stats_; }

    [[nodiscard]] const assets::TerrainWorldRefusals& refusals() const noexcept {
        return refusals_;
    }

    [[nodiscard]] const TerrainSlotTable& slots() const noexcept { return slots_; }

    [[nodiscard]] TerrainFrame frame() const noexcept { return frame_; }

    [[nodiscard]] TerrainFrame retired() const noexcept { return retired_; }

private:
    enum class Phase : std::uint8_t {
        Loading,      // heightfield requested
        Building,     // heightfield ready and checked; palette resolving / waiting for a slot
        Resident,     // in a slot
        Refused,      // load / palette / world check failed — sticky while kept
        UploadFailed, // TerrainPass refused it — sticky while kept
    };

    struct Record {
        Phase phase = Phase::Loading;
        assets::HeightfieldAssetHandle heightfield{};
        TerrainPaletteHandle palette = kInvalidTerrainPalette;
        bool splat = false;
        bool waiting_for_slot = false;
        TerrainResidentId id{};
        assets::TerrainTileEdges edges{};
    };

    // What a slot holds, kept until RECLAIM — after its record is gone.
    struct Payload {
        assets::TerrainTileCoord coord{};
        TerrainTileId pass_tile = kInvalidTerrainTile;
        TerrainPaletteHandle palette = kInvalidTerrainPalette;
        std::uint64_t bytes = 0;
    };

    struct Submitted {
        TerrainFrame frame = 0;
        rhi::SubmitTicket ticket{};
        bool retired = false;
    };

    void retire_submitted();
    void reclaim_slots();
    void forget(Record& r);
    void advance(assets::TerrainTileCoord c, Record& r, const core::Vec3& eye);
    [[nodiscard]] bool check_world(assets::TerrainTileCoord c,
                                   const assets::HeightfieldAsset& asset,
                                   const assets::TerrainTileEdges& edges);
    [[nodiscard]] std::optional<TerrainResidentId> take_slot(const core::Vec3& eye);
    void update_gauges();

    rhi::Device& device_;
    TerrainPass& pass_;
    assets::AssetServer& server_;
    const assets::TerrainWorld& world_;
    std::filesystem::path world_dir_;
    TerrainLayerBuilder* builder_;
    TerrainResidencyConfig config_;
    TerrainSlotTable slots_;
    std::vector<Payload> payloads_;
    std::map<assets::TerrainTileCoord, Record> records_; // ordered: deterministic iteration
    std::deque<Submitted> submitted_;
    TerrainFrame frame_ = 0;
    TerrainFrame retired_ = 0;
    bool frame_open_ = false;
    TerrainResidencyStats stats_{};
    assets::TerrainWorldRefusals refusals_{};
};

} // namespace rime::render
