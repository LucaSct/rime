// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The page-cache policy (M18.5). The header states the two retirement rules; the notes here are
// about the order things happen in within a frame, because that order is what makes them hold:
//
//   begin_frame   promote retired uploads            (rule 1: resident only once retired)
//   mark_used     stamp last_read = current           (pins the slot until THIS frame retires)
//   request       stamp last_wanted = current         (protects the next cut from eviction)
//   plan_uploads  evict only last_read <= retired     (rule 2: reuse only once every reader
//   retired)
//
// Because mark_used runs before plan_uploads, a page drawn this frame can never be this frame's
// victim, and because a victim's last read has retired, the copy into its slot — recorded into
// this frame — cannot overtake a draw that still reads the old bytes.
//
// Victim search is a linear scan over every registered page. That is O(pages) per upload and
// deliberately unoptimized: the counts in play are thousands, the uploads per frame are bounded by
// the budget, and an intrusive LRU list is a change to make after a profile says so, not before.

#include "rime/render/virtual_geometry_page_cache.hpp"

#include <algorithm>
#include <limits>

namespace rime::render {

bool VirtualGeometryPageCacheConfig::is_valid() const noexcept {
    // Pool offsets travel to the GPU as u32 word/vertex indices, so the pool must be addressable in
    // 32 bits of bytes; the staging ring likewise.
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
    return slot_count != 0 && slot_bytes != 0 && slot_bytes % 4 == 0 &&
           upload_budget_bytes >= slot_bytes && staging_segments != 0 && pool_bytes() <= kMax &&
           staging_bytes() <= kMax;
}

VirtualGeometryPageCache::VirtualGeometryPageCache(const VirtualGeometryPageCacheConfig& config)
    : config_(config) {
    if (!config_.is_valid()) {
        return; // every entry point checks is_valid(); an invalid cache registers nothing
    }
    slots_.resize(config_.slot_count);
    free_slots_.reserve(config_.slot_count);
    for (std::uint32_t slot = config_.slot_count; slot != 0; --slot) {
        free_slots_.push_back(slot - 1); // descending, so pop_back hands out the lowest slot
    }
    staging_last_use_.assign(config_.staging_segments, 0);
}

VirtualGeometryPageCache::AssetEntry* VirtualGeometryPageCache::find(assets::AssetId id) noexcept {
    const auto it = assets_.find(id.value);
    return it == assets_.end() ? nullptr : &it->second;
}

const VirtualGeometryPageCache::AssetEntry*
VirtualGeometryPageCache::find(assets::AssetId id) const noexcept {
    const auto it = assets_.find(id.value);
    return it == assets_.end() ? nullptr : &it->second;
}

std::optional<std::uint32_t> VirtualGeometryPageCache::take_slot() {
    if (free_slots_.empty()) {
        return std::nullopt;
    }
    const std::uint32_t slot = free_slots_.back();
    free_slots_.pop_back();
    return slot;
}

// A page with a live (resident or uploading) dependent cannot be evicted: the dependent would stay
// "resident" while reading a slot that now holds someone else's bytes.
void VirtualGeometryPageCache::adjust_dependents(AssetEntry& entry, std::uint32_t page, int delta) {
    const assets::VirtualGeometryPage& record = entry.asset->pages[page];
    for (std::uint32_t i = 0; i < record.dependency_count; ++i) {
        const std::uint32_t dependency =
            entry.asset->page_dependencies[record.first_dependency + i];
        std::uint32_t& live = entry.pages[dependency].live_dependents;
        live = delta > 0 ? live + 1 : (live == 0 ? 0 : live - 1);
    }
}

void VirtualGeometryPageCache::drop_to_absent(std::uint64_t asset,
                                              AssetEntry& entry,
                                              std::uint32_t page) {
    PageEntry& p = entry.pages[page];
    if (p.state == PageState::Resident) {
        (void)residency_.evict_page(assets::AssetId{asset}, page);
    }
    adjust_dependents(entry, page, -1);
    slots_[p.slot].asset = 0;
    // Keep the free list sorted descending so the lowest slot is always reused first: allocation
    // is then a pure function of the request history, which is what makes the tests deterministic.
    free_slots_.insert(
        std::lower_bound(free_slots_.begin(), free_slots_.end(), p.slot, std::greater<>{}), p.slot);
    p.state = PageState::Absent;
    p.slot = 0;
    p.upload_frame = 0;
}

bool VirtualGeometryPageCache::register_asset(
    assets::AssetId id,
    const assets::VirtualGeometryAsset& asset,
    std::vector<VirtualGeometryPageUpload>& permanent_uploads) {
    if (!is_valid() || !id.is_valid() ||
        assets::validate_virtual_geometry(asset) != assets::VirtualGeometryError::None ||
        asset.vertex_stride == 0 || config_.slot_bytes % asset.vertex_stride != 0) {
        return false;
    }
    if (const AssetEntry* existing = find(id)) {
        return existing->asset == &asset;
    }
    std::uint32_t permanent = 0;
    for (const assets::VirtualGeometryPage& page : asset.pages) {
        if (page.byte_size > config_.slot_bytes) {
            return false; // a page that fits no slot could never be streamed
        }
        if (page.permanently_resident) {
            ++permanent;
            // The coarse cut must be drawable from permanent pages alone, or "never evicted" would
            // still leave it waiting on a streamed dependency — a hole with extra steps.
            for (std::uint32_t i = 0; i < page.dependency_count; ++i) {
                if (!asset.pages[asset.page_dependencies[page.first_dependency + i]]
                         .permanently_resident) {
                    return false;
                }
            }
        }
    }
    if (permanent > free_slots_.size() || !residency_.register_asset(id, asset)) {
        return false;
    }

    AssetEntry entry;
    entry.asset = &asset;
    entry.pages.resize(asset.pages.size());
    for (std::uint32_t page = 0; page < asset.pages.size(); ++page) {
        if (!asset.pages[page].permanently_resident) {
            continue;
        }
        const std::uint32_t slot = *take_slot();
        SlotEntry& s = slots_[slot];
        s.asset = id.value;
        s.page = page;
        ++s.generation;
        entry.pages[page].state = PageState::Permanent;
        entry.pages[page].slot = slot;
        permanent_uploads.push_back(
            {id, page, slot, s.generation, asset.pages[page].byte_size, 0, 0});
    }
    stats_.permanent_pages += permanent;
    assets_.emplace(id.value, std::move(entry));
    return true;
}

void VirtualGeometryPageCache::begin_frame(VirtualGeometryFrame current,
                                           VirtualGeometryFrame retired_through) {
    // Clock hygiene first. A repeated or backwards frame number would stamp last_read with a frame
    // that may already count as retired, which is precisely how a slot gets reused under a live
    // reader; so the frame advances by one regardless, and the claim is counted.
    if (current <= current_) {
        ++stats_.clock_rejected;
        current = current_ + 1;
    }
    if (retired_through >= current || retired_through < retired_) {
        ++stats_.clock_rejected;
        retired_through = retired_;
    }
    current_ = current;
    retired_ = retired_through;
    requests_.clear();
    uploads_.clear();
    planned_ = false;
    stats_.frame_upload_bytes = 0;

    // Rule 1: promote every upload whose frame has retired. Dependencies are uploaded no later than
    // their dependents, so they retire in the same batch or earlier; the fixpoint loop lets a
    // dependent wait for its dependency within the batch, since the residency contract accepts a
    // page only once its dependencies are resident.
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto it = in_flight_.begin(); it != in_flight_.end();) {
            if (it->frame > retired_) {
                ++it;
                continue;
            }
            AssetEntry* entry = find(it->asset);
            PageEntry& p = entry->pages[it->page];
            if (!residency_.request_page(it->asset, it->page)) {
                ++it; // a dependency is still pending in this batch; try again next pass
                continue;
            }
            (void)residency_.complete_page(it->asset, it->page);
            p.state = PageState::Resident;
            ++stats_.uploads_retired;
            it = in_flight_.erase(it);
            progress = true;
        }
    }
    // Anything retired but still unpromotable depends on a page that was abandoned out from under
    // it. It cannot become drawable, so it gives its slot back rather than holding it forever.
    for (auto it = in_flight_.begin(); it != in_flight_.end();) {
        if (it->frame > retired_) {
            ++it;
            continue;
        }
        drop_to_absent(it->asset.value, *find(it->asset), it->page);
        ++stats_.uploads_abandoned;
        it = in_flight_.erase(it);
    }
}

