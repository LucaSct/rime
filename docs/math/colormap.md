# Field colormap: trilinear sampling + transfer function (viewer brick C1)

## What this is

The viewer shows a computed simulation field — ICEM's `.icef` temperature, displacement magnitude,
velocity — as colour on the part (and on the interior a cross-section reveals). Three pieces:

1. a **world → texture-coordinate** map, so a fragment can look up the field at its 3-D position;
2. **trilinear** interpolation of the field grid (done by the GPU sampler);
3. a **transfer function** (colormap) from a scalar value to an RGB colour, with a legend.

The field is uploaded as a 3-D texture (ADR-0013): channel **R** = value, **G** = validity.

## 1. World → texture coordinate (the affine map)

`.icef` stores the field on a regular node lattice: node $(i,j,k)$ sits at world position
$\mathbf p = \mathbf o + (i,j,k)\,h$ for grid origin $\mathbf o$ and spacing $h$ (see ICEM's
`docs/math/field-io.md`). A 3-D texture of $n_x\times n_y\times n_z$ texels addresses texel $i$ at
normalised coordinate $(i+\tfrac12)/n$ (texel **centres**). Composing, the texture coordinate for a
world point is, per axis,

$$ \mathrm{uvw} = \frac{(\mathbf p - \mathbf o)/h + \tfrac12}{\;\mathbf n\;}
              = \mathbf p\odot\underbrace{\frac{1}{h\,\mathbf n}}_{\textstyle \mathbf{scale}}
              + \underbrace{\Big(\frac{1}{2\,\mathbf n} - \frac{\mathbf o}{h\,\mathbf n}\Big)}_{\textstyle \mathbf{bias}}. $$

So a world point maps to its texture coordinate with **one multiply–add**, $\mathrm{uvw} = \mathbf
p\odot\mathbf{scale} + \mathbf{bias}$. The viewer precomputes `scale` and `bias` per axis when it loads
the field (`field.hpp`) and hands them to the shader in the push constant (`field_scale.xyz`,
`field_bias.xyz`); the shader does the multiply–add and samples. Node $i$'s centre maps to $(i+\tfrac12)/n$
exactly, so a fragment sitting on a node reads that node's value, and points between nodes get the
hardware blend below.

## 2. Trilinear interpolation

A point inside a grid cell is the volume-weighted blend of the 8 surrounding nodes — linear in each
axis, hence *tri*linear:

$$ f(\mathbf u) = \sum_{c\in\{0,1\}^3} \Big(\textstyle\prod_{a} \big[(1-t_a)\,[c_a{=}0] + t_a\,[c_a{=}1]\big]\Big)\, f_{\mathbf i + c}, \qquad t_a = \mathrm{frac}(u_a\,n_a - \tfrac12). $$

This is exactly what a `sampler3D` with `Filter::Linear` computes in hardware, so the shader just calls
`texture(u_field, uvw)` — no manual blend. `AddressMode::ClampToEdge` keeps lookups just outside the
lattice (a surface fragment on the boundary) reading the edge value rather than wrapping.

**Boundary fix (dilation).** A sparse part leaves lattice nodes *outside* the solid with no value. If
those were left at a fill constant, trilinear blending near the surface would drag the colour toward
that constant. So the loader **dilates** the value channel one voxel: each absent node takes the mean
of its present 6-neighbours. Linear sampling at the boundary then blends real field values, and the
surface colour reads true right up to the edge. The **validity** channel keeps the *true* solid mask
(for the cut-face clip that arrives with the solid cap, B2b) — only the value channel is dilated.

## 3. Transfer function (the colormap)

The scalar is normalised to $t = \mathrm{clamp}\!\big((f - v_\min)/(v_\max - v_\min),\,0,\,1\big)$ over
the field's value range $[v_\min, v_\max]$ (passed in `field_scale.w`, `field_bias.w`), then mapped to
colour. We use a **5-stop "turbo-lite"** ramp — blue (cold) → cyan → green → yellow → red (hot):

