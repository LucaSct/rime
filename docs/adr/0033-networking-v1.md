# ADR-0033: Networking v1 — server authority, event-replay destruction, own UDP transport

- Status: Accepted
- Date: 2026-07-28

## Context

Milestone 11 is **networking + networked destruction** — the ROADMAP's "done when": *two clients see
synchronized destruction at meaningful scale*. Unlike M10, which entered with a full architecture ADR
and brick ladder, M11 enters with two sentences and a set of **scattered, deliberate seams** that were
built for it all along:

- **The fixed simulation tick** ([ADR-0023](0023-app-fixed-tick-loop.md)) — the sim advances in equal,
  deterministic steps decoupled from the render frame, with previous/current snapshots already kept
  (explicitly named there as "an M11 enabler" for interpolation/prediction).
- **Same-binary determinism** ([ADR-0026](0026-physics-core.md)) — physics steps bit-identical across
  worker counts, with a `world_hash` to prove it. The same ADR makes **cross-platform lockstep an
  explicit non-goal**, which forecloses deterministic lockstep as the networking model.
- **The destruction event-replay contract** ([ADR-0029](0029-destruction-model.md)) — the whole
  damage → connectivity → detach path is a deterministic function of its inputs, applied in canonical
  order, with stable part-id addressing *"derivable, not guessed"* — named in that ADR as "the M11.4
  addressing." M8 was built so destruction decisions can be **replayed from events**, not just
  observed as state.
- **Canonical event streams** (`core::EventChannel`, M7.9/M8.4) — physics and destruction already
  publish double-buffered, deterministically-ordered event batches; the replication layer has
  something stable to serialize.
- **Reflection** (M1.7/M4.1) — components are registered through reflection with a schema hash, so
  snapshot serialization can be *generated*, not hand-written per component, and a client running
  mismatched code is rejected at handshake instead of misread (the M9 editor protocol already proves
  this pattern over the wire).
- **Platform sockets + the streaming protocol** (S0/S1, [design/net-sockets.md](../design/net-sockets.md))
  — TCP/UDS byte streams with a versioned, length-prefixed message protocol. Battle-tested by the
  editor, and explicitly labeled the *seed* of `engine/net`. But: **blocking TCP only** — no UDP, no
  non-blocking I/O.

What is missing is everything that makes it a system: the networking-model decision itself (authority,
replication strategy, transport), the `engine/net` module, and the brick ladder. This ADR supplies the
first and third; the code bricks supply the second. Consistent with the other `.0` decision bricks
(ADR-0024/0026/0029/0030/0032), **this is a pure decision brick: the proof is the ADR, no code.**

The reference point is Frostbite's model (Battlefield-class, 64+ players): a **server-authoritative**
architecture where clients send inputs and render an interpolated, replicated view — *never* trusting
client state — with destruction prioritized so everyone sees the same structure break. We adopt the
principles, not the scale target: M11's proof is two clients; the design must not close the door to 64.

## Decision

### 1. Authority: dedicated-server-authoritative; a listen server is a degenerate case, not a design

One process **owns the simulation** (the server). Clients send *inputs/intents*; the server simulates
the tick and publishes *results*. No client state is ever authoritative. Rationale:

- **Determinism is a property we can only enforce on one machine.** ADR-0026 guarantees bit-identical
  results *same-binary, same inputs* — which the server has. Cross-client float drift (platform,
  compiler, math libraries) is real, so "everyone simulates and hopes" (lockstep) was rejected there;
  we honor that rejection here.
- **Cheat resistance is structural.** A client that cannot write world state cannot teleport walls.
- **Frostbite-shaped.** The engine we are fusing toward is server-authoritative at 64 players; the
  v1 seams (interest sets, packet budgets) are designed so scale is work, not a rewrite.

A **listen server** (a player hosts) is the same code with the server and one client in one process —
an embedding, not an architecture. The M11 proof runs a **dedicated headless server** + two clients
(it must: the CI box is headless, and the dev server is the target test bed).

