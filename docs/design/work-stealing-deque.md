# Chase-Lev work-stealing deque — design note (M1.6a)

Companion to `engine/core/include/rime/core/jobs/chase_lev_deque.hpp`. This is the most
ordering-sensitive code in the engine so far, so the *why* of every atomic and fence is written
out here. The job system (M1.6b) gives each worker thread one of these.

## Work stealing in one paragraph

Each worker owns a deque of jobs. It runs its own jobs from the **bottom** like a stack (LIFO):
the most recently spawned job is usually the hottest in cache, and — crucially — the owner's
push/pop touch only its own end, so in the common case they are *uncontended*. When a worker runs
dry, it becomes a **thief** and steals a job from the **top** of some other worker's deque (FIFO
on that end). Stealing the *oldest* job tends to grab a coarse task near the root of the
fork/join tree, which yields more future work and so fewer steals. The deque is therefore
deliberately **asymmetric**: one owner on the bottom, many thieves on the top. Synchronization is
paid only when a thief actually races the owner — which is exactly when there is contention to
manage.

## Structure

```
top  ──▶ [ . . J J J J J . . ]  ◀── bottom
         thieves steal here   owner push/pop here
```

- `bottom_` (atomic int64): one past the last element; the owner's end.
- `top_` (atomic int64): the next element to steal; thieves advance it with CAS.
- `array_` (atomic pointer): a **circular buffer** of `2^k` atomic slots; index `i` maps to
  `slots[i & (cap-1)]`. Indices grow monotonically (never reused), so there is no ABA problem on
  `top_`/`bottom_`.

Indices are **signed** because `pop()` transiently does `bottom_-- ` which can dip below `top_`
on an empty deque; the comparison `top <= bottom` then correctly reports empty.

## The operations and their orderings

The orderings below are from Lê, Pop, Cohen & Nardelli (2013), which fixed the original 2005
paper for weak memory models (ARM/POWER). They are not interchangeable with "just use seq_cst
everywhere" (slower) or "relaxed everywhere" (wrong). Each one earns its place.

### `push` (owner only)

```
b = bottom (relaxed);  t = top (acquire);  buf = array (relaxed)
if full: buf = grow(); array.store(buf, release)
buf.put(b, item)                 // relaxed store into the slot
fence(release)                   // (A)
bottom.store(b+1, relaxed)       // (B)
```

The **release fence (A)** before publishing the new `bottom` (B) guarantees that any thief which
later observes the incremented bottom also observes the slot write — otherwise a thief could read
the index as in-range and then load a *garbage* slot. (A)+(B) is the producer half of the
happens-before with `steal`'s acquire loads.

### `pop` (owner only — take from bottom, LIFO)

```
b = bottom (relaxed) - 1
buf = array (relaxed)
bottom.store(b, relaxed)         // claim the bottom slot tentatively
fence(seq_cst)                   // (C) — the crux
t = top (relaxed)
if t <= b:                       // non-empty
    x = buf.get(b)
    if t == b:                   // exactly one element left
        if !top.CAS(t -> t+1, seq_cst): x = EMPTY   // a thief beat us to it
        bottom.store(b+1)        // deque now empty
    return x
else:                            // empty
    bottom.store(b+1)            // restore
    return EMPTY
```

The **seq-cst fence (C)** is the heart of the algorithm. It orders the owner's `bottom--` against
a thief's `top++` so the two cannot both succeed on the **single last element**. Without a single
total order over those two seq-cst operations, both sides could read "one element present," both
take it, and the job runs twice. With (C) and the seq-cst CAS, exactly one wins; the loser sees
the effect of the winner.

### `steal` (any thief — take from top, FIFO)

```
t = top (acquire)
fence(seq_cst)                   // (D) — pairs with (C)
b = bottom (acquire)
if t < b:                        // non-empty
    buf = array (acquire)        // see the slot the owner released in push
    x = buf.get(t)               // read BEFORE claiming
    if !top.CAS(t -> t+1, seq_cst): return ABORT   // lost the race; retry
    return x
return EMPTY
```

The thief reads the value **before** the CAS: if it claimed the index first and then read, the
owner could overwrite the slot in between. `ABORT` (distinct from `EMPTY`) tells the scheduler the
deque may still have work — just try again or move on — versus genuinely empty. The **seq-cst
fence (D)** pairs with `pop`'s (C): together they give a consistent global view of the
`(top, bottom)` pair across owner and thief.

## Buffer growth and lifetime

When `push` finds the buffer full it allocates one of **double** size, copies the live range
`[t, b)` preserving each element's *logical* index, and publishes the new pointer with a release
store. Two subtleties:

1. **Index-preserving copy.** Element at logical index `i` lands at `i & (newcap-1)` in the new
   buffer but is the *same value* as `i & (oldcap-1)` in the old one. So a thief that loaded the
   *old* `array_` pointer just before the swap and reads index `i` from it still gets the correct
   value. Stale-buffer reads remain valid for any index in `[t, b)`.
2. **Retire, don't free.** The old buffer is kept alive (in `buffers_`, an owner-only vector of
   `unique_ptr`) until the deque is destroyed, precisely because an in-flight thief may still hold
   a pointer into it. Freeing it eagerly would be a use-after-free. This is the standard Chase-Lev
   "garbage buffer" approach; for a long-lived per-worker deque the retained buffers are bounded
   by the peak size and freed in the destructor.

## Correctness testing

The single-threaded tests pin LIFO/FIFO semantics and growth. The real proof is the concurrent
test: one owner pushing + popping while `max(2, hardware_concurrency)` thieves steal 100k items,
asserting **every item is consumed exactly once** — no losses (a broken happens-before drops a
slot write) and no duplicates (a broken last-element race). doctest's macros are not thread-safe,
so workers only touch atomics and all assertions run on the main thread after the join. The case
was run 40× under contention while developing. Note: x86 is TSO (strongly ordered), so it cannot
*by itself* exercise the weak-ordering bugs the seq-cst fences guard against — the orderings are
written to the proof, and the CI matrix broadens the hardware over time.

## Deliberate limitations (labeled, per CLAUDE.md)

- **No false-sharing padding yet.** `top_` and `bottom_` likely share a cache line, so owner and
  thieves ping-pong it. Padding to separate cache lines is a known win, deferred until the job
  system gives us something to *measure* (measure before optimizing).
- **Default-heap buffers.** Allocation uses `make_unique` (RAII), not the engine allocators —
  the same later seam noted for the slot map.
- **Single owner.** Push/pop are owner-only by contract; only `steal` is multi-thread. The job
  system enforces this by construction (one deque per worker).

## References

- D. Chase, Y. Lev, *Dynamic Circular Work-Stealing Deque*, SPAA 2005.
- N. M. Lê, A. Pop, A. Cohen, F. Zappa Nardelli, *Correct and Efficient Work-Stealing for Weak
  Memory Models*, PPoPP 2013 — the C11-atomics orderings implemented here.
