// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "rime/core/jobs/chase_lev_deque.hpp"

// The work-stealing job system: the engine's answer to "use all the cores." It owns a pool of
// worker threads, each with its own Chase-Lev deque (M1.6a). A worker runs jobs from its own
// deque (LIFO) and, when idle, STEALS from another worker — so load balances itself with no
// central queue to bottleneck on. The submitting thread (the one that created the system) also
// owns a deque and HELPS run jobs while it waits, so it is never idle during a parallel region.
//
// Fork/join is expressed with a Counter: submitting a job bumps it, finishing a job drops it,
// and wait() runs jobs until it hits zero. parallel_for is the ergonomic front door built on
// that. Design rationale and the threading model are written up in docs/design/job-system.md.
//
// Threading contract: construct the system on, and submit/wait from, one "main" thread (or from
// within a running job). This matches a game's main-loop model and keeps the per-deque
// single-owner rule the Chase-Lev deque requires.
namespace rime::core {

struct Job; // defined in job_system.cpp; the deque only ever holds Job* (trivially copyable)

class JobSystem {
public:
    // Fork/join counter: number of outstanding jobs in a group. Wait on it to join.
    using Counter = std::atomic<int>;

    // num_workers == 0 picks a sensible default: hardware_concurrency() - 1, leaving a core for
    // the submitting thread (which helps via wait()), so workers + submitter == core count.
    explicit JobSystem(unsigned num_workers = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Submit one task. If `counter` is given, it is incremented now and decremented when the task
    // finishes — pass the same counter to wait() to join. Must be called from the main thread or
    // from within a running job.
    void run(std::function<void()> task, Counter* counter = nullptr);

    // Run jobs (own, then stolen) until `counter` reaches zero. The caller participates instead of
    // blocking, so there is no deadlock even if every worker is busy. After it returns, all writes
    // performed by the joined jobs are visible to the caller.
    void wait(Counter& counter);

    // Split [0, count) into chunks of `chunk` consecutive indices, run them across the pool, and
    // join before returning. `body(std::size_t index)` is invoked once per index. Because we join
    // before returning, `body` may safely capture local state by reference.
    template <class F> void parallel_for(std::size_t count, std::size_t chunk, F&& body) {
        if (count == 0) {
            return;
        }
        if (chunk == 0) {
            chunk = 1;
        }
        Counter counter{0};
        for (std::size_t begin = 0; begin < count; begin += chunk) {
            const std::size_t end = std::min(count, begin + chunk);
            // body is captured by reference; safe because wait() below outlives every job.
            run(
                [&body, begin, end] {
                    for (std::size_t i = begin; i < end; ++i) {
                        body(i);
                    }
                },
                &counter);
        }
        wait(counter);
    }

    // Worker threads only. The submitting thread additionally helps, so the number of threads that
    // can make progress in parallel is worker_count() + 1.
    [[nodiscard]] unsigned worker_count() const noexcept { return num_workers_; }

    [[nodiscard]] unsigned participant_count() const noexcept { return num_workers_ + 1; }

private:
    Job* get_job(int queue_index) noexcept; // pop own, else steal a random other
    void execute(Job* job);
    void worker_main(int queue_index);

    unsigned num_workers_ = 0;
    std::atomic<bool> stop_{false};
    // One deque per worker, plus one for the submitting/main thread (the last index). Pointers so
    // the non-movable deques have stable addresses.
    std::vector<std::unique_ptr<ChaseLevDeque<Job*>>> queues_;
    std::vector<std::thread> workers_;
};

} // namespace rime::core
