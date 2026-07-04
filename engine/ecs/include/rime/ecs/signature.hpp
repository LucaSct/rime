// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>

#include "rime/ecs/component.hpp"

// A ComponentSignature is the *identity* of an archetype: the SET of component types its entities
// share (ADR-0018). It is stored as a sorted, deduplicated `vector<ComponentId>`, so two signatures
// with the same components compare and hash equal regardless of the order they were built in, and
// membership is a binary search. A signature is small — a handful of ids — so a flat sorted vector
// beats a hash set on both cache behavior and code simplicity.
//
// `with()` / `without()` produce the signature an entity moves to when a component is added or
// removed (the "archetype move" of M4.2b); `contains_all()` is the superset test a query uses to
// decide whether an archetype matches (M4.3).
namespace rime::ecs {

class ComponentSignature {
public:
    ComponentSignature() = default;
    ComponentSignature(std::initializer_list<ComponentId> ids);
    explicit ComponentSignature(std::vector<ComponentId> ids);

    [[nodiscard]] bool contains(ComponentId id) const noexcept;

    // True iff every id in `other` is also in this signature (this ⊇ other) — the query match test.
    [[nodiscard]] bool contains_all(const ComponentSignature& other) const noexcept;

    // A new signature with `id` added / removed; this one is left unchanged. Adding a present id or
    // removing an absent one returns an equal signature.
    [[nodiscard]] ComponentSignature with(ComponentId id) const;
    [[nodiscard]] ComponentSignature without(ComponentId id) const;

    [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

    [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }

    [[nodiscard]] const std::vector<ComponentId>& ids() const noexcept { return ids_; }

    [[nodiscard]] std::size_t hash() const noexcept;

    friend bool operator==(const ComponentSignature& a, const ComponentSignature& b) noexcept {
        return a.ids_ == b.ids_;
    }

    friend bool operator!=(const ComponentSignature& a, const ComponentSignature& b) noexcept {
        return !(a == b);
    }

private:
    std::vector<ComponentId> ids_; // sorted ascending by underlying value, unique
};

} // namespace rime::ecs

// Hash specialization so a signature can key the World's archetype map (M4.2b).
template <> struct std::hash<rime::ecs::ComponentSignature> {
    std::size_t operator()(const rime::ecs::ComponentSignature& s) const noexcept {
        return s.hash();
    }
};
