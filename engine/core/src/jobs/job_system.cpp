// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/jobs/job_system.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "rime/core/diagnostics/assert.hpp"

namespace rime::core {

// A job is just the work plus the group counter to drop when it finishes. The Chase-Lev deques
// hold Job* (trivially copyable); the Job objects themselves live in a per-thread ring (below).
//
// `live` is the ring's reuse interlock. It is set when the slot is handed out and cleared by
// whichever thread finishes the job — which is generally NOT the thread that allocated it, since
// jobs get stolen. So "is this slot free again?" is inherently a cross-thread question, and the
// flag has to be an atomic rather than a plain bool.
//
// One slot per cache line. Without the alignment two adjacent slots share a line, so retiring a
// job — a store to `live` by whichever worker ran it — invalidates the line a *different* worker
// may be reading a neighbouring job's callable from. That is false sharing directly on the retire
// path, which every single job pays. Ring slots are the one place in this system where unrelated
// threads touch neighbouring objects by construction, so the alignment is bought deliberately
// rather than deferred: the cost is rounding each slot up to 64 bytes, i.e. ~1 MiB per 16384-slot
// segment.
struct alignas(64) Job {
    std::function<void()> fn;
    JobSystem::Counter* counter = nullptr;
    std::atomic<bool> live{false};
};

namespace {

// Job storage: a SEGMENTED RING with a per-slot reclamation handshake.
//
// Each thread that submits jobs owns a ring of Job storage, so allocating a job is a pointer bump
// with no heap traffic on the hot path. The subtle part is when a slot may be handed out AGAIN.
// The deques hold raw Job* into this storage, so reusing a slot whose job is still queued or
// still running corrupts a live job in place: the assignment to `fn` races with the call to it,
// and the overwritten job's work is silently dropped while its counter increment stands (so the
// replacement runs twice and wait() can hang or return early). That reuse rule used to be an
// unchecked assumption — "callers join before the head wraps" — which a single fork/join group
// larger than the ring quietly violates.
//
// Two ideas make it safe, and both are load-bearing:
//
//   1. A per-slot `live` flag. Note that a count of completed jobs cannot substitute: jobs finish
//      out of order (own-deque pops are LIFO, steals are FIFO), so "fewer than N outstanding"
//      does not prove that the *specific* slot the head landed on is one of the finished ones.
//      The check has to be per-slot.
//   2. GROW rather than wait when the head lands on a live slot. Blocking until the slot frees is
//      the tempting alternative and it deadlocks: because jobs are stolen, two threads can each
//      be executing a job that occupies a slot in the OTHER thread's ring, and if both wrap and
//      wait, each waits for a job only the other can finish. Helping cannot break the cycle —
//      the slot occupants are executing, not stealable. Growing is wait-free, so nested run()
//      from inside a running job stays trivially re-entrant.
//
// Segments give growth stable addresses: bodies are heap-allocated once and never move, so a Job*
// already sitting in a deque stays valid for the life of the owning thread. Growth appends;
// nothing is relocated. Capacity therefore settles at the thread's high-water mark of
// simultaneously in-flight jobs and is reused from then on — steady-state frame code allocates
// nothing. (This is the classic arena/segmented-ring pattern, with the single `live` bit playing
// the role of deferred reclamation.)
//
// Memory ordering — one protocol, three accesses to `live`:
//   claim  — store(true, relaxed) by the owner. Only the owner ever stores true, and push()'s
//            release fence orders it before the pointer is published, so the claim precedes any
//            retire in the modification order and a retire can never be lost.
//   retire — store(false, release) by whichever thread executed the job, after fn() returns and
//            after `counter` has been copied out. It runs BEFORE the group-counter decrement, so
//            wait() observing zero implies every slot in the group is already reusable.
//   reuse  — load(acquire) by the owner, which synchronizes-with that release: the executor's
//            accesses happen-before the owner overwrites `fn`. The pair lives on the slot itself
//            rather than a bare fence so ThreadSanitizer can follow the edge — the same
//            reasoning the deque's slot ordering is written up with.
constexpr std::size_t kJobSegmentSize = 1u << 14; // 16384 jobs per segment (power of two)
// Growth is legitimate — a burst above the high-water mark. UNBOUNDED growth is not: it means
// run() without a matching wait(), i.e. a submission leak. Trip well past any sane frame so the
// engine bug is caught in checked builds, while unchecked builds degrade to memory pressure
// rather than corruption.
constexpr std::size_t kMaxJobSegments = 256; // 4M jobs in flight from one thread

// WHICH DEQUE THIS THREAD SUBMITS TO, PER JOB SYSTEM. A worker's index, or num_workers for the
// system's own submitting thread. Absent (-1) means "not a participant in THIS system" —
// submitting from such a thread is a contract violation.
//
// This used to be a bare `thread_local int`, shared by every JobSystem alive on the thread, and
// the constructor overwrote it. The header's own comment predicted the consequence and asked for
// it to be revisited "before anything nests pools" — and then the editor host began constructing
// three (engine/app/editor_host_app.cpp: the Application's own pool, a 2-worker asset pool, and a
// transient pool during play Stop). After the second construction, every main-thread submission
// into the FIRST system pushed into and popped from one of that system's WORKER deques, so two
// threads acted as the Chase-Lev owner of one deque. That is outside what any work-stealing proof
// covers: the algorithm is correct, its single-owner precondition was being violated from above.
// The bounds assert could not catch it — with hw >= 4 the wrong index is still in range, which is
// precisely the "in-range-yet-wrong" case the old comment named.
//
// A fixed-size array scanned linearly, rather than a map: a thread realistically binds to one or
// two systems, the lookup is on the submit path, and this costs no allocation and no hashing.
struct QueueBinding {
    std::uint64_t system_id = 0; // 0 == empty; ids start at 1
    int index = -1;
};

constexpr std::size_t kMaxBoundSystemsPerThread = 8;
thread_local std::array<QueueBinding, kMaxBoundSystemsPerThread> t_bindings{};

[[nodiscard]] int queue_index_for(std::uint64_t system_id) noexcept {
    for (const QueueBinding& b : t_bindings) {
        if (b.system_id == system_id) {
            return b.index;
        }
    }
    return -1; // this thread never bound to that system — see run()'s assert
}

void bind_queue(std::uint64_t system_id, int index) noexcept {
    for (QueueBinding& b : t_bindings) {
        if (b.system_id == system_id || b.system_id == 0) {
            b.system_id = system_id;
            b.index = index;
            return;
        }
    }
    // More live job systems on one thread than this array holds. Raising the bound is trivial;
    // needing to is a design smell worth seeing.
    RIME_ASSERT(false && "too many JobSystems bound on one thread");
}

void unbind_queue(std::uint64_t system_id) noexcept {
    for (QueueBinding& b : t_bindings) {
        if (b.system_id == system_id) {
            b = QueueBinding{};
            return;
        }
    }
}

// Segment bodies are unique_ptr-owned arrays: Job holds an atomic, so it is neither copyable nor
// movable, which rules out a flat std::vector<Job> (resize needs MoveInsertable) — and a flat
// vector could not grow anyway without invalidating every Job* already in a deque.
thread_local std::vector<std::unique_ptr<std::array<Job, kJobSegmentSize>>> t_segments;
thread_local std::size_t t_head = 0;     // circular index into [0, t_capacity)
thread_local std::size_t t_capacity = 0; // t_segments.size() * kJobSegmentSize
thread_local std::uint64_t t_rng = 0;

// Power-of-two segment size, so the compiler turns these into a shift and a mask.
Job& slot_at(std::size_t index) {
    return (*t_segments[index / kJobSegmentSize])[index % kJobSegmentSize];
}

// Append a segment and point the head at its first slot, which is always free. Existing slots keep
// their addresses; slots the redirect skips are picked up on the head's next lap.
void grow_ring() {
    t_head = t_capacity;
    t_segments.push_back(std::make_unique<std::array<Job, kJobSegmentSize>>());
    t_capacity += kJobSegmentSize;
    RIME_ASSERT(t_segments.size() <= kMaxJobSegments); // submission leak if this ever fires
}

Job* allocate_job(std::function<void()> fn, JobSystem::Counter* counter) {
    if (t_head == t_capacity) {
        t_head = 0; // lap the ring (also the first-call case, where head == capacity == 0)
    }
    if (t_capacity == 0 || slot_at(t_head).live.load(std::memory_order_acquire)) {
        grow_ring(); // still queued or executing somewhere: never overwrite live work
    }
    Job& job = slot_at(t_head);
    ++t_head;
    job.fn = std::move(fn);
    job.counter = counter;
    job.live.store(true, std::memory_order_relaxed); // ordered by push()'s release fence
    return &job;
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

std::size_t JobSystem::job_segment_size() noexcept {
    return kJobSegmentSize;
}

std::size_t JobSystem::job_segment_count() noexcept {
    return t_segments.size();
}

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

    // A process-unique id, taken before any worker starts so worker_main can bind against it.
    static std::atomic<std::uint64_t> s_next_id{1};
    id_ = s_next_id.fetch_add(1, std::memory_order_relaxed);

    // This (constructing) thread owns the last queue; it submits here and helps drain via wait().
    // Bound per-system, so constructing a second JobSystem on this thread no longer repoints this
    // one's submissions at a worker's deque.
    bind_queue(id_, static_cast<int>(num_workers_));

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
    // Release this thread's binding. Worker threads' bindings die with their TLS when they exit.
    unbind_queue(id_);
}

void JobSystem::run(std::function<void()> task, Counter* counter) {
    // The index is looked up for THIS system: submit from the thread that constructed it, or from
    // within one of its own running jobs. A thread that never bound to this system gets -1 rather
    // than silently borrowing whatever index another system left behind.
    const int queue_index = queue_index_for(id_);
    RIME_ASSERT(queue_index >= 0); // submit from the main thread or from within a running job
    RIME_ASSERT(queue_index < static_cast<int>(queues_.size()));
    Job* job = allocate_job(std::move(task), counter);
    if (counter != nullptr) {
        // Bump AFTER allocating but BEFORE the push that makes the job visible. Allocation can now
        // grow the ring, and growing allocates — so incrementing first would strand the increment
        // if that ever threw, leaving wait() to spin on a counter no job will ever pay back.
        // Nothing can decrement in the gap, because no executor can see the job until it is pushed.
        // In a fork/join group all run() calls happen-before the wait() on the same thread, so a
        // relaxed increment is correctly ordered.
        counter->fetch_add(1, std::memory_order_relaxed);
    }
    queues_[static_cast<std::size_t>(queue_index)]->push(job);
}

void JobSystem::wait(Counter& counter) {
    const int queue_index = queue_index_for(id_);
    RIME_ASSERT(queue_index >= 0);
    // Help run jobs (ours first, then stolen) until the group is done, rather than blocking — so
    // the calling thread is a full participant and cannot deadlock waiting on a busy pool.
    while (counter.load(std::memory_order_acquire) > 0) {
        Job* job = get_job(queue_index);
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
    Counter* counter = job->counter; // copy out BEFORE publishing the slot as free
    // Retire the ring slot: the release store orders fn() and the counter read ahead of the
    // "free" signal, so the owning thread's acquire-load makes them happen-before its overwrite.
    // Past this point the slot may already have been reused — do not touch `job` again. Retiring
    // before the counter decrement means wait() seeing zero implies every slot in the group is
    // reusable.
    job->live.store(false, std::memory_order_release);
    if (counter != nullptr) {
        // acq_rel so the chain of decrements carries every job's writes through to the waiter's
        // acquire-load of zero (see docs/design/job-system.md, "visibility").
        counter->fetch_sub(1, std::memory_order_acq_rel);
    }
}

void JobSystem::worker_main(int queue_index) {
    bind_queue(id_, queue_index);
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
