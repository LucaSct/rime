# Replication — what a peer holds, and how we know

The living design note for `engine/replication` and `engine/destruction_net`. The *decisions* live in
[ADR-0033](../adr/0033-networking-v1.md) and its amendments; this is the standing set of rules the
next diff has to keep satisfying. It is editable, not append-only — when a new mechanism enforces the
invariant below, add it to the table rather than writing another amendment.

Its sibling is [reliability.md](reliability.md), which does the same job one storey down for the
transport. This document exists because five bugs in two bricks turned out to be the same bug, and
nobody noticed until the fifth. The sixth was found *by this document* — see
[instance six](#instance-six-a-dead-slot-that-read-as-eternally-arriving), which is the argument for
keeping it current rather than tidy.

---

## The invariant

> **Any claim about what a peer holds needs evidence of *holding* — never evidence of some
> correlated-but-weaker event.**

That sentence alone is too vague to catch anything in review, which is exactly how this kept
recurring. It has two corollaries with genuinely different failure mechanics, and the second one is
the one that keeps getting missed.

### Corollary 1 — the scalar-watermark rule

A monotonic position (`AckTracker::watermark_`, `ClientState::acked_baseline`,
`ClientState::complete_through`) may advance to tick T only on evidence that the peer holds **every
fact attributed to T**.

- *"The bytes arrived"* is not that evidence. A packet can arrive whole, parse cleanly, and still
  have every record in it discarded (ADR-0033 **A13**).
- *"We sent it"* is not that evidence either, because sending can be **partial**. The packets that
  went out arrive and are honestly acked, while the ones the budget dropped were never transmitted
  at all.

Note that this needs **two independent clamps, one on each side of the wire** — the client refusing
to over-claim what it applied, and the server refusing to over-trust an honest ack against an
incomplete send. Fixing only one side leaves the other half of the bug in place, which is what
happened between A13 and the m11.5 foundation fix.

### Corollary 2 — the per-item record rule

A per-item record of what the peer has been told (`ClientState::was_relevant[]`,
`ClientState::announced[]`) may be strengthened only by evidence about **that exact item's own
transmission outcome**. Specifically it may never be:

- **inferred from a coarser proxy that has a blind spot.** "Did the value change since the baseline"
  cannot see *"never sent because it was filtered out"* — an entity entering a relevancy set has by
  definition not changed, so a pure filter loses it forever.
- **overwritten as a side effect of a different message kind's bookkeeping for the same slot.** A
  recycled index appears in both the despawn and spawn lists in one tick; a rollback keyed to the
  message kind rather than to the pre-tick value has one clobber the other.
- **left behind when the item it describes ceases to exist.** A record keyed by a recyclable index
  outlives its subject. Both directions are wrong: a stale *set* bit hands the next tenant a claim
  that it has been sent something it has not, and a stale *clear* bit describes a slot that no longer
  names anything — which, if the surrounding code reads "not held" as "about to be sent", is an
  entity eternally arriving and never arrived.

The two corollaries share a principle but not a shape, and a single sentence covering both ends up
too loose to catch corollary-2 bugs. Keep them named separately.

### What is *not* covered

Local presentation state is not a claim about a peer. The interpolation alpha (frame-time within a
tick) is a local number and is not subject to any of this — worth saying out loud so the rule does
not get over-applied to something that was never a cross-peer correctness question.

---

## Where it is enforced today

| Mechanism | File | Which corollary, and against what |
|---|---|---|
| `AckTracker` (complete-tick watermark) | `replication/snapshot.hpp` | 1 — a multi-part tick may not be acked on one part |
| Deferred-record hold + replay | `replication/src/client_replicator.cpp` | 1 — bytes that could not be applied are held, so the ack stays honest (A14) |
| `complete_through` clamp | `replication/src/server_replicator.cpp` | 1 — an honest ack may not advance the baseline past what was never sent |
| Rotation `cursor` | `replication/src/server_replicator.cpp` | 1 (liveness half) — the clamp makes the ack correct; this makes delivery actually happen |
| `was_relevant[]` + forced send on entry | `replication/src/server_replicator.cpp` | 2 — a version delta cannot see "never sent because filtered" |
| Pre-tick value in the `announced[]` rollback | `replication/src/server_replicator.cpp` | 2 — a kind-keyed rollback clobbers a recycled index's other entry |
| Per-batch verification in `apply_next_batch` | `destruction_net/src/destruction_client.cpp` | 2 — a fingerprint must be compared at ITS OWN fracture boundary, not against whatever state a catch-up burst ended on |
| `was_relevant[]` cleared in `despawn()` | `replication/src/server_replicator.cpp` | 2 — a per-item bit may not outlive the item and be inherited by the slot's next tenant |
| Dead slots score 0, not the live default | `replication/src/server_replicator.cpp` | 2 — "no policy scored it" must not read as "relevant"; see instance six |
| `composition_checks_unverified()` | `destruction_net/src/destruction_client.cpp` | neither — but see the counting rule below |
| `full_walk_ticks()` / `delta_ticks()` | `replication/src/server_replicator.cpp` | neither — the counting rule applied to a skip that stops happening |

---

## The counting rule

Every path that skips, drops, or defers something **gets a counter**. No exceptions, and it is worth
stating as a rule because the one place it was missed cost real proof strength.

The composition check in `destruction_net` had a silent `return` when its batch had already been
applied. The proof asserted `composition_matches() > 0` and `mismatches() == 0` — both true while
verifying almost nothing, because the skips were invisible. Adding the counter and asserting

```
matches + mismatches + unverified == checks_sent
```

immediately exposed **three further uncounted skip paths** that had been there all along. Two were
genuinely unverifiable and are now counted; the third — a catch-up loop discarding every batch's
expectations but the last — turned out to be fixable rather than merely countable, by verifying each
batch at the start of the *next* `apply_next_batch`, which is precisely the moment its own `update()`
has run. Counting it first is what made it visible enough to fix.

A proof that cannot see how much it skipped is not a weaker proof; it is a **misleading** one, because
it still reads as passing. When adding a counter feels like noise, that is the moment it is most
worth adding.

### The harder half: count the skips that *stop* happening

The rule above catches work that was silently dropped. Its mirror image is an optimization that
silently stops applying — and that one is worse, because there is no wrong output to notice. The bytes
on the wire are identical, the state still converges, every test still passes, and only the cost
moves.

`full_walk_ticks()` exists for exactly that. `publish_delta` gives up the per-chunk "changed since
baseline" skip on any tick where something enters a client's relevant set; the counter reports how
often that happened, and `delta_ticks()` gives it a denominator. A steady state should keep the ratio
near zero. **If it approaches 1.0, relevancy is thrashing and the "entries are rare and bursty"
assumption the trade rests on has stopped being true.**

Generalize it: when code takes a fast path *conditionally*, the condition going permanently false is
a defect that no correctness test can see. Count the condition, not just the work.

---

## Instance six: a dead slot that read as eternally arriving

Found while wiring m11.5's distance culling, by adding the counter above and watching it read 30 out
of 30 on a quiet world.

`publish_delta`'s entry test reads two slot-indexed arrays: `priority_by_index_`, which defaulted
**every** slot to relevant, and `was_relevant[]`, which the cull path clears. But the policy only ever
scores slots the `NetIdMap` still holds, and the allocator's slot vector never shrinks. So a slot that
was culled (`was_relevant = 0`) and then despawned (unscored, stuck at the relevant default) read
forever after as *relevant now, irrelevant last tick* — an entity permanently entering, which no
longer existed. Every subsequent tick became a full walk of every archetype, chunk and column, for
every client, for the life of the process.

Two things make this the same bug as the other five rather than a new one:

1. **A per-item record outlived its item** (corollary 2's third bullet, added for this).
2. **An absence was read as a positive claim.** "No policy scored this slot" is not evidence of
   relevance; it is evidence of nothing. Defaulting it to *relevant* turned missing information into
   an assertion — structurally the same move as treating "we sent it" as "they hold it".

Both halves are fixed: slots naming no live entity score 0, and `despawn()` clears `was_relevant` for
that index on every client. The second is currently *masked* — a freshly spawned entity always has a
fresh column write, so it leaves on the ordinary delta path rather than needing the entry path — but
masked is not fixed, and the previous five instances all survived as long as they did by being masked
by something.

**Why distance culling is what exposed it.** The defect needs a cull followed by a despawn. Before
m11.5 nothing was ever culled, so no `was_relevant` bit was ever 0 at despawn time and the whole
shape was unreachable. Debris are culled and despawned continuously by design, which turns a
formally-possible state into the steady state. Expect the same of the next mechanism that makes a
rare transition common.

---

## Deliberate limitations

- **The relevancy call still walks every replicated entity per client.** The policy can be cheap per
  entity, but nothing narrows the span yet, so the O(clients × entities) pass stands. Narrowing it
  needs a spatial index over replicated entities — its own brick.
- **Composition mismatch is detected, not repaired.** Repair needs a client→server request path or a
  periodic authoritative broadcast — late-join machinery.
- **Debris velocity is not replicated**, only transforms. The local solver's velocity is kept, which
  is a good estimate precisely because both peers launched the chunk from the same impulse.

---

## For m11.6 (interpolation) — build it to the rule the first time

1. **Store the previous/current pair as a component**, not a side-table keyed by `NetId::index`.
   A despawn+respawn recycle then gets fresh state for free from the ECS's own generation safety; a
   slot-keyed side table inherits corollary 2's risk directly — stale history from a dead incarnation
   bleeding into the entity that reused its slot.
2. **Drive the previous→current rotation off the same `mark_changed` signal the delta path uses**, not
   off "the tick counter advanced". `replay_deferred` marks a component changed at *replay* time,
   which can be many ticks after the packet nominally arrived; a rotation keyed to arrival would
   back-date or skip exactly those records.
3. **A newly-appearing entity must snap, not blend** — there is no previous. Structurally the same
   case as the relevancy-entry fix, so use the same pattern (an explicit "do I have a valid previous"
   flag) rather than reinventing it.
4. **The client→server input watermark is a fresh instance of corollary 1**, not a special case.
   Consumed ≠ arrived, consumed ≠ latest-received. If buffered input bursts ever need multi-part
   framing, re-derive the completeness discipline deliberately rather than assuming a small message
   never needs it — that assumption is what made the original baseline bug look like a non-issue
   until a wall that stops changing gave it a case where it mattered.
