// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/snapshot.hpp"

namespace rime::replication {

bool AckTracker::tracking_complete() const noexcept {
    if (part_count_ == 0 || part_count_ > 32) {
        return false; // nothing tracked yet, or a part_count we cannot represent — never complete
    }
    // All low `part_count_` bits set. Computed in 64-bit and truncated so part_count_ == 32 does
    // not shift a 32-bit 1 by 32, which is undefined behaviour rather than the 0 you might expect.
    const auto full = static_cast<std::uint32_t>((std::uint64_t{1} << part_count_) - 1);
    return parts_seen_ == full;
}

void AckTracker::observe(ecs::Version tick,
                         std::uint8_t part_index,
                         std::uint8_t part_count) noexcept {
    if (part_count == 0 || part_count > 32 || part_index >= part_count) {
        return; // malformed header from a peer; the caller drops the packet too
    }

    if (tick > tracking_tick_) {
        // A newer tick starts. If the one we were accumulating turned out complete, it becomes the
        // watermark; if it did not, it is simply abandoned — deliberately. Abandoning is the
        // conservative direction: the watermark stays put, so the server keeps re-offering that
        // tick's changes. Erring the other way is what causes permanent divergence.
        if (tracking_complete()) {
            watermark_ = tracking_tick_;
        }
        tracking_tick_ = tick;
        parts_seen_ = 0;
        part_count_ = part_count;
    } else if (tick < tracking_tick_) {
        // A straggler from an older tick, reordered behind a newer one. Its DATA is still applied
        // by the caller (older state for an entity the newer packet did not mention is better than
        // no state), but it can never advance the watermark: we already gave up on that tick.
        return;
    }

    parts_seen_ |= (std::uint32_t{1} << part_index);
    if (tracking_complete() && tracking_tick_ > watermark_) {
        watermark_ = tracking_tick_;
    }
}

} // namespace rime::replication
