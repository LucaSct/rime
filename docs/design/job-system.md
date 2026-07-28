# Work-stealing job system — design note (M1.6b)

Companion to `engine/core/include/rime/core/jobs/job_system.hpp`. Builds directly on the
Chase-Lev deque ([work-stealing-deque.md](work-stealing-deque.md)). This is the engine's
mechanism for using every core; the renderer, ECS, and physics will all express their parallelism
through it rather than spawning threads themselves.

## The model

There are `P` worker threads, each owning one Chase-Lev deque. A worker:

1. runs jobs from **its own** deque, taking the most recent first (LIFO) — cache-hot and
   contention-free; and
2. when its deque is empty, becomes a **thief** and steals the *oldest* job from a *random* other
   deque (FIFO on that end).

LIFO-for-self / FIFO-for-steal is the classic work-stealing choice: your own newest job is the
hottest in cache, while the oldest job in someone else's deque is usually nearer the root of the
fork/join tree, so stealing it tends to hand the thief a big subtree of work — minimizing how
often anyone has to steal. Random victim selection spreads steal traffic so the queues don't all
converge on one poor victim.

There is **no central queue**, so there is no single point everything contends on — the whole
reason work stealing scales.

### The submitting thread is a participant

The thread that creates the `JobSystem` (the game's main thread) also gets a deque — the last
one. `run()` pushes there; `wait()` makes that thread **help execute jobs** instead of sleeping.
So during a parallel region the main thread is a full worker, and `workers + main == core count`
(the default picks `hardware_concurrency() - 1` workers for exactly this reason). It also means
`wait()` can never deadlock by blocking a thread the work needs: the waiter always pitches in.

This keeps the Chase-Lev single-owner rule intact — every deque has exactly one thread that
pushes/pops its bottom (a worker, or the main thread for its own deque); all other access is
`steal()`.

## Fork / join

Parallelism is expressed with a `Counter` (`std::atomic<int>`):

- `run(task, &counter)` increments the counter and pushes the job.
- When a job finishes, the system decrements the counter.
- `wait(counter)` runs jobs until the counter hits zero.

`parallel_for(count, chunk, body)` is the ergonomic front door: it splits `[0, count)` into
chunk-sized jobs sharing one counter and waits before returning — so `body` may capture locals by
reference. Nesting works: a running job may call `parallel_for`/`run`/`wait` again (it pushes to
*its* worker's deque and helps drain), which is how a parallel system can parallelize its own
sub-steps. (Tested: a job that forks a second level and joins.)

### Visibility — why the counter orderings are what they are

A subtle correctness point. Each job's completion does `counter.fetch_sub(1, acq_rel)` and the
waiter does `counter.load(acquire)` in a loop. The **acq_rel** on every decrement is load-bearing:
fetch_sub is a read-modify-write, so the decrements form a *release sequence* on the counter, and
because each one also *acquires* the previous value, each job sees the writes of the job that
decremented before it. The final decrement (to zero) therefore carries — transitively — the writes
of **all** the jobs, and the waiter's acquire-load of zero synchronizes with it. Net effect: when
`wait()` returns, every result produced by the joined jobs is visible to the caller. Increments use
`relaxed`: in a fork/join group all `run()` calls are sequenced-before the `wait()` on the same
thread, so nothing stronger is needed.

## Job allocation — a segmented per-thread ring

Allocating a job must be cheap (it happens thousands of times per frame), so each submitting thread
keeps a **ring** of `Job` storage (`kJobSegmentSize = 16384` slots per segment); allocating is a
pointer bump and a lap. The `Job` itself holds a `std::function` (the work), the group counter, and
a `live` flag; the deques only ever store `Job*`, which is trivially copyable as the deque
requires. Publication is safe across threads because the deque's release/acquire pair orders the
job's fields (written before `push`) ahead of a thief's read.

The subtle part is when a slot may be handed out **again**. Because the deques hold raw `Job*`,
reusing a slot whose job is still queued or still running corrupts a live job in place: the
assignment to `fn` races with the call to it, and the overwritten job's work is silently dropped
while its counter increment stands — so the replacement runs twice and `wait()` can hang or return
early. This was originally left as an unchecked *assumption* ("callers join before the head wraps"),
which any single fork/join group larger than the ring quietly violates. Two ideas make it safe, and
both are load-bearing:

1. **A per-slot `live` flag**, set on claim and cleared by whichever thread *executes* the job.
   Because jobs are stolen, "is this slot free again?" is inherently a cross-thread question. A
   count of completed jobs cannot substitute for the per-slot check: jobs finish out of order
   (own-deque pops are LIFO, steals are FIFO), so "fewer than N outstanding" does not prove that
   the *specific* slot the head landed on is one of the finished ones.
2. **Grow rather than wait** when the head lands on a live slot. Blocking until the slot frees is
   the tempting alternative and it deadlocks: two threads can each be executing a job that occupies
   a slot in the *other* thread's ring, and if both lap and wait, each waits for a job only the
   other can finish. Helping cannot break the cycle, because the slot occupants are executing, not
   stealable. Growing is wait-free, so nested `run()` from inside a running job stays trivially
   re-entrant.

Segments give growth **stable addresses**: segment bodies are heap-allocated once and never move,
so a `Job*` already sitting in a deque stays valid for the life of the owning thread. Growth
appends; nothing is relocated (a flat `std::vector<Job>` could not grow at all here — reallocation
would invalidate every outstanding pointer). Capacity settles at the thread's high-water mark of
simultaneously in-flight jobs and is reused from then on, so steady-state frame code allocates
nothing.

### The ordering protocol

Three accesses to `live`, and the retire/reuse pair is what makes reuse safe:

- **claim** — `store(true, relaxed)` by the owner. Only the owner ever stores `true`, and `push()`'s
  release fence orders it before the pointer is published, so the claim precedes any retire in the
  modification order and a retire can never be lost.
- **retire** — `store(false, release)` by whichever thread executed the job, *after* `fn()` returns
  and *after* `counter` has been copied into a local (past the store the slot may already have been
  reused, so the `Job` must not be touched again). It runs **before** the group-counter decrement,
  which buys a useful invariant: `wait()` observing zero implies every slot in that group is already
  reusable.
- **reuse** — `load(acquire)` by the owner, which synchronizes-with that release, so the executor's
  accesses happen-before the owner overwrites `fn`. The pair lives on the slot itself rather than a
  bare fence so ThreadSanitizer can follow the edge — the same reasoning
  [work-stealing-deque.md](work-stealing-deque.md) gives for its slot ordering.

Slots are `alignas(64)` — one per cache line. Ring slots are the one place in this system where
unrelated threads touch neighbouring objects by construction: retiring a job stores to `live` from
whichever worker ran it, and without the alignment that store invalidates the line a *different*
worker may be reading the next slot's callable from. That is false sharing directly on the retire
path, paid once per job, so the alignment is bought rather than deferred; it costs ~1 MiB per
16384-slot segment.

Unbounded growth is a different bug from a legitimate burst: it means `run()` without a matching
`wait()`, a submission leak. `kMaxJobSegments` (256 segments ≈ 4M in-flight jobs from one thread)
trips a `RIME_ASSERT` well past any sane frame, so the engine bug is caught in checked builds while
unchecked builds degrade to memory pressure rather than corruption.

## Deliberate limitations (labeled, per CLAUDE.md)

- **Idle workers back off, they don't park.** A worker with no work yields, then sleeps briefly.
  This avoids pegging a core at 100% when idle but still wastes a little power; **condition-variable
  parking** (wake on submit) is the better long-term design and a clear next optimization — to be
  added with measurement.
- **`std::function` jobs.** Convenient and flexible, but each job pays type-erasure (and possibly a
  heap allocation for large captures). A fixed-size inline payload + function pointer is the
  data-oriented upgrade once we have real frame workloads to measure against.
- **One submitting thread.** Submit from the creating thread or from within a job. Arbitrary
  external threads submitting would need their own deques (or an MPMC intake queue). Sufficient for
  a main-loop engine; revisit if needed.
- **Job-ring memory is high-water-mark, never returned.** The segmented ring grows to the largest
  number of jobs a thread ever had in flight at once and keeps that memory for the thread's
  lifetime. That is the right trade for frame code (no allocation in steady state), but a tool that
  submits one enormous batch and then idles holds the segments anyway. Returning segments would
  need a quiescent point to prove no `Job*` is outstanding; not worth it until a workload asks.
- **Growth is per-segment, not per-slot.** If the head lands on a live slot the ring appends a whole
  16384-slot segment rather than hunting for the next free slot, so a thread that stays near
  saturation can hold more slots than its true peak. Scanning for a free slot would be a
  micro-optimization on a cold path; measure before adding it.

## The proof (M1.6 "done when")

`samples/jobs_core_saturation` computes a CPU-heavy kernel over 4M items serially and via
`parallel_for`, checks the two results are identical, and prints the speedup. On an N-core machine
the speedup trends toward N — visible evidence the pool keeps the cores busy and balances the load
by stealing. The unit tests (`tests/core/jobs_test.cpp`) prove each index runs exactly once,
results match serial, counters fully join, and nested fork/join is correct.

## Where this goes

Everything parallel in the engine routes through here: ECS systems running over archetypes (M4),
render-graph pass setup and command recording (M5), and the physics step (M7). Designing it early
(a roadmap "seam before features" bet) means those systems are written data-parallel from day one
rather than retrofitted. *Inspired by: Molecular Matters' job system, Naughty Dog's fiber-based
scheduler (fibers are a later option), and TBB/Cilk work stealing.*
