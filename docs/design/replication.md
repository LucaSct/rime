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
| `credit_sent` per accepted part | `replication/src/server_replicator.cpp` | 2 — a refused `send_unreliable` is not evidence of holding |
| Byte-budget drops block `complete_through` | `replication/src/server_replicator.cpp` | 1 — a trimmed tick is partial, however complete it looks to the packer |
| `entry_pass_records()` / `delta_ticks()` | `replication/src/server_replicator.cpp` | neither — the counting rule, applied to relevancy churn |
| `records_too_large()` + the build-time size guard | `replication/src/server_replicator.cpp` | 1 — an undeliverable record must not jam the watermark, nor pass as delivered |
| Priority aging (`Budget::starvation_gain`) | `replication/src/server_replicator.cpp` | 1 (liveness half) — the rotation cursor's job on the *prioritized* path, which the cursor never reached |

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

This is where `full_walk_ticks()` came from. `publish_delta` used to give up the per-chunk "changed
since baseline" skip on any tick where something entered a client's relevant set, and that counter
reported how often. It earned its keep immediately — it read **30 out of 30** on a quiet world and
exposed instance six — and then it earned its keep a second way, by making the cost visible enough to
argue about, which is what led to the [entry pass](#the-entry-pass--how-newly-relevant-entities-are-sent)
removing the gate entirely.

The counter did not survive that, and should not have: with no gate left there is no widening to be
stuck. `entry_pass_records()` replaces it and counts the **real work** instead of the disabled
optimization. That is the better end state — a counter that measures something the code actually
does, rather than one watching for a flag to jam.

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
- **A parented entity with no `WorldTransform` cannot be located** by `distance_relevancy`, so it is
  treated as unmeasurable and always relevant. Resolving it would mean re-implementing
  `propagate_transforms` one entity at a time inside the policy. Give such an entity a
  `WorldTransform` instead.
- **Composition mismatch is detected, not repaired.** Repair needs a client→server request path or a
  periodic authoritative broadcast — late-join machinery.
- **Debris velocity is not replicated**, only transforms. The local solver's velocity is kept, which
  is a good estimate precisely because both peers launched the chunk from the same impulse.

---

## Priority aging — why an ordering needs a liveness rule

A budget plus a strict ordering is a starvation machine. `publish_delta` sorts by priority when a
relevancy policy is installed and rotates by the resume cursor when one is not — and those branches
were exclusive, so the cursor, which exists precisely to stop a permanently over-budget world
starving its tail, never ran on the path relevancy uses. The sort branch's own comment claimed it
did.

The consequence was not slow delivery but **no** delivery: the same highest-priority prefix went out
every tick and everything below the cut-off was never sent. Measured with 501 entities against a
per-tick allowance of ~180: **176 delivered, 325 permanently missing** — the same 176 the m11.5
foundation bug produced, because it is the same packet arithmetic underneath a different cause.

**The fix is to age the priority.** Each tick a record is built for a client and then dropped by a
budget, that entity's `starved_ticks` increments; the ordering key becomes
`priority + starvation_gain × starved_ticks`, and delivery resets the count to zero.

Why this rather than rotating the sorted list: rotation guarantees delivery but throws away what
priority is *for*, and the choice is not actually between fairness and locality. Aging keeps the
ordering nearest-first **among entities equally owed**, and makes being passed over itself a claim on
the next tick's budget. It also degrades correctly at both ends — with a budget that covers the
working set nothing is ever starved, every age is zero, and the order is exactly what the policy
asked for; with a budget that cannot, the tail rises instead of sinking.

The gain sets the worst-case wait. `distance_relevancy` scores in (0, 1] (2.0 unpositioned), so the
default 0.05 lets the least important entity in the world overtake the most important one after ~40
ticks — well under a second at 60 Hz. Setting it to zero restores strict priority, which is only
defensible when the budget is known to cover the working set.

`starved_ticks` is a per-item record keyed by a recyclable index, so it is cleared in `despawn()`
alongside `was_relevant`. It is a claim about what we OWE rather than what the peer HOLDS — not
corollary 2 proper — but the recycle hazard does not care which way the claim points: a new tenant
inheriting the dead entity's grievance would jump the queue on its behalf.

---

## A record that cannot fit is a schema fault, and must say so

An entity's record is **never split**. Parts are independently-complete packets, not fragments of one
logical message — losing one costs that packet's entities for that tick rather than the whole tick.
The consequence is easy to miss: an entity whose replicable components exceed the payload budget *on
their own* cannot be transmitted at all, by any amount of budget or patience.

That is a schema problem — split the component, or stop replicating it — and the engine's only job is
to **say so**. Both ways of not saying so were live in this module at some point:

- Build the oversized part and let the channel refuse it, while still counting the tick complete →
  the entity is **silently lost forever**.
- Build it, let it be refused, and honestly refuse to count the tick complete → `complete_through`
  **jams permanently**, the baseline never advances, and the whole world is re-sent every tick.
  Measured: `complete_through` stuck at 0 across 60 ticks.

Priority aging made the second worse rather than better, which is worth noting as a pattern: an
undeliverable record accumulates starvation, so it sorts *first* every tick and reserves a part
forever. A correct fix exposed a latent fault by removing what had been hiding it.

The guard drops such a record at build time and counts it in `records_too_large()`. The rest of the
world converges, the watermark advances, and the real fault is visible instead of being either
invisible or catastrophic. **`kHeaderBytes` is shared** between the packing loop and the guard on
purpose — those two disagreeing about the header size is exactly how an undeliverable record gets
built anyway.

A note on why this is easy to hit by accident: the reflection system has no array kind, only scalars
and nested structs, so an oversized component does not look oversized. Thirty-two transforms is 1280
bytes and reads as four fields.

**A consequence worth writing down, because it looks like missing test coverage and is not.** With
that guard in place, `Session::send_unreliable` can no longer fail from `publish_delta`: it refuses on
a non-Connected session (already filtered by `publish`) or a payload over
`ReliableChannel::kMaxPayload`, and every part is capped at `kMaxReplicationPayload`, which sits under
it with a deliberate reserve. A link-level failure cannot surface either — `ReliableChannel::transmit`
ignores `Link::send`'s result, which is right for an unreliable channel (a refused datagram is
indistinguishable from a lost one) but means the replicator never learns of it. The refused-part
handling is therefore honest handling of a documented contract that the current constants make
unreachable, not a gap wanting a `ScriptedNetwork` "refuse on demand" capability. Spend that reserve
and it becomes live and already correct.

