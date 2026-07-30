// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction_net/composition.hpp"

#include <cstring>

#include "rime/destruction/components.hpp"

// See composition.hpp for what this guards and why a hash rather than the composition itself.
namespace rime::destruction_net {

namespace {

constexpr std::uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnv1a64Prime = 0x100000001b3ull;

void fold(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (int shift = 56; shift >= 0; shift -= 8) {
        hash ^= (value >> shift) & 0xFFull;
        hash *= kFnv1a64Prime;
    }
}

} // namespace

std::uint64_t debris_composition_hash(const destruction::DestructionWorld& destruction,
                                      destruction::InstanceId instance) noexcept {
    std::uint64_t hash = kFnv1a64OffsetBasis;

    // The chunk COUNT leads, so "three chunks whose members happen to concatenate the same way as
    // two" cannot collide with the two-chunk case — the exact shape of the A12 divergence, where
    // ten parts left as one island on one peer and as two islands on the other. Without a count and
    // a per-chunk length, those two states fold identically and the check would have missed the one
    // bug it exists to catch.
    std::uint64_t chunks = 0;
    for (std::size_t d = 0; d < destruction.debris_count(); ++d) {
        if (destruction.debris_source(d) == instance) {
            ++chunks;
        }
    }
    fold(hash, chunks);

    for (std::size_t d = 0; d < destruction.debris_count(); ++d) {
        if (destruction.debris_source(d) != instance) {
            continue;
        }
        const std::span<const std::uint32_t> members = destruction.debris_parts(d);
        fold(hash, members.size());
        for (const std::uint32_t part : members) {
            fold(hash, part);
        }
    }
    return hash;
}

std::uint64_t shared_state_hash(const ecs::World& world,
                                const replication::NetIdMap& map,
                                const destruction::DestructionWorld& destruction) {
    std::uint64_t hash = kFnv1a64OffsetBasis;

    map.for_each([&](replication::NetId net_id, ecs::Entity entity) {
        const auto* ref = world.get<destruction::DestructibleInstanceRef>(entity);
        if (ref == nullptr || ref->instance == destruction::kUnboundInstance) {
            return; // replicated, but not a standing destructible — not ours to describe
        }
        const destruction::InstanceId instance{ref->instance, 0};

        // The shared NAME leads, so two peers holding different SETS of walls differ here even if
        // every wall they do share is identical.
        fold(hash, net_id.index);
        fold(hash, net_id.generation);

        const std::uint32_t parts = destruction.instance_part_count(instance);
        fold(hash, parts);
        for (std::uint32_t p = 0; p < parts; ++p) {
            fold(hash, destruction.part_alive(instance, p) ? 1u : 0u);
            // Health as its exact bit pattern. The point of a witness is bit-identity; folding a
            // rounded or tolerance-compared value would hide precisely the sub-ULP drift it exists
            // to catch.
            const float health = destruction.part_health(instance, p);
            std::uint32_t bits = 0;
            std::memcpy(&bits, &health, sizeof(bits));
            fold(hash, bits);
        }

        // Composition, not transforms: the rubble's SHAPE is derived identically on both peers, its
        // trajectory is not (that is what replication corrects, and what m11.6 will interpolate).
        fold(hash, debris_composition_hash(destruction, instance));
    });
    return hash;
}

} // namespace rime::destruction_net