void VirtualGeometryPageCache::mark_used(assets::AssetId id, std::uint32_t page) {
    AssetEntry* entry = find(id);
    if (entry == nullptr || page >= entry->pages.size() ||
        (entry->pages[page].state != PageState::Resident &&
         entry->pages[page].state != PageState::Permanent)) {
        ++stats_.use_of_nonresident;
        return;
    }
    entry->pages[page].last_read = current_;
}

void VirtualGeometryPageCache::request(assets::AssetId id,
                                       std::uint32_t page,
                                       std::uint32_t priority) {
    AssetEntry* entry = find(id);
    if (entry == nullptr || page >= entry->pages.size()) {
        ++stats_.requests_invalid;
        return;
    }
    PageEntry& p = entry->pages[page];
    p.last_wanted = current_;
    if (p.requested_in == current_) {
        // Already asked this frame: keep the better priority, count it once.
        for (Request& r : requests_) {
            if (r.asset == id.value && r.page == page) {
                r.priority = std::min(r.priority, priority);
            }
        }
        return;
    }
    p.requested_in = current_;
    ++stats_.requests;
    switch (p.state) {
        case PageState::Resident:
        case PageState::Permanent:
            ++stats_.requests_satisfied;
            return;
        case PageState::Uploading:
            ++stats_.requests_in_flight;
            return;
        case PageState::Absent:
            requests_.push_back(
                {id.value, page, priority, static_cast<std::uint32_t>(requests_.size())});
            return;
    }
}