---

## The entry pass — how newly-relevant entities are sent

An entity ENTERING a client's relevant set has, by definition, not changed since that client's
baseline. The chunk walk's "changed since baseline" test therefore cannot see it, and something has
to send it anyway.

**The design this replaced** gated the whole chunk walk on one global boolean: if anything anywhere
was entering, every column of every chunk of every replicated archetype was re-examined for that
client. Its cost was proportional to the size of the entire replicated world and it was triggered by
a single transition anywhere in it — a static 10,000-entity level walked in full because one distant
chunk of rubble drifted into range. The comment defending it claimed transitions are "rare and
bursty"; at the ADR's target (64 clients, ~1000 debris, debris and viewpoints both moving) the
chance that *some* pair crosses *some* radius on a tick approaches 1, so the flag was effectively
stuck on and the chunk-version skip — which the module header calls the whole reason this design
needs no history buffer — was off whenever relevancy was on.

**What is built instead:** a targeted pass over the candidates already being iterated for relevancy,
which serializes the full state of exactly those entities that are entering and pushes into the same
record arrays. Everything downstream (sort, budget, packetize, `credit_sent`) is agnostic about which
pass produced a record. The chunk walk goes back to being an unconditional version delta, and skips
any row the entry pass already emitted in full.

Cost is now O(what changed) + O(what is entering for this client), with no coupling to how much of
the rest of the world exists — which makes the "rare and bursty" assumption **unnecessary** rather
than better-founded. `entry_pass_records()` counts the real work; a settled world with a stationary
viewpoint holds it at zero, and a churning policy shows up as records rather than as a silently
disabled optimization.

