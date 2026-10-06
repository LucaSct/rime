// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Terrain render residency (m19.8a). The header has the design; the notes here are about ORDER —
// which step of a frame may destroy what, and why it is safe at that point.

#include "rime/render/terrain_residency.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "rime/assets/heightfield_asset.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/render/terrain_builder.hpp"
#include "rime/rhi/device.hpp"

namespace rime::render {

// ── TerrainSlotTable ─────────────────────────────────────────────────────────────────────────────

TerrainSlotTable::TerrainSlotTable(std::uint32_t slots) : slots_(slots) {}

std::optional<TerrainResidentId> TerrainSlotTable::acquire() {
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (s.state == State::Free) {
            s.state = State::Occupied;
            s.last_read = 0;
            return TerrainResidentId{i, s.generation};
        }
    }
    return std::nullopt;
}

bool TerrainSlotTable::resolve(TerrainResidentId id) const noexcept {
    return id.slot < slots_.size() && slots_[id.slot].state == State::Occupied &&
           slots_[id.slot].generation == id.generation;
}

bool TerrainSlotTable::mark_read(TerrainResidentId id, TerrainFrame frame) noexcept {
    if (!resolve(id)) {
        return false;
    }
    Slot& s = slots_[id.slot];
    s.last_read = std::max(s.last_read, frame);
    return true;
}

bool TerrainSlotTable::evict(TerrainResidentId id) noexcept {
    if (!resolve(id)) {
        return false;
    }
    Slot& s = slots_[id.slot];
    s.state = State::Retiring;
    // Bumped at EVICTION, not at reuse: from this instant no id — however many copies a caller
    // kept — resolves to this slot, so nothing new can be drawn from it while it waits to retire.
    ++s.generation;
    return true;
}

bool TerrainSlotTable::abandon(TerrainResidentId id) noexcept {
    if (!resolve(id) || slots_[id.slot].last_read != 0) {
        return false;
    }
    Slot& s = slots_[id.slot];
    s.state = State::Free;
    ++s.generation;
    return true;
}

std::uint32_t TerrainSlotTable::reclaim(TerrainFrame retired, std::vector<std::uint32_t>& out) {
    std::uint32_t waiting = 0;
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (s.state != State::Retiring) {
            continue;
        }
        // THE RULE (m18.5's "reuse waits for the last reader to retire"): free only when the
        // confirmed watermark has reached the newest frame that read this slot. A slot no frame
        // ever read has last_read 0 and is free at once.
        if (s.last_read <= retired) {
            s.state = State::Free;
            s.last_read = 0;
            out.push_back(i);
        } else {
            ++waiting;
        }
    }
    return waiting;
}

std::uint32_t TerrainSlotTable::count(State st) const noexcept {
    std::uint32_t n = 0;
    for (const Slot& s : slots_) {
        n += s.state == st ? 1u : 0u;
    }
    return n;
}

// ── TerrainResidency ────────────────────────────────────────────────────────────────────────────

TerrainResidency::TerrainResidency(rhi::Device& device,
                                   TerrainPass& pass,
                                   assets::AssetServer& server,
                                   const assets::TerrainWorld& world,
                                   std::filesystem::path world_dir,
                                   TerrainLayerBuilder* builder,
                                   const TerrainResidencyConfig& config)
    : device_(device), pass_(pass), server_(server), world_(world),
      world_dir_(std::move(world_dir)), builder_(builder), config_(config),
      slots_(std::max<std::uint32_t>(config.slots, 1)), payloads_(slots_.capacity()) {
    config_.slots = slots_.capacity();
    config_.retention_radius = std::max(config_.retention_radius, config_.activation_radius);
    lod_ = world_.level_count() > 1;
    if (lod_) {
        ranges_ = terrain_lod_ranges(world_, config_.lod);
        // The pinned root cover must fit, or the residency cannot promise coverage at all.
        const std::size_t roots = world_.tiles(world_.level_count() - 1).size();
        if (roots > slots_.capacity()) {
            lod_refused_ = true;
            ++stats_.root_cover_refusals;
            RIME_WARN("terrain residency: the world's {} root tiles exceed the {}-slot budget — "
                      "refused, nothing will be drawn",
                      roots,
                      slots_.capacity());
        }
    }
}

