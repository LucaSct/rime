// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

// EventChannel<T> — a generic double-buffered event queue: a system PUSHES typed events as it runs,
// PUBLISHES them once at a tick boundary, and consumers read the published batch as a stable span
// until the next publish. It is the "events are data the game reads AFTER the tick, not callbacks
// fired DURING it" pattern generalized out of the physics engine (M7.9's contact/sleep spans), so
// any producer — destruction (M8.4, the first customer), and later the FX / lighting / audio queues
// — gets the same shape without re-deriving it. Physics keeps its own private spans; this is the
// shared vocabulary for everyone above it.
//
// Why double-buffered (two vectors, swapped on publish) rather than one that you fill and clear?
// Because it cleanly separates "the frame being read" from "the frame being written": a consumer
// can hold the view() span across the whole post-tick fan-out while — in a later threaded world —
// the next tick's producer already begins pushing into the other buffer. Swapping instead of
// copying keeps publish() O(1) and lets both vectors amortize to a steady capacity (no per-tick
// allocation once warm). The read buffer's storage is reused two ticks later, never freed in steady
// state.
//
// Determinism is the CALLER's job, not the channel's: view() hands events back in exactly push()
// order, so a producer that pushes in a canonical order (as the destruction update does — every
// table walked in ascending index order) yields a reproducible stream, event for event. The channel
// adds no ordering of its own precisely so it cannot perturb one.
namespace rime::core {

template <class T> class EventChannel {
public:
    // Queue one event into the buffer currently being written. Order is preserved verbatim.
    void push(const T& event) { write_.push_back(event); }

    void push(T&& event) { write_.push_back(std::move(event)); }

    // Close the frame: the events written since the last publish become the readable batch, and the
    // previously-readable batch is cleared to receive the next frame. Call once per tick, after the
    // producer has finished pushing and before consumers read. Swapping (not copying) is what makes
    // this O(1) and allocation-free once the vectors have grown to their working size.
    void publish() noexcept {
        read_.swap(write_);
        write_.clear();
    }

    // The events published by the most recent publish() — a stable view until the next one. Empty
    // after a publish() that followed no push()es (the "channel is clean next tick" property a
    // consumer relies on to know a quiet tick produced nothing).
    [[nodiscard]] std::span<const T> view() const noexcept { return {read_.data(), read_.size()}; }

    // Events pushed since the last publish() but not yet visible through view(). Mostly for tests
    // and asserts — a well-behaved producer publishes before anyone looks.
    [[nodiscard]] std::size_t pending() const noexcept { return write_.size(); }

    // Drop everything, both frames. For teardown / reset; a normal tick uses publish().
    void clear() noexcept {
        read_.clear();
        write_.clear();
    }

private:
    std::vector<T> read_;  // the published frame consumers see through view()
    std::vector<T> write_; // the frame being filled by push() for the next publish()
};

} // namespace rime::core
