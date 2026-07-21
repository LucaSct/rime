# Cascaded shadow mapping — derivation notes (m10.1)

These notes derive how the sun casts a real shadow in Rime: why "render the scene's depth from the
light, then compare from the camera" answers *is this point in shadow*, why one shadow map is not
enough and how the view frustum is split into **cascades**, and the three tricks that turn a naïve
shadow map (which crawls with shimmer and is speckled with acne) into a stable, soft one — a
**rotation-invariant** cascade fit, **texel snapping**, and **bias + PCF**.

They are the companion to three pieces of code, all deliberately terse because the *why* lives here:

- `engine/render/src/lighting/shadows.cpp` — `compute_cascades` fits the light frustums (GPU-free,
  unit-tested), and `CascadedShadowMap::add` declares the per-cascade depth passes.
- `engine/render/shaders/pbr_forward_shadowed.frag` — `sun_shadow(world_pos, N)` selects a cascade,
  applies bias, and does the PCF compare.
- `tests/render/shadow_test.cpp` — the structural proof (the sun darkens the floor under a box; remove
  the box or disable shadows and it lights back up).

Conventions follow [ADR-0004](../adr/0004-math-conventions.md): right-handed world, column-vector math
($\mathbf v' = M\mathbf v$), Vulkan clip space with $z \in [0,1]$ and a y-down NDC (our `perspective`
and `ortho` already bake the y-flip). The design context is [ADR-0032 §3](../adr/0032-lighting-v2.md).
GitHub renders the `$…$` / `$$…$$` math below.

---

## 1. The shadow test: depth from the light, compared from the camera

A point is in shadow when *something else is closer to the light along the same ray*. That is exactly
what a depth buffer records — the nearest surface along each ray from a camera — so we render one
**from the light's point of view**. Give the sun an orthographic "camera" (its rays are parallel, so
the projection is orthographic, not perspective) looking down the light direction, and rasterise the
scene's depth into a texture: the **shadow map**. Each texel now holds the distance from the light to
the closest occluder in that direction.

To shade a fragment at world position $\mathbf p$, transform it into that same light clip space with
the light's `view_proj`:

$$\mathbf p_{\text{light}} = M_{\text{light}}\,\begin{bmatrix}\mathbf p\\ 1\end{bmatrix}, \qquad
\mathbf u = \tfrac{1}{2}\,\mathbf p_{\text{light},xy} + \tfrac{1}{2}, \qquad
d = \mathbf p_{\text{light},z}.$$

$\mathbf u$ is where this fragment lands in the shadow map, and $d$ is *its own* depth from the light.
Read the stored occluder depth $d_{\text{map}}(\mathbf u)$ and compare:

$$\text{lit} \iff d \le d_{\text{map}}(\mathbf u) + \varepsilon.$$

If the fragment is no farther than the nearest recorded occluder (within a bias $\varepsilon$, §4),
nothing is between it and the sun — it is lit. If it is farther, something was drawn closer to the
light, and the fragment is in that thing's shadow. The whole test is a **re-projection**: we render
the world once from the light, once from the camera, and ask whether the two views agree about who is
closest to the sun.

Because the projection is orthographic, $w = 1$ after projection and the perspective divide is a
no-op; the shader still divides (`p = lc.xyz / lc.w`) so the same code survives if a spot light ever
swaps in a perspective light matrix (m10.2).

### The depth pass is the pre-pass, aimed from the light

Rendering the shadow map needs *only* positions — no shading, no material. That is precisely the
depth pre-pass the forward renderer already owns (`depth_only.vert` + `DepthPrepass`). A cascade is
structurally the *same pass* with a different `view_proj` and a different target layer, so
`CascadedShadowMap::add` reuses `DepthPrepass::add` verbatim, pointing uniform binding 0 at the
cascade's light matrix instead of the camera's. One depth-only pipeline serves both roles; there is no
second "shadow shader" on the vertex side.

---

## 2. Cascades: one shadow map cannot cover a whole view

A single shadow map has a fixed resolution spread over the light's whole frustum. To cover a large
scene it must be huge in world units, so each texel is metres wide — shadows near the camera turn into
blocky staircases. Shrink it to sharpen the foreground and the background falls outside the map
entirely. The tension is fundamental: near geometry needs fine texels, far geometry needs coverage.

**Cascaded shadow maps** resolve it by slicing the camera's depth range $[z_n, z_f]$ into $N$
sub-ranges and giving each its *own* shadow map, tightly fit to just that slice. The near cascade
covers a small volume at high resolution; each farther cascade covers a larger volume at the same
texture resolution, so texel density falls off with distance — matching the screen, where distant
things are smaller anyway. Rime stores the $N$ maps as the layers of one depth **array texture** (the
m10.1a array-texture RHI), one `render into layer c` pass per cascade.

### Where to split: the practical (logarithmic ⊕ uniform) scheme

Two natural ways to place the split distances $s_i$, and both are wrong alone:

- A **uniform** split, $s_i = z_n + (z_f - z_n)\,\tfrac{i}{N}$, spaces the cuts evenly in world
  distance. It wastes the near cascades (perspective already makes near geometry big on screen) and
  under-resolves the foreground.