TerrainResidency::~TerrainResidency() {
    // Every frame this residency was told about must finish before its tiles are destroyed. An
    // open frame's submission is unknown to us, so only an idle device answers for it.
    if (frame_open_) {
        device_.wait_idle();
    }
    for (const Submitted& s : submitted_) {
        if (!s.retired) {
            device_.wait(s.ticket);
        }
    }
    for (std::uint32_t i = 0; i < slots_.capacity(); ++i) {
        if (slots_.state(i) != TerrainSlotTable::State::Free) {
            pass_.release(payloads_[i].pass_tile);
            if (builder_ != nullptr && payloads_[i].palette != kInvalidTerrainPalette) {
                builder_->release(payloads_[i].palette);
            }
        }
    }
    for (auto& [k, r] : records_) {
        forget(r);
    }
}

void TerrainResidency::retire_submitted() {
    // The page pool's walk (virtual_geometry_page_pool.cpp, begin_frame): in order, stopping at the
    // first submission still running. A later fence that happens to signal first does not move
    // the watermark past an earlier one — the watermark's claim is "every frame up to here".
    while (!submitted_.empty()) {
        const Submitted& front = submitted_.front();
        if (!front.retired && !device_.is_complete(front.ticket)) {
            break;
        }
        retired_ = front.frame;
        ++stats_.frames_retired;
        submitted_.pop_front();
    }
}

void TerrainResidency::reclaim_slots() {
    std::vector<std::uint32_t> freed;
    stats_.reclaim_waits += slots_.reclaim(retired_, freed);
    for (const std::uint32_t slot : freed) {
        // Safe to destroy NOW and only now: the slot table freed this slot because every frame
        // that read it is at or below the confirmed watermark.
        Payload& p = payloads_[slot];
        pass_.release(p.pass_tile);
        if (builder_ != nullptr && p.palette != kInvalidTerrainPalette) {
            builder_->release(p.palette); // the textures' last readers are retired too
        }
        p = Payload{};
        ++stats_.reclaims;
    }
}

void TerrainResidency::forget(Record& r) {
    if (r.heightfield.is_valid()) {
        server_.release(r.heightfield);
        r.heightfield = {};
        ++stats_.heightfield_releases;
    }
    // A palette still on the RECORD was never drawn (a drawn one moved into a slot's payload), so
    // no frame can be sampling its textures: it is released at once.
    if (r.palette != kInvalidTerrainPalette) {
        if (builder_ != nullptr) {
            builder_->release(r.palette);
        }
        r.palette = kInvalidTerrainPalette;
    }
}

bool TerrainResidency::check_world(assets::TerrainTileKey k,
                                   const assets::HeightfieldAsset& asset,
                                   const assets::TerrainTileEdges& edges) {
    const assets::TerrainTileCheck fit = assets::check_tile(world_.grid(), k, asset);
    if (fit != assets::TerrainTileCheck::Ok) {
        refusals_.count(fit);
        return false;
    }
    // Against every RESIDENT edge neighbour of the same level: the samples on a shared edge must
    // be the same integers, or the two tiles draw a crack (terrain_world.hpp).
    const assets::TerrainTileCoord c = k.coord;
    const std::array<assets::TerrainTileCoord, 4> around = {
        {{c.x + 1, c.z}, {c.x - 1, c.z}, {c.x, c.z + 1}, {c.x, c.z - 1}}};
    for (const assets::TerrainTileCoord n : around) {
        const auto it = records_.find({k.level, n});
        if (it != records_.end() && it->second.phase == Phase::Resident &&
            !assets::edges_match(edges, c, it->second.edges, n)) {
            ++refusals_.border_mismatches;
            return false;
        }
    }
    return true;
}

