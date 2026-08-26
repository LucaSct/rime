# engine/gameplay_net — the networked player

`rime::gameplay_net` is where [`gameplay`](../gameplay/README.md) and
[`replication`](../replication/README.md) meet, so that neither has to know about the other. It was
scoped in [ADR-0035](../../docs/adr/0035-vision-demo-m12.md) §3/§4 (m12.3) and it holds the
**server-authoritative** half of the player: the consume loop, the session ↔ avatar registry, and
the one component that pairs authoritative state with the input position that produced it.

Since m12.4 it also holds the **`Predictor`**: the client half that makes your own input visible
now instead of a round trip from now.

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

## Prediction and reconciliation (m12.4)

The client runs **the same `step_character`** the server will run, on the same command, immediately
— then checks its work when the authority's answer arrives. It is not trust: the server still
decides what happened, and the client is snapped whenever it guessed wrong.

Per tick: reconcile against the mirror's `{CharacterState, LastProcessedInput}` → predict this
tick's command → publish the predicted pose to the **transforms only**. `CharacterState` on the
mirror is never written by the client — it is the authority's word and the input to the next
comparison, and overwriting it would make reconciliation compare the prediction against itself and
agree forever.

**The comparison is on resulting state at `q`, never on a diff of command lists** — m11.6c's rule,
because `consumed_through` steps over permanent gaps, so a command leaving the send buffer means
*the server will never act on it*, not *the server acted on it*. A list diff is wrong exactly under
loss, the condition it exists for.

**The `Predictor` keeps its own command history and does NOT replay from `unacked()`**, which is a
deliberate departure from ADR-0035 §4's sketch. `unacked()` retires on
`ClientInputSender::acked_through`, which comes from `InputAck` — a *different* unreliable
superseding stream from the one carrying the snapshot's `q`. The ack is therefore routinely fresher
(measured: on 60 of 300 ticks under 30% loss), so replaying only `unacked()` would skip every
command in `(q, acked_through]` — commands the server consumed and the client predicted — and the
prediction would slide backwards on every correction. The ring holds `{sequence, command, state}`
and retires on the **reconciled `q`**, which is the only frontier that makes the replay set
complete.

The lost-command property survives unchanged: a command the server never received sits below the
ack jump, `q` moves past it, the ring is trimmed past it, and the replay excludes it. The comparison
at `q` then disagrees, a correction fires, and the client snaps to a truth in which that input never
happened.

**Why there is a tolerance at all**, given both sides run the same function on the same inputs: they
do not run it against the same *world*. The client's physics world is built from mirrors that arrive
at their own pace. Without a gate the predictor would correct every tick on a clean link. The
epsilons are confined there, mid-flight; **at quiescence the two states must be equal bit for bit**,
and that is what the proof asserts.

One consequence worth knowing before it surprises someone: **a permanently-lost command only stops
mattering when a later one arrives.** If the last thing a client ever sends is dropped, the server's
frontier stops just short of it and the two sides sit exactly one tick of travel apart until
somebody says something else. A real client keeps sending while standing still, so this is a
property of contrived silence rather than of play — but it is why the proofs settle *with* input.

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
| m12.4 | `Predictor` — the `{sequence, command, state}` ring, the tolerance gate, rewind and replay; proofs: own-input response ≤ 1 tick against a prediction-off control, bit-exact convergence at quiescence, corrections non-zero under loss, a reconciliation-off divergence control, and the ack-fresher-than-snapshot case | landed |
| m12.5 | predicted-player **smoothing** — a correction's displacement is absorbed into a decaying visual offset, bounded so a large one is still shown at once; proofs: the drawn pose slides where m12.4's jumped, the simulation is bit-identical with smoothing on and off, and a slide finishes. (Snapshot interpolation v2 is the other half and lives in [`replication`](../replication/README.md).) | landed |
| m12.6 | Track FX brick fx1 — the GPU draw pass for the existing deterministic CPU sim | next |

## The numbers

Measured by `tests/gameplay_net/latency_test.cpp`, with no clock synchronisation anywhere: count
the client's *own* ticks between stamping sequence S onto a command and seeing a mirrored
`LastProcessedInput >= S` on its own avatar. Both endpoints are events on one machine's clock, so
no offset can enter (the ADR-0030 §5 trick).

| link | own-input latency | visible position lag while walking at 6 m/s |
| --- | --- | --- |
| 48 ms one-way (6 round-trip ticks at 60 Hz) | **6 ticks** | **0.30 m** |
| zero-latency loopback | 2 ticks | — |

ADR-0035 §1 asks for **≤ 1 tick** with prediction on, against exactly that control. m12.4 delivers
it, and the control is re-run beside it in the same case so the two numbers come from one tape:

| | own-input response at 48 ms one-way |
| --- | --- |
| prediction **on** (m12.4) | **1 tick** |
| prediction **off** (m12.3, the control) | **6 ticks** — the round trip |

The rest of what m12.4 measures, all from `tests/gameplay_net/prediction_test.cpp`:

| property | measured |
| --- | --- |
| worst prediction-vs-authority distance while walking | 0.20 m (a round trip of travel — the prediction runs *ahead*, which is the point) |
| under 25% loss, 300 ticks | 124 pairings, **2 corrections**, 122 within tolerance, worst error 0.20 m |
| divergence after 300 lossy ticks | reconciliation **on** 0.2 m · **off** 1.1 m |
| with 13 permanently-lost commands | 10 corrections, then bit-exact agreement |
| ack fresher than the snapshot | on **60 of 300** ticks — the condition §4's `unacked()` sketch would have got wrong |

## Named costs

- **The weapon is not predicted.** `step_weapon` is pure and would replay fine, but a client-side
  shot resolves against a world that differs from the server's, so predicting hits means predicting
  *wrong* hits and then unwinding damage. The mover is predicted; the trigger waits. Nothing in
  `CharacterState` depends on the weapon, so the two are exactly consistent as they stand.
- **Smoothing is presentation-only, and that is load-bearing.** `state()` is the simulation's
  answer and never carries the offset; `visual_position()` is what a renderer draws. Feeding the
  offset back would put a hidden accumulator inside a function m12.2 proved pure, and the symptom
  would be a rare desync rather than a failing test. The proof asserts the two arms' simulation
  trajectories are bit-identical over 250 lossy ticks while the drawn poses differ on 156 of them.
- **m12.5 is the milestone's designated cut** — ADR-0035 §5 lists it last on the cut order after
  audio and fx1b, and it is the only brick in this module that a player would notice the *absence*
  of rather than the *failure* of. Everything above it works without it: corrections would simply be
  shown as jumps, which is m12.4's behaviour and is exactly what `smoothing_decay = 0` restores.
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
authority over hostile input, ordering under 25% loss, and the transform-handoff regression),
`latency_test.cpp` (the prediction-off baseline, convergence at quiescence, two clients each told
which avatar is their own), `prediction_test.cpp` (m12.4 — every case with its own negative
control), `smoothing_test.cpp` (m12.5), `weapon_glue_test.cpp` (the consumer glue, and the only file
here that links `rime::destruction`).

The clients in `prediction_test.cpp` run a real client tick: their own `PhysicsWorld` over the same
tiled level, `PhysicsSync` binding their replicated mirrors, and the predictor between the two. The
m12.3 cases deliberately do **not** — their clients never simulate, because those cases *are*
m12.4's negative control and a control that quietly acquired the mechanism it controls for would
turn the comparison into a tautology.