- A **logarithmic** split, $s_i = z_n \left(\tfrac{z_f}{z_n}\right)^{i/N}$, spaces them evenly in
  *perspective* (post-projection screen area). It is ideal for perspective foreshortening but pushes
  almost all cascades into the first few metres, starving the distance.

The **practical split** (Zhang et al., *Parallel-Split Shadow Maps*) blends them with a single knob
$\lambda \in [0,1]$:

$$s_i = \lambda \underbrace{\,z_n\!\left(\tfrac{z_f}{z_n}\right)^{i/N}}_{\text{logarithmic}}
       + (1-\lambda)\underbrace{\Big(z_n + (z_f - z_n)\tfrac{i}{N}\Big)}_{\text{uniform}},
       \qquad i = 0 \dots N.$$

$\lambda = 1$ is pure log (crisp near, thin far), $\lambda = 0$ pure uniform; Rime's default
$\lambda = 0.5$ (`cascade_split_lambda`) leans on the log term for the foreground while keeping the far
cascade usefully large. `compute_cascades` evaluates this directly, then builds a perspective frustum
for each slice $[s_c, s_{c+1}]$ and fits a light frustum around it (§3).

### Cascade selection in the shader: first one inside

The forward pass must pick *which* cascade a fragment reads. The cascades nest coarse-around-fine, so
the rule is simply **the first cascade the fragment falls inside**, walked near→far
(`sun_shadow`'s loop): cascade 0 is the tightest map, and if the fragment projects within its
$[0,1]^2$ bounds (a hair inside the border, to avoid the clamped edge) that is the sharpest shadow
available. Only if it falls outside cascade 0 do we fall through to 1, then 2. A fragment past the last
cascade is treated as **lit** (return $1$) rather than popped to a hard edge — beyond the shadow
distance there is simply no shadow data, and "lit" is the graceful default. Selecting by containment
(rather than by comparing view-space depth to the split distances) means the choice uses the exact
same projection the compare will, so a fragment is never told it is inside a cascade it actually misses.

---

## 3. Killing the shimmer: a rotation-invariant fit + texel snapping

The naïve cascade fit — bound the slice's 8 frustum corners with a tight axis-aligned box in light
space each frame — *shimmers*. As the camera moves or turns, the fitted box changes size and position
continuously, so the mapping from world to shadow texels slides underfoot and shadow edges crawl and
sparkle every frame. Two independent causes, two fixes.

### 3a. Rotation shimmer → a bounding **sphere**

If the fit's *size* changes when the camera merely rotates, texel density changes, and edges crawl. A
tight box around the frustum corners does exactly that — rotate the frustum and the box's extents
breathe. The fix is a bounding **sphere**: compute the slice corners' centroid and the max distance to
it,

$$\mathbf c = \tfrac{1}{8}\sum_{k} \mathbf q_k, \qquad r = \max_k \lVert \mathbf q_k - \mathbf c\rVert,$$

and fit the light's orthographic box to that sphere ($[-r, r]$ on each axis). A sphere has no
orientation, so $r$ — and therefore the world-units-per-texel $2r / \text{resolution}$ — is
**invariant under camera rotation**. The texel size is now constant frame to frame; only the *centre*
moves, which §3b handles. (The price is a little wasted resolution — a sphere circumscribes the box —
paid gladly to stop the crawl.)

### 3b. Translation shimmer → **texel snapping**

Even with a fixed texel size, if the cascade centre moves by a fraction of a texel each frame, every
world point shifts to a slightly different spot *within* its texel and the rasterised edge dances. The
fix is to let the centre move only in **whole-texel steps**, so the shadow's sampling grid stays
locked to world space. Work in the light's *rotation-only* frame (the shadow map's own plane), where
one texel is $t = 2r / \text{resolution}$ world units on a side, and floor the centre to that grid:

$$\mathbf c_{\text{ls}} = R_{\text{light}}\,\mathbf c, \qquad
c_{\text{ls},x} \leftarrow t \left\lfloor \tfrac{c_{\text{ls},x}}{t} \right\rfloor, \quad
c_{\text{ls},y} \leftarrow t \left\lfloor \tfrac{c_{\text{ls},y}}{t} \right\rfloor,$$

then rotate back ($\mathbf c_{\text{snap}} = R_{\text{light}}^{-1}\,\mathbf c_{\text{ls}}$). Now as the
camera glides, the centre jumps in texel-sized increments: every world point keeps landing on the
*same* texel until the whole grid steps over by one, so the shadow is rock-steady instead of swimming.
This is the property the unit test pins — nudge the camera far less than a texel and cascade 0's
matrix must not change (`approx_eq`).

### The light "camera" placement

With centre $\mathbf c_{\text{snap}}$ and radius $r$ fixed, the light is pulled back along its own
direction far enough that its near plane sits *behind* the slice, so occluders floating between the sun
and the slice still make it into the map:

$$\mathbf e = \mathbf c_{\text{snap}} - \hat{\mathbf l}\,(r + z_{\text{extra}}), \qquad
z_{\text{extra}} = r,$$

