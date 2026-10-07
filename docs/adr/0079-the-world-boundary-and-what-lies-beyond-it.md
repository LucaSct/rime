# ADR-0079: the world boundary — three shells, one predicate, and extraction rather than a wall

- Status: **Provisional** — the rules are decided; every byte and millisecond below is an estimate
  until a cooked world and a pinned run say otherwise
- Date: 2026-10-07

## Context

The owner's requirement: **terrain must exist everywhere the player can look, including when they
leave the playable area by cheating or by accident, and maps should be large enough that leaving is
not reachable in the first place.**

Measured 2026-10-07: there is **no kill plane, no world-bounds check and no respawn** anywhere in
the samples, and nothing is drawn below the horizon, so a player who walks off the street falls
forever through dark atmosphere. Separately, `samples/99-the-block` references the terrain system
**zero times** — the vision demo is a street in a void — while M19 ships a terrain system that
streams a 4 km world with a six-level LOD chain at a measured 300 m/s travel envelope (ADR-0073).

### The second half of the requirement is arithmetically false, and that is the starting point

Three models were consulted independently (`gpt-6-astra` via `codex`; `claude-opus-5-5` and
`claude-fable-5-1` via devpass). **All three rejected "large enough to be unreachable", with
consistent arithmetic:**

| from the centre of a 4 km square, the nearest edge is 2 km away | time to reach it |
|---|---|
| on foot, 5 m/s | ~400 s |
| vehicle, 30 m/s | ~67 s |
| aircraft, 150 m/s | ~13 s |

To make a straight-line escape take a 30-minute match, the world would have to be roughly **18 km**
for infantry, **108 km** for vehicles and **540 km** for aircraft. At 2 m spacing, 4 km of 16-bit
heights with mips is about **10 MiB**; 540 km is about **181 GiB** — heights alone, before
materials, collision, buildings or duplication. Only foot traffic can be contained by size.

**So size buys margin, not unreachability, and the edge must be designed regardless.** The first
half of the requirement — terrain everywhere the player can look — is entirely achievable and is
what the shells below deliver.

## Decision

### Three shells

- **Shell A — the playable hull.** Cooked, destructible, fully simulated. Recommended **4–6 km** for
  mixed infantry and vehicles: enough that infantry never meets the edge within a match.
- **Shell B — the buffer band**, roughly 1–2 km beyond the hull. Real streamed cooked terrain at
  coarse LOD, drivable, **non-destructible**, and deliberately empty: no spawns, no cover, no
  objectives. The band *is* the penalty — there is nothing in it to exploit.
- **Shell C — the horizon skirt.** A **procedural, non-collidable** heightfield from a seed,
  extending past the horizon, lit by the physical sky only. Zero cooked bytes.

**On Shell C being procedural:** one consult rejected procedural terrain outright as "a determinism
risk for no gameplay gain". That objection is correct for *simulated* procedural terrain and does
not apply here, because Shell C is **non-collidable and never enters the simulation** — no entity
stands on it, nothing collides with it, and no destruction touches it. It is scenery. The
alternative, a cooked far-field, was costed at about 13 MiB permanently resident; the procedural
skirt costs a few MiB of generated LOD and no disk. Horizon distance from 100 m altitude is about
36 km and from 500 m about 80 km (sqrt(2Rh)), and covering 80 km of cooked far-field at 64 m spacing
would be roughly 40 MiB — nearly half the residency budget, which is why it is generated.

### One predicate

An entity is **exterior** when:

```
tile_coord is outside (hull union band)   OR   height < world_min - 50 m
```

Exterior entities are removed from the simulation on that tick. **That single predicate is the kill
plane, the world-bounds check and the debris cull**, and because it is a pure function of
deterministic simulation state evaluated identically on every peer, and the removal is folded into
the destruction-state hash, the boundary is **not a second source of truth — it is part of the
truth**. Comparisons are made in **integer tile coordinates**, never in floats, to avoid platform
divergence.

**The floor is absolute, not relative.** It sits below the cooked world minimum, not below the
terrain height at the entity's own position. A relative "below the terrain at my (x, z)" test
**seals destruction**: it would invalidate legitimate craters, basements, cooked tunnels and the
holes our headline feature exists to create. This was the sharpest warning of the three consults and
it is recorded as a rule, not a preference.

### What happens at the edge: extraction (the owner's decision, 2026-10-07)

Three designs were offered — kill-and-respawn, teleport-to-spawn, and extraction. **Extraction is
chosen.** Crossing the band's outer boundary commits an extraction: combat participation is removed
first, then **ordinary death and respawn timing and scoring apply**. Inward steering assistance acts
before the line, sized from each vehicle's stopping and turning envelope.

Why this over the other two, recorded so it is not relitigated:

- **No countdown sanctuary, no healing shortcut, no instant relocation to advantageous ground.** A
  teleport primitive would make crossing the line a fast-travel button and would need its own
  anti-abuse rules.
- **Accidental approaches stay recoverable; deliberate escape has a predictable cost.** A plain kill
  punishes an aircraft that overshot a turn at 150 m/s — a flying mistake, not a cheat — in exactly
  the case most likely to happen by accident.
- The cost accepted: it is the most machinery of the three, and the steering assistance needs
  playtesting rather than derivation.

### The band is destruction-inert

Inside Shell B, weapons cannot damage cooked buildings and debris that enters the band is deleted.
So the edge **cannot affect the destruction hash at all**, and it **removes** simulation load rather
than adding it — which matters while `sim.block` p99 is 8.328 ms against a 6.0 ms budget (ADR-0077),
over, and the frame's limiter.

### Authority

The exterior test is evaluated **only in the fixed-point simulation, on the server-authoritative
position** — never on interpolated render positions. Clients predict the warning UI only; extraction
and respawn are server events, so a client that skips its own countdown is extracted anyway. The
client never reports "I am in bounds"; the server computes it. Retiring debris never restores its
destroyed parent.

## Consequences

- `samples/99-the-block` must be placed on streamed terrain. A street in a void contradicts the
  requirement, and this is the first brick.
- Residency is camera-local, so near-LOD resident bytes do **not** grow with world area; far LODs
  do, which is why the horizon is generated rather than cooked. Cooked **disk** bytes grow with
  area: each doubling of world width costs 4x.
- The band's coarse terrain is estimated at 10–15 MiB of the 96 MiB residency budget. Unmeasured.
- The exterior predicate adds a point-in-polygon test per entity per tick — negligible against the
  8.3 ms slice, and the debris cull should return more than it costs.
- **The likeliest failure is the A/B/C seams**: height, material and lighting discontinuity where
  cooked terrain meets procedural, visible from altitude as a ring. Budget a seam-blend pass over
  the outer ~500 m of Shell B and test from 500 m before anything else. The runner-up failure is a
  mismatch between visible ground, collision and recovery volumes after a collapse.
- Nothing here closes M18 gate 7.

## What was checked

- No bounds, kill plane or respawn exists: no such symbol in `samples/99-the-block/main.cpp` or
  `samples/hello-game/`.
- The block never references terrain: zero matches for `terrain` outside an SSR comment.
- Terrain capability: ADR-0073's 4 km world, six LOD levels, 300 m/s envelope, 96 MiB residency.
- Over-budget simulation slice: ADR-0077's table, `sim.block` p99 8.328 against 6.0, "still over".
- The escape-distance arithmetic above was produced independently by three models and the figures
  agree to within the precision quoted. It is arithmetic on our own speeds, not a measurement of
  anything.

Every byte and millisecond here is an **estimate**. No cooked world of this shape exists yet.
