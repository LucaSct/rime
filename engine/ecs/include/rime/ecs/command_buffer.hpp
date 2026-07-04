// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "rime/ecs/entity.hpp"
#include "rime/ecs/world.hpp"

// A CommandBuffer records STRUCTURAL changes — spawn, despawn, add/remove component — to replay
// later, at a safe point. It exists because iteration forbids structural change: a Query (serial or
// par_for_each) and the system Schedule (M4.4a/b) are scanning archetypes, and adding/removing a
// component or spawning/despawning would move entities between archetypes mid-scan, reallocating
// the very storage being read — concurrently, under the scheduler. So a system that needs to
// restructure the world *records the intent* here, and the Schedule APPLIES it at the next phase
// boundary (M4.4c), when no iteration is in flight.
//
// Each command is captured as a small closure over the typed World call, so the buffer stays
// type-agnostic and apply() is simply "run them in order" — no per-type erasure machinery, and the
// closure remembers the component type for free. Recording is thread-safe (a mutex guards the
// record list) so a body may push commands from inside par_for_each, i.e. from many worker threads
// at once; structural change is the rare, bursty path (ADR-0018), so the lock is uncontended in
// practice. apply() runs single-threaded at the safe point and clears the buffer for reuse.
//
// Order: commands replay in the order recorded. Under parallel recording that order is unspecified
// ACROSS threads, so record commutative edits (independent spawns/despawns/adds) — the usual case;
// don't rely on the relative order of two commands produced on different worker threads.
namespace rime::ecs {

class CommandBuffer {
public:
    CommandBuffer() = default;
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    // Spawn an entity carrying `comps` (none = an empty entity) when applied.
    template <class... Ts> void spawn(const Ts&... comps) {
        record([comps...](World& world) { (void)world.spawn_with(comps...); });
    }

    // Despawn `e` when applied (a no-op if it is already gone by then).
    void despawn(Entity e) {
        record([e](World& world) { (void)world.despawn(e); });
    }

    // Add (or overwrite) component T on `e` when applied.
    template <class T> void add_component(Entity e, const T& value) {
        record([e, value](World& world) { (void)world.add_component<T>(e, value); });
    }

    // Remove component T from `e` when applied.
    template <class T> void remove_component(Entity e) {
        record([e](World& world) { (void)world.remove_component<T>(e); });
    }

    // Replay every recorded command into `world`, in record order, then clear. Call only at a safe
    // point (no query/system iterating) — the Schedule does this at each phase boundary. Because it
    // mutates world structure, it must run single-threaded.
    void apply(World& world) {
        for (auto& command : commands_) {
            command(world);
        }
        commands_.clear();
    }

    void clear() { commands_.clear(); }

    // Inspection, for a safe point (not while another thread may be recording).
    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

    [[nodiscard]] bool empty() const noexcept { return commands_.empty(); }

private:
    void record(std::function<void(World&)> command) {
        // Only the push is guarded; building the closure (its one heap allocation) happens before
        // the lock, at the call site, keeping the critical section to a single vector append.
        const std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(std::move(command));
    }

    std::mutex mutex_;
    std::vector<std::function<void(World&)>> commands_;
};

} // namespace rime::ecs
