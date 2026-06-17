// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.6b (work-stealing job system). parallel_for partitions a range so every index runs
// exactly once and its results match a serial computation; run()+wait() joins a group of tasks via
// a counter; an empty range is a no-op; and nested parallelism (a job that forks more jobs) joins
// correctly. As with the deque test, doctest macros stay on the main thread — jobs touch only
// atomics/preallocated storage, and we assert after the join.

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <vector>

#include "rime/core/jobs.hpp"

using namespace rime::core;

TEST_CASE("a freshly built system reports a sane participant count") {
    JobSystem jobs;
    CHECK(jobs.worker_count() >= 1);
    CHECK(jobs.participant_count() == jobs.worker_count() + 1);
}

TEST_CASE("parallel_for runs every index exactly once") {
    JobSystem jobs;
    constexpr std::size_t n = 100003; // not a multiple of the chunk size -> exercises the remainder
    std::vector<std::atomic<int>> visits(n);
    for (auto& v : visits) {
        v.store(0, std::memory_order_relaxed);
    }
    jobs.parallel_for(
        n, 256, [&](std::size_t i) { visits[i].fetch_add(1, std::memory_order_relaxed); });

    bool each_once = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (visits[i].load(std::memory_order_relaxed) != 1) {
            each_once = false;
            break;
        }
    }
    CHECK(each_once);
}

TEST_CASE("parallel_for results match a serial computation") {
    JobSystem jobs;
    constexpr std::size_t n = 50000;
    auto f = [](std::size_t i) { return static_cast<double>(i) * 1.5 - 3.0; };

    std::vector<double> serial(n);
    std::vector<double> parallel(n);
    for (std::size_t i = 0; i < n; ++i) {
        serial[i] = f(i);
    }
    jobs.parallel_for(n, 128, [&](std::size_t i) { parallel[i] = f(i); });

    bool equal = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (parallel[i] != serial[i]) {
            equal = false;
            break;
        }
    }
    CHECK(equal);
}

TEST_CASE("run + wait joins a group of independent tasks") {
    JobSystem jobs;
    constexpr int task_count = 2000;
    std::atomic<int> done{0};
    JobSystem::Counter counter{0};
    for (int i = 0; i < task_count; ++i) {
        jobs.run([&done] { done.fetch_add(1, std::memory_order_relaxed); }, &counter);
    }
    jobs.wait(counter);
    CHECK(done.load() == task_count);
    CHECK(counter.load() == 0); // fully joined
}

TEST_CASE("parallel_for with zero count does nothing") {
    JobSystem jobs;
    std::atomic<int> calls{0};
    jobs.parallel_for(0, 16, [&](std::size_t) { calls.fetch_add(1, std::memory_order_relaxed); });
    CHECK(calls.load() == 0);
}

TEST_CASE("nested parallel_for (a job that forks more jobs) joins correctly") {
    JobSystem jobs;
    constexpr std::size_t outer = 16;
    constexpr std::size_t inner = 1000;
    std::vector<std::atomic<long long>> partial(outer);
    for (auto& p : partial) {
        p.store(0, std::memory_order_relaxed);
    }

    jobs.parallel_for(outer, 1, [&](std::size_t o) {
        std::atomic<long long> inner_sum{0};
        // Fork a second level of jobs from inside a running job, and join it.
        jobs.parallel_for(inner, 64, [&](std::size_t k) {
            inner_sum.fetch_add(static_cast<long long>(o * inner + k), std::memory_order_relaxed);
        });
        partial[o].store(inner_sum.load(std::memory_order_relaxed), std::memory_order_relaxed);
    });

    long long total = 0;
    for (auto& p : partial) {
        total += p.load(std::memory_order_relaxed);
    }
    long long expected = 0;
    for (long long v = 0; v < static_cast<long long>(outer * inner); ++v) {
        expected += v;
    }
    CHECK(total == expected);
}
