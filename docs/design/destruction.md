# Destruction — design note (M8)

Companion to `engine/destruction` (M8) and the fracture cook in `tools/asset-pipeline`. Like
[`physics.md`](physics.md), this is the running design record — it grows a section per brick, in the
order the bricks land. The *decisions* live in [ADR-0029](../adr/0029-destruction-model.md) (and the
physics substrate in [ADR-0026](../adr/0026-physics-core.md)/[0027](../adr/0027-convex-hull-shapes.md)/
[0028](../adr/0028-compound-shapes.md)); this note covers the *systems* reasoning — the pipeline, the
data, and the trade-offs — and the math derivations get their own notes as they land.

Current coverage: the model (M8.0, this note's overview + ADR-0029), the fracture cook (M8.1), the
load/stand/bind runtime (M8.2), and damage/connectivity/fracture (M8.3 — the wall breaks). The event
fan-out (M8.4), lifetime/budgets (M8.5), and the proof sample (M8.6) each add a section as they ship.

## The model (M8.0)

Destruction in Rime is **not** a special-case physics stage — it is a consumer of the rigid-body core
that M7 built to be its substrate. The whole pipeline is a chain of shipped seams:

```
  cook (Rust, offline)            runtime (C++, engine/destruction)
  ─────────────────────           ─────────────────────────────────────────────────────
  source mesh                     load Destructible asset
    → seeded Voronoi parts          → register_hull per part, register_compound the whole   (once/pattern)
    → convex part hulls             → stand an instance = ONE static compound body
    → bond + anchor graph           → accumulate contact impulse (+ explicit ops) → per-part health
    → per-part render meshes         → part dies → connectivity solve (union-find over live bonds)
    → Destructible (RMA1)            → unsupported parts detach → BODY SWAP → debris (hull/compound bodies)
                                     → one event stream (PartDamaged/PartDied/IslandDetached/DebrisSettled)
                                       fans out to VFX + audio + gameplay
```

The load-bearing ideas, each pinned in ADR-0029:

- **Intact = one static compound body.** A fracture *pattern* (its parts + graph) is cooked once and
  registered once (ADR-0028's compound *is* the intact destructible, ADR-0027's hull store *is* the
  debris shape economy). Instances share the registered geometry by id.
- **Damage is contact impulse.** `ContactEvent.normal_impulse` is the solver's summed ∫F·dt, and
  `child_a`/`child_b` already name the struck part (M7.9/M7.12). Above a cooked threshold (which
  fences the `m·g·dt` resting-support case) it is damage to that part. A CCD projectile's arrest
  (M7.10) is the same signal; an explosion is `overlap_sphere` + a radius→part damage op + an
  `apply_impulse` push (M7.7).
- **Fracture is a body swap.** Compounds are immutable, so a broken wall is *replaced*: destroy the
  instance body, re-register the anchored remainder, and spawn each detached island as a new dynamic
  body (a hull for one part, a runtime compound for several). This is why the hull/compound stores
  must learn to release ids (M8.5).
- **Determinism, end to end.** The damage → fracture path is a deterministic function of its inputs
  (canonical op order, ascending-part-id remainders, `world_hash`-stable free-list ids) — the exact
  contract M11 networked destruction replays events against.
- **One event stream, many listeners.** Health transitions publish through a generic double-buffered
  `EventChannel<T>` in canonical order (the M7.9 "data, not callbacks" discipline, one layer up); VFX,
  audio, and gameplay subscribe independently. M8 ships the seam plus a dust stub and a null-audio
  backend — the real systems are Track FX and `engine/audio` later.

See ADR-0029 for the eight decisions and their rejected alternatives; the brick sections below fill in
the how as each lands.

## The fracture cook (M8.1)

The cook is offline tooling (Rust, `tools/asset-pipeline`) that turns a convex source — v1: a box
(wall, column, slab) — into the `Destructible` RMA1 asset the runtime loads. The partition is a
**seeded Voronoi diagram** clipped to the box: each cell is the intersection of half-spaces (the six
box faces plus one bisector per other site), so every part is **convex by construction** — no
quickhull needed to *generate* it (quickhull's robustness role only appears when a future brick
fractures a non-convex mesh). Cell vertices come from plane-triple intersections kept inside every
half-space; each face is the CCW loop of the vertices on its plane — exactly the CSR shape
`register_hull` validates. Volume and COM follow from the divergence theorem, so a part's mass fraction
is its volume fraction under uniform density; two cells sharing a face are **bonded** (strength ∝ the
shared area); a part touching the cook's anchor plane is an **anchor**. The full derivation is in
[`docs/math/voronoi-fracture.md`](../math/voronoi-fracture.md); the asset byte layout is in
[`assets.md`](assets.md). The done-when is the cross-language oracle: every cooked part registers into
a real `PhysicsWorld` (the physics validator *is* the cook's acceptance gate), with the summed hull
volumes matching the source box.

## The runtime: load, stand, bind (M8.2)

`engine/destruction` is born here — a removable feature module (guardrail 2) depending only on core,
ecs, physics, and assets. Its seam is `DestructionWorld`, and M8.2 is the "load, stand, bind" third of
destruction: a cooked pattern becomes standing physics. Damage and the fracture body-swap (M8.3), the
event fan-out (M8.4), and lifetime (M8.5) grow this same class.

### Patterns and instances: the shape economy

`register_pattern(asset, world)` is the **cold path**. It walks the cooked parts, `register_hull`s each
one (the cook already emitted COM-centred CSR geometry that passes the hull validator — the M8.1 oracle
proved it), and `register_compound`s the lot, each child the part's hull at its cooked COM (identity
rotation — an intact part is not rotated relative to its destructible). One asset → one `PatternId`,
holding the compound id, the per-part hull ids and COMs (kept for M8.3's fracture, which re-registers
surviving subsets), and the cooked bonds/anchors/damage material. If any hull or the compound is
rejected, the whole pattern is rejected with no partial registration — a malformed cook fails loudly.

`spawn(pattern, placement, world)` is the **hot path**, and it is deliberately cheap: one
`create_body` for a **static compound** at the placement, plus two small per-part vectors (health = 1,
alive = true). A pattern registers once; a wall of a hundred instances shares that one hull set and
compound — the ADR-0027/0028 shape economy, realized. The intact body being *static* is what makes a
standing destructible free: it never integrates or solves, so `WorldStats` over a scene of untouched
walls reads zero awake bodies and zero active islands (the M8.2 "≈ static baseline" proof). It
participates in collision only when something hits it — which is exactly how M8.3 will hear damage.

### Per-part state, and why it is here now

Each instance carries a per-part `health`/`alive` SoA. On M8.2 it is inert (everything alive, full
health) — the point is to settle the **state model and its accessors** (`part_alive`, `part_health`,
`part_placement`) before M8.3's damage logic stands on them, the same reader-before-writer discipline
the asset formats followed. `part_placement` carries a part's cooked COM through the instance
transform — the hook a per-part render leaf will draw at (M8.6).

### What M8.2 defers, honestly

The ADR-0029 §5 **render-leaf entities** (and the intact-wall pixel proof) are *not* built here: M8.2
is GPU-free, so it runs on every CI OS and under the sanitizers with no device. The physics proof — a
raycast hits the intact wall's face exactly where a plain static box's would (x = +1.0 for a 2 m wall)
— is the stronger *correctness* statement that the compound stands right; the visual binding lands with
the M8.6 sample, where a camera and render path exist. The ECS `Destructible` component is reflected
here (the authoring surface), but the full author-a-Destructible-entity → instance bind system also
waits for the sample; M8.2 drives spawning through the `DestructionWorld` API directly.

### Proofs

`tests/destruction/world_test.cpp`, pure CPU, loading the committed `wall.rdest` fixture (shared with
the assets oracle) into a real `PhysicsWorld`: the pattern's part/bond/anchor counts survive the round
trip; the intact wall stands as one live static body and a **raycast hits its +X face at x ≈ 1.0** (a
ray over the top misses); a stepped scene of the bound wall stays at **zero awake bodies / zero active
islands** and hashes identically across two runs (deterministic — the M11 contract); `part_placement`
matches `core::transform_point`; unknown ids are safe no-ops and a **degenerate part is rejected**; and
25 instances share one registered pattern (the shape economy, ASan-covered). `components_test.cpp`
proves the `Destructible` component reflects and registers. Green under dev + ASan/UBSan + TSan.

## Damage, connectivity & fracture (M8.3)

M8.3 is where the wall breaks. Three systems land in `engine/destruction/src/damage.cpp`, all
running in the **sequential tail** of the tick (`DestructionWorld::update(world)`, called once,
after `PhysicsWorld::step` — ADR-0029 §8), plus one physics seam (`RayHit::child`, so hitscan can
name the struck part exactly as contact events already do).

### The damage pipeline: two sources, one canonical op list

Damage reaches a part from two directions, and both reduce to the same normalized currency — a
per-part *damage op* (instance, part, amount, carried impulse) — before anything is applied:

- **Contact events.** `update()` drains `contact_events()`; each region's `BodyId` resolves back
  to an instance through a sorted body→instance table (a binary-searched vector — no hash map
  anywhere near the damage path), and `child_a`/`child_b` name the part through the instance's
  child→part remap. Damage is `(normal_impulse − damage_threshold) · damage_scale`, the cooked
  pattern material; the threshold is what fences the resting-support case (a standing wall's own
  parts exchange `m·g·dt` every tick as `Persisted` events, and that must never erode them). The
  op carries the event's impulse along the side's shove direction (`−normal` for a, `+normal` for
  b) at the contact point.
- **Explicit ops.** `apply_damage(instance, point, radius, amount, impulse)` queues a radius op
  (explosions, scripted hits; hitscan = a small radius at the `RayHit::child` part). At update
  time it expands against the cooked part AABBs carried through the instance placement, with
  **linear falloff** (full at the centre, zero at the rim — the ratified v1 shape). Explicit
  pushes are applied *centrally* on detach — a blast centre is usually outside the debris body,
  and an invented lever arm there would add spin nobody asked for.

The combined list is applied in ADR-0029 §3's **canonical order**: explicit ops sorted by
(instance, part, op bytes — floats compared as raw bit patterns, a total order), then contact ops
in the event stream's already-canonical order. One fixed sequence ⇒ the float accumulation into
health is bit-reproducible regardless of how the inputs arrived; the determinism test feeds the
same tick's blasts in permuted arrival orders and demands identical hashes.

### The support solve: union-find from the anchors

When a part's health reaches zero it is **killed** (`alive = 0`) and its instance is marked dirty;
a killed part leaves the wall as its own debris chunk (the body swap, below). Per dirty instance, a
**union-find**
(disjoint-set) pass runs over the *live* bond graph — a bond holds only while both endpoints still
stand — seeded from the still-standing anchors. Union-find is made deterministic by one rule: a
component's representative is its **smallest part id** (unite attaches the larger root under the
smaller), so the partition is a pure function of the alive bits and the cooked bond list. One
ascending scan then splits the standing parts into the anchored **remainder** (ids ascending —
which *is* the next child→part table, ADR-0029 §4) and the unsupported **islands**, grouped by
root. The islands plus each part killed outright this tick (each a single-part group, ADR-0029 §2)
are the **detached groups**; sorted by smallest member — they are disjoint, so that is a strict
total order — they give the canonical debris creation order. v1 deliberately re-solves
the whole instance rather than solving incrementally — see the numbers below for why that is the
right trade.

