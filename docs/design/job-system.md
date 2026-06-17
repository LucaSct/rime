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

## Job allocation — a per-thread ring

Allocating a job must be cheap (it happens thousands of times per frame), so each submitting thread
keeps a fixed **ring** of `Job` storage (`kJobRingSize = 16384`); allocating is a pointer bump and
a wrap. A ring slot is safe to reuse once the job that lived there has finished — and callers
`wait()` (drain) before the head wraps around — so as long as a single fork/join group has fewer
than `kJobRingSize` jobs in flight, no live job is ever overwritten. The `Job` itself holds a
`std::function` (the work) and the group counter; the deques only ever store `Job*`, which is
trivially copyable as the deque requires. Publication is safe across threads because the deque's
release/acquire pair orders the job's fields (written before `push`) ahead of a thief's read.

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
- **Bounded in-flight jobs per thread** (`kJobRingSize`). A growable/generation-checked job pool is
  the refinement if a single group ever needs more.

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
