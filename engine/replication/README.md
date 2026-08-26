# `engine/replication` — ECS state over the wire (M11.3)

Turns "an `ecs::World` changed" into bytes on a `net::Session`, and back. Server assigns identity
and publishes state; clients mirror it. Decisions live in
[ADR-0033](../../docs/adr/0033-networking-v1.md) §4, as corrected by its **A9/A10** amendments.

## Why this is its own module

It is the one place that needs **both** `rime::ecs` (to walk the world through reflection) and
`rime::net` (to move bytes) — so it exists precisely so that neither of those has to depend on the
other. `engine/net` has been `core + platform` only since it was born, and m11.2 paid real design
cost to keep it that way (the component schema hash crosses into `NetDriver::Config` as an opaque
`u64`). Putting replication inside `engine/net` would have undone that and broken guardrail #2.

`engine/editorhost` already has this shape one storey up (`ecs` + `stream`, neither aware of the
other). Nothing below depends on this module, so it remains removable.

## The pieces

| File | What it is |
|---|---|
| `net_id.hpp` | `NetId` (a generational `core::Handle`), its server-side allocator, and the `NetId ↔ Entity` map |
| `wire_schema.hpp` | Component type → wire id, derived identically on both peers, transmitted never |
| `snapshot.hpp` | Message tags, wire layouts, the `Replicated` marker, and `AckTracker` |
| `server_replicator.hpp` | Assigns identity, announces structure, publishes deltas per client |
| `client_replicator.hpp` | Applies spawns/despawns/state, reports its baseline |

## Four ideas worth knowing before you edit this

**1. The delta needs no history buffer.** The usual way to answer "what changed since the client's
baseline" is a per-client ring of past snapshots — O(clients × history × entities). `ecs::Chunk`
already stamps every component *column* with the world version it was last written at (ADR-0018 §4,
built for the editor, not for us), which answers the same question at O(1) memory for *any* point in
the past. So per-client state here is one integer: the baseline they acknowledged.

The cost that is real: the comparison pass runs once per client per tick and does not amortize
across clients whose baselines have diverged. At the ADR's 64-player target this is where to point a
profiler first — ahead of bandwidth. m11.5's relevancy work shrinks this same loop.

**2. Nothing in the transport acks a snapshot.** ADR-0033 §4 claimed the reliability layer's ack
bitfield doubles as the baseline tracker. It does not — that machinery serves only the
reliable-ordered stream (amendment **A9**). Snapshots ride the unreliable-sequenced channel, which
by design acknowledges nothing, so the baseline ack is an ordinary replication message travelling
back up the client's own unreliable channel.

**3. The watermark only advances past a tick whose every part arrived.** A tick too big for one
datagram becomes several independently-complete packets. If a client acked a tick on the strength of
*one* of its parts, the server would compute the next delta as "changed since T" — and entities
written at T that lived in a lost part would never be re-offered. Stop moving, and they are wrong
forever. `AckTracker` exists to make that impossible; the four values it keeps are the whole
mechanism.

**4. The generation check in `NetIdMap::resolve` is not decoration.** Spawn/despawn are
reliable-ordered, snapshots are unreliable-sequenced, and the two have **no ordering relative to
each other** — that is the point of the split. So a delta can name a recycled NetId whose `Spawn` is
still resending. Keyed on index alone, the map would write the new entity's state onto the old
entity's mirror. Comparing generations turns that into a clean miss.

## Using it

Register every replicable component *before* constructing a replicator, then hook the two calls into
`Application`'s ordered sim stage — inbound in `PreSim` (so the tick runs against corrected state),
outbound in `Publish` (so what is described is the tick's final state):

```cpp
replication::ServerReplicator server{world};
app.add_sim_stage(SimStage::PreSim,  [&](ecs::World& w, double) {
    driver.update(now_ms, events);
    server.on_session_events(events);
    (void)server.apply_inbound(driver);
});
app.add_sim_stage(SimStage::Publish, [&](ecs::World&, double) { server.publish(driver, now_ms); });
```

Opt an entity in with `server.replicate(entity)`. **Despawn it with `server.despawn(entity)`, never
`world.despawn()`** — the latter leaves a phantom on every client with nothing to repair it.

Since m12.3 (ADR-0035 §6) that discipline has a **backstop**. `publish` walks the live NetId slots
once per tick — not once per client — and for any whose entity is no longer alive it retracts the id
properly, logs a warning naming it, and increments `net_ids_orphaned()`. So the mistake costs a
one-tick-late despawn instead of a permanent phantom. It is still a discipline, not an enforcement:
the counter exists precisely so a repaired mistake does not become an invisible one, and it should
read **zero** in any healthy game. A non-zero value names a call site to change.

### Sharing a session with another module

`apply_inbound` **drains** the sessions, which means it takes sole ownership of the mail —
`Session::drain_received` moves messages out. That is fine while replication is the only tenant. The
moment another module also reads (m11.4's damage-op stream is the first), the app must drain once
itself and hand the same span to each subsystem:

```cpp
inbox.clear();
(void)session->drain_received(inbox);
state_client.apply_messages(inbox);                     // replication's tags
destruction_client.apply_messages(inbox, map, world);   // destruction_net's tags
```

The first payload byte is a **shared tag registry**, documented at the top of `snapshot.hpp`:
`0x01–0x3F` replication, `0x40–0x7F` `destruction_net`, the rest unallocated. Each module ignores
tags outside its own block — counted as `foreign_messages()`, not as malformed, because another
module's well-formed message is not an error. Before m11.4 nothing said the space was shared, and
the next module would naturally have started its own enum at 1: a silent collision with `Spawn`, in
which whichever module drained first consumed and misparsed the other's traffic.

## Named limits

- **Entity-reference fields do not replicate.** A component containing an `ecs::Entity` (e.g.
  `ecs::Parent`) holds a handle into the *sender's* directory, which names nothing in the receiver's.
  Such types are excluded from the wire schema and listed in `WireSchema::excluded_names()`.
  Translating them through the `NetIdMap` is a follow-up.
- **No prioritization.** Over the per-tick packet budget, the remainder waits for the next tick —
  latency, not loss, and deliberately not solved here with an ad-hoc heuristic. That is m11.5.
- **No interpolation.** Mirrors snap to the last state received. m11.6 builds the history buffer.
- **Chunk-grain change detection over-includes.** An entity that did not move but shares a chunk
  with entities that did gets re-sent. Bounded by chunk occupancy; the mitigation is a content
  discipline (keep movers out of static-dominated archetypes), worth measuring before m11.4's debris
  makes it urgent.
- **Out-of-order state is held, and the buffer is bounded.** A delta record whose `Spawn` has not
  landed is kept and replayed when it does (ADR-0033 A14), so nothing is lost and the tick can still
  be acknowledged. The buffer is capped at `kMaxDeferredRecords` because the ids keying it come from
  the peer; on overflow the oldest record is evicted and that tick goes unacknowledged, falling back
  to the server re-offering it. An honest peer never reaches that, but a scene spawning more than the
  cap in a single burst will briefly pay the slower path.