bool TerrainResidency::check_parent(assets::TerrainTileKey k,
                                    const assets::HeightfieldAsset& asset) {
    if (!lod_ || k.level + 1 >= world_.level_count()) {
        return true; // a root, or a world without a chain: no parent to agree with
    }
    const assets::TerrainTileKey p{k.level + 1, assets::terrain_parent_coord(k.coord)};
    if (refused_parents_.contains(p) || verified_pairs_.contains({p, k})) {
        return true;
    }
    const auto it = records_.find(p);
    if (it == records_.end() || it->second.phase != Phase::Resident ||
        !it->second.heightfield.is_valid()) {
        ++stats_.parent_waits; // the parent is wanted first, so this resolves within frames
        return false;
    }
    const assets::HeightfieldAsset* parent = server_.get(it->second.heightfield);
    ++stats_.coincidence_checks;
    if (parent != nullptr && assets::samples_coincide(*parent, p, asset, k)) {
        verified_pairs_.insert({p, k});
        return true;
    }
    // The 8d1 handoff's rule: the PARENT is the one refused. The child's samples are the cooked
    // truth (they are what physics collides with); a parent that disagrees with them would morph
    // the child onto a surface it is not a subsample of.
    ++refusals_.coincidence_mismatches;
    refuse_parent(p);
    return true;
}

void TerrainResidency::refuse_parent(assets::TerrainTileKey parent) {
    refused_parents_.insert(parent);
    ++stats_.refused_parents;
    const auto it = records_.find(parent);
    if (it == records_.end()) {
        return;
    }
    Record& r = it->second;
    if (r.phase == Phase::Resident) {
        slots_.evict(r.id); // fence-safe as always: reclaimed once its last reader retires
        ++stats_.evictions;
        if (r.pinned) {
            ++stats_.pinned_evictions; // counted: a refusal, not pressure, took a root
        }
        r.id = {};
    }
    forget(r);
    r.phase = Phase::Refused;
}

float TerrainResidency::distance(assets::TerrainTileKey k, const core::Vec3& eye) const {
    return lod_ ? static_cast<float>(terrain_lod_distance(world_, k, eye))
                : world_.grid().distance_xz(k.coord, eye);
}

std::optional<TerrainResidentId> TerrainResidency::take_slot(assets::TerrainTileKey k,
                                                             const core::Vec3& eye) {
    // m19.8d2: the reserved root slots. A non-root may take a free slot only while more are free
    // than there are roots still waiting for one, so the root cover always fits.
    const auto may_acquire = [&]() {
        if (!lod_) {
            return true;
        }
        const auto rec = records_.find(k);
        if (rec != records_.end() && rec->second.pinned) {
            return true;
        }
        std::uint32_t waiting_roots = 0;
        for (const auto& [rk, r] : records_) {
            waiting_roots += r.pinned && r.phase != Phase::Resident && r.phase != Phase::Refused;
        }
        return slots_.count(TerrainSlotTable::State::Free) > waiting_roots;
    };
    if (may_acquire()) {
        if (const auto id = slots_.acquire()) {
            return id;
        }
    }
    // Pick a victim (ties broken by key, so the choice is deterministic).
    //  * m19.8a: the FARTHEST resident tile beyond retention. A kept tile is never evicted for a
    //    wanted one — that is what the hysteresis promises — so with every slot kept the wanted
    //    tile waits, counted.
    //  * m19.8d2: the farthest resident tile that is neither pinned nor wanted nor prefetched;
    //    failing that, for a WANTED tile, the farthest that is only prefetched. Nothing evicts a
    //    wanted tile or a root.
    const bool requester_wanted = wanted_.contains(k);
    auto victim = records_.end();
    float victim_d = 0.0f;
    int victim_class = 0; // 2 = unkept, 1 = prefetch-only; higher wins, then distance
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        const Record& r = it->second;
        if (r.phase != Phase::Resident) {
            continue;
        }
        const float d = distance(it->first, eye);
        int cls = 0;
        if (!lod_) {
            cls = d > config_.retention_radius ? 2 : 0;
        } else if (!r.pinned && !wanted_.contains(it->first)) {
            if (!prefetch_.contains(it->first)) {
                cls = 2;
            } else if (requester_wanted) {
                cls = 1;
            }
        }
        if (cls == 0) {
            continue;
        }
        if (cls > victim_class || (cls == victim_class && d > victim_d)) {
            victim = it;
            victim_d = d;
            victim_class = cls;
        }
    }
    if (victim == records_.end()) {
        return std::nullopt;
    }
    slots_.evict(victim->second.id);
    ++stats_.evictions;
    forget(victim->second); // a resident parent's retained heightfield goes back to the server
    records_.erase(victim); // the payload stays with the slot until it is reclaimed
    // The evicted slot may already be reclaimable (no unretired frame read it).
    reclaim_slots();
    return may_acquire() ? slots_.acquire() : std::nullopt;
}

