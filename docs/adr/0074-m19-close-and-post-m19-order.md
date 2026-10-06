# ADR-0074: M19 close — the stack's state, its done-criteria, and the order after M19

- Status: Accepted
- Date: 2026-10-06

## Context

M19 (terrain) is code-complete as a stacked PR series but not done. Every brick has been pushed
and none is merged. **No GitHub Actions CI has run on any of them**: the Actions minutes are used
up until 2026-11-01, so every gate was run locally. Until then, a green local run is the only
evidence the stack has.

The bricks, compressed from the ADRs and the orchestrator's table:

| brick | PR | ADR | what |
| --- | --- | --- | --- |
| m19.1 | #238 (merged) | 0060 | cooked heightfield, grid-walk collision |
| m19.2 | #240 (merged) | 0061 | convex queries, speculative CCD against terrain |
| m19.3 | #243 (merged) | 0062 | drawn surface from the same u16 samples physics collides with |
| m19.4 | #244 | 0063 | splat blending: four layers, a partition of unity |
| m19.5 | #245 | 0064 | shared GGX BRDF; metallic and roughness join the splat blend |
| m19.6 | #246 | 0065 | terrain reads the sky: SH diffuse, sky-view mirror, env-BRDF |
| m19.7a | #247 | 0066 | `TerrainLayer` asset: material, packed albedo+height texture, world UVs |
| m19.7b | #249 | 0066, addendum | terrain samples the layer textures; per-layer UV scales |
| m19.7c + fix 1 | #252 | 0066, addenda | height-based blend, using the effective height c·h |
| m19.8b | #248 | 0067 | releasable, generation-checked asset ownership; streamed heightfields |
| m19.8c | #251 | 0068 | regional terrain collision, deferred admission |
| m19.8a | #254 | 0069 | render residency, the terrain world manifest |
| m19.8d1 | #255 | 0070 | LOD cook: nested subsampling, geometric error |
| m19.8d2 | #256 | 0071 | CDLOD render, exact geomorphs, parent fallback |
| m19.8d3 | #257 | 0072 | baked far appearance |
| m19.8e | #258 | 0073 | byte budget, upload cap, frustum culling, travel envelope (unpinned) |

The stack also carries #250 (the G-buffer change, +4 B/px; per the orchestrator, not in any ADR).

ADR-0068 (m19.8c) is not in this tree: it lives on its own branch, and ADRs 0068, 0071, 0072, 0073
and 0069 are marked provisional on that account. Renumbering at merge is expected if numbers clash.

m19.7b and m19.7c are not separate ADRs. They are dated addenda to ADR-0066, which is where their
decisions are recorded.

Why an order is needed now: M18's gate 7 and M19's pinned terrain run both need the owner's RTX 3060
with root for clock pinning, and the editor's known defects (#253) block trusting the authoring loop
that M20 would ship games with. Without a recorded order, each of those would be picked up on the day
someone was free rather than in the sequence the project chose.

## Decision

### 1. The order after M19

**Perf → Editor → M20 → Team/browser → M21.** These are named phases. They are not renumbered
milestones, and "Perf" and "Team/browser" do not get milestone numbers.