std::span<const VirtualGeometryPageUpload> VirtualGeometryPageCache::plan_uploads() {
    if (planned_ || !is_valid()) {
        return uploads_;
    }
    planned_ = true;
    std::sort(requests_.begin(), requests_.end(), [](const Request& a, const Request& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.order < b.order;
    });

    const std::uint32_t segment = static_cast<std::uint32_t>(current_ % config_.staging_segments);
    if (staging_last_use_[segment] > retired_) {
        // The frame that last filled this segment may still be copying out of it.
        stats_.deferred_staging_busy += requests_.size();
        return uploads_;
    }
    const std::uint64_t segment_base = std::uint64_t{segment} * config_.upload_budget_bytes;
    std::uint32_t budget_left = config_.upload_budget_bytes;
    bool budget_exhausted = false;

    for (const Request& r : requests_) {
        AssetEntry& entry = assets_.at(r.asset);
        PageEntry& p = entry.pages[r.page];
        if (p.state != PageState::Absent) {
            continue; // nothing in this loop changes an absent request's state but itself
        }
        const assets::VirtualGeometryPage& record = entry.asset->pages[r.page];
        // Head-of-line on bandwidth: once the best remaining request does not fit, nothing behind
        // it goes either. Letting smaller pages slip past would starve a large, important one;
        // stopping means the head gets a full budget next frame, so every request makes progress.
        if (budget_exhausted || record.byte_size > budget_left) {
            budget_exhausted = true;
            ++stats_.deferred_budget;
            continue;
        }
        bool dependencies_ok = true;
        for (std::uint32_t i = 0; i < record.dependency_count; ++i) {
            const PageState s =
                entry.pages[entry.asset->page_dependencies[record.first_dependency + i]].state;
            dependencies_ok = dependencies_ok && s != PageState::Absent;
        }
        if (!dependencies_ok) {
            ++stats_.deferred_dependency;
            continue;
        }

        std::optional<std::uint32_t> slot = take_slot();
        if (!slot) {
            // Rule 2: a victim's last reader must have retired. Among those, least recently used
            // (read or wanted) goes first; ties go to the lowest slot for determinism.
            std::uint64_t victim_asset = 0;
            std::uint32_t victim_page = 0;
            VirtualGeometryFrame victim_recency = std::numeric_limits<VirtualGeometryFrame>::max();
            std::uint32_t victim_slot = std::numeric_limits<std::uint32_t>::max();
            for (auto& [key, candidate_entry] : assets_) {
                for (std::uint32_t page = 0; page < candidate_entry.pages.size(); ++page) {
                    const PageEntry& c = candidate_entry.pages[page];
                    if (c.state != PageState::Resident || c.live_dependents != 0 ||
                        c.last_read > retired_ || c.last_wanted >= current_) {
                        continue;
                    }
                    const VirtualGeometryFrame recency = std::max(c.last_read, c.last_wanted);
                    if (recency < victim_recency ||
                        (recency == victim_recency && c.slot < victim_slot)) {
                        victim_asset = key;
                        victim_page = page;
                        victim_recency = recency;
                        victim_slot = c.slot;
                    }
                }
            }
            if (victim_asset == 0) {
                ++stats_.deferred_no_slot;
                continue;
            }
            drop_to_absent(victim_asset, assets_.at(victim_asset), victim_page);
            ++stats_.evictions;
            slot = take_slot();
        }

        SlotEntry& s = slots_[*slot];
        s.asset = r.asset;
        s.page = r.page;
        ++s.generation;
        p.state = PageState::Uploading;
        p.slot = *slot;
        p.upload_frame = current_;
        adjust_dependents(entry, r.page, +1);

        const VirtualGeometryPageUpload upload{assets::AssetId{r.asset},
                                               r.page,
                                               *slot,
                                               s.generation,
                                               record.byte_size,
                                               segment_base +
                                                   (config_.upload_budget_bytes - budget_left),
                                               current_};
        budget_left -= record.byte_size;
        uploads_.push_back(upload);
        in_flight_.push_back(upload);
        ++stats_.uploads_issued;
        stats_.upload_bytes += record.byte_size;
        stats_.frame_upload_bytes += record.byte_size;
    }
    if (!uploads_.empty()) {
        staging_last_use_[segment] = current_;
    }
    stats_.max_frame_upload_bytes =
        std::max(stats_.max_frame_upload_bytes, stats_.frame_upload_bytes);
    return uploads_;
}

