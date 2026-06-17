// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/jobs/job_system.hpp"

#include <chrono>
#include <cstdint>
#include <functional>

#include "rime/core/diagnostics/assert.hpp"

namespace rime::core {

// A job is just the work plus the group counter to drop when it finishes. The Chase-Lev deques
// hold Job* (trivially copyable); the Job objects themselves live in a per-thread ring (below).
struct Job {
    std::function<void()> fn;
    JobSystem::Counter* counter = nullptr;
};

namespace {

// Each thread that submits jobs owns a fixed ring of Job storage, so allocating a job is a
// pointer bump with no heap traffic on the hot path. A slot is safe to reuse once the job that
// lived there has finished; callers join (wait) before the head wraps, so as long as a single
// fork/join group stays under kJobRingSize jobs, no in-flight job is overwritten. (A growable or
// generation-checked pool is a later refinement; this is the classic ring used by game job
// systems.)
constexpr std::size_t kJobRingSize = 1u << 14; // 16384 jobs in flight per thread

// Which deque this thread owns: a worker's index, or num_workers for the submitting thread.
// -1 means "not a participant" — submitting from such a thread is a contract violation.
thread_local int t_queue_index = -1;
thread_local std::vector<Job> t_ring;
thread_local std::size_t t_ring_head = 0;
thread_local std::uint64_t t_rng = 0;

Job* allocate_job(std::function<void()> fn, JobSystem::Counter* counter) {
    if (t_ring.empty()) {
        t_ring.resize(kJobRingSize);
    }
    Job* job = &t_ring[t_ring_head];
    t_ring_head = (t_ring_head + 1) & (kJobRingSize - 1); // power-of-two wrap
    job->fn = std::move(fn);
    job->counter = counter;
    return job;
}

// Pick a random victim queue to steal from, never ourselves. Randomization spreads steal traffic
// so workers don't all hammer the same victim (which would just move the bottleneck).
int pick_victim(int self, int num_queues) noexcept {
    if (t_rng == 0) {
        t_rng =
            0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(self) + 1) * 0xD1B54A32D192ED03ull;
    }
    t_rng ^= t_rng << 13; // xorshift64
    t_rng ^= t_rng >> 7;
    t_rng ^= t_rng << 17;
    int v = static_cast<int>(t_rng % static_cast<std::uint64_t>(num_queues));
    if (v == self) {
        v = (v + 1) % num_queues;
    }
    return v;
}

} // namespace

JobSystem::JobSystem(unsigned num_workers) {
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        hardware = 4; // unknown; pick a reasonable default
    }
    // Leave a core for the submitting thread, which helps via wait(); so workers + submitter ==
    // hardware concurrency.
    num_workers_ = num_workers != 0 ? num_workers : (hardware > 1 ? hardware - 1 : 1);

    const int num_queues = static_cast<int>(num_workers_) + 1;
    queues_.reserve(static_cast<std::size_t>(num_queues));
    for (int i = 0; i < num_queues; ++i) {
        queues_.push_back(std::make_unique<ChaseLevDeque<Job*>>(1024));
    }

    // This (constructing) thread owns the last queue; it submits here and helps drain via wait().
    t_queue_index = static_cast<int>(num_workers_);

    workers_.reserve(num_workers_);
    for (unsigned i = 0; i < num_workers_; ++i) {
        workers_.emplace_back([this, i] { worker_main(static_cast<int>(i)); });
    }
}

JobSystem::~JobSystem() {
    stop_.store(true, std::memory_order_release);
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void JobSystem::run(std::function<void()> task, Counter* counter) {
    RIME_ASSERT(t_queue_index >= 0); // submit from the main thread or from within a running job
    if (counter != nullptr) {
        // Bump before publishing the job. In a fork/join group all run() calls happen-before the
        // wait() on the same thread, so a relaxed increment is correctly ordered.
        counter->fetch_add(1, std::memory_order_relaxed);
    }
    Job* job = allocate_job(std::move(task), counter);
    queues_[static_cast<std::size_t>(t_queue_index)]->push(job);
}

void JobSystem::wait(Counter& counter) {
    RIME_ASSERT(t_queue_index >= 0);
    // Help run jobs (ours first, then stolen) until the group is done, rather than blocking — so
    // the calling thread is a full participant and cannot deadlock waiting on a busy pool.
    while (counter.load(std::memory_order_acquire) > 0) {
        Job* job = get_job(t_queue_index);
        if (job != nullptr) {
            execute(job);
        } else {
            std::this_thread::yield();
        }
    }
}

Job* JobSystem::get_job(int queue_index) noexcept {
    // Fast path: take our own most-recent job (LIFO, cache-hot).
    auto mine = queues_[static_cast<std::size_t>(queue_index)]->pop();
    if (mine.status == DequeStatus::Success) {
        return mine.value;
    }
    // Otherwise steal one job from a random other queue. Abort (a lost race) just yields nullptr;
    // the caller loops, so we naturally retry against a possibly different victim next time.
    const int num_queues = static_cast<int>(queues_.size());
    if (num_queues > 1) {
        const int victim = pick_victim(queue_index, num_queues);
        auto stolen = queues_[static_cast<std::size_t>(victim)]->steal();
        if (stolen.status == DequeStatus::Success) {
            return stolen.value;
        }
    }
    return nullptr;
}

void JobSystem::execute(Job* job) {
    job->fn();
    if (job->counter != nullptr) {
        // acq_rel so the chain of decrements carries every job's writes through to the waiter's
        // acquire-load of zero (see docs/design/job-system.md, "visibility").
        job->counter->fetch_sub(1, std::memory_order_acq_rel);
    }
}

void JobSystem::worker_main(int queue_index) {
    t_queue_index = queue_index;
    int idle_spins = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        Job* job = get_job(queue_index);
        if (job != nullptr) {
            execute(job);
            idle_spins = 0;
        } else {
            // Back off when there's no work so idle workers don't burn a core at 100%. Parking on
            // a condition variable is the better long-term answer (a labeled later optimization).
            if (++idle_spins < 64) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }
}

} // namespace rime::core
