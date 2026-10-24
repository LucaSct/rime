// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/audio/destruction_voicer.hpp"

#include <algorithm>
#include <cmath>

#include "rime/audio/bank.hpp"

namespace rime::audio {

void DestructionVoicer::add(DestructionKind kind, core::Vec3 position, float magnitude) {
    ++stats_.events_in;
    const float m = std::clamp(magnitude, 0.0f, 1.0f);
    switch (kind) {
        case DestructionKind::DebrisSettled:
            // Settling only ever makes the roar quieter. Never a sound of its own.
            ++stats_.settles_seen;
            intensity_ = std::max(0.0f, intensity_ - config_.settle_drop);
            return;
        case DestructionKind::IslandDetached:
            // Handled in update() (needs the backend); remembered as a region with a flag-sized
            // magnitude so the centroid and intensity still see it.
            pending_collapses_.push_back({position, m});
            return;
        case DestructionKind::PartBroken:
            break;
    }
    // The spatial hash: floor(position / region_size) per axis. Integer cells make "same region"
    // an exact comparison rather than a distance test, and the lookup stays a short linear scan
    // because a window rarely holds more than a few dozen regions.
    const float inv = 1.0f / std::max(0.5f, config_.region_size);
    const auto cx = static_cast<std::int32_t>(std::floor(position.x * inv));
    const auto cy = static_cast<std::int32_t>(std::floor(position.y * inv));
    const auto cz = static_cast<std::int32_t>(std::floor(position.z * inv));
    Region* region = nullptr;
    for (Region& r : regions_) {
        if (r.x == cx && r.y == cy && r.z == cz) {
            region = &r;
            break;
        }
    }
    if (region == nullptr) {
        regions_.push_back({cx, cy, cz, 0.0f, 0.0f, position, 0});
        region = &regions_.back();
    }
    region->sum += m;
    ++region->count;
    if (m >= region->strongest) {
        region->strongest = m;
        region->at = position;
    }
    window_open_ = true;
}

void DestructionVoicer::flush_window(AudioBackend& backend) {
    // Rank regions by total magnitude (a region of sixty small bricks beats one lone brick), ties
    // by position of insertion so the result is deterministic.
    std::stable_sort(regions_.begin(), regions_.end(), [](const Region& a, const Region& b) {
        return a.sum > b.sum;
    });
    float unvoiced = 0.0f;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        const Region& r = regions_[i];
        if (i < config_.max_transients) {
            backend.play(sound::kPartBreak, r.at, std::min(1.0f, 0.35f + 0.65f * r.strongest));
            ++stats_.transients_played;
            // Whatever else the region held beyond its one voiced event feeds the bed.
            unvoiced += std::max(0.0f, r.sum - r.strongest);
            stats_.folded_into_bed += r.count > 0 ? r.count - 1 : 0;
        } else {
            unvoiced += r.sum;
            stats_.folded_into_bed += r.count;
        }
        const float w = std::max(0.05f, r.sum);
        centroid_sum_ = centroid_sum_ + r.at * w;
        centroid_weight_ += w;
    }
    intensity_ = std::min(1.0f, intensity_ + unvoiced * config_.bed_per_event);
    regions_.clear();
    window_open_ = false;
    window_age_ = 0.0f;
}

void DestructionVoicer::update(AudioBackend& backend, float dt) {
    for (const auto& c : pending_collapses_) {
        backend.play(sound::kCollapse, c.at, 1.0f);
        ++stats_.collapses_played;
        // A detached island is a big, sustained event: it kicks the bed hard.
        intensity_ = std::min(1.0f, intensity_ + 0.45f * std::max(0.4f, c.magnitude));
        centroid_sum_ = centroid_sum_ + c.at * 1.0f;
        centroid_weight_ += 1.0f;
    }
    pending_collapses_.clear();

    if (window_open_) {
        window_age_ += dt;
        if (window_age_ >= config_.window_seconds) {
            flush_window(backend);
        }
    }

    // The bed: intensity decays exponentially; while it is high enough, keep a rumble sounding.
    const float decay = std::exp(-dt / std::max(0.1f, config_.bed_decay_seconds));
    intensity_ *= decay;
    centroid_sum_ = centroid_sum_ * decay;
    centroid_weight_ *= decay;
    bed_cooldown_ -= dt;
    trickle_cooldown_ -= dt;
    const core::Vec3 where =
        centroid_weight_ > 1e-4f ? centroid_sum_ * (1.0f / centroid_weight_) : core::Vec3{0, 0, 0};
    if (intensity_ >= config_.bed_start_threshold && bed_cooldown_ <= 0.0f) {
        backend.play(sound::kRumble, where, std::min(1.0f, 0.3f + intensity_));
        bed_cooldown_ = config_.bed_restart_seconds;
        ++stats_.bed_starts;
    }
    // When the roar is dying (but not gone) it is voiced as sparse, quiet rubble: the trickle.
    if (intensity_ > 0.02f && intensity_ < config_.bed_start_threshold * 2.5f &&
        trickle_cooldown_ <= 0.0f) {
        backend.play(sound::kDebrisSettle, where, 0.12f + 0.5f * intensity_);
        trickle_cooldown_ = config_.trickle_interval_seconds;
        ++stats_.trickles;
    }
}

} // namespace rime::audio
