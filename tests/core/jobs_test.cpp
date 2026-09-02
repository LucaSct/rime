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
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

#include "rime/core/jobs.hpp"

using namespace rime::core;

namespace {

// The job ring is thread_local and lives as long as its thread, so anything that reasons about
// ring *geometry* — where the allocation head sits, how many segments exist — cannot run on
// doctest's main thread: every test case executed before it has already moved that thread's head
// and possibly grown its ring. Such a case would then either prove nothing (the head no longer
// laps where the test assumes) or fail purely because of test order.
//
// So the ring-geometry cases below run their whole scenario on a dedicated thread, which starts
// with an empty ring. The JobSystem is constructed *inside* that thread so the thread owns the
// submitting deque, and results come back through atomics: doctest macros stay on the main
// thread, as everywhere else in this file.
void on_a_fresh_ring_thread(const std::function<void()>& body) {
    std::thread runner(body);
    runner.join();
}

// A releaser used by the cases that deliberately keep jobs un-finished while they submit. It
// waits for the submitting thread to say it is done, and gives up after a deadline.
//
// The deadline is a DEADLOCK GUARD, not part of the choreography: it exists so that a future fix
// which made allocation *wait* for a busy slot would fail this test rather than hang CI forever.
// It is deliberately far longer than any legitimate submission — a short deadline would fire
// mid-submission on a slow (e.g. sanitizer) build, unblock the pinned jobs early, and let a
// broken implementation pass.
std::thread spawn_releaser(const std::atomic<bool>& submit_done, std::atomic<bool>& release) {
    return std::thread([&submit_done, &release] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!submit_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        release.store(true, std::memory_order_release);
    });
}

} // namespace

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

