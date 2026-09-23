// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/virtual_geometry.hpp"

// Render-owned page state for M18. This is the CPU seam that a future upload scheduler will use;
// it deliberately does not know about Vulkan buffers, addresses, or command submission.
namespace rime::render {

class VirtualGeometryResidency {
public:
    VirtualGeometryResidency() = default;

    VirtualGeometryResidency(const VirtualGeometryResidency&) = delete;
    VirtualGeometryResidency& operator=(const VirtualGeometryResidency&) = delete;

    // The asset must outlive this cache registration. Asset bytes remain owned by assets; only the
    // frame-local resident/in-flight state is render-owned. Re-registering the same id is
    // idempotent for the same object and rejects a conflicting object.
    [[nodiscard]] bool register_asset(assets::AssetId id,
                                      const assets::VirtualGeometryAsset& asset);

    // Start a transient page request. A request is accepted only once all of its declared page
    // dependencies are resident; in-flight dependencies are intentionally not sufficient. A
    // permanent or already in-flight page returns true without creating duplicate work.
    [[nodiscard]] bool request_page(assets::AssetId id, std::uint32_t page);

    // Complete an accepted request. A failed upload clears in-flight state and leaves the page
    // unavailable so a later frame may retry it.
    [[nodiscard]] bool complete_page(assets::AssetId id, std::uint32_t page, bool succeeded = true);

    // Eviction is only for resident transient pages. Permanent and in-flight pages are never
    // evicted; invalid or already unavailable pages return false.
    [[nodiscard]] bool evict_page(assets::AssetId id, std::uint32_t page);

    [[nodiscard]] bool is_resident(assets::AssetId id, std::uint32_t page) const noexcept;
    [[nodiscard]] bool is_in_flight(assets::AssetId id, std::uint32_t page) const noexcept;

    // Directly consumable by render::select_virtual_geometry. The returned span remains valid
    // until this asset is re-registered or the residency object is destroyed.
    [[nodiscard]] std::span<const std::uint8_t>
    page_residency_bytes(assets::AssetId id) const noexcept;

private:
    struct State {
        const assets::VirtualGeometryAsset* asset = nullptr;
        std::vector<std::uint8_t> resident;
        std::vector<std::uint8_t> in_flight;
    };

    [[nodiscard]] State* find(assets::AssetId id) noexcept;
    [[nodiscard]] const State* find(assets::AssetId id) const noexcept;

    std::unordered_map<std::uint64_t, State> assets_;
};

} // namespace rime::render
