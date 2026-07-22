# Screen-space reflections — derivation notes (m10.7b + m10.7c)

These notes derive how Rime turns the specular term from a highlight-only approximation into a real
reflection of the scene the renderer just drew: the **reflection ray** built in view space, the
**fixed-step linear march** that walks it through the depth buffer, how a screen position and a depth
sample are turned back into a view-space **position** by inverting the projection, the **thickness**
test that decides a hit, the **miss classification** that keeps the effect honest at screen edges,
and the **Fresnel** and **roughness** weights that blend the reflection into the frame
energy-consistently.

**m10.7c** completes the picture with the two follow-ups m10.7b named: where the screen march has no
answer — a ray off the screen, into the background, or belonging to a surface too rough for a sharp
sample — the reflection falls back to the **DDGI probe field** (§5.1) sampled in the reflection
direction, instead of a flat sky constant; and the roughness fade becomes a **cone** (§6) that blends
the sharp screen hit toward that (inherently blurry, pre-integrated) field as roughness rises, so a
rough surface reflects a blurred world rather than a black hole. With DDGI off, both reduce exactly to
m10.7b's flat-ambient behaviour.

They are the companion to the code, which is deliberately terse because the *why* lives here:

- `engine/render/shaders/ssr_resolve.frag` — the resolve: one fragment per pixel, the march, the
  hit/miss logic, the probe fallback and roughness cone (m10.7c), and the weighted composite.
- `engine/render/src/lighting/ssr.cpp` / `include/rime/render/lighting/ssr.hpp` — the engine side:
  the fullscreen pipeline, the `GpuSsrUniforms` block the shader reads, the point+clamp sampler, and
  the DDGI atlas bindings the fallback samples.
- `tests/render/ssr_test.cpp` — the structural proofs. m10.7b: a smooth floor reflects a bright cube
  onto a known pixel, a matte floor does not, the sky-facing control does not brighten. m10.7c: a
  reflective floor reflects a sunlit wall that exists *only as an SDF twin* (invisible to the screen,
  so the reflection must be the probe fallback), on both a smooth floor (the fallback) and a rough one
  (the cone — where m10.7b showed nothing).

The thin G-buffer the march consumes (octahedral world normal + roughness + a geometry mask) is
[m10.7a](../../engine/render/README.md); its own encoding is derived in
[tangent-space.md](tangent-space.md) (the octahedral mapping is shared with DDGI, see
[ddgi.md](ddgi.md)).

