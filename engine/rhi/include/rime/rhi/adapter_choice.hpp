// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Pure adapter-selection logic, deliberately free of Vulkan types so it is unit-testable without a
// GPU. The Vulkan backend (src/vulkan/device_vulkan.cpp) reduces the physical-device list to these
// candidates and then asks this function which one to use; the tests pin the rules here.
namespace rime::rhi {

// One adapter that already passed the Vulkan 1.3 + dynamicRendering + synchronization2 bar.
struct AdapterCandidate {
    std::string name;   // VkPhysicalDeviceProperties::deviceName
    int type_score = 0; // the existing deviceType score: discrete 1000, integrated 500,
                        // virtual 250, cpu 100, other 10
};

struct AdapterChoice {
    std::size_t index = 0;             // which candidate won
    bool preference_matched = false;   // a preference was given AND matched this candidate
    bool preference_unmatched = false; // a preference was given and matched NOTHING
};

// Choose among `candidates`. `preference` is a case-insensitive SUBSTRING of the adapter name;
// empty means no preference. Returns nullopt only when `candidates` is empty.
[[nodiscard]] std::optional<AdapterChoice>
choose_adapter(std::span<const AdapterCandidate> candidates, std::string_view preference);

} // namespace rime::rhi
