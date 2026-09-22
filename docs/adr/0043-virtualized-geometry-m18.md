# ADR-0043: M18 virtualized geometry — rigid opaque meshes first

- Status: Accepted
- Date: 2026-09-22

## Context

M18 is the virtualized-geometry milestone deferred by ADR-0041. Existing `MeshAsset` data is one
conventional indexed vertex/index blob per mesh, and `SceneRenderer` expands it into CPU draw
items. That remains the compatibility path; it cannot become an accidental file-format change just
because a renderer wants clusters.

The earlier question was whether terrain belongs in this milestone. ADR-0041's pre-commitment
answers it: if both tracks survive M18's opening decision, terrain becomes M19. A heightfield,
collision, splat blending and world streaming are not supplied by a mesh cluster hierarchy.

## Decision

M18 virtualizes **rigid, opaque mesh geometry**. Skinned, deforming, masked, procedural and terrain
geometry continue through the conventional renderer and are counted as such. Destruction parts may
use the path only when each independently cooked rigid part satisfies the same contract.

The cook writes a separate, versioned virtual-geometry companion payload. It does not alter RMA1
`MeshAsset` or redefine an existing mesh handle. The payload's durable, CPU-visible contract is:

- cluster geometry ranges, material slots, conservative local AABBs and finite simplification error
  measured in local-space metres;
- replacement groups forming a checked acyclic DAG, so a complete parent representation replaces a
  complete set of children rather than independently selected clusters opening cracks;
- a page directory with byte ranges and dependencies, with a permanently resident coarse cut; and
- source vertex-layout identity and schema/cooker versions, while GPU addresses, page-table slots,
  descriptor indices and packed visibility tokens remain runtime-only details.

`engine/assets` validates and owns the CPU payload. `engine/render` owns uploads, residency,
in-flight retirement, selection and GPU data. `AssetState::Ready` continues to mean CPU-resident;
it does not imply that a fine page is renderable. Scene extraction stays GPU-free and passes stable
instance/material references to the GPU path rather than expanding selected clusters back into CPU
`DrawItem`s.

The initial RHI scope is ordinary buffers, a fixed-size compute-written indirect-command array,
indirect-read graph state/barriers, and `R32Uint` visibility plus material resolve. Mesh shaders,
sparse memory, buffer-device addresses, descriptor indexing, bindless materials and 64-bit atomics
are explicitly not prerequisites. Software rasterization for sub-pixel triangles remains part of
the milestone's final hybrid path, not something a hardware-only prototype may silently omit.

## Delivery order and gates

1. Versioned payload + reader/cooker validation. Reject cycles, non-finite bounds/errors, invalid
   ranges/dependencies and counts/offsets beyond fixed ceilings. The test fixtures are the binary
   contract.
2. A resident leaf-cluster path through hardware visibility and material resolve for one textured,
   multi-material rigid mesh in game and editor. It must agree with conventional identity/depth,
   UV gradients, normal maps and picking.
3. Cooked replacement groups and a CPU reference selector. The proof bounds projected error,
   selects no ancestor beside one of its descendants, and shows complete coverage at transitions.
4. GPU selection plus indirect submission against that oracle. Overflow is counted and falls back
   safely; no GPU readback decides the same frame's draw list. Complexity sweeps report candidates,
   selected triangles, CPU submission and GPU time at a fixed projected size.
5. Bounded streaming: delayed pages and camera teleports retain the coarse cut without holes,
   memory plateaus, and submitted pages cannot be evicted.
6. Hybrid software rasterization: hardware and software paths agree on depth/identity winners,
   near-plane clipping and ties, and the split earns a measured micro-triangle gain.

## Consequences

M18 gains a reversible asset/render seam and a conventional fallback instead of a second renderer
that only works for a demo mesh. It does not claim terrain or a completed Nanite-style pipeline at
the first resident-leaf slice. Terrain is M19; M18 must not consume its collision or streaming
scope merely to make the word "geometry" sound broader.