The proof is `"entry work is proportional to what entered, not to how big the world is"`: a settled,
quiet 121-entity world, one entity brought into range, and **exactly one** entry record. That number
stays 1 as the world grows, which is the whole claim.

---

## Transform history (m11.6a) — built, and the two traps it hid

`PreviousTransform` + `interpolated_transform` in `replication/interpolation.hpp` are the buffer
ADR-0023 §3 left as a seam. Three rules shaped it: a **component** rather than a `NetId`-keyed side
table (a despawn+respawn recycle then gets fresh state from the ECS's own generation safety, instead
of inheriting corollary 2's risk — stale history from a dead incarnation bleeding into the entity
that reused its slot); rotated on the **apply** rather than on a tick boundary (`replay_deferred`
applies records many ticks after arrival, and a rotation keyed to the tick counter would back-date or
skip exactly those); and an explicit `valid` flag so a first appearance **snaps** instead of blending
out of the world origin — structurally the same case as the relevancy-entry send, solved the same
way rather than reinvented.

Two defects surfaced while proving it, both worth keeping because neither was visible by reading:

**A redundant re-send collapsed the blend.** The server re-sends until the baseline advances — a
round trip — so the same value routinely lands two ticks running. Rotating on those set
`previous := current` and destroyed a genuine gap, making the client snap through exactly the motion
interpolation exists to smooth. The rotation now fires only when the incoming value actually differs.

**The first version of that guard did nothing, silently.** It compared with `memcmp` over
`ecs::LocalTransform`. `core::Quat` is over-aligned, so `sizeof(Transform)` exceeds the 40 bytes it
packs into and the remainder is padding whose contents nothing defines — the comparison read it, never
matched, and the guard became a no-op that still compiled and still looked right. **Never `memcmp` a
struct with padding to decide whether a value changed**; compare fields, or compare the packed
serialization.

---

## Drawing the blend (m11.6b) — and the bug the consumer exposed in the producer

m11.6a computed a blend nothing could read. Three things stood between it and the screen, and only
the first was expected.

**`render` cannot call `replication`, and must not learn how.** `rime::render` links rhi/ecs/assets;
`rime::replication` links ecs/net. Neither depends on the other and neither should — a renderer that
knew about netcode is a layering mistake paid for at every future backend. They meet on the one
module below both: **`ecs::RenderTransform`**, a new unreflected component meaning *the pose to draw
this frame*. `replication::update_render_transforms(world, alpha)` deposits it once per frame from
the render callback; `extract_scene` prefers it over `WorldTransform` where present and falls back
where not, which costs a world that never replicates one null check.

The split is also what keeps the blend out of the simulation. `WorldTransform` stays the simulated
truth that physics and gameplay read; `alpha` is wall-clock-derived, so a tick that could see it
would stop being deterministic, and the m11.7 cross-peer proof rests on ticks being deterministic.
`Application` hands `alpha` out through exactly one channel (`FrameContext`), so the pass can only
be reached from the one place it is safe to reach it from — a sim stage could not call it correctly
even by mistake, because the parameter is not in a `TickFn`'s signature.

