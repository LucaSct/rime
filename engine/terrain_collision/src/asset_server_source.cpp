// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/terrain_collision/asset_server_source.hpp"

#include <algorithm>
#include <utility>

namespace rime::terrain_collision {

namespace {

[[nodiscard]] bool before(const TileManifestEntry& e, std::pair<std::int32_t, std::int32_t> at) {
    return std::pair{e.x, e.z} < at;
}

} // namespace

void AssetServerTileSource::set_tile(const TileManifestEntry& entry) {
    const auto it =
        std::lower_bound(manifest_.begin(), manifest_.end(), std::pair{entry.x, entry.z}, before);
    if (it != manifest_.end() && it->x == entry.x && it->z == entry.z) {
        *it = entry;
    } else {
        manifest_.insert(it, entry);
    }
}

bool AssetServerTileSource::remove_tile(std::int32_t x, std::int32_t z) {
    const auto it = std::lower_bound(manifest_.begin(), manifest_.end(), std::pair{x, z}, before);
    if (it == manifest_.end() || it->x != x || it->z != z) {
        return false;
    }
    manifest_.erase(it);
    return true;
}

const TileManifestEntry* AssetServerTileSource::find(std::int32_t x,
                                                     std::int32_t z) const noexcept {
    const auto it = std::lower_bound(manifest_.begin(), manifest_.end(), std::pair{x, z}, before);
    return (it != manifest_.end() && it->x == x && it->z == z) ? &*it : nullptr;
}

bool AssetServerTileSource::current_revision(std::int32_t x,
                                             std::int32_t z,
                                             std::uint32_t& revision) const {
    const TileManifestEntry* entry = find(x, z);
    if (entry == nullptr) {
        return false;
    }
    revision = entry->revision;
    return true;
}

assets::HeightfieldAssetHandle AssetServerTileSource::request(const TileKey& key) {
    const TileManifestEntry* entry = find(key.x, key.z);
    // A key the manifest no longer names — the tile is gone, or it moved to another revision since
    // the plan was made. An invalid handle reads as Failed to the caller, which is the truth: this
    // revision cannot be loaded.
    if (entry == nullptr || entry->revision != key.revision) {
        return {};
    }
    const assets::HeightfieldAssetHandle handle = server_.request_heightfield(entry->path);
    if (!handle.is_valid()) {
        return handle;
    }
    const auto it = std::find_if(outstanding_.begin(),
                                 outstanding_.end(),
                                 [&](const Outstanding& o) { return o.handle == handle; });
    if (it != outstanding_.end()) {
        ++it->owners;
    } else {
        outstanding_.push_back({handle, key.revision, 1});
    }
    return handle;
}

assets::AssetState AssetServerTileSource::state(assets::HeightfieldAssetHandle handle) const {
    return server_.state(handle);
}

TilePayload AssetServerTileSource::resolve(assets::HeightfieldAssetHandle handle) const {
    const auto it = std::find_if(outstanding_.begin(),
                                 outstanding_.end(),
                                 [&](const Outstanding& o) { return o.handle == handle; });
    if (it == outstanding_.end()) {
        return {};
    }
    return {server_.get(handle), it->revision};
}

bool AssetServerTileSource::release(assets::HeightfieldAssetHandle handle) {
    const auto it = std::find_if(outstanding_.begin(),
                                 outstanding_.end(),
                                 [&](const Outstanding& o) { return o.handle == handle; });
    if (it == outstanding_.end()) {
        return false; // never ours, or already given back: do not spend someone else's ownership
    }
    if (--it->owners == 0) {
        outstanding_.erase(it);
    }
    return server_.release(handle);
}

std::size_t AssetServerTileSource::outstanding() const noexcept {
    std::size_t total = 0;
    for (const Outstanding& o : outstanding_) {
        total += o.owners;
    }
    return total;
}

} // namespace rime::terrain_collision
