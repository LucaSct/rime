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
    for (auto& [c, r] : records_) {
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

bool TerrainResidency::check_world(assets::TerrainTileCoord c,
                                   const assets::HeightfieldAsset& asset,
                                   const assets::TerrainTileEdges& edges) {
    const assets::TerrainTileCheck fit = assets::check_tile(world_.grid(), c, asset);
    if (fit != assets::TerrainTileCheck::Ok) {
        refusals_.count(fit);
        return false;
    }
    // Against every RESIDENT edge neighbour: the samples on a shared edge must be the same
    // integers, or the two tiles draw a crack (terrain_world.hpp).
    const std::array<assets::TerrainTileCoord, 4> around = {
        {{c.x + 1, c.z}, {c.x - 1, c.z}, {c.x, c.z + 1}, {c.x, c.z - 1}}};
    for (const assets::TerrainTileCoord n : around) {
        const auto it = records_.find(n);
        if (it != records_.end() && it->second.phase == Phase::Resident &&
            !assets::edges_match(edges, c, it->second.edges, n)) {
            ++refusals_.border_mismatches;
            return false;
        }
    }
    return true;
}

std::optional<TerrainResidentId> TerrainResidency::take_slot(const core::Vec3& eye) {
    if (const auto id = slots_.acquire()) {
        return id;
    }
    // No free slot: evict the FARTHEST resident tile beyond retention (ties broken by coordinate,
    // so the choice is deterministic). A kept tile is never evicted for a wanted one — that is
    // what the hysteresis promises — so with every slot kept the wanted tile waits, counted.
    const auto& grid = world_.grid();
    auto victim = records_.end();
    float victim_d = config_.retention_radius;
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->second.phase != Phase::Resident) {
            continue;
        }
        const float d = grid.distance_xz(it->first, eye);
        if (d > victim_d) {
            victim = it;
            victim_d = d;
        }
    }
    if (victim == records_.end()) {
        return std::nullopt;
    }
    slots_.evict(victim->second.id);
    ++stats_.evictions;
    records_.erase(victim); // the payload stays with the slot until it is reclaimed
    // The evicted slot may already be reclaimable (no unretired frame read it).
    reclaim_slots();
    return slots_.acquire();
}

void TerrainResidency::advance(assets::TerrainTileCoord c, Record& r, const core::Vec3& eye) {
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
        const assets::TerrainTileCheck fit = assets::check_tile(world_.grid(), c, *asset);
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
    if (!check_world(c, *asset, edges)) {
        ++stats_.refused_world;
        forget(r);
        r.phase = Phase::Refused;
        return;
    }
    const std::optional<TerrainResidentId> id = take_slot(eye);
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
    p.coord = c;
    p.pass_tile = tile;
    p.palette = r.palette; // the slot owns the palette now: released at reclaim, after retirement
    p.bytes = pass_.tile_bytes(tile);
    r.palette = kInvalidTerrainPalette;
    // The GPU has its own copy of the samples, and the borders are kept for the neighbour check,
    // so the CPU payload is handed back to the asset server now.
    forget(r);
    r.edges = std::move(edges);
    r.id = *id;
    r.phase = Phase::Resident;
    ++stats_.uploads;
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

    const auto& grid = world_.grid();
    // 1. Forget what is no longer kept and never became resident (a load the camera outran).
    for (auto it = records_.begin(); it != records_.end();) {
        Record& r = it->second;
        if (r.phase != Phase::Resident &&
            grid.distance_xz(it->first, eye) > config_.retention_radius) {
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
        if (records_.contains(c)) {
            continue;
        }
        const assets::TerrainWorldTile* t = world_.find(c);
        const std::filesystem::path rel(t->path);
        Record r;
        r.heightfield = server_.request_heightfield(rel.is_absolute() ? rel : world_dir_ / rel);
        ++stats_.heightfield_requests;
        records_.emplace(c, std::move(r));
    }
    // 3. Advance every non-resident record, NEAREST FIRST, so the closest tiles take the slots.
    std::vector<std::pair<float, assets::TerrainTileCoord>> order;
    for (const auto& [c, r] : records_) {
        if (r.phase == Phase::Loading || r.phase == Phase::Building) {
            order.emplace_back(grid.distance_xz(c, eye), c);
        }
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
    });
    for (const auto& [d, c] : order) {
        const auto it = records_.find(c); // an eviction may have erased OTHER records, never this
        if (it != records_.end()) {
            advance(c, it->second, eye);
        }
    }
    // 4. Count every wanted tile that is not resident, under exactly one reason.
    TerrainMissCounts& m = stats_.missing_this_frame;
    for (const assets::TerrainTileCoord c : wanted) {
        const Record& r = records_.at(c);
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
    for (std::uint32_t i = 0; i < slots_.capacity(); ++i) {
        if (slots_.state(i) == TerrainSlotTable::State::Occupied) {
            add_tile(graph, {i, slots_.generation(i)}, hdr, depth, view_proj, eye, light, sky);
        }
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
    const auto it = records_.find(c);
    return it != records_.end() && it->second.phase == Phase::Resident ? it->second.id
                                                                       : TerrainResidentId{};
}

std::vector<assets::TerrainTileCoord> TerrainResidency::resident_tiles() const {
    std::vector<assets::TerrainTileCoord> out;
    for (const auto& [c, r] : records_) {
        if (r.phase == Phase::Resident) {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace rime::render
