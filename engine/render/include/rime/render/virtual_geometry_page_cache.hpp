// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/virtual_geometry.hpp"
#include "rime/render/virtual_geometry_residency.hpp"
#include "rime/render/virtual_geometry_selection.hpp"

// M18.5 (ADR-0043 gate 5): the POLICY half of bounded virtual-geometry page streaming. It decides
// which page goes into which slot of a fixed-capacity pool, when, and which page leaves to make
// room — and it knows nothing about Vulkan, buffers or command lists. The GPU half
// (virtual_geometry_page_pool.hpp) executes its decisions; keeping the two apart is what lets every
// rule below be proved on the CPU with a fake frame clock, deterministically, in microseconds.
//
// THE FRAME CLOCK. Frames are numbered 1, 2, 3, … by the caller. Each begin_frame() carries two
// numbers: the frame about to be recorded (`current`) and the newest frame whose GPU work is
// CONFIRMED finished (`retired_through`: every frame <= it has retired; 0 = none yet). The
// confirmed part is the whole point. In a real loop it comes from a fence poll (the pool derives it
// from rhi::Device::is_complete on each frame's SubmitTicket), never from "N frames have passed",
// which is only true until a driver hitches.
//
// TWO RULES FOLLOW FROM IT, and they are the two ways a streaming cache corrupts a frame:
//
//   1. A page becomes RESIDENT only when the frame that uploaded it has retired. "We recorded the
//      copy" is not holding — the copy may still be queued behind three frames of work — and the
//      repo's replication guardrail (CLAUDE.md #5) is the same rule in another costume: a "what
//      they have" fact strengthens only on confirmed holding. Until then the page is UPLOADING and
//      the selector cannot see it, so the coarser ancestor keeps drawing.
//   2. A slot is REUSED only when every frame that read its old page has retired. Each page
//      remembers the last frame whose draws read it (mark_used); eviction picks only pages whose
//      last read is <= retired_through. Overwriting a slot any sooner lets an in-flight frame pull
//      half-new vertices — a torn mesh for one frame, the classic streaming flicker.
//
// The permanently-resident coarse cut is pinned into its own slots at registration and is never a
// victim, which is what makes the fallback hole-free: whatever else is missing, the selector can
// always stop at a resident ancestor. Every way a request does not become an upload — budget,
// staging still in flight, no evictable slot, a dependency not yet on its way — has its own
// counter, because a streaming proof that cannot see what it deferred still reads as passing.
namespace rime::render {

using VirtualGeometryFrame = std::uint64_t;

struct VirtualGeometryPageCacheConfig {
    // Pool capacity in fixed-size slots. Fixed-size slots (the Nanite shape) make allocation a free
    // list and eviction a one-for-one swap with no fragmentation; the price is that a small page
    // wastes the rest of its slot.
    std::uint32_t slot_count = 0;
    // Bytes per slot. Must be a multiple of 4 (indices are read as u32 words from the pool) and of
    // every registered asset's vertex stride (vertices are addressed in whole vertices from the
    // pool's start). Every page of a registered asset must fit in one slot.
    std::uint32_t slot_bytes = 0;
    // Upload bytes per frame. This is also the size of one staging-ring segment, so "a frame never
    // uploads more than its budget" is enforced twice: by the planner and by the space it has.
    // Must be >= slot_bytes, or a maximum-size page could never be uploaded at all.
    std::uint32_t upload_budget_bytes = 0;
    // Staging-ring depth in frames. Frame F writes segment F % staging_segments, and only once the
    // last frame that used that segment has retired; a deeper ring lets uploads keep flowing while
    // more frames are in flight.
    std::uint32_t staging_segments = 3;

    [[nodiscard]] bool is_valid() const noexcept;

    [[nodiscard]] std::uint64_t pool_bytes() const noexcept {
        return std::uint64_t{slot_count} * slot_bytes;
    }

