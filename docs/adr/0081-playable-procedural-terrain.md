# ADR-0081: bounded playable procedural terrain in the block

- Status: Accepted for the `99-the-block --play` prototype
- Date: 2026-10-14
- Supersedes: ADR-0079's non-collidable procedural skirt and extraction policy **for this sample**;
  that ADR's competitive game boundary remains a separate design.

## Context

The owner walked off the authored street and requested generated terrain, a distant horizon and
bounded memory and disk use. Independent Claude Opus and GPT-6 Astra consultations both supported
streaming a seeded world around the player. They differed on extending the large cooked-world
pipeline versus connecting the existing terrain draw, residency and collision interfaces directly.
The latter is the smaller sample integration; both approaches still need coordinate rebasing for
unlimited travel. The owner chose playable streaming first, with safe recovery at the practical
coordinate limit.

## Decision

Generate heights from global integer metre coordinates, an explicit seed and a versioned integer
noise function. Adjacent tiles and coincident samples at different LODs agree exactly. No generated
tile is written to disk. The original street remains the authored collision floor: generated
terrain sits below its slab and blends into an outer flat band, then hills.

Install collision through `terrain_collision::TileSource` independently on both simulation peers.
Pin the original block and demand support for all current physics shapes, including sleeping bodies
and debris, plus the player's next-step travel envelope. Both collision barriers run on the main
thread before gameplay, prediction or the parallel physics steps. A refused barrier stops progress
before consuming input. Each source has 256 reusable payload slots, and departed collision records
and payloads are released.

Draw through `TerrainPass`, not the add-only mesh registry. Six nested LOD squares hold 304
non-overlapping tiles, with coarse tiles omitted where finer tiles cover them. The ring covers at
least four kilometres around the eye; play's far plane is 3072 metres. The terrain stage participates
in HDR, depth, material/normal G-buffers and camera motion vectors before sky, reflections and TAA.
Its direct lighting uses the scene sun's RGB radiance.

Retain unchanged GPU tiles. On a changed ring, wait for submitted readers before reclaiming outgoing
slots and uploading replacements. This is deliberately synchronous: no per-frame wait on unchanged
centres, no claim of hitch-free asynchronous streaming. The measured ring uses about 7.8 MiB of tile
resources; hard admission ceilings are 640 slots and 96 MiB. These figures exclude the renderer's
other resources and physics allocations. A GPU smoke route checks uploads, evictions, unchanged
centres, bounded residency and visible ground across signed coordinates and reversals.

The practical sample limit is absolute X/Z of 8192 metres. An authoritative player beyond it, below
the absolute -54 metre recovery floor, or at non-finite coordinates returns to the supported spawn
with zero velocity and reseeded client prediction. This is a prototype recovery rule. It adds no
combat extraction, score, death or replication protocol. The generator's own representable range is
wider; that does not make prediction and rendering safe for arbitrary coordinates.

## Consequences

The block now has playable ground beyond its original street and a generated horizon without a
growing disk cache. Memory follows resident tiles rather than distance travelled. The collision
smoke route exercises actual walking and a 100 m/s configured controller, plus recovery; the GPU
route separately exercises drawing and resource churn. Neither is a frame-time guarantee.

Floating-origin rebasing through prediction and replication, asynchronous generation/upload,
terrain material authoring and smooth far-field shading remain subsequent work. Recovery at eight
kilometres is explicit; this implementation is not infinitely large travel.
