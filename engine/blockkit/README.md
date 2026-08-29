# `rime::blockkit` — the vision demo's block, as content

The building prefab, the procedural assembly, and the palette that decides what the whole thing
looks like. It turns parameters into an ECS world — placements, roles, `Destructible` intents,
lights and a camera — which `rime::scene` then writes as a `.rscene`.

Built at **m13.2c** (ADR-0035 §1). `rime-blockgen --out block.rscene --stats` runs it.

## What it makes

8 buildings, 8 × 8 m footprint, 3 storeys of 3 m, down a 44 m street 12 m wide. Sixteen slabs per
building: 11 full walls, 2 half-walls (the split front ground wall, whose gap *is* the doorway) and
3 horizontal slabs. Plus 12 destructible crates, the street furniture, and the dusk lighting rig.

| | |
|---|---|
| destructible instances | 140 (128 slabs + 12 crates) |
| parts | 2,016 — against ADR-0035 §1's floor of 1,500 |
| structures | 8 — against a floor of 8 |
| local lights | 36 (24 interior points + 12 street spots) + the sun — against a floor of 32 |
| entities | 213 |

## Three things worth knowing before changing it

**The scene carries a role, not a material.** `render::MeshRef`/`MaterialRef` are *dense indices
into runtime registries*. Authoring them into a scene file is correct only while every loader builds
its registries in the identical order — insert one material at the front of the palette and every
entity silently shades as something else, with nothing failing. So the `.rscene` carries placement
plus `SlabRole{building, storey, kind, tint}`, and `apply_palette()` derives the look at load. The
same split `.rdest` already makes, and it means the entire appearance of the demo is
`palette.hpp` — re-tintable without regenerating the scene.

**Nine cooks, not five.** The naive prefab makes every wall the full 8 m footprint and every floor
the full 8 × 8, which leaves 0.3 × 0.3 corner columns and a 0.15 m lip where each floor meets its
walls. Static interpenetration is invisible while everything stands and is exactly wrong the moment
it breaks: overlapping debris hulls resolve their penetration by flinging apart. Side walls and
floors get shortened cooks (7.4 = 8 − 2 × 0.3) and butt cleanly.

**It is C++ and not Rust tooling, against the repo's own rule.** `.rscene` keys every component
record by its C++ reflection `type_hash`; a Rust emitter would have to reproduce those across a
language boundary, where a drift is an invisible mis-author rather than a clean load error.

## Layering

Depends on `ecs` (the world it fills), `render` and `destruction` (the components it writes —
interfaces only) and `core` (math). Deliberately **not** on `physics` or `rhi`: assembly is pure
data, which is what lets its proof run GPU-free on every CI OS and under both sanitizers. Nothing
depends on it — delete the directory and the engine still builds (guardrail 2).

Proofs: `tests/blockkit` — determinism, the `.rscene` round trip, the scale floors measured from the
*loaded* world, palette coverage (`tests/blockkit/blockkit_test.cpp`); the block standing in a real
physics world on two peers plus the peak-debris capability check (`block_standup_test.cpp`); and the
drawn frame (`block_render_test.cpp`, Vulkan only).
