# engine/gameplay_net — the networked player

`rime::gameplay_net` is where [`gameplay`](../gameplay/README.md) and
[`replication`](../replication/README.md) meet, so that neither has to know about the other. It was
scoped in [ADR-0035](../../docs/adr/0035-vision-demo-m12.md) §3/§4 (m12.3) and it holds the
**server-authoritative** half of the player: the consume loop, the session ↔ avatar registry, and
the one component that pairs authoritative state with the input position that produced it.

It is the third module placed by the same guardrail-2 argument that created `replication` and
`destruction_net`. Folding it into `replication` would give the transport a hard dependency on
gameplay; folding it into `gameplay` would make a single-player character controller depend on
sockets. Both parents stay removable, and so does this.

**It does not link `rime::destruction`.** A shot reports *what* it hit; turning that into damage is
a game rule, and the glue lives in the consumer — the M8.4 fan-out rule that keeps `vfx` out of
destruction, applied again. `ShotEvent` therefore carries the already-resolved damage arguments
rather than a `destruction::DamageOp`, and the ~20 lines a real game writes are shown in full in
`tests/gameplay_net/weapon_glue_test.cpp`.

## The loop, and its one load-bearing commitment

Per tick, per player, in this order: **drain** the session's accepted commands (draining is what
advances `consumed_through`, so the ack means "the game has this" and never "a packet arrived
carrying it") → run `step_character` **once per command** in sequence order → resolve fire from the
pose the move produced → write `CharacterState`, `WeaponState` and `WorldTransform` → stamp
`LastProcessedInput{sequence}`.

**One step per command, and not one per tick.** A tick that consumed three commands advances the
mover three times; a tick that consumed none advances it **not at all** — the avatar freezes and
`ticks_starved` counts it. That second half looks like a bug and is the design: m12.4 reconciles by
comparing the client's predicted state *after command q* against the server's state *at q*, and
that comparison is only like-for-like if both sides ran the mover the same number of times over the
same inputs. A server that helpfully repeated the last command on a starved tick would turn every
packet loss into a phantom correction caused by its own helpfulness.

**The pairing rides the ordinary snapshot path.** `LastProcessedInput` is a replicated component on
the avatar, not a message: the two halves cannot come apart, because they are the same bytes. A
separate message would have to be ordered against the Delta carrying the state it describes, across
two channels [ADR-0033](../../docs/adr/0033-networking-v1.md) §3 deliberately gives no ordering
between. This is A15's `DebrisOrigin` move, reused on purpose.

**Do not reconcile against `InputAck`.** It carries a per-session `consumed_through` and it is
tempting. It does not work: the ack rides an unreliable stream that supersedes, so the newest ack a
client holds is routinely *fresher* than the newest snapshot it holds — pairing tick T's state with
tick T+2's frontier claims the server had applied commands it had not. Different frontiers;
collapsing them is the same mistake as collapsing `received_through` into `consumed_through`.

## The rate budget

"One step per command" hands a client a speed multiplier if it simply sends faster. Each player
carries an allowance that refills by **one per tick** and saturates at `max_command_burst` (8, i.e.
~133 ms of catch-up at 60 Hz). A client sending at the tick rate never touches it; a client catching
up after jitter spends the slack and is fully served; a client sending persistently over rate is
served at one command per tick and the surplus is **dropped, oldest first** — under a genuine
over-rate burst the newest command is what the player is doing now.

Dropping rather than deferring is what keeps the acknowledgement honest. `consumed_through` is
explicitly *not* a completeness claim — it steps over commands the server will never act on — so a
client retiring a dropped command has learned the truth. Deferring would advance the frontier over
commands still sitting in a queue, which is the replication invariant violated upstream. Every drop
is counted.

## The wire

One message, in the **0x80–0xBF** block of the shared session tag registry
(`replication/snapshot.hpp`):

| tag | kind | channel | why |
| --- | --- | --- | --- |
| `0x80` | `AssignPlayer` | reliable-ordered, server→client | "this NetId is your avatar" is true once and forever, and no later message repairs its loss — the Spawn/Despawn argument verbatim |

Nothing else needed one. The client stores the **NetId** and resolves it through the `NetIdMap` on
every ask rather than caching an entity, because reliable-ordered delivery is not ordered against
the unreliable Delta stream and an assignment can legitimately arrive before its Spawn binds.

Why a message at all, when every avatar already carries `LastProcessedInput`: every client's
sequence numbering starts at 1, so two players moving in step are indistinguishable in the snapshot.
The session is the only thing that knows, and it is server-side only.

## Status

| Brick | Provides | State |
| --- | --- | --- |
| m12.3 | `GameplayServer` (consume loop, rate budget, shot events), `GameplayClient` v1, `PlayerRegistry`, replicated `LastProcessedInput`, `AssignPlayer`; proofs: one-step-per-command, the rate budget against a negative control, ordering under scripted loss, bit-exact convergence at quiescence, the weapon→destruction glue end to end, and the **prediction-off latency baseline** | landed |
| m12.4 | `Predictor` — the `{sequence, state}` ring, the tolerance gate, rewind and replay | next |

## The number m12.4 must beat

Measured by `tests/gameplay_net/latency_test.cpp`, with no clock synchronisation anywhere: count
the client's *own* ticks between stamping sequence S onto a command and seeing a mirrored
`LastProcessedInput >= S` on its own avatar. Both endpoints are events on one machine's clock, so
no offset can enter (the ADR-0030 §5 trick).

| link | own-input latency | visible position lag while walking at 6 m/s |
| --- | --- | --- |
| 48 ms one-way (6 round-trip ticks at 60 Hz) | **6 ticks** | **0.30 m** |
| zero-latency loopback | 2 ticks | — |

ADR-0035 §1 asks for **≤ 1 tick** with prediction on, against this control. Recording it now, before
the thing it controls for exists, is the point: a baseline measured afterwards is a baseline chosen
to be beaten.

## Named costs (v1)

- **No prediction and no reconciliation.** That is m12.4, and the whole reason this brick ships
  separately is to have an honest control for it.
- **No lag compensation**, and none is planned for M12: a shot is resolved against the world as it
  stands on the tick the server consumed the command, so a player shooting a moving target must
  lead it by their own latency. ADR-0035 §4 rules it out on the grounds that the block's targets are
  buildings, and buildings do not dodge.
- **A dropped command is never re-offered.** By design (see the rate budget), and safe because
  reconciliation compares resulting *state*, never a diff of command lists.
- **`PlayerRegistry::session_for` is a linear scan.** Asked once per shot for attribution, never per
  tick per entity. If a future brick makes it hot, the fix is a hash map.

## Tests

`tests/gameplay_net/` — a server and N clients on a `ScriptedNetwork` over a virtual clock, so loss
and latency are inputs rather than environment luck and every wait is a bounded tick loop.
`consume_loop_test.cpp` (one step per command, the rate budget and its negative control, server
authority over hostile input, ordering under 25% loss), `latency_test.cpp` (the baseline above,
convergence at quiescence, two clients each told which avatar is their own), `weapon_glue_test.cpp`
(the consumer glue, and the only file here that links `rime::destruction`).
