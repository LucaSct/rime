// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "rime/core/jobs/job_system.hpp"
#include "rime/ecs/system.hpp"

// A Schedule is an ordered list of Systems batched into parallel PHASES by their access sets, then
// run on the JobSystem — the M4.4b payoff on top of M4.4a's per-system chunk parallelism. Two
// levels of parallelism stack: the schedule runs independent systems side by side (task
// parallelism), and each system can still fan its own work across chunks with par_for_each (data
// parallelism).
//
// How phases are formed (ASAP leveling of the conflict order). Systems keep their declared order.
// Each system is placed in the phase one past the LAST earlier system it conflicts with — formally
//     phase(j) = 0                                   if no earlier system conflicts with j
//     phase(j) = 1 + max{ phase(i) : i < j, i ⨯ j }  otherwise
// Two guarantees fall straight out of that definition:
//   * within a phase, no two systems conflict — so the whole phase runs concurrently, race-free;
//   * conflicting systems keep their declared order — if i<j conflict, phase(i) < phase(j), so i
//     runs (and finishes) in an earlier phase than j.
// Phases run in sequence with a join between them; the join is the write-visibility barrier that
// makes a later phase see an earlier phase's writes. The number of phases equals the longest chain
// of mutually-conflicting systems — the irreducible amount of serialization the declared hazards
// force. It is O(N²) conflict checks over N systems, trivial for the dozens of systems a frame has.
//
// Structural-change rule: a system body may read and write component DATA directly, but must NOT
// add/remove components or spawn/despawn on the World mid-phase — that would restructure the
// archetypes other systems in the same phase are concurrently scanning. Instead it records those
// edits into the CommandBuffer it is handed, and the Schedule APPLIES each phase's command buffers
// at the join at the end of that phase (M4.4c) — the safe point where no system is iterating, so a
// later phase sees an earlier phase's spawns/despawns.
namespace rime::ecs {

class Schedule {
public:
    Schedule() = default;

    // Append a system. Order matters: it fixes the run order of any systems that conflict with it.
    Schedule& add(System system);

    // Convenience: build the System in place from its parts.
    template <class Body> Schedule& add(std::string name, SystemAccess access, Body&& body) {
        return add(System{std::move(name),
                          std::move(access),
                          std::function<void(World&, core::JobSystem&, CommandBuffer&)>(
                              std::forward<Body>(body))});
    }

    // (Re)compute the phase batching from the systems' access sets. Called automatically by run()
    // when the system list changed; exposed so tests (and tools) can inspect the plan without
    // running it.
    void rebuild();

    // Run every system: for each phase in order, run its systems concurrently on `jobs` and join
    // before advancing to the next phase. Rebuilds the plan first if the system list changed.
    void run(World& world, core::JobSystem& jobs);

    [[nodiscard]] std::size_t system_count() const noexcept { return systems_.size(); }

    // The computed phases, each a list of system indices (into the add() order). Valid after
    // rebuild() or run(); empty before the first build or when there are no systems.
    [[nodiscard]] const std::vector<std::vector<std::uint32_t>>& phases() const noexcept {
        return phases_;
    }

    [[nodiscard]] std::size_t phase_count() const noexcept { return phases_.size(); }

    [[nodiscard]] const std::string& system_name(std::uint32_t index) const {
        return systems_[index].name;
    }

private:
    std::vector<System> systems_;
    std::vector<std::vector<std::uint32_t>>
        phases_;         // phases_[p] = system indices that run in phase p
    bool dirty_ = false; // systems changed since the last rebuild()
};

} // namespace rime::ecs