### The body swap, without pops

A registered compound is immutable (ADR-0028), so fracture **replaces** the standing body
(ADR-0029 §2): destroy it, `register_compound` the remainder, stand a fresh static body; each
detached group becomes a dynamic body (one part → its hull, several → a runtime dynamic compound —
a multi-part island keeps its shape, the Frostbite look). The placement recipe is the whole trick:
`register_compound` re-centres child poses on the subset's combined COM, so the new body goes at
**`placement ∘ centroid`** with children authored at their cooked COMs — which lands every part
exactly where it stood before the swap. `spawn()` uses the same recipe for the intact body, so
intact and post-fracture placement are one invariant, proven by raycasting the same face before
and after a break (translated *and* rotated placements) and by checking a newborn debris body
sits at its part's pre-break `part_placement`. Debris inherit the parent's motion at their new COM
(`v + ω × r` — zeros for today's static walls, written generally) plus the damage impulses that
struck their member parts, applied in canonical op order. A directly-killed part flies off as its
own single-part chunk carrying the very impulse that felled it (ADR-0029 §2 — the struck piece
leaves the wall); an orphaned island carries whatever ops hit its members. Either way the push is
the accumulated applied impulse, so momentum is conserved and never doubled (an op landing on
already-dead rubble is skipped).

Dead parts must stop colliding, so the swap runs whenever the **membership changed** (any death),
not only when an island detached — a shot hole in a wall is a real hole: the rebuilt compound no
longer occludes there (the freed chunk, born in place, is a separate body that tumbles away).
A tick of mere resting contacts changes nothing and swaps nothing. The old compound (and hull)
ids are deliberately leaked: the stores are append-only until m8.5's `unregister`
(ADR-0027/0028's recorded deferral).

### Determinism: the M11 witness

`state_hash()` fingerprints all destruction state field by field (never a padded struct) in one
canonical order — every instance's body id, alive bits, health values; every debris body's
identity and composition — and pairs with `PhysicsWorld::world_hash()`. The headline test runs a
scripted 8-blast, 180-tick collapse of the cooked 100-part wall and demands the hash pair (plus
the full island compositions) be **bit-identical** across: two runs, physics on 1/2/4 job-system
workers and sequential, and permuted same-tick op arrival. That is the exact contract M11.4
replays events against.

### The numbers (Release, dev box, 16-core, single-threaded update)

Worst-case single-part loss on the 100-part wall — one update() doing the full 100-part
union-find, the death bookkeeping, and the swap that re-registers a 99-child remainder compound —
and the bare runtime `register_compound` at that scale:

| measurement | cost |
| --- | --- |
| `update()`: kill 1 of 100, re-solve + swap 99-child remainder | ~20 µs |
| bare `register_compound`, 100 hull children | ~4.4 µs |

(Debug builds: ~248 µs / ~30 µs.) The full re-solve + re-register at 100 parts costs ~0.1% of a
60 Hz tick, so ADR-0029 §2's per-part-statics fallback stays unexercised and the incremental-solve
seam stays a design note, not code. Printed by the `M8.3-COST` test tag on every run for
drift-watching.

### Deferred from this brick, honestly

- **Event emission** (PartDamaged/PartDied/IslandDetached) — m8.4's `EventChannel<T>`; today the
  break emits nothing and physics is the only listener.
- **Damage to detached debris** — deferred entirely to m8.5 (with lifetime/budgets): debris bodies
  are not in the body→instance table, so contacts on them are ignored. Keeping v1's deterministic
  core small won over "single-part debris splits" from the kickoff notes.
- **Wake-on-swap**: a *sleeping* body resting against a wall whose part is destroyed under it is not
  explicitly woken (it would hang mid-air until touched). No M8.3 scenario hits it; the honest
  fix (wake bodies overlapping the swapped body's bounds, in canonical order) belongs with m8.5's
  lifetime policy.
- **Density** is a single engine constant (1000 kg/m³) applied to cooked part volumes; it joins
  the cooked damage material when a real material system lands.

## Event fan-out (M8.4)

A break is interesting to more than the physics: it wants a puff of dust, a crack of sound, a score
tick. M8.4 gives destruction a **data event stream** — never callbacks fired mid-solve — that any
number of systems read *after* the tick, none of them known to destruction.

### The channel

`core::EventChannel<T>` (new, in `rime/core/containers/`) is the M7.9 pattern generalized: a producer
`push()`es typed events, `publish()`es once at a tick boundary, and consumers read the published
batch as a stable `view()` span until the next publish. It is **double-buffered** (two vectors,
swapped on publish) so a consumer can hold the span across the whole post-tick fan-out while — in a
later threaded world — the next tick already begins filling the other buffer; the swap keeps
`publish()` O(1) and allocation-free once warm. The channel imposes **no ordering of its own** — it
hands events back in `push()` order — precisely so it cannot perturb the producer's canonical one.
Physics keeps its own private spans; this is the shared vocabulary for everyone above it (fx,
lighting, audio will each instantiate their own `EventChannel<their-event>`).

### The four events

`DestructionWorld::update()` publishes a `DestructionEvent` stream (`rime/destruction/events.hpp`):

- **PartDamaged** — a part took damage this tick and still stands (`magnitude` = health removed).
- **PartDied** — a part's health hit zero this tick; it leaves as its own debris chunk (ADR-0029 §2).
- **IslandDetached** — a group of *still-standing* parts lost support and broke free as one debris
  body (`body` names it; `magnitude` = the impulse it flew off with). A killed part's own chunk is
  **not** an IslandDetached — that death is already a PartDied, so the two never double-count (killed
  parts are dead, islands are alive: disjoint sets).
- **DebrisSettled** — a debris body came to rest, straight off a physics `Slept` event (M7.9 shipped
  this for exactly this purpose). The hook m8.5's lifecycle (settle → linger → freeze) hangs on.

Every payload carries a **world-space AABB** (`world_bounds`) — the M10-C2 hook: a lighting or
culling consumer keys off *where* a break happened without re-deriving it from part ids and a
placement it cannot see.

### Order is canonical, so the stream is replay-stable

Emission order is fixed: settle events first (from the step that just ran), then damage events per
part in ascending id (the stage-2 op runs are already sorted by `(instance, part)`), then detachments
per island in creation order (smallest member first). No unordered container touches the path. So the
event stream is a pure function of the tick's inputs, exactly like the fracture it narrates — the M11
replay contract, extended to the fan-out. A quiet tick publishes an empty frame (the channel is clean
every tick that broke nothing).

### The consumers (and why they're removable)

Three listeners read the one immutable span, none aware of the others:

- **VFX dust** (`engine/vfx`, `DustField`) — a small, deletable CPU particle field: a PartDied /
  IslandDetached blooms a puff filling the event's `world_bounds`, which then drifts and fades. It is
  a *stub* in the honest sense (track fx1 replaces the whole module); it is capped at a fixed budget
  and fully deterministic, so it unit-tests with no device.
- **Audio** (`engine/audio`, `AudioBackend` + `NullAudioBackend`) — the newborn audio seam: one call,
  `play(sound, position, gain)`. v1 ships only the null backend, which *logs* calls instead of making
  sound, so a headless test asserts the break played the right things. Track au1 swaps a real mixer in
  behind the interface without touching a call site.
- **Gameplay** — a score/tally, demonstrated in the test.

Because each reads the same const span and destruction knows none of them, the fan-out is
**guardrail-2 removable**: the test's *removability drill* runs a break with all three listeners, then
again with VFX dropped, and asserts the audio + gameplay observations are byte-identical — no
listener's output can depend on another being present.

### What M8.4 defers, honestly

The dust's **actual GPU draw** (an additive billboard pass after PBR, before tonemap) and its
coverage-delta **pixel proof** land with the **M8.6 sample**, where a device and a render path exist —
the same GPU-free discipline M8.2/M8.3 followed. What lives here is the *simulation* (spawn, drift,
age, retire, capped, deterministic) and a CPU **coverage proxy** (Σ size²·alpha) that the m8.6 pass
confirms on-screen: it jumps on a burst and decays to zero as the puff ages out. Keeping the stub
GPU-free is deliberate — the plan's own risk note is "keep it deletable".

## Lifetime & budgets (M8.5)

## Lifetime & budgets (M8.5)

*(pending — debris lifecycle, `WorldStats`-driven budgets, hull/compound `unregister`.)*