void VirtualGeometryPageCache::abandon_frame(VirtualGeometryFrame frame) {
    for (auto it = in_flight_.begin(); it != in_flight_.end();) {
        if (it->frame != frame) {
            ++it;
            continue;
        }
        drop_to_absent(it->asset.value, *find(it->asset), it->page);
        ++stats_.uploads_abandoned;
        it = in_flight_.erase(it);
    }
    if (is_valid()) {
        VirtualGeometryFrame& last = staging_last_use_[frame % config_.staging_segments];
        if (last == frame) {
            last = 0; // nothing ran, so nothing is still reading the segment
        }
    }
}

bool VirtualGeometryPageCache::is_resident(assets::AssetId id, std::uint32_t page) const noexcept {
    const AssetEntry* entry = find(id);
    return entry != nullptr && page < entry->pages.size() &&
           (entry->pages[page].state == PageState::Resident ||
            entry->pages[page].state == PageState::Permanent);
}

bool VirtualGeometryPageCache::is_uploading(assets::AssetId id, std::uint32_t page) const noexcept {
    const AssetEntry* entry = find(id);
    return entry != nullptr && page < entry->pages.size() &&
           entry->pages[page].state == PageState::Uploading;
}

std::optional<std::uint32_t>
VirtualGeometryPageCache::resident_slot(assets::AssetId id, std::uint32_t page) const noexcept {
    if (!is_resident(id, page)) {
        return std::nullopt;
    }
    return find(id)->pages[page].slot;
}

std::uint32_t VirtualGeometryPageCache::slot_generation(std::uint32_t slot) const noexcept {
    return slot < slots_.size() ? slots_[slot].generation : 0;
}

