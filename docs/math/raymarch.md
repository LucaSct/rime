# Raymarched isosurface & DVR (viewer brick C2)

## What this is

C1 colours the part's surface by a scalar field. C2 looks *into* the field volume: a full-screen GPU
**raymarch** of the same 3-D field texture that draws either

- an **isosurface** — the surface where the field equals a chosen isovalue (an isotherm), lit by the
  field gradient; or
- a **DVR** (direct volume rendering) — a cloudy view from front-to-back colour/opacity compositing.

No geometry and no depth buffer: one full-screen triangle, and `iso.frag` marches a ray per pixel
through the field volume (R = value, G = validity, from C1). See `shaders/iso.{vert,frag}`, `iso.hpp`.

## 1. Per-pixel world ray

For a pixel at NDC $\mathbf q = (\text{ndc}_x,\text{ndc}_y)$, the view ray is reconstructed from the
**inverse view-projection** $V^{-1}$ (pushed each frame). Unprojecting the near ($z=0$) and far ($z=1$,
our depth convention) clip points and dividing by $w$ gives world points $\mathbf p_n, \mathbf p_f$; the
ray is

$$ \mathbf r(t) = \mathbf p_n + t\,\hat{\mathbf d}, \qquad \hat{\mathbf d} = \frac{\mathbf p_f-\mathbf p_n}{\lVert\cdot\rVert}. $$

The Y-flip and perspective are already baked into $V$, so $V^{-1}$ yields correct rays with no special-casing.

## 2. Into texture space, then ray–box

The world→texture map is the diagonal affine $\mathrm{uvw} = \mathbf p\odot\mathbf{s} + \mathbf b$ (the
same `field_scale`/`field_bias` as C1). A point transforms with scale **and** bias; a *direction* with
scale only. So we march in uvw space, where the field volume is the unit box $[0,1]^3$, and clip the ray
to it with the **slab method**:

$$ t_{\mathrm{lo}} = \max_a \min\!\Big(\tfrac{0-o_a}{d_a},\tfrac{1-o_a}{d_a}\Big), \quad
   t_{\mathrm{hi}} = \min_a \max\!\Big(\tfrac{0-o_a}{d_a},\tfrac{1-o_a}{d_a}\Big), $$

with $\mathbf o = \mathbf p_n\odot\mathbf s + \mathbf b$, $\mathbf d = \hat{\mathbf d}\odot\mathbf s$. If
$t_{\mathrm{hi}} \le \max(t_{\mathrm{lo}},0)$ the ray misses the volume → discard (transparent).

## 3. Isosurface

March $N$ steps from $t_{\mathrm{lo}}$ to $t_{\mathrm{hi}}$, sampling the (trilinear) field $f$. The
isosurface is the first **sign change** of $f-\text{iso}$ inside the solid (samples with validity $<0.5$
are skipped). At a crossing between steps the hit is refined linearly,
$t^\* = t-\Delta t + \tfrac{f_{\text{prev}}-\text{iso}}{(f_{\text{prev}})-(f_{\text{cur}})}\,\Delta t$,
and the **surface normal** is the field gradient by central differences,

$$ \mathbf n \propto \nabla f = \tfrac{1}{2\varepsilon}\big(f(\mathbf u+\varepsilon\hat x)-f(\mathbf u-\varepsilon\hat x),\,\dots\big), $$

(an isosurface is a level set, so $\nabla f$ is normal to it). It is shaded two-sided ($|\,\mathbf n\cdot
\mathbf L|$ — the gradient's sign is arbitrary) with the colormap of the isovalue. No crossing → discard.

## 4. DVR

Instead of stopping at a crossing, accumulate front-to-back: at each step inside the solid, map the value
to colour $\mathbf c=\text{colormap}(t)$ and a small opacity $\alpha\propto t$ (so the hot end shows
through), and composite

$$ \mathbf C \mathrel{+}= (1-A)\,\alpha\,\mathbf c, \qquad A \mathrel{+}= (1-A)\,\alpha, $$

stopping early once $A$ saturates. This is the standard emission-absorption (over) operator.

## Verification

`tests/rhi/iso_offscreen_test` raymarches a cube whose field ramps along z: the isosurface at a low value
reads cold-blue, at a high value hot-red (the isotherm lands where the colormap places that value), and the
DVR composites more of the frame than a single isosurface. End to end, `icem_viewer --iso <K>` on the
brick23 temperature field shows the isotherm; `[`/`]` move it, `V` toggles DVR. The isotherm coincides with
the C1 clip-plane slice contour at the same value (both read the same volume + colormap).
