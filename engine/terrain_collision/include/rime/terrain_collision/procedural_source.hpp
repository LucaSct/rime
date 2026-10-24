// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/assets/procedural_terrain.hpp"
#include "rime/terrain_collision/terrain_collision.hpp"

// A TileSource that computes its tiles instead of loading them (assets/procedural_terrain.hpp).
//
// THE CONTRACT IT KEEPS. Every `request` takes one ownership and is balanced by one `release`; the
// payload is owned by this source until that release, and TerrainCollision already copies the
// samples into physics at install and gives the handle straight back. So at steady state the source
// holds almost nothing — a handful of payloads between a request and the barrier's install.
//
// A FIXED POOL, ON PURPOSE. Handles are slots in a pool sized once at construction
// (`max_slots`, default 256). That is a hard cap on live payloads, and a hard cap on metadata: the
// slot table never grows, so a million request/release cycles cost exactly what one does. A request
// that finds the pool full is REFUSED with an invalid handle and counted — TerrainCollision reads
// an invalid handle as a failed load, so the tile goes `Failed` and a required tile fails the
// commit loudly. It is never turned into "no terrain here".
//
// WHAT `current_revision` ANSWERS. The configured version for every tile coordinate inside the
// finite supported world, whatever the pool is doing: running out of slots is a refusal of a
// REQUEST, not an absence of ground. It is false only beyond +/-2^24 m
// (assets::kProceduralMaxWorldMetres), where the world genuinely ends — and a request for such a
// key, or for any key against an unsupported version, is counted in `refused_unsupported`.
//
// Handles are generation-checked: releasing a slot bumps its generation, so a handle kept past its
// release reads `Stale` and resolves to nothing, even after the slot has a new tenant.
//
// Threading: main thread only (it is called from the barrier). No files, no threads, no globals.
namespace rime::terrain_collision {

class ProceduralTileSource final : public TileSource {
public:
    static constexpr std::uint32_t kDefaultMaxSlots = 256;
    static constexpr std::uint32_t kMaxSlotsCeiling = 1u << 16;

    struct Options {
        assets::ProceduralTerrainConfig config{};
        std::uint32_t max_slots = kDefaultMaxSlots; // clamped to [0, kMaxSlotsCeiling]
        // Serve the same distant ranges the renderer draws (assets::apply_procedural_backdrop), so
        // ground beyond 150 m is where it LOOKS to be. Off by default: the pinned golden tiles and
        // the determinism proofs describe the plain generator.
        bool backdrop = false;
    };

    struct Stats {
        std::uint32_t live_payloads = 0;        // gauge
        std::uint32_t peak_payloads = 0;        // high-water mark of the above
        std::uint64_t payload_sample_bytes = 0; // gauge: sample bytes of live payloads
        std::uint64_t peak_payload_sample_bytes = 0;
        std::uint64_t requests = 0;         // every TileSource::request call
        std::uint64_t generated = 0;        // requests that produced a payload
        std::uint64_t releases = 0;         // releases that gave a payload back
        std::uint64_t stale_releases = 0;   // releases of a handle that was not live
        std::uint64_t refused_requests = 0; // = refused_pool_full + refused_unsupported
        std::uint64_t refused_pool_full = 0;
        std::uint64_t refused_unsupported = 0; // bad version / config, key revision, coordinate
    };

    explicit ProceduralTileSource(const Options& options);

    [[nodiscard]] bool
    current_revision(std::int32_t x, std::int32_t z, std::uint32_t& revision) const override;
    [[nodiscard]] assets::HeightfieldAssetHandle request(const TileKey& key) override;
    [[nodiscard]] assets::AssetState state(assets::HeightfieldAssetHandle handle) const override;
    [[nodiscard]] TilePayload resolve(assets::HeightfieldAssetHandle handle) const override;
    bool release(assets::HeightfieldAssetHandle handle) override;

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] std::uint32_t max_slots() const noexcept {
        return static_cast<std::uint32_t>(slots_.size());
    }

    [[nodiscard]] const assets::ProceduralTerrainConfig& config() const noexcept { return config_; }

private:
    struct Slot {
        std::uint32_t generation = 0;
        bool occupied = false;
        TileKey key{};
        assets::HeightfieldAsset asset{};
    };

    [[nodiscard]] const Slot* live(assets::HeightfieldAssetHandle handle) const noexcept;

    assets::ProceduralTerrainConfig config_;
    bool config_valid_ = false;
    bool backdrop_ = false;
    std::vector<Slot> slots_;         // sized once; never grows or shrinks
    std::vector<std::uint32_t> free_; // stack of free slot indices, lowest on top at start
    Stats stats_{};
};

} // namespace rime::terrain_collision