void TerrainResidency::advance(assets::TerrainTileKey k, Record& r, const core::Vec3& eye) {
    if (r.phase == Phase::Loading) {
        const assets::AssetState s = server_.state(r.heightfield);
        if (s == assets::AssetState::Loading) {
            return;
        }
        const assets::HeightfieldAsset* asset = server_.get(r.heightfield);
        if (s != assets::AssetState::Ready || asset == nullptr) {
            ++stats_.refused_loads;
            forget(r);
            r.phase = Phase::Refused;
            return;
        }
        const assets::TerrainTileCheck fit = assets::check_tile(world_.grid(), k, *asset);
        if (fit != assets::TerrainTileCheck::Ok) {
            refusals_.count(fit);
            ++stats_.refused_world;
            forget(r);
            r.phase = Phase::Refused;
            return;
        }
        r.splat = asset->has_splat();
        if (r.splat) {
            if (builder_ == nullptr) {
                ++stats_.refused_palettes;
                forget(r);
                r.phase = Phase::Refused;
                return;
            }
            std::array<assets::AssetId, 4> layers{};
            std::copy(std::begin(asset->layers), std::end(asset->layers), layers.begin());
            r.palette = builder_->request(layers);
        }
        r.phase = Phase::Building;
    }
    if (r.phase != Phase::Building) {
        return;
    }
    if (r.palette != kInvalidTerrainPalette) {
        const TerrainPaletteState ps = builder_->update(r.palette);
        if (ps == TerrainPaletteState::Pending) {
            return;
        }
        if (ps == TerrainPaletteState::Failed) {
            ++stats_.refused_palettes;
            forget(r);
            r.phase = Phase::Refused;
            return;
        }
    }
    const assets::HeightfieldAsset* asset = server_.get(r.heightfield);
    // The border check runs HERE, immediately before the upload, against whatever is resident at
    // this moment — a neighbour that arrived while this tile waited for its palette or a slot is
    // checked too.
    assets::TerrainTileEdges edges = assets::tile_edges(*asset);
    if (!check_world(k, *asset, edges)) {
        ++stats_.refused_world;
        forget(r);
        r.phase = Phase::Refused;
        return;
    }
    // m19.8d2: the child agrees with its parent before it can be drawn under it.
    if (!check_parent(k, *asset)) {
        return;
    }
    const std::optional<TerrainResidentId> id = take_slot(k, eye);
    if (!id) {
        r.waiting_for_slot = true;
        return;
    }
    r.waiting_for_slot = false;
    const TerrainTileId tile =
        r.splat ? pass_.upload(*asset, *builder_->palette(r.palette)) : pass_.upload(*asset);
    if (tile == kInvalidTerrainTile) {
        slots_.abandon(*id); // never read: straight back to Free
        ++stats_.upload_failures;
        forget(r);
        r.phase = Phase::UploadFailed;
        return;
    }
    Payload& p = payloads_[id->slot];
    p.key = k;
    p.pass_tile = tile;
    p.palette = r.palette; // the slot owns the palette now: released at reclaim, after retirement
    p.bytes = pass_.tile_bytes(tile);
    r.palette = kInvalidTerrainPalette;
    // The GPU has its own copy of the samples, and the borders are kept for the neighbour check,
    // so the CPU payload is handed back to the asset server now — except a LOD PARENT's, which
    // its children are compared against as they arrive (check_parent).
    if (!(lod_ && k.level > 0)) {
        forget(r);
    }
    r.edges = std::move(edges);
    r.id = *id;
    r.phase = Phase::Resident;
    ++stats_.uploads;
}

