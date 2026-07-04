// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/signature.hpp"

#include <algorithm>

namespace rime::ecs {

namespace {
// ComponentId is a scoped enum; compare/hash on its underlying integer value.
[[nodiscard]] std::uint32_t value_of(ComponentId id) noexcept {
    return static_cast<std::uint32_t>(id);
}

// Put ids in a canonical form — ascending, no duplicates — so equal sets have identical storage.
void normalize(std::vector<ComponentId>& ids) {
    std::sort(ids.begin(), ids.end(), [](ComponentId a, ComponentId b) {
        return value_of(a) < value_of(b);
    });
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}
} // namespace

ComponentSignature::ComponentSignature(std::initializer_list<ComponentId> ids) : ids_(ids) {
    normalize(ids_);
}

ComponentSignature::ComponentSignature(std::vector<ComponentId> ids) : ids_(std::move(ids)) {
    normalize(ids_);
}

bool ComponentSignature::contains(ComponentId id) const noexcept {
    return std::binary_search(ids_.begin(), ids_.end(), id, [](ComponentId a, ComponentId b) {
        return value_of(a) < value_of(b);
    });
}

bool ComponentSignature::contains_all(const ComponentSignature& other) const noexcept {
    // Both are sorted, so a linear merge (std::includes) answers "this ⊇ other" in O(n+m).
    return std::includes(ids_.begin(),
                         ids_.end(),
                         other.ids_.begin(),
                         other.ids_.end(),
                         [](ComponentId a, ComponentId b) { return value_of(a) < value_of(b); });
}

bool ComponentSignature::intersects(const ComponentSignature& other) const noexcept {
    // Both id vectors are sorted ascending, so walk them together (a merge) — O(n+m), no
    // allocation. The first id present in both means the sets overlap.
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < ids_.size() && j < other.ids_.size()) {
        const std::uint32_t a = value_of(ids_[i]);
        const std::uint32_t b = value_of(other.ids_[j]);
        if (a == b) {
            return true;
        }
        if (a < b) {
            ++i;
        } else {
            ++j;
        }
    }
    return false;
}

ComponentSignature ComponentSignature::with(ComponentId id) const {
    if (contains(id)) {
        return *this;
    }
    std::vector<ComponentId> next = ids_;
    next.push_back(id);
    return ComponentSignature(std::move(next)); // ctor re-normalizes (keeps it sorted)
}

ComponentSignature ComponentSignature::without(ComponentId id) const {
    std::vector<ComponentId> next;
    next.reserve(ids_.size());
    for (const ComponentId existing : ids_) {
        if (existing != id) {
            next.push_back(existing);
        }
    }
    ComponentSignature result;
    result.ids_ = std::move(next); // already sorted + unique (we only removed one)
    return result;
}

std::size_t ComponentSignature::hash() const noexcept {
    // FNV-1a over the id sequence: order-independent is unnecessary (ids_ is canonical), so a
    // simple sequential mix is enough and collision-cheap for the small sets we key archetypes on.
    std::size_t h = 1469598103934665603ull; // FNV offset basis
    for (const ComponentId id : ids_) {
        h ^= value_of(id);
        h *= 1099511628211ull; // FNV prime
    }
    return h;
}

} // namespace rime::ecs
