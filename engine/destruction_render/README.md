# `rime::destruction_render` — per-part render leaves

The bridge that turns a `DestructionWorld`'s parts and debris into entities the renderer draws.
Built at **m13.2d**.

## The gap it closes

A destructible's *parts* are what you see: 12–28 convex chunks per slab, standing in the intact
compound, then flying off in a debris body, then frozen where they landed. Nothing in the engine
ever turned that into something drawable. `destruction/world.hpp` says so outright — *"per-part
render leaves land with the [sample]"* — so `10-destructible-wall` hand-rolls its own
`build_leaves`/`refresh_leaves` against one instance of 60 parts.

Fine for one wall. Not a strategy for m13.2c's block: 140 instances, 2,016 parts, with m13.3 and
m13.5 each about to copy the loop.

## How it works

**One mesh per (pattern, part) — not per (instance, part).** This is the whole reason the block is
affordable: its 140 instances share 9 cooked patterns, so the geometry is ~148 part meshes uploaded
once and instanced 2,016 times by transform alone. Per-instance upload would mean 2,016 vertex
buffers of identical data.

A leaf's pose comes from whichever of three states its part is in, and the three are the
destructible's whole visual life cycle (ADR-0029 §8):

- **standing** — the part's placement in its instance's compound;
- **detached** — its debris body's pose, composed with the part's offset within that island (about
  the island's *volume-weighted* combined COM, since that is what `register_compound` re-centres on);
- **frozen** — left exactly where it was. m8.5 destroys a settled debris body but keeps the roster
  row, because a render leaf outliving its physics body at its last pose is the cheap way to keep
  rubble on screen.

A fourth state ends the leaf: **retired**, when m13.2b's visual budget (ADR-0032 C6) evicts the
rubble. The leaf is *despawned*, not hidden — a hidden leaf still costs an extraction visit and a
frustum test every frame, which is the leak C6 exists to stop.

## Layering, and the GPU seam

`destruction` must not depend on `render` (it sits below it and is proven GPU-free), and `render`
must not depend on `destruction` (a renderer that knows what a fracture is has lost the seam). So
the glue is its own module depending on both — the guardrail-2 argument that produced `replication`
and `destruction_net`. Nothing depends on this one.

`register_pattern(..., meshes = nullptr)` is a designed seam, not a convenience: `MeshRegistry` owns
an `rhi::Device&` and uploads on add, so minting a `MeshId` needs a GPU while the part COMs, the
leaf entities and their poses do not. That is what lets the leaf life cycle be proven headless
(`tests/destruction_render`) on every CI OS and under both sanitizers, leaving only "the pixels are
there" to the device proof (`tests/blockkit/block_render_test.cpp`).

Every field of `LeafStats` exists because the failure it names is otherwise invisible — in
particular `instances_without_meshes`, which at block scale is an entire building that is quietly
not there.