Peer-to-peer and deterministic lockstep are **rejected** (lockstep: ADR-0026's non-goal, plus one
delayed input stalls everyone; P2P: no authority, no cheat story, no natural home for relevancy).

### 2. Replication: hybrid — snapshots for state, *event replay* for destruction decisions

The one decision that makes networked destruction cheap instead of brutal. Replicating a fracture as
*state* means streaming every debris body's transform at snapshot rate — the bandwidth scales with
rubble. Replicating it as an **event** — *"part-set P detached under damage event D at tick T"* — is a
few dozen bytes, and each client **replays the deterministic damage → detach function locally**
(ADR-0029's contract) to produce bit-identical part topology. This is why M8 was built as a pure,
canonically-ordered function with derivable part ids: the fracture *decision* replicates as data the
size of a chat message.

The hybrid boundary is precise:

- **Destruction *topology* (which parts broke, when): event-replicated, server-authoritative,
  reliable-ordered.** Applied on both sides at the same tick. `world_hash`-style verification proves
  clients match.
- **Dynamic *state* (debris transforms, body velocities, anything that moves continuously):
  snapshot-replicated from the server, unreliable-sequenced.** Debris physics is deterministic
  same-binary, but we do not *depend* on that across platforms or long horizons: the server's
  snapshots are the corrective authority, so any drift self-heals on the next snapshot instead of
  diverging forever. Clients may extrapolate between snapshots purely cosmetically.
- **Player-controlled entities** ride the same snapshot channel, with client-side interpolation
  between the previous/current snapshots ADR-0023 already keeps. **Prediction in v1 is a seam, not a
  feature** (§6 non-goals): the proof's clients are observers with camera input; the prediction
  interface is defined so a player controller slots in later without reworking replication.

Why not pure event-replay (full lockstep of everything)? It needs cross-platform determinism — the
ADR-0026 non-goal — and one client's hitch stalls the world. Why not pure state replication? Bandwidth
scales with debris count exactly where destruction is supposed to scale hardest, and it throws away
the determinism M7/M8 already paid for. The hybrid spends each where it's strong.

### 3. Transport: our own UDP datagram socket + a thin reliability layer — TCP stays for tools

Game-state sync cannot ride TCP: head-of-line blocking turns one lost packet into a stall of
*newer, more correct* state — the one thing a real-time game must never wait for. So M11 grows the
transport in `platform` the same way S1 grew the codec:

- **`UdpSocket`** in `platform` (next to the S0 TCP socket, same seam discipline: no OS type in the
  header, `socket_posix.cpp` shared by Linux/macOS, Win32 its own file). Datagrams, non-blocking
  receive, `send_to`/`recv_from` with explicit endpoints. This is the second transport the
  net-sockets design note predicted would graduate `engine/net` into existence.
- **A thin reliability layer in `engine/net`** — not a library: sequence numbers + ack bitfields +
  resend, giving two channel classes over one socket: **reliable-ordered** (events, spawn/despawn,
  handshake — the destruction events of §2 live here) and **unreliable-sequenced** (snapshots — late
  or lost state is simply superseded by the next snapshot, so we *drop*, never resend). This is the
  classic Quake/ENet shape, deliberately minimal, and written to be read (VISION #3).
- **Loss/latency/jitter are test inputs, not environment luck.** The reliability layer sits behind a
  `Link` seam with a deterministic in-process implementation that injects scripted loss/reorder/delay
  — so CI proves correctness under 30% loss without a network, and the determinism story stays intact.
- **TCP/UDS keep their jobs**: the streaming protocol (Track S) and the M9 editor wire are untouched.
  M11 does not disturb S0/S1.

**No third-party networking library** (ENet, yojimbo, QUIC stacks): VISION #1/#3 — the reliability
layer is small, central to the engine's identity, and exactly the kind of code this repo exists to
teach. QUIC/TLS is the S2 internet-transport conversation and folds in behind the same `Link` seam.

### 4. Identity & serialization: server-assigned `NetId`s, reflection-driven snapshots

- **`NetId`**: a server-assigned, session-stable integer id for replicated entities, mapped to the
  local generational ECS `Handle` on each side by a `NetIdMap`. Server creates → assigns → clients
  bind on spawn. (The destruction path needs no NetIds for parts: ADR-0029's part-id addressing is
  already derivable and identical on all sides — debris *bodies*, being dynamic state, get NetIds.)
- **Serialization is generated from reflection** (M1.7/M4.1): snapshot writers/readers walk reflected
  component fields — no per-component hand code, and the **schema hash handshake** (the M9 editor
  protocol's proven pattern) rejects mismatched builds at connect. Quantization (compressed
  transforms) is a named m11.5+ optimization seam; v1 ships full precision and *measures*.
- **Baseline + delta**: each snapshot carries only components changed since the baseline the client
  has acked — the ack bitfield from §3 doubles as the delta-baseline tracker, which is why the
  reliability layer and replication are one module.

### 5. Relevancy & prioritization: destruction events are world-relevant; debris is budgeted

- **Destruction events are never culled.** Everyone's wall breaks — structure is gameplay truth.
  (They are small; see §2.)
- **Dynamic state is budgeted per client per tick**: distance-based relevancy for debris/body
  transform updates, nearest-first priority, and a hard per-packet byte budget. This is the seam
  Frostbite's 64-player scaling lives behind — v1 implements distance + budget, measures, and leaves
  richer scoring (visibility, gameplay importance) as named follow-ups.
- **Interest is per-client state on the server** (each client gets a different snapshot), which the
  authority model (§1) makes natural.

### 6. Non-goals for v1 (named, not hidden)

Client-side prediction of a player controller (seam only) · lag compensation/rewind · join-in-progress
mid-match (v1 joins before the session starts; late-join = full baseline snapshot, a fast-follow) ·
NAT traversal, relays, matchmaking · TLS/encryption/anti-cheat · 64-player scale proof (two clients
prove the model; the dev-server scale run is a fast-follow measurement, not the milestone gate) ·
cross-platform lockstep (ADR-0026, unchanged) · QUIC/internet transport (S2).

### 7. Module shape: `engine/net`, removable, layered on seams only

Per guardrail #2 the engine must build with `engine/net` deleted. It depends on `core`, `ecs`,
`platform` (sockets), and *interfaces* — replication reads the ECS world through reflection;
destruction integration consumes the ADR-0029 event channel and calls the documented damage
entry points; physics is never touched directly. The server is an `app` embedding: `Application`'s
fixed tick *is* the server tick — no second loop.

## The brick ladder (m11.0–m11.7)

- **m11.0** — ADR-0033 (this) + ROADMAP ladder. *Proof: the ADR.*
- **m11.1** — **Transport v2**: `platform::UdpSocket` (POSIX-shared + Win32, non-blocking) and the
  `engine/net` reliability layer (sequence/ack/resend, reliable-ordered + unreliable-sequenced
  channels) over a scripted-loss deterministic `Link` seam. *Proofs: datagram round-trip on all three
  OSes; delivery/ordering/no-resend-on-sequenced properties hold under 30% injected loss and
  reordering, bit-reproducibly, GPU-free in CI.*
- **m11.2** — **Sessions**: `NetDriver`/`Session` seam, connect handshake with schema-hash rejection,
  heartbeat/timeout, a headless loopback + LAN two-process smoke. *Proof: two processes connect,
  exchange hello, detect peer death.*
- **m11.3** — **Replication core**: `NetId` registry + `NetIdMap`, reflection-generated snapshot
  writers/readers, spawn/despawn + ack-baseline delta replication. *Proof: a moving ECS world
  replicated to a second process; worlds hash-identical after convergence.*
- **m11.4** — **Networked destruction**: server-authoritative damage events on the reliable-ordered
  channel; clients replay damage → detach at the same tick (the ADR-0029 addressing, at last);
  debris bodies ride m11.3 snapshots as the drift-correcting authority. *Proof: server + client
  fracture hash-identically; injected mid-fracture packet loss still converges.*
- **m11.5** — **Relevancy + budgets**: per-client interest, distance culling for debris transforms,
  nearest-first priority, per-tick byte budget. *Proof: a 1000+-debris break stays inside budget
  while a near client sees full fidelity and a far client sees the topology + sparse state.*
- **m11.6** — **Interpolation + input path**: snapshot interpolation buffer on the ADR-0023
  prev/current seam, client input messages upstream, the prediction *interface* (implementation
  deferred). *Proof: smooth remote motion at low snapshot rates; inputs arrive tick-tagged.*
- **m11.7** — **The milestone proof**: `samples/12-networked-destruction` — a dedicated headless
  server + two clients over loopback, a wall breaks under fire, **both clients see the same
  destruction at meaningful scale** (the ROADMAP's "done when"), self-checked in CI (deterministic
  scripted match, hash-verified) with a `--serve`-style human mode.

Deferred fast-follows (tracked in ROADMAP, not cancelled): late-join baseline snapshots, dev-server
scale run (16+ clients), transform quantization, richer relevancy scoring, player-controller
prediction, lag compensation.

## Consequences

**Positive.** The M7/M8 determinism investment pays off exactly as designed — destruction replicates
as events the size of messages, and the milestone's headline (everyone sees the same wall break) is a
property of the *model*, not of bandwidth. The transport/reliability layer is small, own-code, and
teachable. The `Link` seam keeps every networking proof deterministic and GPU-free, matching the CI
reality. The editor/streaming wire is untouched.

**Negative / costs.** A hand-rolled reliability layer is a correctness liability we accept
deliberately — mitigated by the scripted-loss harness being *the* first proof, not an afterthought.
Server authority means a single client experience always pays round-trip latency (no local-instant
destruction); acceptable for v1 and industry-standard. Snapshot replication of debris means far
clients see simplified rubble motion — that is the design (§5), but it must be *measured* to feel
right, which is m11.5's job.

**Follow-ups forced by this ADR.** `platform` grows a second socket kind (m11.1); the ROADMAP M11
section gains the ladder above; the glossary gains the networking vocabulary (NetId, snapshot,
relevancy, tick-tagging). M12's "Block" inherits a networked foundation rather than needing one
retrofitted — which was the point of sequencing M11 before M12.

## Alternatives considered

- **Deterministic lockstep (input-only networking).** Rejected: requires the cross-platform
  determinism ADR-0026 explicitly declines; one stalled client stalls all; joining mid-match requires
  full replay; no authority for anti-cheat. The event-replay *half* of §2 captures lockstep's
  bandwidth virtue for the one domain where we *can* prove determinism.
- **Pure snapshot replication (state-only).** Rejected for destruction topology: bandwidth scales with
  debris count, and it discards the deterministic function M8 already provides. Kept for dynamic
  state, where it is the right tool.
- **TCP everywhere (extend the S0 sockets).** Rejected: head-of-line blocking on state sync. Kept for
  tools/streaming where reliability matters more than freshness.
- **ENet / yojimbo / a QUIC stack.** Rejected for v1: the reliability layer is ~small, core to the
  engine's teaching mission (VISION #3), and QUIC belongs to the S2 internet-transport decision, which
  this ADR deliberately does not make.

---

## Amendment (2026-07-28, post-m11.1 review): ground-truth corrections the later bricks need

An independent codebase survey after m11.1 found that several bricks above assumed APIs and
properties that do not exist yet. ADRs are append-only, so rather than rewriting §2/§4/§6, this
amendment corrects the record and adjusts the ladder. Each item was verified against the code.

### A1. The replicated destruction unit is the committed damage-OP stream, not `apply_damage` calls

§2 says clients "replay the deterministic damage → detach function." The survey found a hole in
replaying *commands*: half the damage stream is **emergent, not commanded** — the runtime converts
the local solver's contact impulses into damage ops each tick (`damage.cpp`, the
`world.contact_events()` drain). A client replaying only explicit `apply_damage` calls would
diverge from the server the first time a debris pile erodes a part by resting on it. The
correction: the replication unit is the **committed, canonically-ordered damage-op list** (already
the deterministic unit ADR-0029 §"canonical order" defines) — the server runs the full
contact→damage conversion, commits the sorted op list, and replicates *that*; clients apply ops
and never run their own contact→damage conversion for replicated instances. The determinism
contract is unchanged (the op list was always the deterministic function's input); what changes is
naming precisely which artifact crosses the wire. m11.4 owns the op-list serialization.

### A2. `compute_type_hash` collides across types with identical field lists — fix before §4's handshake

The survey found a live collision in-tree today: `render::MeshAsset{uint64 asset}` and
`destruction::Destructible{uint64 asset}` hash identically because `compute_type_hash`
(`core/reflect/type_info.hpp`) folds field names/types/order but **not the type's own name** —
and both current resolvers take the first match. The §4 schema-hash handshake would inherit this:
two unrelated components with the same shape could be silently substituted. Fix (a pre-m11.3
micro-brick, owned by the replication work that depends on it): fold the type's registered name
into the fingerprint. This changes existing embedded hashes (cooked assets, `.rscene` files carry
them), so the brick must bump the affected format versions in the same commit.

> **Implemented 2026-07-29.** `compute_type_hash` now folds the registered type name; the seven
> cooked schema hashes and every checked-in fixture (`.rmesh`, `.rtex`, `.rsdf`, `.rskel`,
> `.rdest`, `.ranim`, `.rscene`) were regenerated, and `reflect_test`'s "identical shapes hash
> equal" case inverted. One deviation from the plan above: the **container/scene format versions
> were deliberately not bumped**. The hash *is* the compatibility gate, and a stale file already
> fails with a precise, actionable diagnostic — `schema hash mismatch (re-cook needed)` and
> `component '<name>' schema drift: file hash …, engine … — re-save the scene`. A version bump
> would replace those with a blunter `UnsupportedVersion` and would also reject files whose
> container framing never changed. Revisit only if the container layout itself changes.

### A3. Destruction/physics have no state-application API — m11.4 grows one

`grep set_body_state|spawn_at|allocate_at` over `engine/` returns nothing: `apply_damage` is the
only mutator on the destruction path. A client that joins late, or that the server must correct,
cannot be handed *state* ("these parts are gone, this debris is here") — only the event stream.
m11.4 therefore includes a **state-application seam**: apply a server-authoritative detach set
(topology correction, replaying the same body-swap code path) and set debris body states
(kinematics of last resort when snapshots and local sim disagree). It is used by late-join and by
drift correction; the common path stays event replay.

### A4. Interpolation is BUILT at m11.6, not reused

§6/§"interpolation" claimed ADR-0023 "already keeps previous/current snapshots." ADR-0023 says the
opposite: the previous-transform history buffer is explicitly a *documented, unbuilt seam* and the
interpolation alpha has no consumer. m11.6's scope grows accordingly: build the per-component
previous-tick history and the alpha-blended consume path, then wire snapshot interpolation onto
it. (This also un-blocks M12's render-side interpolation, so the cost was always coming.)

### A5. `Application` grows the ordered sim stage before net code needs it

ADR-0032 ruled that `Application` gains an *ordered* sim stage; today `on_fixed_tick` is a single
replacing slot. Networking needs an ordered place in the tick (poll inputs → apply remote ops →
sim → publish snapshots), so the ordered stage lands as part of **m11.2** (small, and the session
layer is its first multi-hook customer).

### A6. m11.7's shooter is a deterministic server-side script, not a player controller

There is no character controller, player, or weapon in `engine/`, and `post_input`/`frame_input`
have no production callers. The milestone proof does not need one: `samples/12-networked-
destruction` scripts its damage source **server-side** (a deterministic scripted match — timed
shots at named parts, so the whole run is hash-verifiable in CI), with clients as camera
observers whose input rides the m11.6 path for proof-of-flow. A real player controller + weapon
is M12 scope, where it always was. The ROADMAP's M11 ladder is updated to match A1–A6.

---

## Amendment (2026-07-29, m11.2): the session wire, and where the handshake actually lives

Two corrections the m11.2 implementation forced. Both change bytes on the wire, so m11.3 inherits
them and they belong in the record rather than in a code comment.

### A7. The handshake is connectionless, and every datagram carries an incarnation salt

**§3 is wrong where it lists the handshake as riding the reliable-ordered channel** ("events,
spawn/despawn, handshake — the destruction events of §2 live here"). A `ReliableChannel` is a
conversation with an *established* peer: sequence spaces, ack state, resend queues, a 256-message
backlog budget. None of that means anything before both sides agree a conversation exists, and
allocating one on first sight of an endpoint is precisely the unbounded-map DoS the handshake must
prevent — a single spoofed datagram would mint the heaviest object in the module.

The handshake, heartbeat, and disconnect are therefore **connectionless control packets** with their
own retry/timeout, and a channel is allocated **only on acceptance**. The validate-before-allocate
order is: frame parse → salt → identity triple (protocol version, app id, schema hash). A
wrong-build or wrong-game peer costs the server exactly one reply datagram and no state. Control
packets are discriminated from channel traffic by the high bit of the first payload byte (control
tags are ≥ 0x80; the m11.1 channel enum is 0..2), so the channel path is byte-identical to m11.1 and
`ReliableChannel` was not modified at all.

**The session wire gains a 4-byte incarnation salt on every datagram**, control and channel alike:

```
[salt:u32][payload...]        # channel traffic is therefore 17 header bytes, not 13
```

The hole it closes could not be seen at m11.1, because it only exists once sessions do. When a peer
dies and reconnects from the same address, its **old incarnation's packets may still be in flight**,
and the fresh `ReliableChannel` starts every sequence space at 0 — so a stale `seq 5` is
indistinguishable from legitimate early traffic and gets *buffered into the new stream*, silently
corrupting it. The salt (the client's and server's random halves, FNV-folded — not xor'd, since xor
maps equal salts to the reserved 0) stamps the incarnation on every datagram, and stale bytes are
dropped at the driver before they reach channel state. Reincarnation is detected by the same value:
a `ConnectRequest` from a known endpoint carrying the *same* client salt is a duplicate attempt (the
accept is idempotently re-sent, allocating nothing), while a *different* one means the peer re-rolled
it — only a fresh `connect()` does that — so the old session is reaped as `Replaced`.

One consequence worth stating: **reincarnation detection is only as good as the salt seed.** Salts
come from `NetDriver::Config::salt_seed`; a game that hardcodes it makes a restarted client
indistinguishable from a duplicate request. Seed from OS entropy at startup.

Not needed after all: `platform::Endpoint` does **not** grow `std::hash`/`operator<=>`. The routing
table is a linear scan over the slot vector — the same call `ScriptedNetwork` already makes for its
node table, and at `max_sessions` ≤ 64 it is noise next to the syscall that delivered the packet.

### A8. A5's ordered sim stage landed as three phases: `PreSim` / `PostSim` / `Publish`

`Application`'s single replacing `on_fixed_tick` slot is now the ordered stage ADR-0032 §8 ruled on:

```
PreSim → [Schedule] → [propagate_transforms] → PostSim → Publish
```

The phases are the *gaps around* the two steps the loop already owns, because those steps are
neither optional nor hooks. Networking is the first customer needing two at once (poll and apply
remote ops in `PreSim`, publish snapshots in `Publish`). Steps within a phase run in registration
order; `on_fixed_tick` survives unchanged as sugar for the one `PostSim` entry it owns, replaced in
place so its position among later-registered steps is stable — every existing caller is untouched.

The phase is deliberately named `PostSim`, not `PostPhysics`: it *is* where the physics bridge runs,
but `app` does not depend on `physics` (simulation-tick.md §1), and a stage must not be named after
a module its own module cannot see.
