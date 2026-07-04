// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <functional>
#include <string>
#include <utility>

#include "rime/core/jobs/job_system.hpp"
#include "rime/ecs/signature.hpp"
#include "rime/ecs/world.hpp"

// A System is a named unit of frame work — a function that runs over the world — together with a
// declaration of the components it READS and the components it WRITES (its "access set"). That
// declaration is what lets the scheduler (schedule.hpp, M4.4b) decide which systems may run at the
// same time: two systems are safe to run concurrently exactly when neither writes a component the
// other touches. The scheduler batches non-conflicting systems into parallel phases from these
// sets.
//
// The access sets carry the whole correctness argument for concurrent systems. When the scheduler
// runs systems A and B together it is *because* their sets don't conflict, which means every memory
// location A writes is one B never reads or writes (and vice versa) — so their component-column
// writes land in disjoint memory and there is no data race, even when they iterate the same
// archetype's chunks. The flip side is a contract: a system must declare its access truthfully. A
// body that writes a component it didn't list is a data race the scheduler cannot see (the same
// discipline Unity DOTS and Bevy require of their declared/inferred access).
namespace rime::ecs {

// The components a system reads and writes. Read-only sharing is safe, so only WRITES create
// hazards: two systems conflict iff one writes a component the other reads or writes.
struct SystemAccess {
    ComponentSignature reads;
    ComponentSignature writes;

    // True iff running `*this` and `other` concurrently would be a data hazard. Read-read overlap
    // is deliberately NOT a conflict — that is what lets many reader systems share one phase.
    [[nodiscard]] bool conflicts_with(const SystemAccess& other) const noexcept {
        return writes.intersects(other.writes)    // both mutate the same component (write-write)
               || writes.intersects(other.reads)  // we mutate what they read     (write-read)
               || reads.intersects(other.writes); // they mutate what we read     (read-write)
    }
};

// A schedulable unit: a name (for debugging / the future editor), an access declaration, and the
// body. The body is handed the World and the JobSystem, so it may parallelize its own work across
// chunks with Query::par_for_each (M4.4a) while the scheduler parallelizes across systems.
struct System {
    std::string name;
    SystemAccess access;
    std::function<void(World&, core::JobSystem&)> run;
};

// Build a component set (a system's read or write access) from component TYPES, registering each
// with `world` if needed so its id exists. Access is expressed in the ComponentIds `world` assigns,
// so resolve it against the same World the schedule will run on:
//
//   System integrate{"integrate",
//                    {/*reads*/ signature_of<Velocity>(world), /*writes*/
//                    signature_of<Position>(world)},
//                    [](World& w, core::JobSystem& j) { /* w.query<...>().par_for_each(j, ...) */
//                    }};
template <class... Ts> [[nodiscard]] ComponentSignature signature_of(World& world) {
    return ComponentSignature{world.register_component<Ts>()...};
}

} // namespace rime::ecs