std::uint32_t VirtualGeometryPageCache::free_slots() const noexcept {
    return static_cast<std::uint32_t>(free_slots_.size());
}

const assets::VirtualGeometryAsset*
VirtualGeometryPageCache::asset(assets::AssetId id) const noexcept {
    const AssetEntry* entry = find(id);
    return entry == nullptr ? nullptr : entry->asset;
}

VirtualGeometrySelection stream_virtual_geometry(VirtualGeometryPageCache& cache,
                                                 assets::AssetId id,
                                                 const assets::VirtualGeometryAsset& asset,
                                                 float pixels_per_metre,
                                                 float max_projected_error_px) {
    VirtualGeometrySelection selection =
        select_virtual_geometry(asset,
                                {.pixels_per_metre = pixels_per_metre,
                                 .max_projected_error_px = max_projected_error_px,
                                 .page_resident = cache.page_residency_bytes(id)});
    if (selection.rejected_invalid_input != 0 || cache.asset(id) != &asset) {
        selection.groups.clear();
        selection.rejected_invalid_input = 1;
        return selection;
    }
    cache.note_fallbacks(selection.refinement_blocked_by_residency);

    // Walk a page and its dependencies (dependencies first), once per call.
    // validate_virtual_geometry has rejected dependency cycles, so the explicit stack terminates.
    std::vector<std::uint8_t> seen(asset.pages.size(), 0);
    const auto visit_pages = [&](std::uint32_t root, auto&& action) {
        struct Frame {
            std::uint32_t page;
            std::uint32_t next;
        };
        if (seen[root] != 0) {
            return;
        }
        seen[root] = 1;
        std::vector<Frame> stack{{root, 0}};
        while (!stack.empty()) {
            Frame& f = stack.back();
            const assets::VirtualGeometryPage& record = asset.pages[f.page];
            if (f.next < record.dependency_count) {
                const std::uint32_t dependency =
                    asset.page_dependencies[record.first_dependency + f.next++];
                if (seen[dependency] == 0) {
                    seen[dependency] = 1;
                    stack.push_back({dependency, 0});
                }
                continue;
            }
            action(f.page);
            stack.pop_back();
        }
    };

    // 2. Everything the drawn cut reads is pinned to this frame.
    for (const std::uint32_t group_index : selection.groups) {
        const assets::VirtualGeometryGroup& group = asset.groups[group_index];
        for (std::uint32_t c = 0; c < group.cluster_count; ++c) {
            visit_pages(asset.clusters[group.first_cluster + c].page,
                        [&](std::uint32_t page) { cache.mark_used(id, page); });
        }
    }

    // 3. The ideal cut's path, breadth-first by depth so shallower refinements carry better
    //    priority. Groups reachable by more than one parent in the DAG are visited once, at the
    //    shallowest depth that reaches them.
    std::fill(seen.begin(), seen.end(), 0);
    std::vector<std::uint8_t> group_seen(asset.groups.size(), 0);
    std::vector<std::uint32_t> level{asset.coarse_group};
    group_seen[asset.coarse_group] = 1;
    for (std::uint32_t depth = 1; !level.empty(); ++depth) {
        std::vector<std::uint32_t> next;
        for (const std::uint32_t group_index : level) {
            const assets::VirtualGeometryGroup& group = asset.groups[group_index];
            if (group.lod_error_m * pixels_per_metre <= max_projected_error_px) {
                continue; // this group is on the ideal cut; nothing below it is wanted
            }
            for (std::uint32_t k = 0; k < group.child_count; ++k) {
                const std::uint32_t child = asset.child_groups[group.first_child + k];
                if (group_seen[child] != 0) {
                    continue;
                }
                group_seen[child] = 1;
                next.push_back(child);
                const assets::VirtualGeometryGroup& child_group = asset.groups[child];
                for (std::uint32_t c = 0; c < child_group.cluster_count; ++c) {
                    visit_pages(asset.clusters[child_group.first_cluster + c].page,
                                [&](std::uint32_t page) { cache.request(id, page, depth); });
                }
            }
        }
        level = std::move(next);
    }
    return selection;
}

} // namespace rime::render
