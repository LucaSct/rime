// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "hull.hpp"
#include "rime/physics/aabb.hpp"

namespace rime::physics::contact_bounds_detail {

// The existing compound midphase's metre-scale contact margin, not a fitted replacement for
// narrowphase semantics. Tight bounds are NOT sufficient: the frozen city oracle still emits
// eleven near-touch contacts outside them after the GJK containment repair. Keep those calls.
inline constexpr float kCullMargin = 1e-3f;

inline Aabb padded(Aabb a) noexcept {
    a = expanded(a, kCullMargin);
    // At large coordinates a millimetre can round away. One outward ULP makes the stored
    // expansion conservative; it only retains extra candidates, never discards another one.
    const float inf = std::numeric_limits<float>::infinity();
    a.min = {std::nextafter(a.min.x, -inf),
             std::nextafter(a.min.y, -inf),
             std::nextafter(a.min.z, -inf)};
    a.max = {
        std::nextafter(a.max.x, inf), std::nextafter(a.max.y, inf), std::nextafter(a.max.z, inf)};
    return a;
}

inline bool eligible(const ShapeDesc& s, const ConvexHull* h) noexcept {
    switch (s.type) {
        case ShapeType::Sphere:
        case ShapeType::Box:
        case ShapeType::Capsule:
            return true;
        case ShapeType::ConvexHull:
            return h != nullptr;
        default:
            return false;
    }
}

struct Bounds {
    Aabb tight{}, padded{};
};

// A contact build observes a frozen body snapshot: integration has finished and solving has not
// started. Cache each body slot's exact and padded bounds only within that interval. begin()
// resets validity for EVERY build, including inspection calls, so teleports, removals, reused
// generations and dense-array compaction cannot inherit a previous pose's bounds. Slot indexing
// follows the world's existing handle storage; the retained memory is bounded by its slot count.
class Cache {
public:
    void begin(std::size_t slots) {
        bounds_.resize(slots);
        valid_.assign(slots, 0);
        computed_ = 0;
    }

    const Bounds& get(std::uint32_t slot,
                      const ShapeDesc& shape,
                      core::Vec3 pos,
                      core::Quat q,
                      const ConvexHull* hull) {
        assert(slot < bounds_.size());
        if (valid_[slot] == 0) {
            auto& b = bounds_[slot];
            b.tight = hull ? hull_world_aabb(*hull, pos, q) : compute_aabb(shape, pos, q);
            b.padded = padded(b.tight);
            valid_[slot] = 1;
            ++computed_;
        }
        return bounds_[slot];
    }

    [[nodiscard]] std::uint64_t computed() const noexcept { return computed_; }

private:
    std::vector<Bounds> bounds_;
    std::vector<std::uint8_t> valid_;
    std::uint64_t computed_ = 0;
};
} // namespace rime::physics::contact_bounds_detail
