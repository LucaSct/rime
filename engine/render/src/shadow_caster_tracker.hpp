// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "rime/render/lighting/local_shadows.hpp"

namespace rime::render::detail {

// The identity is a full generational entity handle plus one mesh subrange, not extraction order.
// Material IDs and cosmetic properties deliberately have no field here: only changes to shadow
// depth may dirty a cached light. Supply ALL casters, including those outside the camera frustum;
// camera motion changes visibility, not the world geometry a stationary lamp needs to remember.
struct ShadowCasterRecord {
    std::uint64_t entity = 0;
    MeshId mesh = kInvalidMeshId;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    core::Mat4 model{};
    WorldAabb bounds{};
    std::uint32_t flags = 0;
    float alpha_factor = 1.0f;
    float alpha_cutoff = 0.0f;
    rhi::TextureHandle base_color_texture{};
    rhi::BufferHandle vertex_override{};
    std::uint64_t vertex_override_offset = 0;
    bool dynamic = false;
    bool bounds_known = true;
};

class ShadowCasterTracker {
public:
    // The callbacks operate on world regions, independently of camera/projection or draw order.
    // Changed records invalidate BOTH old and new footprints: moving a wall uncovers its old
    // shadow as well as casting a new one. Unknown deformation bounds and ambiguous identities
    // invalidate every light instead; guessing a plausible region would silently retain stale
    // shadows. Map entries retain only the preceding live set; reusable scratch keeps no history.
    template <class Invalidate, class InvalidateAll>
    void update(std::span<const ShadowCasterRecord> records,
                Invalidate&& invalidate,
                InvalidateAll&& invalidate_all) {
        bool all = previous_ambiguous_;
        bool ambiguous = false;
        regions_.clear();
        for (auto& [key, entry] : entries_) {
            (void)key;
            entry.seen = false;
        }
        const auto queue = [&](const Entry& entry) {
            if (entry.valid)
                regions_.push_back(entry.record.bounds);
            else
                all = true;
        };
        for (const auto& source : records) {
            ShadowCasterRecord record = normalized(source);
            const Key key{record.entity, record.mesh, record.first_index, record.index_count};
            const bool valid = valid_record(record);
            auto [it, added] = entries_.try_emplace(key, Entry{record, valid, true});
            if (added) {
                queue(it->second);
                continue;
            }
            Entry& previous = it->second;
            if (previous.seen) {
                // A duplicate can represent two DIFFERENT models/material masks. Keep a bounded
                // representative, refresh all lights now and once more when ambiguity disappears.
                // Never let unordered_map's last-writer-wins hide one caster's removal or edit.
                ambiguous = true;
                all = true;
                continue;
            }
            if (record.dynamic || !same_depth(previous.record, record)) {
                queue(previous);
                queue(Entry{record, valid, true});
            }
            if (!valid)
                all = true;
            previous = {record, valid, true};
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!it->second.seen) {
                queue(it->second);
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        previous_ambiguous_ = ambiguous;
        if (all) {
            invalidate_all();
        } else {
            for (const auto& region : regions_)
                invalidate(region);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Forget tracking only when its owner's shadow maps are also reset/invalidated.
    void clear() noexcept {
        entries_.clear();
        regions_.clear();
        previous_ambiguous_ = false;
    }

private:
    struct Key {
        std::uint64_t entity;
        MeshId mesh;
        std::uint32_t first_index;
        std::uint32_t index_count;
        bool operator==(const Key&) const = default;
    };

    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            std::uint64_t hash = 14695981039346656037ull;
            for (std::uint64_t value : {key.entity,
                                        static_cast<std::uint64_t>(key.mesh),
                                        static_cast<std::uint64_t>(key.first_index),
                                        static_cast<std::uint64_t>(key.index_count)}) {
                hash ^= value;
                hash *= 1099511628211ull;
            }
            return static_cast<std::size_t>(hash);
        }
    };

    struct Entry {
        ShadowCasterRecord record;
        bool valid;
        bool seen;
    };

    static ShadowCasterRecord normalized(ShadowCasterRecord record) {
        record.flags &= DrawItem::AlphaMasked | DrawItem::DoubleSided | DrawItem::ClampUv;
        if ((record.flags & DrawItem::AlphaMasked) == 0) {
            record.flags &= ~DrawItem::ClampUv;
            record.alpha_factor = 1.0f;
            record.alpha_cutoff = 0.0f;
            record.base_color_texture = {};
        }
        if (record.vertex_override.is_valid()) {
            record.dynamic = true;
        } else {
            record.vertex_override = {};
            record.vertex_override_offset = 0;
        }
        return record;
    }

    static bool valid_record(const ShadowCasterRecord& record) {
        if (!record.bounds_known)
            return false;
        for (float value : record.model.m)
            if (!std::isfinite(value))
                return false;
        const auto& b = record.bounds;
        return std::isfinite(b.min.x) && std::isfinite(b.min.y) && std::isfinite(b.min.z) &&
               std::isfinite(b.max.x) && std::isfinite(b.max.y) && std::isfinite(b.max.z) &&
               b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z &&
               std::isfinite(record.alpha_factor) && std::isfinite(record.alpha_cutoff);
    }

    // Explicit components avoid matrix padding and hash collisions as equality shortcuts. Float
    // bits also retain signed zero; invalid nonfinite inputs already force a global refresh.
    static bool same_float(float a, float b) {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    static bool same_vec(core::Vec3 a, core::Vec3 b) {
        return same_float(a.x, b.x) && same_float(a.y, b.y) && same_float(a.z, b.z);
    }

    static bool same_depth(const ShadowCasterRecord& a, const ShadowCasterRecord& b) {
        for (std::size_t i = 0; i < 16; ++i)
            if (!same_float(a.model.m[i], b.model.m[i]))
                return false;
        return same_vec(a.bounds.min, b.bounds.min) && same_vec(a.bounds.max, b.bounds.max) &&
               a.bounds_known == b.bounds_known && a.flags == b.flags &&
               same_float(a.alpha_factor, b.alpha_factor) &&
               same_float(a.alpha_cutoff, b.alpha_cutoff) &&
               a.base_color_texture == b.base_color_texture &&
               a.vertex_override == b.vertex_override &&
               a.vertex_override_offset == b.vertex_override_offset && a.dynamic == b.dynamic;
    }

    std::unordered_map<Key, Entry, KeyHash> entries_;
    std::vector<WorldAabb> regions_;
    bool previous_ambiguous_ = false;
};

} // namespace rime::render::detail