// Regression proof for the job-ring overwrite (M11 deferred finding #1). Job storage comes from a
// per-thread ring and the deques hold raw Job* into it, so lapping the ring while an earlier job
// is still live used to overwrite that job in place — silently dropping its work and, when it was
// already executing, racing on the std::function itself.
//
// Making that deterministic takes three things, all load-bearing:
//
//  1. A fresh ring (see on_a_fresh_ring_thread), so the head starts at slot 0 and the geometry
//     below is exact rather than whatever previous test cases left behind.
//  2. The pinned job must be QUEUED, not running. If it is already executing, the overwrite has no
//     observable effect: the running thread finishes the old body anyway and the replacement runs
//     too, so every count still adds up and only a sanitizer sees anything. So we first park every
//     worker; with no thief free and the submitting thread not yet in wait(), the job we push next
//     provably sits in its deque untouched for the whole lap.
//  3. The pinned job's body must be distinguishable from a filler's. If both merely bump one
//     counter, running a filler twice instead of the pinned job once lands on the same total.
//
// Geometry: the parked jobs take slots [0, workers), the pinned job takes slot `workers`, and the
// fillers run the head all the way around. Before the fix the ring wrapped unconditionally, so the
// last fillers landed back on slots 0, 1, … and evicted the pinned job — pinned_ran == 0 and
// filler_ran == ring + 1. After the fix the head finds slot 0 still live and grows instead, so no
// live job is ever touched.
TEST_CASE("lapping the job ring does not overwrite a live job") {
    const std::size_t ring = JobSystem::job_segment_size();
    std::atomic<int> pinned_ran{0};
    std::atomic<int> filler_ran{0};
    std::atomic<int> counter_left{-1};

    on_a_fresh_ring_thread([&] {
        JobSystem jobs(2);
        const unsigned workers = jobs.worker_count();

        std::atomic<bool> release{false};
        std::atomic<bool> submit_done{false};
        std::atomic<int> parked{0};
        JobSystem::Counter counter{0};

        // 1. Occupy every worker, so nothing submitted below can be stolen away.
        for (unsigned w = 0; w < workers; ++w) {
            jobs.run(
                [&] {
                    parked.fetch_add(1, std::memory_order_release);
                    while (!release.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                },
                &counter);
        }
        while (parked.load(std::memory_order_acquire) < static_cast<int>(workers)) {
            std::this_thread::yield();
        }

        // 2. The pinned job. Every worker is parked and this thread is not in wait(), so it stays
        //    queued — its ring slot is live for the whole lap that follows.
        jobs.run([&] { pinned_ran.fetch_add(1, std::memory_order_relaxed); }, &counter);

        std::thread releaser = spawn_releaser(submit_done, release);

        // 3. Lap the ring.
        for (std::size_t i = 0; i < ring; ++i) {
            jobs.run([&] { filler_ran.fetch_add(1, std::memory_order_relaxed); }, &counter);
        }
        submit_done.store(true, std::memory_order_release);

        jobs.wait(counter);
        releaser.join();
        counter_left.store(counter.load(), std::memory_order_release);
    });

    // The pinned job must not have been evicted by a filler lapping onto its slot.
    CHECK(pinned_ran.load() == 1);
    CHECK(filler_ran.load() == static_cast<int>(ring));
    CHECK(counter_left.load() == 0);
}

// Growth must actually happen when it is needed. Gating every body on a flag that is only released
// after submission finishes keeps all n jobs simultaneously in flight, so the ring cannot recycle
// its way through them and must grow to hold them all.
//
// Without the gate this proves much less than it looks: with trivial bodies the workers retire
// slots faster than the submitting thread can lap onto them, so the head keeps finding free slots,
// growth never triggers, and the case passes while exercising only wrap-reuse.
TEST_CASE("a group larger than the ring grows it and still runs every index exactly once") {
    const std::size_t segment = JobSystem::job_segment_size();
    const std::size_t n = 3 * segment + 7;
    std::vector<std::atomic<int>> visits(n);
    for (auto& v : visits) {
        v.store(0, std::memory_order_relaxed);
    }
    std::atomic<std::size_t> segments{0};

    on_a_fresh_ring_thread([&] {
        JobSystem jobs;
        std::atomic<bool> release{false};
        std::atomic<bool> submit_done{false};
        JobSystem::Counter counter{0};

        std::thread releaser = spawn_releaser(submit_done, release);

        for (std::size_t i = 0; i < n; ++i) {
            jobs.run(
                [&, i] {
                    while (!release.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    visits[i].fetch_add(1, std::memory_order_relaxed);
                },
                &counter);
        }
        submit_done.store(true, std::memory_order_release);

        jobs.wait(counter);
        releaser.join();
        segments.store(JobSystem::job_segment_count(), std::memory_order_release);
    });

    bool each_once = true;
    for (auto& v : visits) {
        if (v.load(std::memory_order_relaxed) != 1) {
            each_once = false;
            break;
        }
    }
    CHECK(each_once);
    // Every job is in flight at once, and the ring grows a whole segment at a time, so it must end
    // up with exactly enough segments to hold them all.
    CHECK(segments.load() == (n + segment - 1) / segment);
}

// The other half of the contract: growth must not be a one-way ratchet. Each wave is exactly one
// segment and is fully joined before the next starts, so every slot has been retired by the time
// the head laps back onto it and the ring must reuse rather than grow. This is the deterministic
// retire-then-reuse path — the acquire/release handshake with no growth papering over it — and it
// is what keeps steady-state frame code allocation-free.
TEST_CASE("sequential fork/join groups reuse retired slots instead of growing") {
    const std::size_t group = JobSystem::job_segment_size();
    std::atomic<std::size_t> total{0};
    std::atomic<std::size_t> segments{0};

    on_a_fresh_ring_thread([&] {
        JobSystem jobs;
        for (int wave = 0; wave < 3; ++wave) {
            jobs.parallel_for(
                group, 1, [&](std::size_t) { total.fetch_add(1, std::memory_order_relaxed); });
        }
        segments.store(JobSystem::job_segment_count(), std::memory_order_release);
    });

    CHECK(total.load() == 3 * group);
    // wait() returning implies every slot in the group was retired (a slot is freed before its
    // group counter is dropped), so waves 2 and 3 must land back in wave 1's single segment.
    CHECK(segments.load() == 1);
}

// Re-entrant overflow: each outer job forks more than a segment of inner jobs, and those inner
// submissions run on whichever thread picked the outer job up — a worker that stole it, or the main
// thread helping inside wait(). So allocation overflows a ring from inside a running job, on more
// than one thread at once. Against the unfixed ring this is the harshest of these cases: it
// segfaults outright, calling a std::function that was destroyed underneath the thread running it.
//
// What it does NOT prove is the deadlock argument for growing rather than waiting. That needs two
// threads each executing a job holding a slot in the *other's* ring, which depends on who steals
// what and cannot be forced from here. It stands as a guard that re-entrant overflow completes at
// all — the property a blocking design would break.
TEST_CASE("nested submissions may overflow a ring without deadlocking") {
    JobSystem jobs;
    const std::size_t outer = jobs.participant_count();
    const std::size_t inner = JobSystem::job_segment_size() + 100;

    std::atomic<std::size_t> total{0};
    jobs.parallel_for(outer, 1, [&](std::size_t) {
        jobs.parallel_for(
            inner, 1, [&](std::size_t) { total.fetch_add(1, std::memory_order_relaxed); });
    });

    CHECK(total.load() == outer * inner);
}

TEST_CASE("a second JobSystem on the same thread does not hijack the first one's deque") {
    // The queue index used to be one `thread_local int` shared by every JobSystem on the thread,
    // overwritten by each constructor. The header's own comment predicted the failure and asked
    // for it to be revisited "before anything nests pools"; the editor host then began building
    // three of them. After the second construction, every main-thread submission into the FIRST
    // system pushed into and popped from one of that system's WORKER deques — two threads acting
    // as the Chase-Lev owner of one deque, which is outside what the algorithm guarantees. The
    // bounds assert could not see it: with enough cores the wrong index is still in range.
    //
    // The observable symptom is a job that is never joined (the group counter never reaches zero,
    // so wait() hangs) or one executed twice. This case reproduces the editor host's sequence:
    // build A, submit through it, build B, then keep submitting through A.
    constexpr std::size_t kItems = 2048;

    JobSystem a(3);
    std::vector<std::atomic<int>> visits(kItems);
    for (auto& v : visits) {
        v.store(0, std::memory_order_relaxed);
    }

    // Baseline: A alone works.
    a.parallel_for(kItems, 32, [&](std::size_t i) { visits[i].fetch_add(1); });
    for (std::size_t i = 0; i < kItems; ++i) {
        REQUIRE(visits[i].load() == 1);
    }

    {
        // B is constructed on this same thread, exactly as the editor host constructs its asset
        // pool while the Application's pool is alive. B has a DIFFERENT worker count, which is
        // what made the stale index land in range but on the wrong deque.
        JobSystem b(2);
        b.parallel_for(64, 8, [](std::size_t) {}); // B is real and usable too

        // ...and A must still work while B is alive. Before the fix this is where a wave went
        // missing: the submission landed in one of A's worker deques.
        for (auto& v : visits) {
            v.store(0, std::memory_order_relaxed);
        }
        a.parallel_for(kItems, 32, [&](std::size_t i) { visits[i].fetch_add(1); });
        for (std::size_t i = 0; i < kItems; ++i) {
            CHECK(visits[i].load() == 1);
        }
    }

    // ...and after B is destroyed, A is still intact — the unbind must not have taken A's binding
    // with it.
    for (auto& v : visits) {
        v.store(0, std::memory_order_relaxed);
    }
    a.parallel_for(kItems, 32, [&](std::size_t i) { visits[i].fetch_add(1); });
    for (std::size_t i = 0; i < kItems; ++i) {
        CHECK(visits[i].load() == 1);
    }
}
