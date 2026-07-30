# rime::destruction_net — networked destruction (M11.4)

The layer that makes a wall break the same way on every machine. The server commits the canonical
**damage-op list** each tick and publishes it on the reliable-ordered channel; clients apply it and
never convert a contact impulse of their own. See
[ADR-0033](../../docs/adr/0033-networking-v1.md) — amendments **A1** (the op list is the replicated
artifact), **A3** (the state-application seam), and **A11–A13** (what "the same tick" means, and the
two bugs building this found).

## Why it is its own module

It depends on `rime::destruction` and `rime::replication` so that **neither depends on the other**.
Folding it into `replication` would give the whole networking stack a hard dependency on the
destruction feature module, and guardrail 2 promises the engine still builds with a feature module
deleted; folding it into `destruction` would make the headline gameplay system depend on the
transport. This is the same shape, and the same argument, that put ECS replication in its own module
at m11.3 (ADR-0033 A10) rather than inside `engine/net`.

## The idea

- **The unit is the op list, not the command.** Half the damage stream is *emergent*: the runtime
  converts the solver's contact impulses into damage ops every tick. A client replaying only explicit
  `apply_damage` calls would diverge the first time a debris pile eroded a part by resting on it. So
  what crosses the wire is the committed, canonically-ordered op list — which was always the
  deterministic function's input (ADR-0029 §3).
- **Ops are expanded, so the arithmetic runs once.** A radius call fans out to one op per overlapped
  part with the falloff already resolved. That float math happens on the server and nowhere else,
  so two peers cannot disagree about it. Replicating the *call* would re-run a distance query and a
  falloff multiply on every client, on a different compiler — which is how sub-ULP drift gets in.
- **Reliable-ordered, not unreliable.** Destruction is a sequence of transitions, each permanently
  changing what the next one means. Lose the op that killed the part holding up an arch and no later
  message repairs it. That is the opposite of a transform snapshot, which is why ADR-0033 §3 splits
  the channels at all. (m11.4b's debris transforms ride the unreliable channel, for the same reason
  inverted.)
- **Instances are named by NetId, never by InstanceId.** An `InstanceId` is a local table position;
  two peers agree on it only if they happened to spawn in the same order, which late-join breaks on
  its first tick. The destructible's *entity* carries a NetId through m11.3, and each peer maps that
  to whatever local instance it likes. Neither side's index ever crosses. This is what the bind path
  (`destruction/bind.hpp`) exists for.
- **Mirrors are `Authority::Remote`.** `apply_damage` becomes a no-op on them and the contact→damage
  drain skips them — one early return, not a forked update path, because two damage pipelines that
  are supposed to stay equivalent will not.

## The two rules that are not obvious

**A tick is atomic.** A tick's op list may span packets; every part must arrive before any of it
reaches the world. Applying half a canonical sequence, running a support solve, then applying the
rest lands the second half on a wall of a different shape than the authority applied it to.

**So is the gap between two ticks.** Two of the server's ticks can arrive in one of the client's.
Handing both to one `update()` skips the fracture boundary between them: alive bits and healths
still converge, but the **debris composition** diverges — parts that detached in two waves leave as
one island. m11.4b addresses debris by roster index, so that is a wrong address, not a cosmetic
difference. One batch per update; a client behind the authority catches up by running extra whole
update cycles.

## Debris (m11.4b)

Determinism gives both peers the same chunks in the same order with the same initial conditions — so
**composition is derived and never sent**. It does not give the trajectory afterwards (the peers are
not in lockstep, their physics worlds differ, and same-binary determinism is not cross-platform), so
**transforms are replicated**. The association crosses as *data*: a reflected
`DebrisOrigin{source NetId, ordinal}` rides m11.3's snapshot path, needing no new message.

Corrections apply on a **tolerance**, not every tick — the replicated transform is authority for
where a chunk ends up, not a per-tick puppet string, and snapping a continuously-simulated body every
frame would replace tumbling rubble with a stutter.

The ordinal is only a safe address because m11.4a's A12 fix makes both rosters agree index for index.
`CompositionCheck` verifies that rather than trusting it, because ordinal addressing fails silently:
it resolves to a *different* chunk, and the client then corrects the wrong rubble.

## The cross-peer witness

`destruction_net::shared_state_hash(world, map, destruction)` is the one number two peers must agree
on — per-part alive bits and health plus debris composition, walked in NetId order. Use it, not
`DestructionWorld::state_hash()`, which folds physics body ids and is a same-process replay witness
only: across a wire it mismatches every tick, which reads as a broken engine rather than as the wrong
question.

## Status

| brick | what | state |
|-------|------|-------|
| M11.4a | the **damage-op stream** — bind path, wire format, client apply, contact suppression, the A3 state-application seam | landed |
| M11.4b | **debris** — the debris↔entity bridge, transform replication, composition-hash drift detection | landed |

## Layout

```
engine/destruction_net/
├── include/rime/destruction_net/
│   ├── wire.hpp                 # MessageTag (0x40 block), the DamageOps/CompositionCheck layouts
│   ├── components.hpp           # DebrisOrigin (replicated) / DebrisRef (local) — the debris bridge
│   ├── composition.hpp          # the composition fingerprint + the CROSS-PEER state witness
│   ├── destruction_server.hpp   # publish the committed op list; keep the debris↔entity bridge
│   └── destruction_client.hpp   # decode, queue one batch per fracture boundary, bind + correct debris
└── src/
    ├── destruction_server.cpp
    ├── destruction_client.cpp
    └── composition.cpp
```

## Sharing a session

One session carries every module's traffic, so the first payload byte is a **shared tag registry**
(documented in `replication/snapshot.hpp`): `0x01–0x3F` replication, `0x40–0x7F` this module. Since
`Session::drain_received` *moves* messages out, subsystems on one session must not each drain for
themselves — the app drains once and hands the same span to each `apply_messages`.

## Building & testing

Built as part of the engine (`scripts/build.sh`). The proofs are GPU-free — cooked geometry in,
physics bodies and packets out — on a `ScriptedNetwork` over a virtual clock, so loss and latency are
inputs rather than environment luck. They run on every CI OS plus ASan/UBSan and TSan:

```bash
ctest --preset dev -R rime_destruction_net_tests
```
