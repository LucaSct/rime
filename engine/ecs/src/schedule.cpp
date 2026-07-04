// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/ecs/schedule.hpp"

#include <algorithm>

namespace rime::ecs {

Schedule& Schedule::add(System system) {
    systems_.push_back(std::move(system));
    dirty_ = true; // the plan no longer reflects the system list
    return *this;
}

void Schedule::rebuild() {
    phases_.clear();
    const std::size_t n = systems_.size();
    if (n == 0) {
        dirty_ = false;
        return;
    }

    // ASAP leveling: level[j] is j's phase index. A system lands one phase past the last EARLIER
    // system it conflicts with (0 if none), which both keeps conflicting systems in declared order
    // and guarantees no two systems in a level conflict (see schedule.hpp). O(N²) conflict checks.
    std::vector<std::uint32_t> level(n, 0);
    std::uint32_t max_level = 0;
    for (std::size_t j = 0; j < n; ++j) {
        std::uint32_t lvl = 0;
        for (std::size_t i = 0; i < j; ++i) {
            if (systems_[i].access.conflicts_with(systems_[j].access)) {
                lvl = std::max(lvl, level[i] + 1);
            }
        }
        level[j] = lvl;
        max_level = std::max(max_level, lvl);
    }

    // Bucket the systems into their phases, preserving add() order within each phase.
    phases_.resize(static_cast<std::size_t>(max_level) + 1);
    for (std::size_t j = 0; j < n; ++j) {
        phases_[level[j]].push_back(static_cast<std::uint32_t>(j));
    }
    dirty_ = false;
}

void Schedule::run(World& world, core::JobSystem& jobs) {
    if (dirty_) {
        rebuild();
    }
    for (const auto& phase : phases_) {
        if (phase.size() == 1) {
            // A lone system needs no fork/join — run it inline (it may still parallelize its own
            // work across chunks via par_for_each). This is the common case for a big system that
            // conflicts with its neighbors and thus sits alone in its phase.
            systems_[phase[0]].run(world, jobs);
            continue;
        }
        // Several independent systems: submit each as a job so they run side by side, then join
        // before the next phase. The join publishes this phase's writes to the next one. A system
        // body may itself call par_for_each here — that submit/wait-from-within-a-job is exactly
        // what the JobSystem's participating wait() supports (no deadlock even if all workers
        // busy).
        core::JobSystem::Counter counter{0};
        for (const std::uint32_t idx : phase) {
            System* system = &systems_[idx];
            jobs.run([system, &world, &jobs] { system->run(world, jobs); }, &counter);
        }
        jobs.wait(counter);
    }
}

} // namespace rime::ecs