void TerrainResidency::request(assets::TerrainTileKey k) {
    if (records_.contains(k)) {
        return;
    }
    Record r;
    r.pinned = lod_ && k.level + 1 == world_.level_count();
    if (refused_parents_.contains(k)) {
        r.phase = Phase::Refused; // sticky: a refused parent is never asked for again
        records_.emplace(k, std::move(r));
        return;
    }
    const assets::TerrainWorldTile* t = world_.find(k);
    const std::filesystem::path rel(t->path);
    r.heightfield = server_.request_heightfield(rel.is_absolute() ? rel : world_dir_ / rel);
    ++stats_.heightfield_requests;
    records_.emplace(k, std::move(r));
}

void TerrainResidency::begin_frame(const core::Vec3& eye) {
    if (frame_open_) {
        // The previous frame was never ended, so its submission — if any — is unknown. Only an idle
        // device proves it finished; waiting is slow but never unsafe. Counted and warned once.
        if (stats_.unclosed_frames++ == 0) {
            RIME_WARN("terrain residency: begin_frame with frame {} still open — waiting idle",
                      frame_);
        }
        device_.wait_idle();
        submitted_.push_back({frame_, {}, true});
        frame_open_ = false;
    }
    retire_submitted();
    reclaim_slots();
    ++frame_;
    frame_open_ = true;
    ++stats_.frames_begun;
    stats_.missing_this_frame = {};
    if (lod_) {
        begin_frame_lod(eye);
        update_gauges();
        return;
    }

    const auto& grid = world_.grid();
    // 1. Forget what is no longer kept and never became resident (a load the camera outran).
    for (auto it = records_.begin(); it != records_.end();) {
        Record& r = it->second;
        if (r.phase != Phase::Resident &&
            grid.distance_xz(it->first.coord, eye) > config_.retention_radius) {
            if (r.phase == Phase::Loading || r.phase == Phase::Building) {
                ++stats_.cancelled_loads;
            }
            forget(r);
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
    // 2. Request every wanted tile not already known.
    const std::vector<assets::TerrainTileCoord> wanted =
        world_.tiles_within(eye, config_.activation_radius);
    for (const assets::TerrainTileCoord c : wanted) {
        request({0, c});
    }
    // 3. Advance every non-resident record, NEAREST FIRST, so the closest tiles take the slots.
    std::vector<std::pair<float, assets::TerrainTileKey>> order;
    for (const auto& [k, r] : records_) {
        if (r.phase == Phase::Loading || r.phase == Phase::Building) {
            order.emplace_back(grid.distance_xz(k.coord, eye), k);
        }
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
    });
    for (const auto& [d, k] : order) {
        const auto it = records_.find(k); // an eviction may have erased OTHER records, never this
        if (it != records_.end()) {
            advance(k, it->second, eye);
        }
    }
    // 4. Count every wanted tile that is not resident, under exactly one reason.
    TerrainMissCounts& m = stats_.missing_this_frame;
    for (const assets::TerrainTileCoord c : wanted) {
        const Record& r = records_.at({0, c});
        switch (r.phase) {
            case Phase::Resident:
                break;
            case Phase::Loading:
                ++m.not_loaded;
                break;
            case Phase::Building:
                ++(r.waiting_for_slot ? m.no_free_slot : m.not_loaded);
                break;
            case Phase::Refused:
                ++m.refused;
                break;
            case Phase::UploadFailed:
                ++m.upload_failed;
                break;
        }
    }
    stats_.missing.not_loaded += m.not_loaded;
    stats_.missing.no_free_slot += m.no_free_slot;
    stats_.missing.upload_failed += m.upload_failed;
    stats_.missing.refused += m.refused;
    update_gauges();
}

bool TerrainResidency::usable(assets::TerrainTileKey k) const {
    const auto it = records_.find(k);
    return it != records_.end() && it->second.phase == Phase::Resident;
}

void TerrainResidency::begin_frame_lod(const core::Vec3& eye) {
    selection_eye_ = eye;
    if (lod_refused_) {
        selection_ = {};
        ideal_ = {};
        return;
    }
    const std::uint32_t top = world_.level_count() - 1;

    // 1. What the camera wants: the IDEAL selection (every tile assumed usable), every ancestor
    //    of its leaves, and the pinned roots; then the prefetch ring one level finer.
    ideal_ = select_terrain_lod(world_, ranges_, eye, [&](assets::TerrainTileKey k) {
        return world_.find(k) != nullptr && !refused_parents_.contains(k);
    });
    wanted_.clear();
    prefetch_.clear();
    for (const assets::TerrainWorldTile& t : world_.tiles(top)) {
        wanted_.insert(t.key());
    }
    for (const TerrainLodLeaf& leaf : ideal_.leaves) {
        for (assets::TerrainTileKey k = leaf.key; k.level <= top;
             k = {k.level + 1, assets::terrain_parent_coord(k.coord)}) {
            if (!wanted_.insert(k).second && k.level > leaf.key.level) {
                break; // the rest of this chain is already in
            }
        }
    }
    for (const TerrainLodLeaf& leaf : ideal_.leaves) {
        const assets::TerrainTileKey n = leaf.key;
        if (n.level == 0) {
            continue;
        }
        const TerrainLodLevel& child_level = ranges_.levels[n.level - 1];
        if (terrain_lod_distance(world_, n, eye) <=
            static_cast<double>(child_level.range) + static_cast<double>(child_level.diagonal)) {
            const std::uint32_t l = n.level - 1;
            const std::int32_t x = n.coord.x * 2;
            const std::int32_t z = n.coord.z * 2;
            for (const assets::TerrainTileKey c : {assets::TerrainTileKey{l, {x, z}},
                                                   assets::TerrainTileKey{l, {x + 1, z}},
                                                   assets::TerrainTileKey{l, {x, z + 1}},
                                                   assets::TerrainTileKey{l, {x + 1, z + 1}}}) {
                if (!wanted_.contains(c)) {
                    prefetch_.insert(c);
                }
            }
        }
    }

    // 2. Forget what is neither wanted nor prefetched and never became resident.
    for (auto it = records_.begin(); it != records_.end();) {
        Record& r = it->second;
        if (r.phase != Phase::Resident && !wanted_.contains(it->first) &&
            !prefetch_.contains(it->first)) {
            if (r.phase == Phase::Loading || r.phase == Phase::Building) {
                ++stats_.cancelled_loads;
            }
            forget(r);
            it = records_.erase(it);
        } else {
            ++it;
        }
    }

    // 3. Request. Pinned roots, then wanted, then prefetch.
    for (const assets::TerrainTileKey k : wanted_) {
        request(k);
    }
    for (const assets::TerrainTileKey k : prefetch_) {
        if (!records_.contains(k)) {
            ++stats_.prefetch_requests;
        }
        request(k);
    }

    // 4. Advance in priority order: roots first, then wanted, then prefetch; within each, nearest
    //    first, ties coarser first (an ancestor is never farther than its descendant, so a chain
    //    loads top-down), then by key.
    struct Pending {
        int cls;
        float d;
        assets::TerrainTileKey k;
    };

    std::vector<Pending> order;
    for (const auto& [k, r] : records_) {
        if (r.phase == Phase::Loading || r.phase == Phase::Building) {
            const int cls = r.pinned ? 0 : wanted_.contains(k) ? 1 : 2;
            order.push_back({cls, distance(k, eye), k});
        }
    }
    std::sort(order.begin(), order.end(), [](const Pending& a, const Pending& b) {
        if (a.cls != b.cls) {
            return a.cls < b.cls;
        }
        if (a.d != b.d) {
            return a.d < b.d;
        }
        if (a.k.level != b.k.level) {
            return a.k.level > b.k.level;
        }
        return a.k < b.k;
    });
    for (const Pending& p : order) {
        const auto it = records_.find(p.k);
        if (it != records_.end()) {
            advance(p.k, it->second, eye);
        }
    }

    // 5. What is drawn: the selection over what is resident and trusted.
    selection_ = select_terrain_lod(
        world_, ranges_, eye, [&](assets::TerrainTileKey k) { return usable(k); });
    stats_.fallback_draws += selection_.fallback_leaves;
    stats_.balance_collapses += selection_.balance_collapses;
    stats_.uncovered_draws += selection_.uncovered;

    // 6. Every wanted tile that is not resident, under exactly one reason (as m19.8a counts).
    TerrainMissCounts& m = stats_.missing_this_frame;
    for (const assets::TerrainTileKey k : wanted_) {
        const auto it = records_.find(k);
        if (it == records_.end()) {
            continue;
        }
        const Record& r = it->second;
        switch (r.phase) {
            case Phase::Resident:
                break;
            case Phase::Loading:
                ++m.not_loaded;
                break;
            case Phase::Building:
                ++(r.waiting_for_slot ? m.no_free_slot : m.not_loaded);
                break;
            case Phase::Refused:
                ++m.refused;
                break;
            case Phase::UploadFailed:
                ++m.upload_failed;
                break;
        }
    }
    stats_.missing.not_loaded += m.not_loaded;
    stats_.missing.no_free_slot += m.no_free_slot;
    stats_.missing.upload_failed += m.upload_failed;
    stats_.missing.refused += m.refused;
}

void TerrainResidency::update_gauges() {
    stats_.resident_slots = slots_.count(TerrainSlotTable::State::Occupied);
    stats_.retiring_slots = slots_.count(TerrainSlotTable::State::Retiring);
    stats_.peak_occupied_slots =
        std::max(stats_.peak_occupied_slots, stats_.resident_slots + stats_.retiring_slots);
    std::uint64_t bytes = 0;
    for (std::uint32_t i = 0; i < slots_.capacity(); ++i) {
        if (slots_.state(i) != TerrainSlotTable::State::Free) {
            bytes += payloads_[i].bytes;
        }
    }
    stats_.resident_bytes = bytes;
    stats_.peak_resident_bytes = std::max(stats_.peak_resident_bytes, bytes);
    stats_.frames_in_flight = static_cast<std::uint32_t>(submitted_.size());
    std::uint32_t roots = 0;
    for (const auto& [k, r] : records_) {
        roots += r.pinned && r.phase == Phase::Resident ? 1u : 0u;
    }
    stats_.pinned_roots = roots;
}

void TerrainResidency::add(RenderGraph& graph,
                           RGTexture hdr,
                           RGTexture depth,
                           const core::Mat4& view_proj,
                           const core::Vec3& eye,
                           const TerrainLight& light,
                           const SkyLightBinding& sky) {
    if (!frame_open_) {
        ++stats_.draws_outside_frame;
        return;
    }
    if (lod_) {
        for (const TerrainLodLeaf& leaf : selection_.leaves) {
            draw_leaf(graph, leaf, hdr, depth, view_proj, eye, light, sky);
        }
        return;
    }
    for (std::uint32_t i = 0; i < slots_.capacity(); ++i) {
        if (slots_.state(i) == TerrainSlotTable::State::Occupied) {
            add_tile(graph, {i, slots_.generation(i)}, hdr, depth, view_proj, eye, light, sky);
        }
    }
}

void TerrainResidency::draw_leaf(RenderGraph& graph,
                                 const TerrainLodLeaf& leaf,
                                 RGTexture hdr,
                                 RGTexture depth,
                                 const core::Mat4& view_proj,
                                 const core::Vec3& eye,
                                 const TerrainLight& light,
                                 const SkyLightBinding& sky) {
    const TerrainResidentId id = resident(leaf.key);
    if (!slots_.mark_read(id, frame_)) {
        ++stats_.stale_draws; // cannot happen: the selection only names resident tiles
        return;
    }
    const assets::TerrainWorldGrid& g = world_.grid();
    const auto n = static_cast<std::int32_t>(g.samples - 1);
    const TerrainLodLevel& lv = ranges_.levels[leaf.key.level];
    TerrainLodDraw lod{};
    lod.enabled = true;
    lod.grid_origin = g.origin;
    lod.base_x = leaf.key.coord.x * n;
    lod.base_z = leaf.key.coord.z * n;
    lod.level = leaf.key.level;
    lod.coarser_edges = leaf.coarser_edges;
    lod.camera = selection_eye_; // the camera the selection — and so the edge guarantees — used
    lod.morph_start = lv.morph_start;
    lod.morph_end = lv.morph_end;
    pass_.add(graph, hdr, depth, payloads_[id.slot].pass_tile, view_proj, eye, light, sky, lod);
    ++stats_.draws;
    ++stats_.lod_draws;
    if (leaf.key.level > 0) {
        ++stats_.fallback_appearance_draws; // a parent: the flat placeholder material (8d3)
    }
}

bool TerrainResidency::add_tile(RenderGraph& graph,
                                TerrainResidentId id,
                                RGTexture hdr,
                                RGTexture depth,
                                const core::Mat4& view_proj,
                                const core::Vec3& eye,
                                const TerrainLight& light,
                                const SkyLightBinding& sky) {
    if (!frame_open_) {
        ++stats_.draws_outside_frame;
        return false;
    }
    // The generation check. mark_read refuses a stale id, and recording the read is what makes
    // this frame a reader the slot must wait for — so the two are one call, not two.
    if (!slots_.mark_read(id, frame_)) {
        ++stats_.stale_draws;
        return false;
    }
    pass_.add(graph, hdr, depth, payloads_[id.slot].pass_tile, view_proj, eye, light, sky);
    ++stats_.draws;
    return true;
}

void TerrainResidency::end_frame(rhi::SubmitTicket ticket) {
    if (!frame_open_) {
        return;
    }
    frame_open_ = false;
    // An invalid ticket means the submission never happened, so nothing read anything: retired.
    submitted_.push_back({frame_, ticket, !ticket.is_valid()});
    stats_.frames_in_flight = static_cast<std::uint32_t>(submitted_.size());
}

void TerrainResidency::end_frame_blocking() {
    if (!frame_open_) {
        return;
    }
    frame_open_ = false;
    submitted_.push_back({frame_, {}, true});
}

TerrainResidentId TerrainResidency::resident(assets::TerrainTileCoord c) const {
    return resident(assets::TerrainTileKey{0, c});
}

TerrainResidentId TerrainResidency::resident(assets::TerrainTileKey k) const {
    const auto it = records_.find(k);
    return it != records_.end() && it->second.phase == Phase::Resident ? it->second.id
                                                                       : TerrainResidentId{};
}

std::vector<assets::TerrainTileCoord> TerrainResidency::resident_tiles() const {
    std::vector<assets::TerrainTileCoord> out;
    for (const auto& [k, r] : records_) {
        if (r.phase == Phase::Resident && k.level == 0) {
            out.push_back(k.coord);
        }
    }
    return out;
}

std::vector<assets::TerrainTileKey> TerrainResidency::resident_keys() const {
    std::vector<assets::TerrainTileKey> out;
    for (const auto& [k, r] : records_) {
        if (r.phase == Phase::Resident) {
            out.push_back(k);
        }
    }
    return out;
}

} // namespace rime::render
