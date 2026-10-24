// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/audio/audio.hpp"
#include "rime/core/math/vec.hpp"

// Turning a storm of destruction events into a handful of sounds (2026-10).
//
// THE PROBLEM. One demolition charge on the city block produces ~360 "part died" events in a single
// tick and ~600 over the next ten seconds. Played one-for-one, that is not a collapse, it is a
// machine gun of identical clicks -- and the owner heard a buzz. A real collapse is not hundreds of
// separate sounds either: the ear hears a few dominant impacts and, around them, one continuous
// roar of everything else. So this class does what the ear does:
//
//   * TRANSIENTS. Events are grouped by ~4 m REGION (a spatial hash) over an 80 ms WINDOW. When a
//     window closes, only the strongest few regions are voiced as discrete impacts -- the slab
//     that really did slam, not each of its sixty bricks.
//   * THE BED. Everything not voiced as a transient is folded into a slowly varying INTENSITY.
//     While intensity is high a low rumble recording is kept sounding (chained one-shots, since
//     the mixer has no looping voices); its gain follows the intensity, which decays with a time
//     constant of a couple of seconds. Ongoing destruction therefore sustains the bed for 2-6 s
//     and a quiet moment lets it die away.
//   * SETTLING. A `DebrisSettled` event never makes an impact: debris coming to rest is the
//     opposite of a hit. It REDUCES the intensity (the roar fades as things stop moving), and the
//     tail of the bed is voiced as a sparse, quiet trickle of small rubble sounds.
//   * COLLAPSE. A detached island is the one big event: voiced at once (the mixer's retrigger
//     window keeps a cascade of them from stacking) and it kicks the bed's intensity hard.
//
// It talks to the `AudioBackend` interface, so it needs no mixer to be tested and a different
// backend can reuse it.
namespace rime::audio {

enum class DestructionKind : std::uint8_t { PartBroken, IslandDetached, DebrisSettled };

struct DestructionVoicerConfig {
    float region_size = 4.0f;          // metres: events closer than this share a region
    float window_seconds = 0.08f;      // aggregation window
    std::uint32_t max_transients = 3;  // discrete impacts voiced per window
    float bed_per_event = 0.012f;      // intensity added per unit of unvoiced magnitude
    float bed_decay_seconds = 2.2f;    // intensity falls by 1/e in this long
    float bed_start_threshold = 0.10f; // intensity needed to (re)start the rumble
    float bed_restart_seconds = 1.8f;  // chained one-shots overlap by their fade
    float settle_drop = 0.05f;         // intensity removed per DebrisSettled event
    float trickle_interval_seconds = 0.3f;
};

struct DestructionVoicerStats {
    std::uint64_t events_in = 0;
    std::uint64_t transients_played = 0;
    std::uint64_t collapses_played = 0;
    std::uint64_t folded_into_bed = 0; // events whose energy went to the bed instead of a voice
    std::uint64_t bed_starts = 0;
    std::uint64_t trickles = 0;
    std::uint64_t settles_seen = 0;
};

class DestructionVoicer {
public:
    explicit DestructionVoicer(const DestructionVoicerConfig& config = {}) : config_(config) {}

    // Queue an event heard this tick. `magnitude` is 0..1 (larger = bigger).
    void add(DestructionKind kind, core::Vec3 position, float magnitude);

    // Close the tick: advance time by `dt`, voice whatever the window decides, drive the bed.
    // Call once per simulation tick, after every `add` for that tick.
    void update(AudioBackend& backend, float dt);

    [[nodiscard]] float bed_intensity() const noexcept { return intensity_; }

    [[nodiscard]] const DestructionVoicerStats& stats() const noexcept { return stats_; }

private:
    struct Region {
        std::int32_t x = 0, y = 0, z = 0;
        float sum = 0.0f;       // total magnitude of events in the region
        float strongest = 0.0f; // the biggest single event
        core::Vec3 at{0, 0, 0}; // where the biggest one was
        std::uint32_t count = 0;
    };

    void flush_window(AudioBackend& backend);

    struct PendingCollapse {
        core::Vec3 at;
        float magnitude;
    };

    std::vector<PendingCollapse> pending_collapses_;

    DestructionVoicerConfig config_;
    std::vector<Region> regions_;
    float window_age_ = 0.0f;
    bool window_open_ = false;
    float intensity_ = 0.0f;
    float bed_cooldown_ = 0.0f;
    float trickle_cooldown_ = 0.0f;
    // Intensity-weighted centroid of recent activity: where the bed is heard from.
    core::Vec3 centroid_sum_{0, 0, 0};
    float centroid_weight_ = 0.0f;
    DestructionVoicerStats stats_;
};

} // namespace rime::audio
