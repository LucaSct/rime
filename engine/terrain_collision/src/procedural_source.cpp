// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/terrain_collision/procedural_source.hpp"

#include <algorithm>
#include <utility>

namespace rime::terrain_collision {

ProceduralTileSource::ProceduralTileSource(const Options& options)
    : config_(options.config), config_valid_(assets::procedural_config_valid(options.config)),
      backdrop_(options.backdrop) {
    const std::uint32_t count = std::min(options.max_slots, kMaxSlotsCeiling);
    slots_.resize(count);
    free_.reserve(count);
    // Pushed highest first so the stack pops slot 0 first: handle values are a function of the
    // request/release history alone.
    for (std::uint32_t i = count; i > 0; --i) {
        free_.push_back(i - 1);
    }
}

const ProceduralTileSource::Slot*
ProceduralTileSource::live(assets::HeightfieldAssetHandle handle) const noexcept {
    if (!handle.is_valid() || handle.index >= slots_.size()) {
        return nullptr;
    }
    const Slot& slot = slots_[handle.index];
    return (slot.occupied && slot.generation == handle.generation) ? &slot : nullptr;
}

bool ProceduralTileSource::current_revision(std::int32_t x,
                                            std::int32_t z,
                                            std::uint32_t& revision) const {
    // Deliberately independent of the pool and of config validity: an exhausted or misconfigured
    // source must fail its REQUESTS visibly, not report that the ground is not there.
    if (!assets::procedural_tile_supported(x, z, 0)) {
        return false;
    }
    revision = config_.version;
    return true;
}

assets::HeightfieldAssetHandle ProceduralTileSource::request(const TileKey& key) {
    ++stats_.requests;
    if (!config_valid_ || key.revision != config_.version ||
        !assets::procedural_tile_supported(key.x, key.z, 0)) {
        ++stats_.refused_requests;
        ++stats_.refused_unsupported;
        return {};
    }
    if (free_.empty()) {
        ++stats_.refused_requests;
        ++stats_.refused_pool_full;
        return {};
    }
    std::optional<assets::HeightfieldAsset> tile =
        assets::generate_procedural_tile(config_, key.x, key.z, 0);
    if (!tile) { // unreachable after the checks above; counted rather than assumed
        ++stats_.refused_requests;
        ++stats_.refused_unsupported;
        return {};
    }
    if (backdrop_) {
        assets::apply_procedural_backdrop(config_, *tile);
    }
    const std::uint32_t index = free_.back();
    free_.pop_back();
    Slot& slot = slots_[index];
    slot.occupied = true;
    slot.key = key;
    slot.asset = std::move(*tile);

    ++stats_.generated;
    ++stats_.live_payloads;
    stats_.payload_sample_bytes += slot.asset.samples.size() * sizeof(std::uint16_t);
    stats_.peak_payloads = std::max(stats_.peak_payloads, stats_.live_payloads);
    stats_.peak_payload_sample_bytes =
        std::max(stats_.peak_payload_sample_bytes, stats_.payload_sample_bytes);

    assets::HeightfieldAssetHandle handle;
    handle.index = index;
    handle.generation = slot.generation;
    return handle;
}

assets::AssetState ProceduralTileSource::state(assets::HeightfieldAssetHandle handle) const {
    if (!handle.is_valid() || handle.index >= slots_.size()) {
        return assets::AssetState::Failed; // same answer the AssetServer gives an invalid handle
    }
    // A free slot and an old generation both mean the ownership is gone.
    return live(handle) != nullptr ? assets::AssetState::Ready : assets::AssetState::Stale;
}

TilePayload ProceduralTileSource::resolve(assets::HeightfieldAssetHandle handle) const {
    const Slot* slot = live(handle);
    if (slot == nullptr) {
        return {};
    }
    return {&slot->asset, slot->key.revision};
}

bool ProceduralTileSource::release(assets::HeightfieldAssetHandle handle) {
    if (live(handle) == nullptr) {
        ++stats_.stale_releases;
        return false; // never ours, or already given back: do not free a new tenant's payload
    }
    Slot& slot = slots_[handle.index];
    stats_.payload_sample_bytes -= slot.asset.samples.size() * sizeof(std::uint16_t);
    --stats_.live_payloads;
    ++stats_.releases;
    // Move-assigning an empty asset destroys the old sample buffer: the storage is freed now, not
    // kept as capacity for the next tenant.
    slot.asset = assets::HeightfieldAsset{};
    slot.occupied = false;
    ++slot.generation; // wraps after 2^32 reuses of one slot; a stale handle that old is not kept
    free_.push_back(handle.index);
    return true;
}

} // namespace rime::terrain_collision
