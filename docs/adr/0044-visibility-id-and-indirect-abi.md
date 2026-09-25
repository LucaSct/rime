# ADR-0044: the virtual-geometry visibility ID is 64-bit, and how an indirect draw finds its cluster

- Status: Accepted
- Date: 2026-09-25
- Supersedes: the `R32Uint` visibility-target half of [ADR-0043](0043-virtualized-geometry-m18.md)'s
  "initial RHI scope" paragraph. Everything else in ADR-0043, including its seven gates, stands.

## Context

ADR-0043 scoped M18's first visibility path as `R32Uint` — 32 bits per pixel naming *which cluster*
covered it. Step 2 (the material resolve) needs *which triangle*, and it needs a generation so a
recycled residency slot cannot be mistaken for a pixel written before the recycle. Three fields do
not fit in 32 bits at town scale: an unshipped version-2 layout tried, and bought its triangle field
by shrinking the slot to 16 bits (65,536 resident clusters) and the generation to 5 bits (32
recycles). Both ceilings are reachable in the target scene, and the visibility buffer is the one
target every pixel writes — its bit budget bounds the whole system, so a ceiling here is not a
render detail that a later brick can relax.

A second, smaller question arrived with step 3b. Once the draw list is a fixed-size,
compute-written indirect-command array (ADR-0043's own scope), a per-cluster push constant is no
longer available: one `vkCmdDrawIndexedIndirect` issues many commands and push constants cannot
vary between them. The shader has to recover *which cluster this command is drawing* from something
the GPU knows.

## Decision

**The visibility ID is 64-bit, `RG32Uint`, format version 3.** The layout is frozen as shipped in
`engine/render/include/rime/render/virtual_geometry_visibility_id.hpp`:

- `.x` — `[6:0]` triangle within the cluster (128, the usual cluster cap), `[31:7]` cluster slot
  (33,554,432 resident clusters);
- `.y` — `[27:0]` allocation generation (268,435,456), `[31:28]` version = 3.

The version always occupies the top nibble of the word that holds it, so a reader identifies the
layout before interpreting anything else, and zero remains the invalid/empty sentinel in every word.
Version 1 (32-bit, cluster-only) stays frozen and readable. **Version 2's number is retired and
must never be reused**: it existed only on an unmerged branch, and a reader that accepted it would
be accepting the ceilings this ADR exists to remove.

The accepted cost is **2x bandwidth on the ID target** — four extra bytes per pixel, on the one
target every pixel writes. This is deliberate: widening a frozen pixel format later is a migration
across the cook, the raster pass, the resolve pass and picking, while bandwidth is a number the
frame budget (ADR-0043 gate 7) already measures and can pay for or reject on evidence.

**An indirect draw identifies its cluster through `gl_DrawID`.** Per-cluster values (the packed ID
words, index and vertex bases, vertex stride, residency slot) move out of push constants into a
std430 record buffer with one 32-byte record per command, indexed by `gl_DrawID`. The view-projection
matrix, which is identical for every cluster in a request, stays a push constant. This requires the
Vulkan 1.1 `shaderDrawParameters` feature, enabled when the device reports it.

Rejected: packing the cluster slot into each command's `firstInstance` and reading
`gl_InstanceIndex`. It needs its own optional feature (`drawIndirectFirstInstance`), it is no more
portable, and it spends the instancing seam — the same field a later brick needs for real
instanced clusters — on identity. Guardrail 3: leave the seam.

**Vertex pulling survives an indexed indirect draw through an identity index buffer.** The pass
binds one `Uint32` index buffer holding the sequence `0,1,2,...,n-1` and sets each command's
`first_index` to the cluster's index base with `vertex_offset = 0`, so `gl_VertexIndex` arrives as
the cluster's global index slot and the vertex stage keeps fetching its own index and vertex from
storage buffers. The triangle is then `(gl_VertexIndex - record.index_base) / 3`. The alternative —
reading the real index buffer through fixed-function index fetch — would leave the fragment stage
with no portable way to name its triangle, because `gl_PrimitiveID` in a fragment shader is gated
behind Vulkan's `geometryShader` feature, which MoltenVK does not have.

**The submitted draw count is a compile-time constant.** The indirect buffer always holds
`kVirtualGeometryMaxIndirectDraws` commands and the draw always submits that many; unused commands
are all-zero, and `instance_count = 0` draws nothing. This is what lets a *GPU-decided* draw count
reach the draw with no same-frame readback, which ADR-0043 gate 4 forbids. Clusters beyond the
capacity bump their own counter (`skipped_over_capacity`) — the replication rule applied to
rendering: a proof that cannot see what it skipped still reads as passing.

## Consequences

The ID's ceilings stop being a design constraint for the rest of M18, and the resolve pass, picking
and any future debug view read one frozen layout with an explicit version. Every pass that touches
the ID target pays four more bytes per pixel, and gate 7's budget run is where that is judged.

The record-buffer indirection is now the ABI between the raster pass and the shader: a brick that
adds a per-cluster value adds a record field, not a push constant, and the record's `std430` size
is asserted in C++ against the shader's struct. The identity index buffer is one more buffer per
declare() whose size tracks the uploaded index count; it is the price of keeping a single vertex
path for hardware raster and the coming software raster, which must agree on triangle identity.