viewed with `look_at(e, c_snap, up)` and projected with `ortho(-r, r, -r, r,\ 0,\ 2r + z_{extra})`.
The $z_{\text{extra}} = r$ margin is headroom for off-slice casters (a tall wall just outside the
slice still shadows into it). The `up` vector swaps from $+y$ to $+z$ when the sun is within
$0.6°$ of straight up/down, where $+y$ would be parallel to the light and the basis degenerate.

---

## 4. Bias: the tug-of-war between acne and peter-panning

The compare $d \le d_{\text{map}}$ is exact in theory and a mess in practice, because $d_{\text{map}}$
is *quantised*: one depth sample stands in for a whole texel's worth of surface. On a surface tilted
relative to the light, that texel spans a range of true depths, so half the fragments it covers test
as *farther* than the single stored sample and shadow themselves — **shadow acne**, a moiré of dark
speckles on lit surfaces. Push the compare the other way too hard and shadows detach from their
casters — **peter-panning**, an object hovering above its own shadow. Bias is the compromise, and Rime
uses two complementary forms.

### 4a. Constant depth bias

Subtract a small constant from the receiver's depth before the compare (`ref = p.z - depth_bias`), so
a fragment must be *meaningfully* farther than the occluder to count as shadowed. This clears the flat
speckle. Alone it is a blunt instrument — too small and acne survives, too large and contact shadows
peter-pan — so it is kept small (`shadow_depth_bias` default $0.0015$ in light-clip depth) and paired
with:

### 4b. Normal-offset bias

Instead of only pushing in depth, move the *sample position* off the surface along its normal before
projecting into light space:

$$\mathbf p' = \mathbf p + \hat{\mathbf n}\,\beta, \qquad \beta = \text{normal\_bias}.$$

This attacks acne where it is worst — at grazing sun angles, where a texel covers the most depth — and
it scales with geometry rather than with depth precision, so it fixes surface acne without lifting
contact shadows the way a large depth bias would. Offsetting *along the normal* (not the light) keeps
the shadow anchored under the object while lifting the sample clear of its own surface.
`shadow_normal_bias` defaults to $0.06$ world units. Together the two biases clear acne at a normal-bias
scale small enough that peter-panning stays imperceptible.

---

## 5. PCF: from a hard binary edge to a soft one

The compare returns a single bit — lit or shadowed — so a raw shadow map has jagged, aliased,
pixel-hard edges. **Percentage-closer filtering** (PCF) softens them by taking *several* compares
around the sample point and averaging the *results* (not the depths — averaging depths and then
comparing is meaningless across a depth discontinuity; PCF compares first, averages after). The
fraction that pass is a soft occlusion in $[0,1]$ — a penumbra.

Rime gets the first level of PCF for free from hardware: the shadow map is sampled through a
**depth-compare sampler** (`sampler2DArrayShadow`, `compareOp = LessEqual`, the m10.1a sampler), so a
single `texture()` fetch does a bilinear-weighted $2\times2$ compare and returns the filtered pass
fraction. The shader then widens that to a $3\times3$ grid of taps spaced `pcf_radius` texels apart:

$$\text{shadow} = \frac{1}{9}\sum_{dy=-1}^{1}\sum_{dx=-1}^{1}
  \operatorname{compare}\!\big(\mathbf u + (dx, dy)\,\rho\,t,\ \ d - \varepsilon\big),
  \qquad t = \tfrac{1}{\text{resolution}},$$

with $\rho = $ `shadow_pcf_radius` (default $1$) and each `compare` itself the bilinear hardware PCF —
so the effective kernel is $3\times3$ hardware-filtered taps, a $6\times6$-texel footprint of
softening. The result multiplies the sun's contribution: $1$ leaves the fragment fully lit, $0$ fully
shadowed, and the fractional values along the edge are the penumbra.

---

## 6. The regression bridge, and what stays out of v1

Shadows are a **feature gate**. With `shadows_enabled = false` (the default), the renderer binds the
*unmodified* `pbr_forward` pipeline — not this shader — and is byte-identical to the M5.6 baseline
(ADR-0032 §11). That is why the shadowed shading lives in a separate `pbr_forward_shadowed.frag`
rather than behind a branch in the base shader: the off path must not carry a single extra
instruction. The shadow proof asserts this bridge directly — with shadows off, the occluder box
darkens nothing (test case (b)).

Only the **primary** directional light (light 0, the sun) casts in v1; the remaining directional
lights are treated as unshadowed fill (a common and cheap simplification — fill light rarely reads as
casting). Local (point/spot) shadows are **m10.2**, and they reuse this exact machinery: a spot light
is a perspective light matrix into the same depth-render-then-compare test (which is why §1 keeps the
perspective divide the ortho path does not strictly need); a point light is six such maps on a cube.
Contact-hardening / variable penumbra (PCSS), and filtering schemes beyond fixed PCF (VSM/ESM), are
deliberately out — this brick buys *correct, stable, soft-enough* sun shadows, and the later bricks
sharpen from there.