    [[nodiscard]] std::uint64_t staging_bytes() const noexcept {
        return std::uint64_t{staging_segments} * upload_budget_bytes;
    }
};

// One planned upload: copy the page's cooked bytes to `staging_offset` in the staging ring now (a
// host write), then to `slot * slot_bytes` in the pool (a GPU copy recorded into frame `frame`).
struct VirtualGeometryPageUpload {
    assets::AssetId asset{};
    std::uint32_t page = 0;
    std::uint32_t slot = 0;
    std::uint32_t generation = 0; // the slot's allocation generation after this upload
    std::uint32_t bytes = 0;
    std::uint64_t staging_offset = 0;
    VirtualGeometryFrame frame = 0;
};

// Cumulative since construction unless noted. Each deferred/rejected path counts once per request
// per frame, so a request stuck for five frames adds five — the counter measures pressure, not
// distinct pages.
struct VirtualGeometryPageCacheStats {
    std::uint64_t requests = 0;           // valid request() calls, deduplicated per frame
    std::uint64_t requests_invalid = 0;   // unknown asset or page index
    std::uint64_t requests_satisfied = 0; // already resident (the request only refreshes LRU)
    std::uint64_t requests_in_flight = 0; // already uploading; nothing more to do
    std::uint64_t uploads_issued = 0;     // copies planned (NOT resident yet — see rule 1)
    std::uint64_t upload_bytes = 0;       // bytes of those copies
    std::uint64_t uploads_retired = 0;    // uploads whose frame retired: pages became resident
    std::uint64_t uploads_abandoned = 0;  // frame's submission failed; the page went back to absent
    std::uint64_t evictions = 0;          // resident transient pages dropped to free a slot
    std::uint64_t deferred_budget = 0;    // did not fit this frame's remaining upload budget
    std::uint64_t deferred_staging_busy = 0; // this frame's staging segment is still in flight
    std::uint64_t deferred_no_slot = 0;      // no free slot and no victim whose last read retired
    std::uint64_t deferred_dependency = 0;   // a dependency is neither resident nor uploading
    std::uint64_t fallbacks = 0; // selections that stopped at an ancestor (note_fallbacks)
    std::uint64_t use_of_nonresident =
        0;                             // mark_used() on a page that is not resident: a caller bug
    std::uint64_t clock_rejected = 0;  // begin_frame() with a retirement claim it cannot trust
    std::uint32_t permanent_pages = 0; // pinned at registration, never evicted
    // This frame only (reset by begin_frame), and the worst frame so far — the budget proof reads
    // the latter, so one over-budget frame anywhere in a run cannot hide behind the average.
    std::uint32_t frame_upload_bytes = 0;
    std::uint32_t max_frame_upload_bytes = 0;
};

class VirtualGeometryPageCache {
public:
    explicit VirtualGeometryPageCache(const VirtualGeometryPageCacheConfig& config);

    VirtualGeometryPageCache(const VirtualGeometryPageCache&) = delete;
    VirtualGeometryPageCache& operator=(const VirtualGeometryPageCache&) = delete;

    [[nodiscard]] bool is_valid() const noexcept { return config_.is_valid(); }

    [[nodiscard]] const VirtualGeometryPageCacheConfig& config() const noexcept { return config_; }

    // Register an asset and pin its permanently-resident pages into slots of their own. Those
    // placements are appended to `permanent_uploads`; the caller must have them on the device
    // before any frame draws the asset (the pool does a blocking upload — confirmed holding by
    // construction). Rejects an invalid asset, a page larger than a slot, a slot size that is not a
    // whole number of the asset's vertices, too few free slots for the coarse cut, and — as the
    // residency contract does — a different asset object under an already-registered id. The
    // asset must outlive the cache: pages are uploaded from its bytes.
    [[nodiscard]] bool register_asset(assets::AssetId id,
                                      const assets::VirtualGeometryAsset& asset,
                                      std::vector<VirtualGeometryPageUpload>& permanent_uploads);

    // Open frame `current`. Promotes uploads whose frame is <= retired_through to resident (rule
    // 1), and remembers retired_through for slot reuse (rule 2). `current` must exceed the last
    // frame; `retired_through` must be below `current` (a frame not yet recorded cannot have
    // retired) and never goes backwards. A claim that breaks either is counted in clock_rejected
    // and the frame opens with the last trusted watermark — a lying clock can only delay
    // residency, never grant it.
    void begin_frame(VirtualGeometryFrame current, VirtualGeometryFrame retired_through);

    // This frame's draws read `page` (the selected cut and its dependencies): pins its slot
    // against reuse until this frame retires, and refreshes its LRU recency. A page that is not
    // resident is counted (use_of_nonresident) and ignored — the selector should never pick one.
    void mark_used(assets::AssetId id, std::uint32_t page);

    // Ask for `page` this frame. Lower `priority` is served first (the streaming driver passes the
    // DAG depth, so coarse refinements land before fine ones and the cut sharpens a level at a
    // time); ties keep request order. A resident page's request refreshes its recency, which is
    // what stops a page the cut is about to need from being chosen as a victim.
    void request(assets::AssetId id, std::uint32_t page, std::uint32_t priority);

    // Count selections that stopped at a resident ancestor because a child was not resident.
    void note_fallbacks(std::uint32_t count) noexcept { stats_.fallbacks += count; }

    // Turn this frame's requests into uploads, in priority order, until the budget is spent.
    // Evicts least-recently-used transient pages whose last read has retired when no slot is free.
    // Pages are UPLOADING afterwards; the returned span is valid until the next begin_frame().
    // Call once per frame, after the requests.
    [[nodiscard]] std::span<const VirtualGeometryPageUpload> plan_uploads();

    // The submission for frame `frame` failed: none of its copies ran. Its uploads return to
    // absent and their slots to the free list (nothing on the GPU reads a slot that was only
    // being uploaded). The staging segment is released with it.
    void abandon_frame(VirtualGeometryFrame frame);