**A replicated entity was undrawable.** Not badly drawn — invisible. A mirror is spawned bare;
`WorldTransform` is unreflected so it never crosses the wire; and `propagate_transforms` only
touches entities that already have *both* transforms. So nothing ever gave a mirror a world pose and
every renderer query skipped it in silence. The replicator now adds one on the first transform
write, **seeded from that write** rather than left default: `bind_destructibles` prefers
`WorldTransform` over `LocalTransform` (`bind.cpp`'s `placement_of`), so a default would stand a
destructible at the world origin for any bind running before that tick's `propagate_transforms`. The
fallback that comment describes stops covering you the moment the component exists.

**`valid` also has to be turned OFF, and m11.6a never did.** This is the real find. `alpha` sweeps
0→1 every tick period on the frame clock's own schedule, whether or not a given entity received
anything. So a previous/current pair left valid after the motion stopped replays its last step
forever — the mirror snaps back and slides forward, once per tick, for as long as it stands still.
Debris coming to rest is the most common event in a destruction engine, so that is the steady state,
not a corner. `ClientReplicator::settle_transform_history()`, called once per tick from Publish,
expires history that the tick did not renew.

The subtlety is **what counts as renewal**. The server re-sends an unacked value for a round trip,
so records keep arriving for an entity that is standing still; keying the settle to "a record was
applied" would hold the blend open for the whole re-send window and produce the same sawtooth, just
bounded. It keys to a **genuinely different value** — the distinction m11.6a's re-send guard already
draws, inherited rather than re-derived. Expiring one tick later is safe precisely because the blend
it drove has already run to alpha≈1 over that tick's frames.

Why m11.6a could not see any of this: its proof drove the entity for 20 straight ticks and then
despawned it. "Moves forever" and "moves, then rests" are the two behaviours the history exists to
tell apart, and only one of them was ever run. The same shape as every other entry in this document
— *the scenario where the mechanism is described, not the one where the two behaviours diverge*.

**Known limitation, named rather than hidden.** The blend always spans exactly one tick period. When
relevancy or the byte budget defers an entity's record for several ticks — which m11.5 makes routine
by design — its motion replays at several times speed over one tick and then holds. That is judder,
not the rewind above, and fixing it properly means interpolating over the *interval a value actually
covers*: the delta header already carries the server `tick`, so the seam is there, and this is the
"snapshot interpolation on top" the roadmap sequences after the alpha consume path.

Deferred with it: two or more *blending* replicated entities parented to each other, which would
need `propagate_transforms`-style depth ordering. Unreachable today rather than untested —
`WireSchema::is_replicable` refuses any type carrying an `Entity` field, so `Parent` cannot cross the
wire at all, and a single replicated parent is already impossible. The one-level composition is
built and proven by direct construction, the same standard the refused-part branch is held to.

---

## One unreliable stream per supersedes-relationship (ADR-0033 A18)

`ReliableChannel::send_unreliable` takes a **stream id**, and picking it is a design decision rather
than a label. A stream is a *supersedes-relationship*: a newer message on a stream makes an older one
on that stream garbage, and the older is dropped on arrival.

Two kinds that say nothing about each other must not share one. They are sent in the same tick, so
they draw consecutive sequence numbers, and on any link with jitter the receiver sees them in either
order — whichever lands second silently discards the other before the application sees its bytes.
This was found before it could bite, while designing the input message that would have become the
second unreliable sender.

The interesting half is where the engine **declines** to split. Every part of a multi-part `Delta`
rides one stream even though the parts complete each other rather than replace each other, because
that shared stream is the only thing keeping deltas ordered and `on_delta` applies records blind.
Split them and part 1 of tick N can land after part 0 of tick N+1, writing a stale value over a fresh
one — and because the client would still count tick N+1 complete and acknowledge it, the server would
advance the baseline, stop re-sending, and the two worlds would sit permanently disagreed. **A
bandwidth cost that the completeness rule self-heals is worth more than a silent-divergence risk.**
The cost is now visible as `unreliable_superseded()` instead of being invisible; closing it properly
needs a per-record staleness guard or real fragment reassembly, which is its own brick.

The general rule, which is corollary 1 wearing different clothes: **a message may only invalidate
another when the sender said they were about the same thing.** Sequence adjacency is not that
statement — it is an artifact of when the two were handed to the transport.

---

## For the input half of m11.6 — build it to the rule the first time

**The client→server input watermark is a fresh instance of corollary 1**, not a special case.
Consumed ≠ arrived, consumed ≠ latest-received. If buffered input bursts ever need multi-part
framing, re-derive the completeness discipline deliberately rather than assuming a small message
never needs it — that assumption is what made the original baseline bug look like a non-issue
until a wall that stops changing gave it a case where it mattered.