Conventions follow [ADR-0004](../adr/0004-math-conventions.md): right-handed world and view space
(the camera looks down $-z$), column-vector math ($\mathbf v' = M\mathbf v$), Vulkan clip space with
$z \in [0,1]$ and a **y-down** NDC (our `perspective` bakes the y-flip). The design context is
[ADR-0032 §5](../adr/0032-lighting-v2.md). GitHub renders the `$…$` / `$$…$$` math below.

---

## 1. The reflection ray

SSR asks a simple question at every lit pixel: *if this surface were a mirror, what would it show?*
The answer is the colour along the ray that leaves the surface in the mirror direction of the view
ray.

The pass runs in **view space**, where the camera sits at the origin looking down $-z$. Reconstruct
the shaded pixel's view position $\mathbf p$ (§2), read its world normal from the G-buffer and rotate
it into view space as $\mathbf n$. The view ray is the direction from the eye to the surface,
$\hat{\mathbf v} = \mathbf p / \lVert\mathbf p\rVert$ (the eye is the origin, so this is just the
normalized position). The reflection ray direction is the classic mirror reflection about the normal:

$$\mathbf r = \hat{\mathbf v} - 2(\hat{\mathbf v}\cdot\mathbf n)\,\mathbf n.$$

This is GLSL's `reflect(v, n)`. Working in view space (rather than world) is what makes the next
step — projecting a marched point back to the screen — a single matrix multiply by the same
projection the camera already uses.

## 2. A screen position + a depth back to a view position

The march needs to compare the ray against the geometry the depth buffer records, and both live as
*positions*, not just directions. Recovering a view-space position from a pixel is the inverse of the
projection.

A pixel at texture coordinate $\mathbf{uv}\in[0,1]^2$ with stored NDC depth $z_d\in[0,1]$ has clip
coordinates $\mathbf c = (2u-1,\; 2v-1,\; z_d,\; 1)$ (the $2t-1$ maps $[0,1]$ texture space to
$[-1,1]$ NDC; the y-flip is already baked into `perspective`, so the same $\mathbf{uv}$ the frame was
rendered with round-trips). Applying the inverse projection and doing the **perspective divide**
gives the view position:

$$\mathbf x = \frac{P^{-1}\mathbf c}{(P^{-1}\mathbf c)_w}.$$

The divide is what turns a homogeneous *ray* into a *point* — skip it and every depth comparison
below is wrong by a per-pixel scale. $P^{-1}$ is computed once on the CPU (`ssr.cpp`) and handed to
the shader in `GpuSsrUniforms`, so the march never inverts a matrix.

## 3. The linear march

v1 marches with fixed steps — no hierarchical-Z acceleration (that is m10.7c; it needs a depth
pyramid pass). From the surface point $\mathbf p$ the shader walks

$$\mathbf s_i = \mathbf p + \mathbf r \cdot (i\,\Delta),\qquad i = 1\dots N,\qquad
  \Delta = \frac{d_{\max}}{N},$$

where $N$ is `ssr_max_steps` and $d_{\max}$ is `ssr_max_distance` in view units. Each step is
projected to the screen — $\mathbf c_i = P\,\mathbf s_i$, then $\mathbf{uv}_i = \tfrac12(\mathbf
c_i.xy/\mathbf c_i.w)+\tfrac12$ — and the depth buffer is sampled there and reconstructed back to a
view position $\mathbf q_i$ (§2) so the ray can be compared against the surface actually on screen at
that pixel.

Fixed **view-space** steps (rather than fixed screen-space steps) keep the sampling density roughly
uniform along the ray in the world; the tradeoff is over-sampling near the camera and under-sampling
far away, which the thickness test (§4) tolerates and a DDA/hi-Z march (m10.7c) removes.

## 4. The hit, and the thickness problem

Both $\mathbf s_i.z$ (the ray) and $\mathbf q_i.z$ (the surface the depth buffer records at that
screen pixel) are negative — the camera looks down $-z$. The ray has gone *behind* the visible
surface exactly when it is further from the camera, i.e. more negative in $z$:

$$\delta_i = \mathbf q_i.z - \mathbf s_i.z > 0.$$

But $\delta_i > 0$ alone is not a hit. The depth buffer is a **height field of the front-most
surface** — it has no idea how thick anything is. A ray that dips a hair behind a thin railing would,
on $\delta>0$ alone, "reflect" whatever wall is metres behind the railing. So a hit also requires the
crossing to be *shallow*:

$$0 < \delta_i < \texttt{thickness}.$$

`ssr_thickness` is the assumed thickness of on-screen geometry, in view units. Too small and the
march tunnels *through* real surfaces (steps straddle a thin crossing and never land inside the
band); too large and it hallucinates reflections of hidden geometry. There is no correct value — the
depth buffer genuinely lacks the information — which is why thickness is a tuned knob and SSR is a
screen-space *approximation*, not a solution. On the hit, the reflected colour is the frame's own
lit colour sampled at $\mathbf{uv}_i$.

## 5. Miss classification — where the screen has no answer

A screen-space technique can only reflect what is on screen. The march stops early, and falls back to
the term $\mathbf p$ (the **probe fallback** of §5.1 when DDGI is on, otherwise the flat ambient/sky
constant $\mathbf a$ = `ssr_ambient`, matched to the forward pass's ambient so a miss is invisible),
in four honest cases:

- **Off the top of the frustum** — a step lands in front of the near plane ($\mathbf s_i.z > -z_n$);
  there is nothing in front of the camera to reflect.
- **Behind the camera** — $\mathbf c_i.w \le 0$; the projection would wrap.
- **Off the side of the screen** — $\mathbf{uv}_i \notin [0,1]^2$; the reflected geometry is out of
  frame. This is the classic SSR failure and the reason for the **edge fade** below.
- **Background** — the sampled pixel is the far plane ($z_d \ge 1$); keep marching (the ray may still
  hit something nearer along its length), and if it never does, it is a miss.

Because a miss returns a term the frame already agrees on — the same field the forward pass shaded
with, or the same ambient with DDGI off — and the whole reflection is gated by the weights of §6, the
no-reflection case is continuous with "SSR off": no black holes, no seams.

### 5.1 The probe fallback (m10.7c)

A flat sky constant is a poor answer to "what does this off-screen ray reflect?" — a shiny floor in a
lit room should mirror *the room*, not a uniform grey, even for the parts of the room the screen
cannot see. m10.7c answers the miss from the **DDGI irradiance field** m10.5 already maintains
(`docs/math/ddgi.md`): the reflection ray $\mathbf r$ is transformed to world space (via `inv_view`,
the reason that matrix joins `GpuSsrUniforms`) and the field is sampled *in the reflection direction*

$$\mathbf p = \operatorname{DDGI}(\mathbf x_\text{world},\, \mathbf r_\text{world}),$$

the identical 8-probe trilinear, Chebyshev-weighted lookup the forward shader uses for indirect
diffuse — only with the reflection direction where the forward pass passes the surface normal. When
`ddgi.enabled` is 0 (DDGI off, or its clipmap gate off) the fallback is the flat ambient $\mathbf a$,
so the pass is byte-for-byte m10.7b.

This couples reflections to the M10 thesis: break a wall, the room behind it fills with bounced light
(m10.5b), and a reflective floor *there* picks that light up in the same frame the probes do — the
same field, sampled twice (diffuse in the forward pass, specular here).

**What it approximates, and what it gets wrong.** The irradiance field stores $E(\omega)$, the
cosine-weighted integral of incoming radiance over the hemisphere about $\omega$ — a *diffuse*
quantity. Sampling it as if it were the directional *radiance* $L(\mathbf r)$ arriving along the
reflection ray is an approximation that **over-blurs**: it returns a hemisphere average where a true
specular probe would return a sharp environment sample. Two honest consequences follow. For the rough
end of the cone (§6) the over-blur is *correct* — a rough lobe wants exactly that wide average. For a
sharp mirror miss it is only a plausible gap-filler: the reflection is smoother and lower-frequency
than the real off-screen geometry would be, but it is continuous, GI-consistent, and never a black
hole. A true specular probe (a pre-filtered radiance cubemap per probe, or an SDF second-trace) is the
named upgrade; the field reuse is what makes v1 free. The `ddgi_sample_irradiance`'s front-facing
"wrap" weight is tuned for surface normals, not reflection rays, so at steep reflection angles some
probes drop out and the re-normalization leans on the survivors — acceptable for a fallback, noted
here rather than hidden.

### Edge fade

A reflection that pops off hard at the screen border is the tell that gives SSR away. Where a hit's
$\mathbf{uv}$ nears an edge, its weight $w_\text{edge}$ falls off over a 10 % border on each side:

$$w_\text{edge} = \prod_{k\in\{x,y\}} \operatorname{smoothstep}(0,0.1,u_k)\,\bigl(1-\operatorname{smoothstep}(0.9,1,u_k)\bigr).$$

In m10.7b the contribution simply faded to zero there; in m10.7c the border **fills with the probe
fallback** instead (§6), so a reflection that leaves the frame hands off to the probe field rather
than vanishing.

## 6. Blending it in — Fresnel, the edge, and the roughness cone

The reflection is **added** to the pixel's own lit colour, scaled so it obeys the things physics
insists on. Let $\mathbf s$ be the sharp screen-march result (the hit colour, or the probe $\mathbf p$
on a miss) and $\mathbf p$ the probe fallback of §5.1.

**The edge blend.** Rather than fade the sharp hit to zero at the border, blend it toward the probe by
the edge weight — so the seam fills with real radiance:

$$\mathbf{m} = \operatorname{lerp}(\mathbf p,\ \mathbf s,\ w_\text{edge}).$$

**The roughness cone.** A single sharp mirror sample is only right for a smooth surface. As roughness
climbs, the true reflection is a *blurred cone* of directions — and the DDGI irradiance field is
exactly a pre-integrated, hemisphere-wide average (§5.1), i.e. the wide-cone limit of that reflection.
So m10.7c does not *fade SSR out* on a rough surface (m10.7b's stop-gap); it **blends the sharp screen
reflection toward the blurry probe field** as roughness rises:

$$c = \operatorname{smoothstep}(0.25, 0.55, \text{roughness}),\qquad
  \mathbf{r} = \operatorname{lerp}(\mathbf m,\ \mathbf p,\ c).$$

At $\text{roughness} \le 0.25$ the cone is $0$ (a mirror — the screen march), at $\ge 0.55$ it is $1$
(pure probe field, and the march is skipped entirely, so a rough surface pays nothing for a screen
trace it would only discard). Between, the reflection is a real blend of screen detail and blurred
environment. Using the probe field as the wide-cone limit is what lets a rough surface reflect a
*blurred world* instead of the black hole m10.7b left — without the mip-chained screen-colour blur the
original sketch imagined (that sharpens the mid-roughness band using on-screen data and is a named
follow-up; it needs an RHI mip-generation/LOD surface that does not exist yet).

**Fresnel.** Reflectivity rises at grazing angles — the effect that sells a wet floor or a lake at
sunset. Schlick's approximation at a dielectric base reflectance $F_0 = 0.04$:

$$F = F_0 + (1-F_0)\,(1 - \mathbf n\cdot(-\hat{\mathbf v}))^5.$$

$\mathbf n\cdot(-\hat{\mathbf v})$ is the cosine between the surface normal and the direction *back to
the camera*. The whole reflection — screen hit or probe field alike — is the specular lobe, so it is
modulated by Fresnel exactly as an environment reflection would be. The final composite is

$$\mathbf{c}_\text{out} = \mathbf{c}_\text{lit} + \mathbf{r}\cdot F.$$

With DDGI off, $\mathbf p = \mathbf a$ (the flat ambient), and the two `lerp`s collapse to m10.7b's
$\mathbf{c}_\text{lit} + \mathbf{c}_\text{refl}\,F\,w_\text{rough}\,w_\text{edge}$ to within the
negligible $\mathbf a\,F$ a miss now adds — the regression bridge the smooth/matte proof pins.

## 7. The artifact zoo v1 accepts (honesty beats chasing)

SSR is famous for its failure modes, and v1 catalogs the ones it lives with rather than pretending
they are gone:

- **Screen-edge cutoff** — reflections of off-screen geometry are not on the screen to sample; m10.7c
  **fills** the gap from the probe field (§5.1) rather than leaving a hole, but the fill is
  approximate (over-blurred, low-frequency — a diffuse field standing in for directional radiance),
  not the true off-screen reflection.
- **Thickness error** — the single tunable thickness is wrong for both very thin and very thick
  geometry at once (§4).
- **Missing contact / disocclusion** — a reflected object hides geometry behind itself in the source
  frame, so its own reflection can have holes (the probe fallback softens, does not fix, this).
- **Rough reflections are the diffuse field** — m10.7c blurs into the DDGI irradiance field as
  roughness rises (§6) rather than fading to nothing, but that field is an over-blurred stand-in for a
  true pre-filtered specular probe; the mid-roughness screen-colour mip-blur that would sharpen it is
  a named follow-up.
- **No temporal stability** — no accumulation/TAA yet, so a moving camera shimmers; deferred for v1
  (there is no TAA infrastructure to hang it on).

Two capabilities the m10.7 sketch imagined are **deferred with named triggers**, not silently
dropped: a **hi-Z acceleration march** (a depth-pyramid DDA in place of the fixed linear steps) waits
for **m12.0** — it is a pure performance optimization whose only payoff is unmeasurable on lavapipe's
exact CPU-rendered depth, while the linear march is already structurally correct; and the
**mip-chained screen-colour blur** for the mid-roughness band waits on an RHI mip-generation/LOD
surface that does not exist yet (the probe cone covers rough reflections without it). Both slot in
behind the same `SsrInputs` / `GpuSsrUniforms` seam.

## 8. Why the resolve is a fullscreen raster pass, and what the proof reads

The march has no need of anything compute offers — no groupshared cooperation, no scatter — so the
resolve is a **fullscreen fragment shader** that writes its result as an ordinary colour attachment,
and m10.7c's probe fallback is just more texture sampling in that same fragment (the DDGI atlases at
three more bindings), no reason to leave raster. That attachment is then sampled by the tonemap
through the exact colour-attachment→sampled path every `forward → tonemap` frame already exercises.
Compute earns its place at **m12.0**, where a hi-Z march wants a shared depth pyramid and temporal
accumulation wants to read and write a history buffer; both slot in behind the same `SsrInputs` /
`GpuSsrUniforms` seam.

The structural proof reads back **`Output::hdr`** — the resolved HDR the tonemap consumes (the
reflection-added target with SSR on, the raw forward HDR with SSR off) — rather than the tonemapped
LDR, exactly as the DDGI thesis proof does. Two reasons: asserting on linear radiance is cleaner than
reasoning through the ACES curve, and it reads the resolve's own output directly rather than one more
pass downstream. The proof is **structural**, never a golden image (the M5.6/M6.4 pattern): m10.7b — a
smooth dark floor's mirror pixel brightens by a wide margin with SSR on, a matte floor's does not (the
roughness fade), and a floor pixel whose mirror ray flies into empty sky does not brighten (it is a
reflection, not a global lift); m10.7c — a reflective floor reflects a sunlit wall present *only* as
an SDF twin (invisible to the screen, so the reflection can only be the probe fallback), on a smooth
floor (the fallback) and a rough one (the cone, where m10.7b showed nothing). Absolute per-pass
timings wait for real hardware at m12.0; lavapipe renders exact depth, so the geometry of the march
and the fallback is provable there even though its cost is not representative.
