# Clustered forward shading — derivation notes (m10.3)

These notes derive how Rime shades hundreds of lights without evaluating hundreds of BRDFs per
pixel: the **froxel** grid over the view frustum, why its depth partition is **logarithmic**, how a
froxel's view-space bounds are recovered by inverting the projection, and the sphere-versus-box test
that decides membership.

They are the companion to four pieces of code, all deliberately terse because the *why* lives here:

- `engine/render/src/lighting/clustered.cpp` — `cluster_depth_slice`, `cluster_bounds` and
  `sphere_touches_cluster`: the froxel maths as plain CPU functions (GPU-free, unit-tested), plus
  the resources and the dispatch.
- `engine/render/shaders/cluster_cull.comp` — the same three functions on the GPU, one invocation
  per froxel, filling the per-froxel light lists.
- `engine/render/shaders/pbr_forward_shadowed.frag` — `fragment_cluster(world_pos)` maps a pixel to
  its froxel and loops only that froxel's list.
- `tests/render/clustered_test.cpp` — the structural proof (the GPU's lists match the CPU model over
  all 3456 froxels; the clustered path matches the uniform-block loop pixel for pixel).

Conventions follow [ADR-0004](../adr/0004-math-conventions.md): right-handed world and view space
(the camera looks down $-z$), column-vector math ($\mathbf v' = M\mathbf v$), Vulkan clip space with
$z \in [0,1]$ and a **y-down** NDC (our `perspective` bakes the y-flip). The design context is
[ADR-0032 §4](../adr/0032-lighting-v2.md). GitHub renders the `$…$` / `$$…$$` math below.

---

## 1. Why cluster at all

[ADR-0022](../adr/0022-forward-pbr.md)'s forward shader loops a fixed uniform block of up to 16
point lights for **every** shaded pixel. At $P$ pixels and $L$ lights that is $P \cdot L$ BRDF
evaluations, and the block's size is a compile-time constant — so 200 lights is both impossible and,
if it were possible, wasteful: a given pixel is reached by a handful of them, and the rest
contribute exactly zero after their falloff window closes.

Clustered forward splits the work in two:

$$
\underbrace{C \cdot L}_{\text{cull once, in compute}} \;+\; \underbrace{P \cdot \bar{\ell}}_{\text{shade}}
$$

where $C$ is the number of froxels (3456 here) and $\bar{\ell}$ the *average* number of lights per
froxel — in a normal scene a handful, no matter how many lights the level contains. The first term
is paid once per frame at froxel resolution; the second is what the pixels actually needed. The
measured shape on lavapipe (`tests/render/clustered_test.cpp`, timings are relative fences only —
[ADR-0032 §11](../adr/0032-lighting-v2.md)): culling 1000 lights costs one sub-millisecond dispatch
and leaves ≈ 7 lights per froxel for shading.

A **froxel** ("frustum voxel") is one cell of a 3-D grid laid over the *view frustum* rather than
over world space: two of its axes are screen tiles, the third is a depth range. That choice is what
makes the fragment-side lookup nearly free — a pixel already knows its screen position and its
depth.

---

## 2. The depth partition: why logarithmic

Split the frustum's depth range $[n, f]$ into $K$ slices. The naïve choice is uniform,
$z_k = n + \frac{k}{K}(f-n)$. With $n = 0.1$, $f = 1000$ and $K = 24$ that gives every slice a
41-unit depth — so the first slice swallows everything from the lens out past the near geometry,
while the far slices carve up empty sky. Uniform slices are wrong because *perspective is not
uniform*: a screen tile's world footprint grows linearly with distance, so a froxel that is roughly
cube-shaped near the camera becomes a long thin shard far away (or vice versa).

The fix is to make each slice a constant **ratio** deeper rather than a constant distance:

$$
z_k = n \left(\frac{f}{n}\right)^{k/K}
$$

Now $z_{k+1}/z_k = (f/n)^{1/K}$ is the same for every $k$: each slice is (say) 30 % deeper than the
one before it, exactly as each is ~30 % wider in world terms because it is further away. Froxels
keep roughly the same aspect ratio from the near plane to the horizon, which is what keeps the
number of froxels a light touches proportional to its *screen* footprint instead of exploding in
the distance.

Inverting for the slice containing a view-space depth $d$:

$$
k(d) = \left\lfloor K \cdot \frac{\ln(d/n)}{\ln(f/n)} \right\rfloor,
\qquad k \in [0, K-1] \text{ after clamping}
$$

Both directions appear in the code — `cluster_bounds` uses the forward form (as
$n \cdot e^{(k/K)\ln(f/n)}$, so the shader precomputes $\ln(f/n)$ once on the CPU and the GPU never
evaluates a division inside a logarithm), and `cluster_depth_slice` / the fragment shader use the
inverse. Clamping at both ends is not cosmetic: a fragment slightly beyond the far plane must still
index a real froxel, and an unclamped index would read past the list buffer.

*Alternative considered.* A hybrid "uniform near, logarithmic far" partition buys slightly better
near-field distribution and is what a few engines ship; it costs a second branch in the hottest
lookup in the shader for a benefit no profile has asked for yet. Standard log-z, noted, move on.

---

## 3. A froxel's bounds: inverting the projection

Culling happens in **view space** — the camera at the origin looking down $-z$ — because a froxel is
trivially axis-aligned there and a light sphere stays a sphere under the rigid world→view transform.

Our perspective matrix (`core::perspective`) is

$$
P = \begin{pmatrix}
p_{00} & 0 & 0 & 0\\
0 & p_{11} & 0 & 0\\
0 & 0 & \frac{f}{n-f} & \frac{fn}{n-f}\\
0 & 0 & -1 & 0
\end{pmatrix},
\qquad
p_{00} = \frac{1}{\text{aspect}\cdot\tan(\theta/2)},
\quad
p_{11} = \frac{-1}{\tan(\theta/2)}
$$

with $p_{11}$ **negative** because Vulkan's NDC is y-down. For a view-space point
$\mathbf v = (x, y, z)$ with $z < 0$, the clip $w$ is $-z = d$ (the distance in front of the
camera), so

$$
\text{ndc}_x = \frac{p_{00}\,x}{d}, \quad \text{ndc}_y = \frac{p_{11}\,y}{d}
\qquad\Longrightarrow\qquad
x = \frac{\text{ndc}_x \cdot d}{p_{00}}, \quad y = \frac{\text{ndc}_y \cdot d}{p_{11}}
$$

That is the whole derivation: a froxel is the set of view-space points whose NDC x/y lie in its tile
and whose depth lies in its slice, so its eight corners are the two NDC x values × two NDC y values
× two slice depths pushed through the formula above. `cluster_bounds` takes the component-wise
min/max of those eight points, and `froxel_bounds` in `cluster_cull.comp` does the identical thing.

Two consequences worth stating plainly, because both look like bugs until you see why they are not:

**The box is bigger than the froxel.** A froxel is a *frustum slab* — its far face is wider than its
near face — so its axis-aligned box has corners the frustum itself does not contain. The box is
therefore **conservative**: the sphere test can say "touching" for a light that in truth just misses
the slab, but never the reverse. Over-inclusion costs a few wasted BRDF evaluations; under-inclusion
would be a light that visibly vanishes in part of the screen. We take that trade knowingly (tighter
tests exist — clipping the sphere against the slab's four side planes — and are the obvious upgrade
if profiling ever shows the wasted evaluations mattering).

**Neighbouring boxes overlap.** Same reason: tile $x$'s box spans its right edge at both the near
*and* far face of the slice, and so does tile $x+1$'s left edge. The property that matters is not
"do the boxes tile exactly?" (they do not) but **"does a fragment land inside the box it is assigned
to?"** — which is what the test asserts, over a spread of points covering the frustum.

---

## 4. Membership: sphere versus box

A point light with a falloff radius $r$ influences exactly the ball of radius $r$ around its centre
(our windowed inverse-square falloff reaches *exactly* zero at $r$ — see `point_contrib` — so this
is a hard bound, not an approximation). A froxel is touched iff that ball intersects its box:

$$
\left\lVert \mathbf c - \operatorname{clamp}(\mathbf c,\; \mathbf b_{\min},\; \mathbf b_{\max}) \right\rVert^2 \le r^2
$$

Clamping the centre into the box gives the closest point of the box to the centre; what is left over
is the shortest distance from the sphere's centre to the box. Comparing squared quantities avoids
the square root. Three lines, and it is the only geometric predicate in the whole technique.

Lights are transformed to view space **once per light per workgroup batch**, not once per froxel —
which is the reason `cluster_cull.comp` stages a batch of 64 lights into shared memory, transforms
them there, and then has all 64 invocations test their own froxel against the staged batch. The
loop reads "batch → `barrier()` → test → `barrier()`", and the second barrier is what stops a fast
invocation from overwriting the staging area while a slower one is still reading it.

---

## 5. The two halves, and why they are written twice

The culling half decides *which lights are in froxel $c$*; the shading half decides *which froxel a
fragment is in*. If those two disagree even slightly — different rounding in the slice formula, a
different tile edge convention — the result is a light that is culled away from pixels it should
reach: a banding artifact that is maddening to debug because both halves look correct in isolation.

Rime writes the maths twice on purpose (C++ in `clustered.cpp`, GLSL in `cluster_cull.comp`) and
then **tests that the two agree**: the proof runs the real dispatch, reads the list buffer back, and
re-derives what every one of the 3456 lists should contain using the CPU functions, asserting that
no froxel lists a light whose sphere clearly misses it and no froxel drops a light whose sphere
clearly reaches it (a ±1 % margin on the radius absorbs last-ulp differences between the two
implementations without weakening the claim). A divergence is then a red test, not a rendering
mystery.

The fragment-side lookup is the third copy of the slice formula, and it is covered by the same
idea from the other direction: the test asserts that a fragment's assigned froxel actually contains
it (§3), which is precisely the property the cull result is only useful under.

---

## 6. Limits and what comes next

- **Point lights only.** Spot lights still ride the m10.2 uniform block (up to 8, each with a shadow
  map). A spot is a cone, and culling cones against froxels wants its own predicate; folding them
  into the lists is a natural follow-up once the shadowed-local count grows past what a block holds.
- **Per-froxel cap.** Lists hold up to `kMaxLightsPerCluster` (64) entries and **clamp** past that:
  a froxel that gathers more keeps the first 64. Never an out-of-bounds write — the failure mode is
  a slightly under-lit pixel in a pile of lights, and the test plants an adversarial pile to prove
  the clamp holds.
- **No depth-bounds refinement.** The cull uses the froxel's full depth range, not the actual depth
  range of the geometry inside it (which a pre-pass could supply). That refinement — "active froxel
  determination" — cuts the light count in froxels that are mostly empty space, and is the standard
  next optimization.
- **Absolute cost is not measured here.** lavapipe runs compute on the CPU, so the timings in the
  proof are relative fences only; real budgets arrive with real hardware at m12.0
  ([ADR-0032 §11](../adr/0032-lighting-v2.md)).
