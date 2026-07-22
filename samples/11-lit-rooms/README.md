# 11-lit-rooms — Milestone 10's "done when"

The whole advanced-lighting stack on **one** scene, and opening a wall visibly changes the light in
the room behind it. It closes M10 the way [`10-destructible-wall`](../10-destructible-wall) closed M8:
the milestone ends in a runnable proof, not a compile. It is the first place every M10 technique runs
together in one frame.

## The whole M10 stack, in one frame

| technique | what it does here |
|-----------|-------------------|
| **CSM directional shadows** (m10.1) | the sun, shadowing the divider through cascades — the shadow this scene lifts. |
| **local spot shadows + cache** (m10.2) | a warm lamp over a pillar, its shadow map cached and re-rendered only when its region is invalidated. |
| **clustered forward** (m10.3) | the point lights, culled into froxels rather than a fixed uniform-block loop. |
| **SDF clipmap + DDGI probes** (m10.4/5) | the global-illumination field, sphere-traced through the SDF the geometry registers. |
| **SSR** (m10.7) | a reflective floor mirroring the lit room, falling back to the probe field where the screen cannot see. |

## The beat (ADR-0032's headline)

A dividing **wall** stands across the floor; a raking sun throws its hard shadow over the strip in
front of it, sealing that strip from direct light. **Break the wall** and — in the same handful of
frames — the shadow lifts: direct sun floods the strip, its CSM shadow is gone, the DDGI bounce
updates, and the reflective floor picks it all up.

The wall "breaks" through the honest **destruction↔lighting seam** M10 built (ADR-0032 C2): its SDF
twin and shadow-caster region are dropped and the lighting caches are invalidated — exactly the hooks
a real M8 destruction event drives ([`10-destructible-wall`](../10-destructible-wall) wires the full
physics version). This sample isolates the *lighting* response, so the wall opens on a script/keypress.

## Run it

```bash
# The headless self-check (the CI-gated done-when):
build/dev/bin/lit_rooms --headless [--frames N] [--ppm out.ppm]

# Stream it live over Track S0 (any key drops the wall; it auto-drops after ~3 s otherwise):
build/dev/bin/lit_rooms --serve [--host 0.0.0.0] [--port 9100]
```

## What the self-check proves (exit 0 = M10 green)

It renders the **full stack** (all six gates on at once — a black frame or a crash would mean two
techniques conflict), breaks the wall, and asserts the **integrated thesis**:

- **it renders lit** — the whole M10 pipeline composes into a lit frame;
- **the dark strip lights up** — the shadowed near floor brightens materially once the divider falls
  (measured ≈ 2.3× on lavapipe: direct sun through the gap, the CSM shadow lifting, and the DDGI
  bounce arriving);
- **the break repaints the frame** — the wall's removal and the relight move a large fraction of the
  pixels.

The run is a skip (exit 0) where no Vulkan device exists; Linux CI's lavapipe runs it for real. The
**rigorous, isolated** GI-mechanism proofs — the pure-bounce rise measured in HDR, Chebyshev leak
guard, destruction-reactive hysteresis — live in [`tests/render/gi_thesis_test.cpp`](../../tests/render/gi_thesis_test.cpp);
this sample is the everything-on-at-once integration and the lived demo. Absolute frame timings wait
for real hardware (m12.0) — lavapipe renders on the CPU, so its cost is not representative, but the
geometry of every technique is provable there.