| $t$ | colour (RGB) | |
|---|---|---|
| 0.00 | (0.20, 0.30, 0.80) | blue |
| 0.25 | (0.10, 0.70, 0.90) | cyan |
| 0.50 | (0.25, 0.80, 0.30) | green |
| 0.75 | (0.95, 0.85, 0.20) | yellow |
| 1.00 | (0.90, 0.20, 0.15) | red |

The colour is the piecewise-linear interpolation between adjacent stops: with $x = 4t$ and segment
$s=\lfloor x\rfloor$, $\mathrm{rgb} = \mathrm{lerp}(c_s, c_{s+1},\,x-s)$. It is monotone in lightness/hue
enough to read a field at a glance and intuitive for temperature (red = hot). A full perceptual map
(viridis / Google's turbo polynomial) is a drop-in replacement for this function later.

The colormapped value becomes the surface **albedo**, which the existing studio shade (Lambert +
hemispherical ambient + Fresnel rim, two-sided so the cut interior is lit) then lights — so the field
reads on the part and on the interior the section reveals. With **no** field bound ($v_\max \le v_\min$)
the albedo is the neutral metal and the shade is byte-for-byte the plain B1/B2 lit pass.

## The legend

`legend.{vert,frag}` draw a vertical bar of the **same** transfer function on the right edge — a true
key for the surface colours (top = $v_\max$, bottom = $v_\min$). Numeric tick labels need text
rendering (Rime's from-scratch UI, brick **E2**); until then the range is printed to the console and the
window title.

## Vector fields: the warp (C3)

A **vector** field — a structural displacement or a modal mode shape (ICEM exports both as vec3 `.icef`
fields) — is shown by **warping** the surface along it. The vec3 is uploaded as an RGBA32F volume
(xyz = vector, w = validity), and the **vertex** shader fetches it (a vertex texture fetch) at each
vertex's rest position $\mathbf p$ and displaces:

$$ \mathbf p' = \mathbf p + g\,\mathbf d(\mathbf p), \qquad \mathbf d = \text{field at }\mathbf p, $$

with a gain $g$. For a **modal mode** the gain is animated, $g(t) = \dfrac{A\,r}{|\mathbf d|_{\max}}\,
\sin(2\pi f t)$, so the shape oscillates between $\pm A r$ peak displacement ($A\approx0.25$ of the part
radius $r$, $f\approx0.6$ Hz) — the mode "breathes". The surface is coloured by the normalised magnitude
$t = |\mathbf d|/|\mathbf d|_{\max}$ through the same colormap, so high-amplitude regions read hot. The
mode shape from `smallest_eigenpair` is $M$-normalised (arbitrary overall scale), which is why the gain
normalises by $|\mathbf d|_{\max}$ — only the *shape* matters. The field volume's descriptor is made
visible to the vertex stage for the fetch (the only RHI change C3 needed).

(The rest normals are reused on the warped surface — an approximation, fine for a deformation preview;
exact warped normals and **streamlines / arrow glyphs** for a true 3-D *velocity* field come with the
flow viz once ICEM computes 3-D CFD, Phase D.)

## Verification

`tests/rhi/mesh_offscreen_test` renders the unit cube with a synthetic field that ramps along z (value 0
at the bottom, 1 at the top) and asserts the part shows **both** blue-dominant (cold) and red-dominant
(hot) pixels — i.e. the world→uvw map, the 3-D sample, and the colormap all ran — while the field-**off**
render stays neutral grey (the plain shade is unchanged). End to end, the viewer loads ICEM's brick23
`field.icef` (temperature 700–800 K on a 7×7×3 grid) and colours the wall patch blue→red.

`tests/rhi/warp_offscreen_test` warps the cube by a vec3 field and checks the surface **moves** (the
warped and undeformed renders differ) and is coloured by magnitude. End to end, `icem_viewer --warp
mode1` animates the brick23 patch's fundamental mode shape, coloured by amplitude.
