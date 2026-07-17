# 10-destructible-wall — Milestone 8's "done when"

A cooked destructible wall stands on the ground, takes a hit, sheds part of itself as tumbling
debris, and drives **one** destruction event stream out to three systems that have never heard of
each other — a VFX dust puff, the null audio backend, and gameplay. It closes M8 the way
[`08-gltf-zoo`](../08-gltf-zoo) closed M6: the milestone ends in a runnable proof, not a compile.

## The whole M8 pipeline, in one sample

| stage | what happens |
|-------|--------------|
| **cook** | `rime fracture` seeds a Voronoi wall → a Destructible RMA1 asset (part hulls + a bond/anchor graph). A CTest fixture runs it first. |
| **stand** | `engine/destruction` registers the pattern **once** (each part a convex hull, the whole a compound) and spawns an instance: one static compound body (ADR-0029 §1). **Per-part render leaves** draw it — the leaves M8.2/8.3 deferred to here. |
| **break** | a blast severs the base seam; a support solve finds the unanchored remainder; the fracture **body swap** re-registers the anchored stump and spawns the detached slab as a dynamic body (ADR-0029 §2). |
| **fan-out** | one event stream (`PartDamaged`/`PartDied`/`IslandDetached`/`DebrisSettled`, M8.4) feeds the dust field, the audio log, and gameplay — destruction knows none of them (guardrail 2). |
| **settle** | debris fall and come to rest; the M8.5 lifecycle is armed to freeze settled rubble so the physics stores stay bounded. |

## Run it

```bash
# The headless self-check (the CI-gated done-when):
build/dev/bin/destructible_wall --headless [--cooked <dir>] [--ppm out.ppm]

# Stream it live over Track S0 (any key re-hits the wall); view with 04-remote-view:
build/dev/bin/destructible_wall --serve [--host 0.0.0.0] [--port 9100]
```

## What the self-check proves (exit 0 = M8 green)

The core is **GPU-free** — the destruction simulation is verified with no device, so it runs on every
CI OS; the pixel proof runs only where a Vulkan device is present (lavapipe on Linux CI).

- **the wall breaks:** parts die (health → 0) and **≥ 1 island detaches** as a real dynamic body;
- **the debris settles:** the world comes fully to rest (`awake_bodies → 0`) within the tick budget;
- **the fan-out fires:** all three consumers react off the one event stream — the dust bloomed, the
  audio log recorded impacts, and gameplay tallied the deaths;
- **it's deterministic:** the whole run is bit-identical (`state_hash` + `world_hash`) on a re-run and
  across 1/2/4 physics worker counts — the M11 replay contract;
- **it renders:** the intact wall draws lit through PBR, and the break visibly repaints the image
  (the dropped slab + tumbling debris move into new pixels).
