# ADR-0005: Rotations are unit quaternions (Hamilton, right-handed, active)

- Status: Accepted
- Date: 2026-06-17

## Context

ADR-0004 fixed the linear-algebra conventions (float, column-major, `v' = M v`, right-handed,
Vulkan clip space) but left *rotation representation* open. Transforms, the camera, animation,
physics, and the scene graph all need to store, compose, and interpolate orientation, and the
choice ripples through every one of them. The options:

- **3×3 rotation matrices** — fast to apply, but 9 floats, and repeated composition drifts away
  from orthonormality (requiring re-orthonormalization), with no natural interpolation.
- **Euler angles** — intuitive and compact (3 floats) but plagued by gimbal lock, an ambiguous
  ordering convention, and ill-defined interpolation.
- **Unit quaternions** — 4 floats, compose by multiplication, "renormalize" instead of
  re-orthonormalize, no gimbal lock, and a well-defined shortest-arc interpolation (slerp).

A secondary, easy-to-get-wrong sub-choice: the **Hamilton** quaternion convention (used in
maths, robotics, Eigen, glm) vs the **JPL** convention (some aerospace). They differ in the
sign of the `ijk` products and are silently incompatible.

## Decision

**Orientation is represented by unit quaternions**, `Quat{x, y, z, w}` with `w` the scalar part,
using the **Hamilton** convention, **right-handed**, and **active** (a quaternion rotates a
vector within a fixed frame; equivalently it maps local-space directions to world space).

- Composition is the Hamilton product and reads **right-to-left**, exactly like matrices
  (ADR-0004): in `a * b`, `b` is applied first. So quaternions and matrices chain identically.
- A vector is rotated by the algebraically-equivalent, cheaper form
  `v' = v + 2 w (u × v) + 2 (u × (u × v))` with `u = q.xyz`, not the literal `q v q*`.
- The 4th-component-last storage `(x, y, z, w)` matches `Vec4` and GLSL.

Matrices remain the representation we *apply on the GPU* and **bake** to (`to_mat4`,
`Transform::to_matrix`); quaternions are what we *store and interpolate*. Euler angles are a
**convenience at the edges only** (`quat_from_euler`, extrinsic X→Y→Z), never the internal form.

## Consequences

**Good**
- Compact, drift-free orientation with cheap composition and correct slerp — the right
  foundation for an animation system and a transform hierarchy.
- One mental model for chaining: quaternion `*` and matrix `*` compose the same way.
- Renormalizing a `Quat` (one `sqrt` + 4 muls) is far cheaper and more stable than
  re-orthonormalizing a 3×3 every frame.

**Costs we accept**
- Quaternions are less immediately intuitive than Euler angles; the derivation note
  (`docs/math/quaternions-transforms.md`) exists to teach the *why* (half-angle, double cover).
- The Hamilton-vs-JPL hazard is real: any external rotation data (a physics lib, an importer)
  must be checked for convention at the boundary.
- `q` and `−q` denote the same rotation (double cover); equality must compare *rotations*
  (`same_rotation`, via `|dot| ≈ 1`), not raw components — a sharp edge we document in code.

## Alternatives considered

- **Matrices as the stored form.** Simplest to apply, but the storage cost, orthonormality
  drift, and lack of interpolation make them wrong for a scene graph / animation. We keep them
  as the *baked, applied* form, which is exactly where they shine.
- **Euler angles as the stored form.** Compact and human-friendly, but gimbal lock and
  interpolation ambiguity are disqualifying for an engine core. Retained only as an input
  convenience with a documented order.
- **JPL quaternion convention.** Equally valid mathematically, but Hamilton matches the wider
  C++/maths ecosystem (Eigen, glm) we will interoperate with; choosing it minimizes surprise.
