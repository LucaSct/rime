# ADR-0004: Math conventions — float, column-major, right-handed, Vulkan clip space

- Status: Accepted
- Date: 2026-06-17

## Context

Every engine subsystem — transforms, the camera, the renderer, physics, the eventual ECS —
shares one linear-algebra vocabulary. The conventions are arbitrary in isolation but
*catastrophic if inconsistent*: a single mismatch (row- vs column-major, left- vs
right-handed, OpenGL's `z ∈ [-1, 1]` vs Vulkan's `z ∈ [0, 1]`) produces geometry that is
mirrored, inside-out, or invisible, and the bug surfaces three modules away from its cause.
So we fix the conventions once, in `engine/core/math`, and write them down.

The choices that interact:

- **Scalar type.** `float` vs `double`.
- **Matrix storage.** Row-major vs column-major, and the matching vector convention (row
  vector `v' = vM` vs column vector `v' = Mv`).
- **Handedness.** Right-handed vs left-handed world space.
- **Clip space / NDC depth.** OpenGL's `z ∈ [-1, 1]` (y up) vs Vulkan/D3D's `z ∈ [0, 1]`,
  and Vulkan's y-down framebuffer.

## Decision

1. **`float` (single precision) is the working scalar.** GPUs are float-native; uploading
   doubles would mean converting every frame, and float halves memory bandwidth — which is
   the binding constraint on the data-oriented hot loops (ECS transforms, skinning, particles).
   Where precision genuinely bites (e.g. large-world origins, physics accumulation) we solve
   it locally — camera-relative rendering, fixed-point or double islands — not by taxing the
   whole engine. `kEpsilon`-based `approx_eq` is the float-aware equality the whole codebase uses.

2. **Matrices are column-major with the column-vector convention `v' = M v`.** Element
   `(row r, col c)` lives at flat index `c * N + r`; each basis vector is a contiguous column.
   This is exactly GLSL/Vulkan's memory layout, so a `Mat4` uploads to a uniform/storage
   buffer **with no transpose**. Composition reads right-to-left: `(A * B) v = A (B v)`, so in
   `model = T * R * S` the scale is applied first — matching the mathematical notation in our
   derivations.

3. **World space is right-handed**, with the convention `x × y = z`. RH is the math/physics
   default and what most DCC tools (Blender, Maya) export, minimizing import surprises.

4. **Clip space targets Vulkan: depth `z ∈ [0, 1]`, NDC y points down.** Vulkan is our
   first and reference RHI backend (ADR-0002). The y-flip is baked into the *projection*
   matrices (`perspective`/`ortho` negate the `(1,1)` entry) so no other engine code thinks
   about it. `[0, 1]` depth (vs OpenGL's `[-1, 1]`) also gives better depth-buffer precision.

The math is written today as **readable scalar code over a SIMD-friendly layout** (16-byte
aligned `Vec4`/`Mat4`, contiguous storage), with a single seam (`math/simd.hpp`) where an
SSE/AVX/NEON backend will later plug in. We do **not** hand-write intrinsics yet: per
CLAUDE.md's "measure before optimizing," there is no renderer or ECS to benchmark against;
the real hot loops arrive with ECS transforms (M4) and PBR (M5). Until then, clarity wins.

## Consequences

**Good**
- One vocabulary for the whole engine; the conventions live in code *and* in
  `docs/math/vectors-matrices.md`, which derives every formula from first principles.
- Zero-friction GPU upload (matrices need no transpose; alignment matches `std140`/`std430`).
- The SIMD seam means we can vectorize later without changing a single public signature.

**Costs we accept**
- `float` will eventually need a precision strategy for very large worlds; we deferred that
  cost deliberately rather than pay it everywhere now.
- Column-major + RH + Vulkan-`[0,1]` differs from the OpenGL tutorials many newcomers learn
  from; the derivation note exists precisely to bridge that gap.
- The scalar path leaves SIMD performance on the table today — an accepted, measured-later
  trade, not an oversight.

## Alternatives considered

- **Row-major + row vectors (`v' = v M`),** as DirectX math and Unreal use. Self-consistent,
  but it would force a transpose on every GLSL upload given our Vulkan-first stance. Rejected
  for that friction.
- **Left-handed world space** (Unreal/Unity). Fine internally, but adds a conversion at every
  DCC import and contradicts the physics/math literature our derivations follow.
- **OpenGL clip space (`z ∈ [-1, 1]`, y up).** Wrong target for a Vulkan-first engine; would
  need fixing up in every projection anyway, with worse depth precision.
- **`double` precision.** Solves large-world precision globally but at a permanent
  bandwidth/throughput cost on hardware that is fundamentally float-first. Rejected in favor
  of local precision strategies where actually needed.
