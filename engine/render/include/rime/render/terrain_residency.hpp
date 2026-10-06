// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/terrain_world.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/terrain_builder.hpp" // TerrainPaletteHandle
#include "rime/render/terrain_lod.hpp"
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
// ── LOD (m19.8d2, ADR-0071): PRESSURE LOWERS DETAIL, NEVER COVERAGE ─────────────────────────────
//
// A world whose manifest carries a LOD chain (`level_count() > 1`) is driven by CDLOD selection
// (terrain_lod.hpp) instead of the two radii, and its records are keyed by (level, x, z):
//
//   * THE ROOT COVER IS PINNED. Every top-level tile has a slot reserved for it from the
//     constructor on: a non-root tile may take a free slot only while more are free than there are
//     roots still waiting, and a resident root is never an eviction candidate. Why pin rather than
//     just "want" them? Because a root is the tile that can be drawn when NOTHING else is
//     resident — the guarantee "coarser, never a hole" needs one tile per patch of world that no
//     amount of pressure can take away. Wanting is not enough: with slots handed out nearest
//     first, a cluster of fine tiles near the camera would fill the budget before a distant root
//     ever arrived, and that root's whole patch would be a hole. A world whose root cover alone
//     exceeds the budget is refused at construction, counted (`root_cover_refusals`), and draws
//     nothing — no budget can honour the guarantee.
//   * WANTED = the selection with every tile assumed usable (the IDEAL selection), plus every
//     ancestor of its leaves up to the roots; PREFETCH = the four children of an ideal leaf whose
//     box is within one level diagonal of the range that would split it. Wanted tiles load and
//     take slots first, nearest first (ties: coarser first, so an ancestor never queues behind its
//     own descendant), then prefetch. A wanted tile may evict a resident tile that is neither
//     wanted nor pinned, and failing that one that is only prefetched; nothing evicts a wanted
//     tile. The fence rule is m19.8a's, unchanged.
//   * DRAWN = the selection with `usable` = resident and trusted. A node whose children are not
//     all resident draws itself: a FALLBACK, counted per tile-frame (`fallback_draws`) where
//     m19.8a counted a hole. Parents draw with the flat (no-splat) material — their appearance is
//     brick 8d3's — counted as `fallback_appearance_draws`.
//   * THE 8d1 HANDOFF: before a child is uploaded, `samples_coincide` compares it with its
//     resident parent (a parent keeps its CPU heightfield while resident, for exactly this). A
//     child waits for its parent (`parent_waits`); the parent is always wanted first. A mismatch
//     REFUSES THE PARENT — evicted, sticky, counted in `refusals().coincidence_mismatches` — and
//     the child is uploaded: the area is drawn by children where they are all resident and by the
//     grandparent where not. A verified pair is remembered, so a parent that is evicted and
//     reloaded is not re-checked against children already proven against the same file.
//
// A world with no chain (level 0 only) is driven exactly as m19.8a drove it: the radii, holes
// counted, no pinning.
//
// ── WHAT IS NOT HERE ────────────────────────────────────────────────────────────────────────────
//
//   * no coarse APPEARANCE: a parent is drawn with the flat material (brick 8d3);
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
    // m19.8d2: how a world with a LOD chain derives its ranges. The radii above are not used for
    // such a world; the selection decides what is wanted.
    TerrainLodView lod{};
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
    // m19.8d2 — LOD worlds only (all zero for a level-0 world).
    std::uint64_t root_cover_refusals = 0;       // the roots alone exceed the slot budget
    std::uint32_t pinned_roots = 0;              // gauge: roots resident right now
    std::uint64_t pinned_evictions = 0;          // must stay 0: no pressure evicts a root
    std::uint64_t lod_draws = 0;                 // leaves drawn
    std::uint64_t fallback_draws = 0;            // tile-frames drawn coarser than selected
    std::uint64_t fallback_appearance_draws = 0; // parents drawn with the placeholder material
    std::uint64_t uncovered_draws = 0;           // root-frames nobody could draw (a refused root)
    std::uint64_t balance_collapses = 0;         // selection nodes collapsed to restore 2:1
    std::uint64_t coincidence_checks = 0;        // parent/child pairs compared (samples_coincide)
    std::uint64_t refused_parents = 0;           // parents refused by a coincidence mismatch
    std::uint64_t parent_waits = 0;              // tile-frames a loaded child waited for its parent
    std::uint64_t prefetch_requests = 0;         // loads requested for the prefetch set
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

    // The id of the level-0 tile resident at `c`, or an invalid id.
    [[nodiscard]] TerrainResidentId resident(assets::TerrainTileCoord c) const;
    // The id of the tile resident at `k` (any level), or an invalid id.
    [[nodiscard]] TerrainResidentId resident(assets::TerrainTileKey k) const;
    // The level-0 coordinates resident right now, in coordinate order.
    [[nodiscard]] std::vector<assets::TerrainTileCoord> resident_tiles() const;
    // m19.8d2: every resident tile, any level, in key order.
    [[nodiscard]] std::vector<assets::TerrainTileKey> resident_keys() const;

    // m19.8d2: true when the world carries a LOD chain and is driven by selection.
    [[nodiscard]] bool lod() const noexcept { return lod_; }

    // m19.8d2: the ranges the selection uses (empty for a level-0 world).
    [[nodiscard]] const TerrainLodRanges& ranges() const noexcept { return ranges_; }

    // m19.8d2: the last begin_frame's drawn selection, and the ideal one it was fed from.
    [[nodiscard]] const TerrainLodSelection& selection() const noexcept { return selection_; }

    [[nodiscard]] const TerrainLodSelection& ideal_selection() const noexcept { return ideal_; }

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
        bool pinned = false; // m19.8d2: a root of a LOD world
    };

    // What a slot holds, kept until RECLAIM — after its record is gone.
    struct Payload {
        assets::TerrainTileKey key{};
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
    void advance(assets::TerrainTileKey k, Record& r, const core::Vec3& eye);
    [[nodiscard]] bool check_world(assets::TerrainTileKey k,
                                   const assets::HeightfieldAsset& asset,
                                   const assets::TerrainTileEdges& edges);
    // m19.8d2: the 8d1 handoff. False = wait (the parent is not resident yet).
    [[nodiscard]] bool check_parent(assets::TerrainTileKey k,
                                    const assets::HeightfieldAsset& asset);
    void refuse_parent(assets::TerrainTileKey parent);
    [[nodiscard]] std::optional<TerrainResidentId> take_slot(assets::TerrainTileKey k,
                                                             const core::Vec3& eye);
    void request(assets::TerrainTileKey k);
    void begin_frame_lod(const core::Vec3& eye);
    [[nodiscard]] bool usable(assets::TerrainTileKey k) const;
    [[nodiscard]] float distance(assets::TerrainTileKey k, const core::Vec3& eye) const;
    void draw_leaf(RenderGraph& graph,
                   const TerrainLodLeaf& leaf,
                   RGTexture hdr,
                   RGTexture depth,
                   const core::Mat4& view_proj,
                   const core::Vec3& eye,
                   const TerrainLight& light,
                   const SkyLightBinding& sky);
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
    std::map<assets::TerrainTileKey, Record> records_; // ordered: deterministic iteration
    std::deque<Submitted> submitted_;
    TerrainFrame frame_ = 0;
    TerrainFrame retired_ = 0;
    bool frame_open_ = false;
    TerrainResidencyStats stats_{};
    assets::TerrainWorldRefusals refusals_{};
    // m19.8d2
    bool lod_ = false;
    bool lod_refused_ = false; // the root cover exceeds the budget: draws nothing
    TerrainLodRanges ranges_{};
    TerrainLodSelection ideal_{};
    TerrainLodSelection selection_{};
    core::Vec3 selection_eye_{0.0f, 0.0f, 0.0f};
    std::set<assets::TerrainTileKey> wanted_;          // ideal leaves + ancestors + roots
    std::set<assets::TerrainTileKey> prefetch_;        // next-finer level inside the ranges
    std::set<assets::TerrainTileKey> refused_parents_; // sticky: a coincidence mismatch
    std::set<std::pair<assets::TerrainTileKey, assets::TerrainTileKey>> verified_pairs_;
};

} // namespace rime::render