    [[nodiscard]] bool is_resident(assets::AssetId id, std::uint32_t page) const noexcept;
    [[nodiscard]] bool is_uploading(assets::AssetId id, std::uint32_t page) const noexcept;
    // The pool slot holding a RESIDENT page; nullopt for an uploading or absent one, so a draw can
    // never be pointed at a slot whose bytes are not confirmed there.
    [[nodiscard]] std::optional<std::uint32_t> resident_slot(assets::AssetId id,
                                                             std::uint32_t page) const noexcept;
    [[nodiscard]] std::uint32_t slot_generation(std::uint32_t slot) const noexcept;
    [[nodiscard]] std::uint32_t free_slots() const noexcept;
    [[nodiscard]] const assets::VirtualGeometryAsset* asset(assets::AssetId id) const noexcept;

    // The confirmed-resident set, in the form every existing consumer already takes:
    // select_virtual_geometry reads page_residency_bytes(), the visibility pass reads residency().
    // Both change only in begin_frame() (promotion) and plan_uploads() (eviction).
    [[nodiscard]] std::span<const std::uint8_t>
    page_residency_bytes(assets::AssetId id) const noexcept {
        return residency_.page_residency_bytes(id);
    }

    [[nodiscard]] const VirtualGeometryResidency& residency() const noexcept { return residency_; }

    [[nodiscard]] const VirtualGeometryPageCacheStats& stats() const noexcept { return stats_; }

    [[nodiscard]] VirtualGeometryFrame current_frame() const noexcept { return current_; }

    [[nodiscard]] VirtualGeometryFrame retired_through() const noexcept { return retired_; }

private:
    enum class PageState : std::uint8_t { Absent, Uploading, Resident, Permanent };

    struct PageEntry {
        PageState state = PageState::Absent;
        std::uint32_t slot = 0;
        VirtualGeometryFrame upload_frame = 0;
        VirtualGeometryFrame last_read = 0;   // newest frame whose draws read this page
        VirtualGeometryFrame last_wanted = 0; // newest frame that requested it
        VirtualGeometryFrame requested_in = 0;
        std::uint32_t live_dependents = 0; // resident or uploading pages that depend on this one
    };

    struct AssetEntry {
        const assets::VirtualGeometryAsset* asset = nullptr;
        std::vector<PageEntry> pages;
    };

    struct SlotEntry {
        std::uint64_t asset = 0; // AssetId value of the occupant; 0 = free
        std::uint32_t page = 0;
        std::uint32_t generation = 0;
    };

    struct Request {
        std::uint64_t asset = 0;
        std::uint32_t page = 0;
        std::uint32_t priority = 0;
        std::uint32_t order = 0;
    };

    [[nodiscard]] AssetEntry* find(assets::AssetId id) noexcept;
    [[nodiscard]] const AssetEntry* find(assets::AssetId id) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> take_slot();
    void adjust_dependents(AssetEntry& entry, std::uint32_t page, int delta);
    void drop_to_absent(std::uint64_t asset, AssetEntry& entry, std::uint32_t page);

    VirtualGeometryPageCacheConfig config_;
    VirtualGeometryFrame current_ = 0;
    VirtualGeometryFrame retired_ = 0;
    bool planned_ = false;
    std::unordered_map<std::uint64_t, AssetEntry> assets_;
    std::vector<SlotEntry> slots_;
    std::vector<std::uint32_t> free_slots_;              // kept sorted descending: pop = lowest
    std::vector<VirtualGeometryFrame> staging_last_use_; // per segment
    std::vector<Request> requests_;
    std::vector<VirtualGeometryPageUpload> uploads_;   // this frame's plan
    std::vector<VirtualGeometryPageUpload> in_flight_; // planned, not yet retired
    VirtualGeometryResidency residency_;
    VirtualGeometryPageCacheStats stats_;
};

// ── The per-frame streaming driver ─────────────────────────────────────────────────────────────
//
// One call per asset instance per frame, between begin_frame() and plan_uploads():
//
//   1. select the cut from the CONFIRMED residency (select_virtual_geometry already falls back to
//      the nearest ancestor whose children are not all resident — that is the hole-free rule, and
//      this function counts it into the cache's `fallbacks`);
//   2. mark every page the cut's clusters read (and their dependencies) as used by this frame;
//   3. request every page the IDEAL cut — the one full residency would select for this camera —
//      needs, including every intermediate group on the way down, because the selector only
//      descends through a group whose children are all resident. Priority is DAG depth.
//
// The returned selection is what the frame draws.
[[nodiscard]] VirtualGeometrySelection
stream_virtual_geometry(VirtualGeometryPageCache& cache,
                        assets::AssetId id,
                        const assets::VirtualGeometryAsset& asset,
                        float pixels_per_metre,
                        float max_projected_error_px);

} // namespace rime::render