1. **Perf.** Closes M18 gate 7 (the block's budget on a clean, clock-pinned Release tree) and runs
   the owner's pinned M19 terrain run (`scripts/perf.sh --sample terrain --commit`). Both need the
   RTX 3060 with root for clock pinning.
2. **Editor.** A Blender-authored asset makes the round trip through the real, clicked editor, with
   reliable undo, save and reload. This closes M16. Its known defects, from the click tests (#253):
   Play→Stop corrupts the scene (handles change, and `ecs::Parent` is saved as null); undo stops
   working after Play/Stop; despawn and remove-component cannot be undone; a closed panel cannot be
   reopened; "place" is clipped off the default layout; there is no New or Open; u64 asset ids are
   rounded.
3. **M20**, the shipped game.
4. **Team/browser.** Browser-only and team authoring, not delivered today. The browser ignores the
   editor band, and the native mirror assumes a single editor.
5. **M21**, the generator.

The reasons: measure before adding; make the authoring tool trustworthy before shipping games made
with it; add collaboration before generated content.

### 2. M19's done-criteria

M19 is done when **both** hold:

- **the stack merges green under CI** (once Actions minutes return, or by owner decision), and
- **the pinned terrain perf run is committed** (`scripts/perf.sh --sample terrain --commit`, on the
  RTX 3060 with clocks pinned, filed under `docs/perf/`).

Until both hold, M19 is "code-complete as a stacked PR series, unmerged, no CI". The unpinned numbers
in `docs/perf/m19.8e-terrain/README.md` are the shape of the costs, not a baseline.

### 3. Luca's decisions (2026-10-05/06), recorded as decided

- **A metallic look.** Metallic and roughness are first-class in the splat blend (ADR-0064), and the
  far appearance keeps them (ADR-0072).
- **The builder decides materials.** The caller resolves the palette; `TerrainLayerBuilder` dispatches
  on the referenced asset's kind (ADR-0066 §2, ADR-0069 §5).
- **Real height maps first.** Heights are the cooked u16 integers, not procedural (ADR-0060, ADR-0062).
- **Regional collision.** Collision is a regional, streamed shape, not a whole-world one (ADR-0068,
  on its branch).
- **Full LOD for seamless travel.** The level chain is cooked and drawn with exact geomorphs and
  crack-free edges (ADR-0070, ADR-0071).
- **Deferred admission from the start.** A body that spawns or teleports is admitted to the
  simulation only once the collision region under it is installed. It is never simulated against ground
  that is not there. A counted whole-tick stall remains only as the safety net for bodies already
  admitted (ADR-0068, on its branch).

## Consequences

Known limits carried forward from the stack, each with the phase or brick that owns it. Where no
owner exists, the entry says so rather than inventing one.

- **Uploads are synchronous** (30–60 ms/MiB measured on the 3060, unpinned; ADR-0073). Owner: M19
  follow-up (staging ring, transfer queue). Not yet a numbered brick.
- **Index buffers are 70–91% of tile bytes** (one per tile, identical; ADR-0073). Owner: M19 follow-up
  (a shared index buffer per grid size, which would cut terrain GPU memory about 4×).
- **Every drawn leaf is its own raster pass** (~100 at 1080p; ADR-0073). Owner: M19 follow-up
  (batched leaves, or indirect draws).
- **Layer textures are not streamed** (counted whole; dependent loads; ADR-0073). Owner: M19
  follow-up.
- **The far appearance loses detail** (a mid-morph fade to a mean; ADR-0072). Owner: M19 follow-up
  (a macro texture, or a bake through the real layer textures).
- **The push block is 208 B; Vulkan guarantees only 128 B** (ADR-0071, ADR-0072). It works on the
  3060 and RADV, and the capability check refuses rather than silently failing (ADR-0064). It is a
  portability risk. **No owner is assigned.**
- **The G-buffer cost of +4 B/px in #250 is unmeasured** (per the orchestrator). Owner: Perf, if the
  pinned run covers it; not yet assigned.
- **A byte budget on a level-0-only world is untested** (ADR-0073: a level-0-only world draws every
  resident tile). Owner: the 8c reconciliation, which ADR-0073 lists as M19's first open item.
- **Regional collision (8c) is not yet reconciled** with the residency. ADR-0073 lists it first under
  "What M19 still has to close". Owner: M19, on its own branch.
- **Editor defects (#253)** are listed under §1.2. Owner: Editor.
- **PR numbers** were checked against GitHub on 2026-10-06: #247 is m19.7a, #248 is m19.8b (based on
  m19.7a), #249 is m19.7b (based on m19.7a), and #258 is m19.8e.
- **Index.** `docs/adr/README.md` does not yet index ADR-0063 or ADR-0068. This ADR adds 0074 and
  leaves those two for the bricks that own them.

## Alternatives considered

- **M20 before Editor.** Rejected: the editor's defects (#253) would corrupt scenes in games shipped
  with it, and the undo/save path is the thing a shipped game depends on.
- **Team/browser before Editor.** Rejected: collaboration on an authoring tool that loses handles on
  Play→Stop would multiply the damage.
- **Perf skipped until the editor is done.** Rejected: M18's gate 7 is the only open clause of the
  M18 block, and it is blocked on hardware the owner has, not on code.
- **Renumbering "Perf" and "Team" as milestones.** Rejected: the roadmap's milestone numbers are
  referenced across ADRs and PRs, and phases do not need numbers to be ordered.
