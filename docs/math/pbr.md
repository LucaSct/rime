# Physically-based rendering — derivation notes (M5.6)

These notes derive, from the rendering equation down to each line of GLSL, the shading the
forward-PBR pass implements. They are the companion to the code: the shaders in
`engine/render/shaders/` (`pbr_forward.frag`, `tonemap.frag`, `depth_only.vert`) are terse because
the *why* lives here. The pipeline-level choices — forward vs. deferred, HDR-then-tonemap, a depth
pre-pass — are decided in [ADR-0022](../adr/0022-forward-pbr.md); this file derives the physics
those choices carry.

Conventions follow [ADR-0004](../adr/0004-math-conventions.md): right-handed world, linear color
throughout the shader, radiance (not display color) as the fragment output. GitHub renders the
`$…$` / `$$…$$` math below.

---

## 1. The rendering equation, and what a real-time renderer keeps

The outgoing radiance $L_o$ from a surface point $\mathbf p$ toward the eye direction
$\mathbf v$ is the emitted radiance plus every incoming direction $\mathbf l$ over the hemisphere
$\Omega$, weighted by the surface's response:

$$
L_o(\mathbf p, \mathbf v) = L_e(\mathbf p, \mathbf v) + \int_{\Omega} f(\mathbf l, \mathbf v)\,
L_i(\mathbf p, \mathbf l)\,(\mathbf n \cdot \mathbf l)\, \mathrm d\mathbf l .
$$

Three pieces earn their place:

- $f(\mathbf l, \mathbf v)$ — the **BRDF** (bidirectional reflectance distribution function): the
  fraction of light arriving along $\mathbf l$ that leaves along $\mathbf v$, per steradian. §2–§4
  build it.
