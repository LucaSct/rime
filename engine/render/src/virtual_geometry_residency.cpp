// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/render/virtual_geometry_residency.hpp"

namespace rime::render {

VirtualGeometryResidency::State* VirtualGeometryResidency::find(assets::AssetId id) noexcept {
    const auto it = assets_.find(id.value);
    return it == assets_.end() ? nullptr : &it->second;
}

const VirtualGeometryResidency::State*
VirtualGeometryResidency::find(assets::AssetId id) const noexcept {
    const auto it = assets_.find(id.value);
    return it == assets_.end() ? nullptr : &it->second;
}

bool VirtualGeometryResidency::register_asset(assets::AssetId id,
                                              const assets::VirtualGeometryAsset& asset) {
    if (!id.is_valid() ||
        assets::validate_virtual_geometry(asset) != assets::VirtualGeometryError::None) {
        return false;
    }
    if (const State* existing = find(id)) {
        return existing->asset == &asset;
    }

    State state;
    state.asset = &asset;
    state.resident.assign(asset.pages.size(), 0);
    state.in_flight.assign(asset.pages.size(), 0);
    for (std::uint32_t page = 0; page < asset.pages.size(); ++page) {
        if (asset.pages[page].permanently_resident) {
            state.resident[page] = 1;
        }
    }
    assets_.emplace(id.value, std::move(state));
    return true;
}

bool VirtualGeometryResidency::request_page(assets::AssetId id, std::uint32_t page) {
    State* state = find(id);
    if (state == nullptr || page >= state->resident.size()) {
        return false;
    }
    if (state->resident[page] != 0 || state->in_flight[page] != 0) {
        return true;
    }
    const assets::VirtualGeometryPage& record = state->asset->pages[page];
    for (std::uint32_t i = 0; i < record.dependency_count; ++i) {
        const std::uint32_t dependency =
            state->asset->page_dependencies[record.first_dependency + i];
        if (state->resident[dependency] == 0) {
            return false;
        }
    }
    state->in_flight[page] = 1;
    return true;
}

bool VirtualGeometryResidency::complete_page(assets::AssetId id,
                                             std::uint32_t page,
                                             bool succeeded) {
    State* state = find(id);
    if (state == nullptr || page >= state->resident.size() || state->in_flight[page] == 0) {
        return false;
    }
    state->in_flight[page] = 0;
    if (succeeded) {
        state->resident[page] = 1;
    }
    return succeeded;
}

bool VirtualGeometryResidency::evict_page(assets::AssetId id, std::uint32_t page) {
    State* state = find(id);
    if (state == nullptr || page >= state->resident.size() || state->resident[page] == 0 ||
        state->in_flight[page] != 0 || state->asset->pages[page].permanently_resident) {
        return false;
    }
    state->resident[page] = 0;
    return true;
}

bool VirtualGeometryResidency::is_resident(assets::AssetId id, std::uint32_t page) const noexcept {
    const State* state = find(id);
    return state != nullptr && page < state->resident.size() && state->resident[page] != 0;
}

bool VirtualGeometryResidency::is_in_flight(assets::AssetId id, std::uint32_t page) const noexcept {
    const State* state = find(id);
    return state != nullptr && page < state->in_flight.size() && state->in_flight[page] != 0;
}

std::span<const std::uint8_t>
VirtualGeometryResidency::page_residency_bytes(assets::AssetId id) const noexcept {
    const State* state = find(id);
    return state == nullptr ? std::span<const std::uint8_t>{} : state->resident;
}

} // namespace rime::render
