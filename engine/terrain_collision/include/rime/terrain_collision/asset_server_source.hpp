// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/terrain_collision/terrain_collision.hpp"

// The production TileSource: terrain tiles as m19.8b streamed heightfields (ADR-0067).
//
// It owns the one thing the AssetServer does not have — the MANIFEST, the table from grid
// coordinate to (revision, cooked file) — and forwards everything else. One `request` is one
// `AssetServer::request_heightfield` and one `release` is one `AssetServer::release`, so the
// ownership count the server keeps is exactly the count of tile records holding the tile.
//
// WHAT THE REVISION CHECK CAN AND CANNOT SEE HERE. A cooked heightfield (payload v2) carries no
// revision of its own, so this source cannot read one out of the bytes: `resolve` reports the
// revision the manifest named WHEN THE LOAD WAS REQUESTED. That catches a request made against a
// manifest that has since moved on (refused at `request`), and it keeps a load that was in flight
// across a manifest edit labelled as what it is. It cannot catch a file on disk that is not the
// revision the manifest says it is. That needs a revision stamp in the cooked format, which is
// recorded as a follow-up in ADR-0068 rather than faked here.
//
// Threading: main thread only, like AssetServer::pump() and the getters it forwards to.
namespace rime::terrain_collision {

struct TileManifestEntry {
    std::int32_t x = 0;
    std::int32_t z = 0;
    std::uint32_t revision = 0;
    std::filesystem::path path;
};

class AssetServerTileSource final : public TileSource {
public:
    explicit AssetServerTileSource(assets::AssetServer& server) : server_(server) {}

    // Add a tile to the manifest, or replace the entry at its coordinate (a new revision).
    void set_tile(const TileManifestEntry& entry);
    bool remove_tile(std::int32_t x, std::int32_t z);

    [[nodiscard]] bool
    current_revision(std::int32_t x, std::int32_t z, std::uint32_t& revision) const override;
    [[nodiscard]] assets::HeightfieldAssetHandle request(const TileKey& key) override;
    [[nodiscard]] assets::AssetState state(assets::HeightfieldAssetHandle handle) const override;
    [[nodiscard]] TilePayload resolve(assets::HeightfieldAssetHandle handle) const override;
    bool release(assets::HeightfieldAssetHandle handle) override;

    // Handles requested through this source and not yet released.
    [[nodiscard]] std::size_t outstanding() const noexcept;

private:
    // One live handle and the revision it was requested as. `owners` counts coalesced requests:
    // the server returns the same handle for the same path, and each still needs its own release.
    struct Outstanding {
        assets::HeightfieldAssetHandle handle{};
        std::uint32_t revision = 0;
        std::uint32_t owners = 0;
    };

    [[nodiscard]] const TileManifestEntry* find(std::int32_t x, std::int32_t z) const noexcept;

    assets::AssetServer& server_;
    std::vector<TileManifestEntry> manifest_; // sorted by (x, z)
    std::vector<Outstanding> outstanding_;
};

} // namespace rime::terrain_collision
