// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/schema_hash.hpp"

#include <algorithm>
#include <vector>

#include "rime/ecs/world.hpp"

namespace rime::ecs {

std::uint64_t component_schema_hash(const World& world) noexcept {
    const ComponentRegistry& registry = world.components();

    std::vector<std::uint64_t> hashes;
    hashes.reserve(registry.count());
    for (std::size_t i = 0; i < registry.count(); ++i) {
        const ComponentInfo& info = registry.info(static_cast<ComponentId>(i));
        if (info.type_info != nullptr) {
            hashes.push_back(info.type_info->type_hash);
        }
    }

    // Order-independence, the property the whole handshake rests on — see the header.
    std::sort(hashes.begin(), hashes.end());

    // FNV-1a over the sorted fingerprints, byte by byte, so the result never depends on host
    // endianness — the same discipline as the byte cursors and PhysicsWorld::world_hash.
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const std::uint64_t type_hash : hashes) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            hash ^= (type_hash >> shift) & 0xFFull;
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

} // namespace rime::ecs