- $L_i$ — incoming radiance. §5 collapses the integral for **punctual** lights.
- $(\mathbf n \cdot \mathbf l)$ — the **geometry cosine** (Lambert's cosine law): a beam hitting at
  a grazing angle spreads its energy over more surface, so each point receives less. This factor is
  not part of the BRDF; it is the projection of the light's solid angle onto the surface.

We drop $L_e$ (no emissive materials in M5) and, for now, approximate the *rest* of the hemisphere —
every surface and the sky, i.e. indirect light — with a constant (§6). Real GI is M10.

## 2. Splitting the BRDF: diffuse + specular

Light that meets a surface either reflects at the interface (**specular** — a mirror-like bounce off
microscopic facets) or refracts in, scatters, and re-emerges (**diffuse** — the body color). Energy
conservation ties them: a photon reflected specularly cannot also scatter diffusely. So

$$
f = \underbrace{k_d\,\frac{\text{albedo}}{\pi}}_{\text{diffuse (Lambert)}} \;+\;
\underbrace{D\,G\,F \big/ (4\,(\mathbf n\cdot\mathbf v)(\mathbf n\cdot\mathbf l))}_{\text{specular (Cook–Torrance)}} .
$$

The Lambertian $1/\pi$ is not decoration: a perfectly diffuse surface reflects its irradiance
*equally* into the hemisphere, and $\int_\Omega \cos\theta\,\mathrm d\mathbf l = \pi$. Dividing by
$\pi$ is what makes "albedo $= 0.8$" mean "reflects 80% of what it receives" rather than
"$80\%\times\pi$". Forgetting it is the single most common energy bug — the tight bound in the M5.6
test (`pbr_pipeline_test.cpp`, part d) exists to catch exactly a lost or doubled $1/\pi$.

$k_d$ is the diffuse weight — the light left over after specular took its share (§4.4).

## 3. Specular: the microfacet model

A rough surface is modeled as a field of tiny perfect mirrors (**microfacets**), each a little
plane with its own normal $\mathbf m$. Only facets whose normal is the **half-vector**

$$
\mathbf h = \frac{\mathbf v + \mathbf l}{\lVert \mathbf v + \mathbf l\rVert}
$$

reflect $\mathbf l$ exactly into $\mathbf v$. The Cook–Torrance specular term is the product of
three statistics over that facet field:

$$
f_\text{spec} = \frac{D(\mathbf h)\,G(\mathbf v,\mathbf l)\,F(\mathbf v,\mathbf h)}
{4\,(\mathbf n\cdot\mathbf v)(\mathbf n\cdot\mathbf l)} .
$$

- $D$ — how many facets point along $\mathbf h$ (the **normal distribution**).
- $G$ — what fraction of those are neither shadowed nor masked by neighbours (**geometry**).
- $F$ — how reflective the facet is at this angle (**Fresnel**).
- the $4\,(\mathbf n\cdot\mathbf v)(\mathbf n\cdot\mathbf l)$ denominator is the Jacobian that
  converts between the half-vector's measure and the light's — it comes from the change of
  variables $\mathbf m \to \mathbf l$, not from any physical term, and §3.2 folds it away.

### 3.1 D — GGX / Trowbridge–Reitz

$$
D(\mathbf h) = \frac{\alpha^2}{\pi\big((\mathbf n\cdot\mathbf h)^2(\alpha^2-1)+1\big)^2}, \qquad
\alpha = \text{roughness}^2 .
$$

This is `d_ggx`. GGX won over the older Beckmann distribution because of its **tail**: away from the
peak it falls off far more slowly, which is what gives real specular highlights their bright core
*and* a soft surrounding halo instead of a hard disc. At $\mathbf n\cdot\mathbf h = 1$ (a facet
facing straight at $\mathbf h$) it peaks at $1/(\pi\alpha^2)$ — for a mirror-smooth
$\alpha = 0.002$ that is $\sim\!10^5$, an intense pinpoint; for a rough $\alpha = 0.7$ it is $\sim
0.6$, a broad smear. That three-orders-of-magnitude spread across roughness is the *structural*
fact the highlight-tightening proof leans on (test part b).

**The $\alpha = \text{roughness}^2$ remap** (Disney 2012) is perceptual: artists move a `roughness`
slider linearly and expect the *visual* change to feel linear, but the lobe width goes as $\alpha$.
Squaring pushes the visually interesting action into the middle of the slider. So the shader stores
perceptual `roughness` and squares it exactly once, at the point of use.

**The floor.** `roughness` is clamped to $[0.045, 1]$ before squaring. A true $\alpha = 0$ makes $D$
a Dirac delta — infinite radiance from a single infinitely small point under a punctual light, which
blooms to a NaN the moment anything divides by it. Frostbite's $0.045$ floor keeps the highlight
finite and still visually mirror-like. This is why the energy-bound test can assume a finite
$D_\text{max}$.

### 3.2 G/V — height-correlated Smith visibility

Rather than compute $G$ and then divide by $4(\mathbf n\cdot\mathbf v)(\mathbf n\cdot\mathbf l)$
— which is a $0/0$ race at grazing angles — we compute the **visibility** term
$V = G / \big(4(\mathbf n\cdot\mathbf v)(\mathbf n\cdot\mathbf l)\big)$ as one expression. With the
height-correlated Smith formulation (Heitz 2014), used by Frostbite and Filament:

$$
V = \frac{0.5}{\;(\mathbf n\cdot\mathbf l)\,\Lambda_v + (\mathbf n\cdot\mathbf v)\,\Lambda_l\;},
\qquad \Lambda_x = \sqrt{(\mathbf n\cdot x)^2\,(1-\alpha^2) + \alpha^2}\, .
$$

This is `v_smith_ggx` line for line ($\Lambda_v$ is `sqrt(n·v² (1-a2)+a2)`, and each is scaled by
the *other's* cosine). "Height-correlated" means the model accounts for a microfacet being masked
(hidden from the eye) and shadowed (hidden from the light) by the *same* correlated bumps, rather
than treating the two independently — subtly more accurate at grazing angles and no more expensive.
The `max(gv+gl, 1e-5)` guards the last sliver before a perfectly grazing denominator reaches zero.

### 3.3 F — Fresnel (Schlick)

Every surface becomes a perfect mirror at a grazing enough angle (the reflection you see stretching
toward the horizon on a dull floor). Fresnel's equations give the exact reflectance; Schlick's
approximation fits them to within a percent for a fraction of the cost:

$$
F(\mathbf v, \mathbf h) = F_0 + (1 - F_0)\,(1 - \mathbf v\cdot\mathbf h)^5 .
$$

`f_schlick`. $F_0$ is the reflectance at normal incidence — the "base reflectivity." The fifth power
is a curve fit, nothing deeper; it climbs to $1$ as $\mathbf v\cdot\mathbf h \to 0$ for every
material, which is why *everything* has a bright rim.

### 3.4 Metals vs. dielectrics — the `metallic` parameter

$F_0$ is where the metallic workflow lives:

$$
F_0 = \operatorname{lerp}(0.04,\ \text{albedo},\ \text{metallic}).
$$

- **Dielectrics** (metallic $= 0$): non-metals — plastic, wood, stone, skin — reflect a nearly
  colorless $\sim\!4\%$ at normal incidence ($F_0 = 0.04$). Their *color* is the diffuse albedo.
- **Metals** (metallic $= 1$): conductors have **no diffuse** — free electrons absorb refracted
  light immediately — and their specular reflection is *tinted*, so $F_0$ *is* the base color
  (gold reflects yellow, copper orange).

Real surfaces are one or the other; the slider exists to blend at boundaries (a scratched painted
metal) and to let one shader path serve both. The energy split in §4.4 reads `metallic` again to
zero out a metal's diffuse.

## 4. Assembling one light's contribution

`shade_light` computes, for a single light with incoming radiance $L_i$ (already reduced to a
direction and a color — §5):

1. $\mathbf n\cdot\mathbf l \le 0$ → return zero. The light is behind the surface; there is no
   transport, and clamping (rather than letting a negative cosine through) prevents light *leaking*
   through to the far side.
2. $\mathbf h = \widehat{\mathbf v + \mathbf l}$, and the clamped cosines $\mathbf n\cdot\mathbf v$
   (floored at $10^{-4}$ so a silhouette pixel doesn't divide by zero), $\mathbf n\cdot\mathbf h$,
   $\mathbf v\cdot\mathbf h$.
3. specular $= D\,V\,F$ (the $/4(\mathbf n\cdot\mathbf v)(\mathbf n\cdot\mathbf l)$ already inside
   $V$).
4. **Energy split.** The specularly reflected fraction is $F$ itself; what is left to refract and
   scatter diffusely is $1 - F$. Metals scatter nothing. So
   $$
   k_d = (1 - F)\,(1 - \text{metallic}), \qquad f_\text{diffuse} = k_d\,\frac{\text{albedo}}{\pi}.
   $$
5. Return $(f_\text{diffuse} + f_\text{spec})\,L_i\,(\mathbf n\cdot\mathbf l)$ — the integrand of §1,
   evaluated for this one direction.

Summing over lights (with a constant ambient added, §6) gives $L_o$. That radiance — which routinely
exceeds $1$ — is written straight to the RGBA16F target. Nothing is clamped to display range in this
shader; that is the tonemap's job (§7), and keeping the two separate is the whole point of rendering
in HDR.

## 5. Lights: from the hemisphere integral to a sum

### 5.1 Punctual lights

A **directional** light (the sun) is infinitely far: one direction, one radiance, no falloff. Its
`direction` is the way light *travels*, so the direction *toward* the light is $-\text{direction}$.

A **point** light radiates from a position. The hemisphere integral collapses to a single evaluation
because the light subtends (effectively) zero solid angle — the Dirac delta in $L_i$ picks out one
$\mathbf l$ and cancels the integral, leaving the BRDF times the light's radiance at the surface
times $\mathbf n\cdot\mathbf l$. (This is exactly why punctual lights are cheap and why area lights,
which *don't* collapse, are a later, harder problem.)

### 5.2 Falloff {#falloff}

Radiant power spreads over the surface of a growing sphere, so irradiance obeys the **inverse-square
law** $E \propto 1/d^2$. Physically correct light never quite reaches zero, though — which is useless
for a renderer that wants to know *which* lights touch a given pixel (the many-lights culling problem
of M10). So we multiply the inverse-square by a smooth **windowing** function that is $1$ near the
light and reaches *exactly* zero at the light's radius $r$ (Karis 2013 / Frostbite):

$$
\text{falloff}(d) = \frac{\operatorname{sat}\!\big(1 - (d/r)^4\big)^2}{d^2}.
$$

`falloff` in the point-light loop. The $(d/r)^4$ inside, squared outside, gives a gentle plateau then
a fast-but-smooth (continuous value *and* derivative) roll to zero — no hard edge where a light
snaps off. The $1/d^2$ still dominates the near field, so brightness near the light stays physical.
$d^2$ is floored ($10^{-4}$) and $r$ floored ($10^{-3}$) so a light *at* a surface point, or a
zero-radius light, cannot divide by zero. Folding `color × intensity` into the light's `radiance` on
the CPU (see `scene_renderer.cpp`) means the shader multiplies falloff by one value, not three.

## 6. Ambient — the honest hack {#ambient}

The hemisphere integral in §1 covers *all* incoming light, but §5 only summed the explicit lights.
Everything else — skylight, and light bounced off other surfaces — is **indirect illumination**, and
computing it for real is global illumination (M10). Until then, unlit sides would be pure black,
which reads as broken.

The stand-in: treat the missing indirect light as a single constant ambient radiance $L_a$ arriving
uniformly from everywhere, and reflect it diffusely:

$$
L_\text{ambient} = \text{albedo}\cdot L_a .
$$

`out_radiance = albedo * frame.ambient.rgb` before the light loops. It is deliberately crude — it
ignores $\mathbf n$, occlusion, and the specular response entirely — but it keeps unlit surfaces
readable and, at a dim default ($L_a = 0.02$), stays an order of magnitude below any directly lit
pixel, so the M5.6 lit-vs-unlit proof can still tell them apart. Being visibly a hack is the point:
the day GI lands, this line retires to a fallback — see §6.1.

### 6.1 Retiring the hack, when GI is on {#ambient-retired}

M10 is that day, for the shadowed forward path. `pbr_forward_shadowed.frag` now branches:

```glsl
if (ddgi.enabled) out_radiance = ddgi_indirect(world_pos, n) * albedo * ao;  // the traced field
else              out_radiance = albedo * frame.ambient.rgb * ao;            // the M5.6 hack
```

The field **replaces** the constant; it does not add to it. That is the entire integration step
(m10.6), and getting it wrong is a real energy bug. The constant $L_a$ stood in for *all* indirect
light, skylight included — and DDGI already carries the skylight: a probe ray that escapes the SDF
without hitting anything returns exactly `frame.ambient` as its sky term (`ddgi_trace.comp`), so that
same constant is already folded into the probe irradiance over the escaped fraction of every probe's
hemisphere. Summing the flat constant *on top* of the field would count the sky twice — the classic
"ambient + GI" wash-out that flattens contact shadows and lifts every crevice off its floor. So once
the field exists, the placeholder retires.

Two consequences the tests pin (`gi_thesis_test.cpp`):

- **In the open-sky limit the two agree.** A surface that sees mostly unobstructed sky receives, from
  DDGI, almost exactly what the flat constant gave it — because DDGI's sky term *is* that constant
  (measured agreement: one 16-bit LSB). The retirement is invisible where the hack was already right;
  it only bites where the hack was wrong.
- **Where the hack was wrong, GI is honestly darker or brighter.** An enclosed room the constant
  painted at $0.8\,L_a$ reads *dimmer* under GI (the sky it assumed is occluded); a patch catching
  fresh bounce off a newly-sunlit neighbour reads *brighter*. Both are the field seeing an occlusion
  or a bounce the constant is blind to — and neither can create energy: the trace is single-bounce
  with a grey-world albedo $<1$ and no probe-feeds-probe feedback, so stored irradiance is bounded by
  its brightest visible source (the m10.5a open-floor pin, $\approx 1.66$, is that finite ceiling;
  the enclosed-room-stays-below-ambient check is the dynamic witness that no free energy appears when
  a wall falls).

The `else` branch is byte-for-byte the M5.6 line above, so with DDGI off — every M10 feature off — the
frame is identical to the pre-GI renderer (ADR-0032 §11). The hack didn't leave; it retired to a
fallback.

**Still deferred** (grey-world albedo, ddgi.md §5): the bounce carries no *colour* yet — a white floor
beside a red wall does not redden, because the SDF hit returns a grey albedo, not the wall's. Coloured
bleed needs a surface-albedo cache and is m10's named GI follow-up; the *specular* half of indirect
(reflections) is m10.7; a probe-sphere / indirect-only **debug view** waits on the view-mode plumbing
m10.3's cluster heatmap also wanted (m10.8).

## 7. Display: HDR → tonemap → sRGB {#display}

The forward pass emits linear **radiance** in RGBA16F, unbounded above $1$. A monitor wants an
sRGB-encoded value in $[0,1]$. Two distinct jobs, in the only order that works (`tonemap.frag`):

### 7.1 Tonemapping — compress the range

**Reinhard** is the pedagogical baseline: $x \mapsto x/(1+x)$ maps $[0,\infty) \to [0,1)$, monotone.
It works but desaturates and flattens bright regions — the highlights that HDR existed to preserve.

We ship **Narkowicz's ACES fit** — a rational approximation of the film-industry ACES tone curve:

$$
\text{tonemap}(x) = \operatorname{sat}\!\left(\frac{x\,(2.51x + 0.03)}{x\,(2.43x + 0.59) + 0.14}\right).
$$

A gentle **toe** keeps shadow contrast, a long **shoulder** rolls highlights off gracefully instead
of clipping them to flat white, and very bright values desaturate the film-like way the eye expects.
Exposure (scaling radiance before the curve) is assumed $1$ — a real exposure system, with physical
light units, is M10.

### 7.2 sRGB encode — the display transfer function

Monitors are not linear: an sRGB display raises its input to $\approx 2.2$ before emitting light. So
we pre-apply the inverse (the sRGB OETF, IEC 61966-2-1):

$$
\text{encode}(c) = \begin{cases} 12.92\,c & c \le 0.0031308 \\ 1.055\,c^{1/2.4} - 0.055 & c > 0.0031308 \end{cases}
$$

`srgb_encode`. It is *not* a plain gamma $2.2$: the small linear segment near black avoids the
infinite slope $c^{1/2.4}$ has at the origin (which would crush and add noise to the darkest tones).
Skip this step and everything mid-tone looks too dark — the classic "why is my render muddy" bug.

We encode **in the shader** rather than writing to an sRGB-format target (which would encode in
hardware for free) so the choice is visible in one place and the pass works with any Unorm target;
[ADR-0022](../adr/0022-forward-pbr.md) notes that a swapchain-facing present pass can switch to the
hardware encode later. Note the asymmetry: base-color *textures* are sRGB-*format* (§ input), so the
sampler hardware **decodes** them to linear on the way in; we only hand-roll the **encode** on the
way out.

## 8. The depth pre-pass contract

The depth pre-pass renders the whole scene with no fragment shader and no color target, laying down
only the nearest-surface depth. The forward pass then tests `CompareOp::Equal` against that depth and
writes color once per pixel — overdraw pays a cheap vertex transform in the pre-pass instead of a
full BRDF in the forward pass. Whether that is a *win* depends on how much overdraw and how expensive
the shading is, so the pass is optional per frame (`SceneRenderer::render(..., use_depth_prepass)`),
and the proof asserts both paths produce **byte-identical** output.

The load-bearing requirement: the forward pass's `Equal` test only passes if the depth it computes is
**bit-identical** to what the pre-pass stored. Two different pipelines (one with a fragment stage, one
without) run two vertex-shader compilations, and a compiler is free to reassociate floating-point
arithmetic differently between them — $a\cdot(b\cdot c)$ vs $(a\cdot b)\cdot c$ round differently,
and a one-ULP disagreement makes the fragment fail its own depth test, dropping the surface in a
shimmer of Z-fighting.

Two things prevent it: both shaders compute `gl_Position` with the **textually identical**
expression `view_proj * (model * vec4(pos, 1.0))` — same parenthesization, same operand order — and
both declare `invariant gl_Position`, the GLSL qualifier that forbids the compiler from optimizing
that output differently across pipelines. Together they guarantee the two pipelines rasterize every
triangle at exactly the same sub-pixel position and depth. `CompareOp::LessEqual` is the documented
fallback if a driver ever violates invariance, at the cost of admitting some double-shading.

---

*These notes back brick **M5.6**. Code: `engine/render/shaders/{pbr_forward,tonemap,depth_only}.*`,
`engine/render/src/{passes,scene_renderer}.cpp`. Decisions: [ADR-0022](../adr/0022-forward-pbr.md).
Proofs: `tests/render/pbr_pipeline_test.cpp` (structural radiometric asserts on a metallic×roughness
sphere grid). Sources: Cook & Torrance 1982; Walter et al. 2007 (GGX); Heitz 2014 (height-correlated
Smith); Karis 2013 (UE4 course notes); Lagarde & de Rousiers 2014 (Frostbite PBR); Narkowicz 2015
(ACES fit).*
