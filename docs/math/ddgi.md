# DDGI — dynamic diffuse global illumination, traced against a signed-distance field

m10.5a's derivation (§1–§8): the irradiance-field idea, spherical Fibonacci sampling, the
octahedral atlas and its border (the classic bug), temporal hysteresis and what it costs in
latency, and the v1 grey-world albedo limitation. m10.5b's derivation (§9–§13, ADR-0032 §11's
"consumption + reactivity" half): how a SHADING pass samples the two atlases back out (the
probe-cage trilinear blend, and the octahedral ENCODE that is §6's decode run backwards), the
Chebyshev visibility weight that stops a probe leaking light through solid geometry, and
`invalidate()` — the destruction-reactive override that turns §8's ~30-frame hysteresis latency
into a handful of frames. Code: `engine/render/include/rime/render/lighting/ddgi.hpp`,
`engine/render/src/lighting/ddgi.cpp`, `engine/render/shaders/ddgi_trace.comp`,
`ddgi_blend_irradiance.comp`, `ddgi_blend_visibility.comp`, `pbr_forward_shadowed.frag` (the
consumer). Reference: Majercik, Gallo, Falcao, Kirchhefer, Krajcevski, "Dynamic Diffuse Global
Illumination with Ray-Traced Irradiance Fields", *JCGT* 8(2), 2019 (the HPG talk of the same
title; also *Ray Tracing Gems* ch. 25). ADR-0032 §2.

## 1. The idea: a lattice of "what does the light look like from here?" probes

Direct lighting answers "how much light reaches this SURFACE point, from this ONE direction (the
sun)?" Global illumination needs the harder question: "how much light arrives at this point, from
EVERY direction, after bouncing off whatever else is in the scene?" Baking that into lightmaps is
the traditional answer — fast at runtime, but frozen the instant a wall you baked around falls
down. DDGI's answer is to compute it live, at a sparse set of points (**probes**) scattered through
the world, each storing an approximation of "the light seen from here" as a function of direction —
and to update that function continuously, a little at a time, every frame, so a probe never needs a
full recompute, only a nudge toward the truth.

Concretely: a probe casts a batch of rays out into the scene. Each ray either hits a surface (which
we shade) or escapes (in which case it sees the sky). The results are folded into two small
per-probe images — **irradiance** (how bright, and in what direction) and **visibility** (how far
away is the nearest thing, for shadowing the probe's own data later) — with a slow *temporal blend*
so single-frame noise averages out over many frames instead of shimmering.

The one substitution Rime makes relative to the reference paper: it has no hardware ray tracing
(ADR-0032's platform floor — lavapipe, forever). Instead of tracing triangles, a probe ray
**sphere-traces through the SDF clipmap** (m10.4b, `docs/math/sdf.md`) the same way m10.4c's spike
measured. This is *why* an SDF clipmap was decision 2's chosen GI substrate in the first place: it
is the thing that makes tracing affordable without hardware RT, and it composes destructible
geometry the moment it changes (m10.4b), which is the entire "walls fall, light updates" thesis.

## 2. The probe lattice

Probes sit on a regular 3-D grid — `count_x × count_y × count_z` of them, `spacing` metres apart —
camera-centred so the densest coverage always surrounds the player, exactly the SDF clipmap's own
reasoning for being a *clipmap* rather than one huge fixed volume. Recentring every frame uses the
identical anti-shimmer trick the clipmap's levels use for their voxel grid (`sdf.md` §8): floor the
ideal (camera-centred) origin to a whole multiple of `spacing`. Sub-spacing camera motion leaves the
snapped origin bit-identical — no probe moves, no history is lost — while crossing a whole spacing
step moves every probe by exactly one lattice step and, in this v1, resets every probe's temporal
history (§6 explains why that is an honest simplification rather than a free lunch).

A probe answers for its OWN position, not a cell's average — `probe(x,y,z)` sits AT
`origin + (x,y,z)*spacing`, no half-cell offset (unlike a voxel, which the SDF's OWN sampling
places at cell centres).

## 3. Sampling directions: spherical Fibonacci, rotated every frame

A probe cannot afford to trace every direction, so it samples `rays_per_probe` (64 by default — the
DDGI-paper norm, and the m10.4c spike's own measured-affordable number) directions chosen by the
**spherical Fibonacci** construction (Keinert et al., 2015): for ray `i` of `n`,

```
golden = π(3 − √5)                     // ≈ 2.3999, the golden angle
y      = 1 − 2(i + 0.5)/n              // z-coordinate, evenly spaced in [-1, 1]
r      = √(1 − y²)
θ      = golden · i
dir    = (cos θ · r,  y,  sin θ · r)
```

This spreads `n` points nearly uniformly over the *whole* sphere with no clustering at the poles —
a low-discrepancy sequence from a closed form, no rejection sampling needed. It is the same formula
already measured in the m10.4c spike (`tests/render/sdf_clipmap_test.cpp`'s `fibonacci_direction`),
mirrored three times now (there, `ddgi_fibonacci_direction` in `ddgi.cpp`, and
`ddgi_trace.comp`'s own copy) — one formula, three call sites, because no shader-include mechanism
exists yet (the same constraint `sdf_compose.comp`/`sdf_probe_trace.comp` already live with).

**Why rotate it every frame.** The 64 directions above are a FIXED set — every probe, every frame,
would sample the exact same 64 rays without the rotation, and no amount of temporal averaging fixes
a biased sample (averaging the same 64 points forever converges to what those 64 points see, not to
the true continuous integral). `DdgiProbes::next_ray_rotation()` draws a fresh, uniformly random
rotation each frame (a random axis via standard sphere point-picking, turned through a random angle
in [0, 2π) — Majercik et al.'s own scheme) and every ray this frame is rotated by it before tracing.
Now successive frames sample DIFFERENT points on the sphere, and the temporal blend (§6) is
averaging over an ever-growing, unbiased sample set — this is what "converges to the true integral"
means concretely. The rotation is a `Mat4` built from `core::quat_from_axis_angle` +
`core::to_mat4` — reused, not re-derived — and its own RNG is a tiny deterministic
[splitmix64](https://prng.di.unimi.it/splitmix64.c) (Vigna): not cryptographic, but deterministic,
which is what keeps the temporal-convergence test (§8) reproducible.

## 4. Tracing and shading one ray

Each ray is `sdf_sphere_trace(probe_pos, rotated_dir, max_trace_distance)` — the identical function
`sdf_probe_trace.comp` already proved (Hart 1996's sphere tracing, lifted verbatim into
`ddgi_trace.comp`; see `sdf.md` §10 for the marching itself). Two outcomes:

**A miss** (the ray escapes to `max_trace_distance`) hands back the flat **sky term** —
`SceneRenderer`'s own ambient constant, unchanged since M5.6. Not multiplied by anything: this is
not a reflection off a surface, it is what the sky looks like.

**A hit** needs a surface **normal** the SDF does not store explicitly — recovered as the field's
own gradient by central differences (`sdf_normal` in `ddgi_trace.comp`), stepped by half the finest
clipmap level's voxel size. The normal feeds direct Lambertian shading against the sun:
`radiance = albedo · sun_radiance · max(dot(normal, to_sun), 0)`, `to_sun` being the negated sun
travel direction (the same convention `pbr_forward.frag` already uses).

**Self-shadowing.** A naked NdotL term would light up ANY surface facing sunward, including the
inside of a sealed room the sun cannot possibly reach — an SDF has no separate shadow-map
machinery, so this brick adds a SECOND sphere-trace, from the hit point toward the sun, through the
very same clipmap. If it escapes, the point is lit; if it hits something first, it is shadowed. This
is the classic, well-known advantage of SDF rendering (occlusion is "free" — one more trace, no
separate shadow pass) and it is what makes "a probe sealed inside a box stays dark" a fact about the
geometry rather than a fact about a test scene built not to have a sun in it. The brief's
alternative — wiring `CascadedShadowMap`'s cascade array, per-cascade matrices, and PCF into a
COMPUTE pass built around an entirely different binding shape — was considered and set aside as
disproportionate coupling for this brick; the self-shadow trace costs roughly one more trace per
HIT ray (misses never pay it) and needs zero new bindings.

## 5. The grey-world albedo — a named limitation, not a silent gap

An SDF hit gives a **position** and a **normal**. It never gives a **colour** — there is no material
information anywhere in a signed-distance volume, cooked or composed. v1 therefore bounces a single
configurable constant, `LightingSettings::ddgi_albedo` (default 0.6, a plausible matte-paint/plaster
average), off every hit, regardless of what is actually there. This is honest and it still
demonstrates the thesis (a moving shadow changes what is lit, and the bounced brightness follows) —
but it is not real colour bleeding. A red wall's bounce light will not tint anything red under this
scheme. Real per-surface albedo needs a **surface cache** — some way to look up "what colour is the
geometry at this hit position" from the SAME cooked-asset pipeline that made the SDF in the first
place — and that is a named, deliberate follow-up, not built here.

**No multi-bounce**, for the identical reason single-bounce is what "one grey albedo" can honestly
support: a hit that is not directly sun-lit contributes exactly zero this frame. Real DDGI
implementations often let probes sample OTHER probes for a second bounce; that is future work.

## 6. The octahedral atlas, and the border

Storing "irradiance as a function of direction" per probe needs a 2-D image (small, GPU-friendly)
that maps to the sphere of directions. Rime uses the **octahedral mapping** (Cigolle, Donow, Evangelakos,
Mara, McGuire, Meyer, "A Survey of Efficient Representations for Independent Unit Vectors", 2014;
Meyer et al. 2010; Engelhardt & Dachsbacher 2008) — a bijection between the unit sphere and the
square `[-1,1]²`:

```
encode(dir):
    p = dir.xy / (|dir.x| + |dir.y| + |dir.z|)
    if dir.z <= 0:  p = (1 − |p.y|) · sign(p.x),  (1 − |p.x|) · sign(p.y)
    return p

decode(u, v):
    n = (u, v, 1 − |u| − |v|)
    if n.z < 0:  n.xy = (1 − |n.y|) · sign(n.x),  (1 − |n.x|) · sign(n.y)
    return normalize(n)
```

(`sign(x)` here never returns 0 — it is ±1 by convention at exactly 0, so the fold never divides by
nothing.) A probe's whole irradiance function fits in one small tile: **6×6 interior texels**
(`kDdgiIrradianceTileInterior`) for the smooth, low-frequency irradiance; **14×14** for visibility
(`kDdgiVisibilityTileInterior`), which needs sharper directionality for the Chebyshev test (§7) —
both numbers are the Majercik-paper defaults.

**The border — the classic bug, and why it needs a real fix, not a bigger texture.** Hardware
bilinear filtering samples up to half a texel past whatever UV you gave it. Near a tile's edge, that
means sampling from WHATEVER texel the atlas happens to place next door — normally a completely
unrelated probe's tile — instead of the texel the octahedral fold actually wraps to there. The fix
is a **1-texel border** on every side of a tile (`kDdgiTileBorder`) holding the CORRECT wrapped
value, so bilinear filtering right at the seam blends two texels that both belong to the same
probe's own map.

What "correct wrapped value" means falls out of one fact: **plug either `u = ±1` or `v = ±1` into
`decode` and every combination of signs lands on the SAME point**, `(0, 0, −1)` — the four corners of
the square are all the "south pole" of the mapping. That single fact is enough to derive the whole
border rule by chasing what happens to a coordinate that drifts just past an edge:

- Push `u` past `+1` by a hair (`u = 1+ε`): decode's `z = 1 − |u| − |v| = −ε − |v|` goes negative,
  triggering the fold branch, and working through the algebra shows this is IDENTICAL to decoding
  `u' = 1−ε, v' = −v` — a point just INSIDE the right edge, at the MIRRORED `v`. Past the left edge
  is the same story reflected. Past the top or bottom edge swaps the roles of `u` and `v`.
- A CORNER (both axes out of range at once) applies both single-axis folds in sequence — the second
  fold's "flip the other axis" undoes and re-does the first fold's flip, landing on the texel
  diagonally opposite in the SAME tile (consistent with all four corners being one point: any corner
  is as good a stand-in for any other as the others).

`ddgi_blend_irradiance.comp`'s `oct_decode_folded` implements exactly this: for a border texel, fold
its (up-to-one-texel-out-of-range) UV back in range with at most two `if`s, THEN decode normally.
The payoff of doing it this way rather than a separate "copy the border" pass: a border texel and
its interior neighbour never have a producer/consumer ordering to get right, because EVERY texel —
border or interior — independently re-derives its own direction and re-runs the identical weighted
gather over this update's rays (§6 below). Two texels that must agree are PROVABLY equal (same
formula, same inputs), not merely hopefully copied correctly.

## 7. The weighted gather, and its π normalization

For one texel's direction `n̂` (`oct_decode_folded` applied to that texel's UV), Rime estimates the
Lambertian irradiance a surface with normal `n̂` would receive by a **cosine-weighted average** over
this update's rays:

```
weight_i  = max(dot(n̂, ray_dir_i), 0)
estimate  = Σ(radiance_i · weight_i) / Σ(weight_i)
```

This is a biased-but-standard estimator (the DDGI-paper norm), and it is worth being precise about
WHAT it estimates, because a future consumer (m10.5b) needs the constant right. If rays are drawn
uniformly over the FULL sphere (as spherical Fibonacci does), a short derivation from first
principles —

```
E[cos⁺]        = (1/4π) ∫_hemisphere cos θ dΩ = 1/4
E[L · cos⁺]    = (1/4π) ∫_hemisphere L cos θ dΩ = E_irradiance / 4π
weighted_avg   = E[L·cos⁺] / E[cos⁺] = E_irradiance / π
```

— shows the weighted average computed above is the TRUE irradiance divided by π. That is a
convenient accident, not a coincidence to be corrected away: Lambertian outgoing radiance is
`albedo/π · E_irradiance`, so `albedo · weighted_avg = albedo · E_irradiance/π` is EXACTLY the
correct outgoing radiance. **The atlas stores the π-normalized quantity directly, so a future
consumer's outgoing radiance is simply `albedo × sampled value` — no extra constant at the sampling
site.** This brick does not consume the atlas (m10.5b does); it is recorded here because getting the
normalization convention right at STORAGE time is what makes consumption trivial later.

A texel whose weight sum is negligible (astronomically unlikely with 64 well-distributed rays, but
not provably impossible) keeps its previously stored value rather than dividing by near-zero.

## 8. Temporal hysteresis — what it buys, and what it costs

The weighted-average estimate above is still a single frame's worth of only 64 rays — noisy. Each
update blends it into the persisted value:

```
stored = hysteresis · stored_prev + (1 − hysteresis) · new_estimate
```

an exponential moving average (EMA). `hysteresis = 0.97` (the default, and the DDGI-paper norm) means
each frame's own noisy sample contributes only 3% — noise averages out over dozens of frames, but a
GENUINE change (a wall breaking, letting light in where none reached before) also takes roughly
`1/(1−hysteresis) ≈ 33` frames to become visible. That is the latency cost decision 2's whole thesis
has to pay somewhere, and m10.5b's job (noted in `ddgi.hpp`'s header, not built here) is to shorten
it where it matters: when a C2 destruction event's world-bounds overlaps a probe's footprint, that
probe's NEXT update should use a much lower hysteresis (or jump the round-robin queue), so the
"light updates" half of the milestone's thesis does not sit behind a 33-frame lag every time.

**A probe's first-ever update is a special case, not a setting**: it uses hysteresis = 0
unconditionally (`DdgiProbes`' own per-probe "primed" bookkeeping, `DdgiStats::newly_primed`), so a
brand-new probe snaps straight to its own first estimate instead of blending toward the atlas's
initial zero over the same 33-frame time constant — the "probes fade in from black" artefact real
engines avoid the identical way.

**A closed-form property worth knowing, and a pitfall this brick's own first test draft fell into**:
unrolling the recurrence gives `stored_n = h^n · stored_0 + (1−h) Σ_{k=1}^{n} h^{n−k} new_k` — the
influence of the VERY FIRST sample decays exactly geometrically, `h^n`, regardless of noise. That
IS worth testing (§9, test 3). What does **not** decay to zero is the STEP-TO-STEP delta between an
already-converged `stored_n` and `stored_{n+1}`, if `new_k` keeps arriving with roughly constant
variance σ² forever (as it does under ongoing random-rotation sampling): a short calculation shows
`Var(delta_∞)/Var(delta_1) = 1/(1+h)`, bounded below by 1/2 for ANY hysteresis less than 1. An EMA
fed constant-variance noise reaches a steady-state JITTER, not silence — a real property of the
recurrence, not a bug, and the reason the test in §9 measures a deliberate step CHANGE (the light
disappearing) rather than the ongoing noise floor.

## 9. Visibility and Chebyshev — the two moments, and what they will be used for

A second atlas (`ddgi_blend_visibility.comp`, RG32Float, the same octahedral tile shape at 14×14
interior) accumulates, with the identical cosine weighting, the two moments a **Chebyshev
one-sided variance bound** needs: `mean = Σ(d·w)/Σw` and `mean2 = Σ(d²·w)/Σw`, where `d` is each
ray's hit distance (or `max_trace_distance` for a miss — "nothing found nearby"). A shading pass
can test "is this probe's stored irradiance actually visible from HERE" by comparing a real
surface's distance to the probe against `mean`/`mean2` (variance `= mean2 − mean²`), softening light
leaking through thin geometry the octahedral MAP alone cannot detect (only the DISTANCE data can).
m10.5a stores the two moments (with the same border-fold, same per-probe temporal hysteresis);
§11 below is where m10.5b actually uses them.

## 10. Consuming the atlases: the probe cage, and octahedral ENCODE

`pbr_forward_shadowed.frag`'s `ddgi_sample_irradiance(world_pos, n)` is the mirror image of
everything §2–§7 built: instead of a compute invocation deciding "which direction am I, and what do
I write", a shading invocation decides "which probes are near me, and what do I read".

**The 8-probe cage.** A fragment's position, expressed in LATTICE units, is
`rel = (world_pos − grid_origin) / spacing`. Flooring `rel` gives the lattice coordinate of the
cage's "low" corner; the fractional remainder is exactly the trilinear weight along each axis
(`frac.x` toward the "high" corner, `1 − frac.x` toward the "low" one, and likewise for y/z) — the
ordinary trilinear-interpolation formula, applied to a scattered lattice of probes instead of a
dense 3-D texture.

**CLAMP, don't extrapolate.** `rel` is clamped to `[0, dims−1]` per axis before flooring — the
identical CLAMP_TO_EDGE a texture sampler applies at an image's border, done here by hand because
there is no hardware address mode for a hand-rolled lattice. This is not a defensive nicety; it is
load-bearing. A surface sitting at a "round" world height — a floor at y = 0 is the norm, and 0 is
a multiple of *every* possible probe spacing — will, more often than a designer expects, land its
fragments EXACTLY on the lattice's own lowest grid line. Unclamped, the bracketing corner on the
far side of that line is correctly dropped (out of range), but the NEAR corner's own trilinear
weight is then the fragment's fractional distance INTO a cell that does not exist below the
lattice — exactly zero. Every one of the 8 corners can end up excluded or zero-weighted
simultaneously: `weight_sum` stays at (or under) its epsilon floor, and the function silently
returns black — not a crash, not a validation warning, just a shading pass that looks like it is
doing nothing. (This is not a hypothetical: m10.5b's own first working draft of this function hit
exactly this, with a floor patch reading pure ambient one cell away from a probe the atlas readback
confirmed was fully lit — see the fix's own comment in the shader.) Clamping first means the
lowest/highest lattice layer on each axis always carries full weight for anything beyond it,
instead of the interpolation quietly degenerating to nothing.

**Octahedral ENCODE.** For a candidate probe and a direction `n` (the fragment's own world normal —
see §11 for why the *geometric*, not normal-mapped, normal), `ddgi_oct_encode(n)` is §6's `encode`
formula run forward — the inverse of `ddgi_blend_irradiance.comp`'s `oct_decode_folded`. Encode and
decode must agree on the SAME mapping, or a probe's stored data reads back as an unrelated
direction's value; because C++, GLSL-for-compute, and GLSL-for-fragment are three separate
compilations with no shared header (the constraint `sdf_compose.comp`/`ddgi_trace.comp` already
live with), this is three hand-synchronized copies of one formula, not one — the reason §6's own
derivation is written out in enough detail to re-derive, not just to copy.

Locating a probe's own texel then inverts the tile-indexing math ITSELF: `ddgi_blend_irradiance.comp`
places physical texel `px` (an integer, border included) at octahedral coordinate
`(px − 1 + 0.5) / interior · 2 − 1`; solving that for `px` given a continuous encoded coordinate
gives `px = interior·(u+1)/2 + 0.5` — a value in "texel index, integer = that texel's own centre"
units (checked against the compute shader's own numbers: interior = 6 puts the mapping's edge,
`u = −1`, at `px = 0.5`, exactly between the border texel and the first interior one, which is
where the seam physically sits). Adding the tile's own integer offset within the atlas and then
applying the ordinary "texel index → normalized UV" conversion (`(index + 0.5) / atlas_size`,
using `textureSize()` so this can never disagree with the texture actually bound) is what lets
hardware BILINEAR filtering do the rest — including, at last, actually crossing the border ring §6
built for exactly this sampling operation.

## 11. The Chebyshev visibility weight, the wrap weight, and re-normalization

Each of the (up to) 8 candidate probes contributes three multiplied weights before its irradiance
counts at all:

- **Trilinear** (§10) — how close the fragment sits to this particular corner.
- **Wrap** — `(dot(n, normalize(probe_pos − world_pos)) + 1) / 2`, Majercik et al.'s own
  adaptive-backface term in its simplest linear form: a probe roughly ahead of the surface (along
  its normal) counts fully, one roughly behind fades toward (not quite) zero. A probe embedded in
  or behind the surface it is meant to be lighting is a poor source for that surface's own
  irradiance regardless of what its data says.
- **Chebyshev visibility** — the one this brick adds meaning to. From the VISIBILITY atlas (§9),
  sampled at probe → fragment direction, `(mean, mean2)` describe what that probe's own rays
  typically found in roughly this direction. If the fragment sits no farther than `mean`, nothing in
  the probe's own data suggests anything is in the way — full weight. Farther than `mean`, the
  one-sided bound
  ```
  variance = max(mean2 − mean², 0)
  weight   = variance / (variance + (dist − mean)²)
  ```
  fades toward zero as `dist` outgrows `mean` by more than the probe's own measured spread. THE LEAK
  THIS STOPS: a probe standing on the sunlit face of a wall has, in the direction facing the wall's
  DARK side, rays that almost all stop tight against the wall itself — a small `mean`, a small
  `variance`. A fragment on the dark side, one interpolation cell away, sits at a `dist` far past
  that `mean` — `weight` collapses toward 0, and that probe's (otherwise bright) irradiance is
  excluded from the blend, exactly as if it had never been a candidate. Without this term, an
  ordinary trilinear blend has no idea a wall is in the way at all — geometry between two probes is
  invisible to a scheme that only ever asks "how far apart are you," never "what's between you."

**Re-normalization.** The three weights multiply into one `total` per corner; `accum` sums
`irradiance · total`, `weight_sum` sums `total`, and the function returns `accum / weight_sum` (or
exactly `0` if `weight_sum` never clears a small epsilon — the "every corner occluded, clamped, or
behind the surface" case, ADR-0032 §11's discipline that idle/degenerate work fails safe rather than
dividing by zero). Re-normalizing by the weight ACTUALLY applied — not a flat `1/8` — is what keeps
a partially-occluded cell honest: a corner the Chebyshev test excludes does not merely go missing
from the sum, it also stops diluting the corners that remain, so a cell with (say) 2 of 8 probes
visible reads as "what those 2 see," not "a quarter of what all 8 would average to."

## 12. Destruction reactivity: `invalidate()`, and what it buys

§8 already named the cost the walls-fall thesis has to pay somewhere: a genuine change takes
`~1/(1 − hysteresis)` updates to become visible — about 33 at the 0.97 default. `DdgiProbes::
invalidate(region)` is where that cost gets paid down, mirroring `SdfClipmap::invalidate`/
`LocalShadowMap::invalidate`'s own C2 contract exactly: queue `region`; the NEXT `add()` call marks
every probe whose lattice point falls within one probe SPACING of it (an "err broad" expansion —
the identical conservatism `LocalShadowMap`'s own frustum-AABB test already uses for the same
reason: a probe sits at a lattice POINT, not a cell, so an event's AABB landing anywhere inside the
cell between two probes must still catch both of that cell's corners, not just whichever one the
event's own, typically tight, box happens to overlap) for `kFastTrackUpdates` (5) further updates of
a much lower hysteresis, `kFastTrackHysteresis` (0.5), instead of the steady-state value.

**What this does NOT do.** It does not force an immediate re-trace — every scheduled probe already
re-traces from whatever the clipmap currently holds, every update, regardless (m10.5a's own
"no C2 hook of its own" note, now half-obsolete: the re-trace was never the missing piece, only how
slowly the STORED value could catch up to it). `invalidate()` only changes the BLEND coefficient the
next few updates use.

**What it buys, exactly.** Unrolling the hysteresis recurrence (§8) makes the first sample's
influence decay as `hⁿ` regardless of `h`. At the fast-tracked `h = 0.5`, 5 updates leave
`0.5⁵ ≈ 3.1%` of the pre-invalidation value's influence — materially gone. At the untouched default
`h = 0.97`, the SAME 5 updates leave `0.97⁵ ≈ 85.9%` — barely moved. Both numbers are exact
predictions of the recurrence, not estimates, and `tests/render/ddgi_test.cpp`'s own invalidate()
test measures both directly (≈3.2% and ≈85.8% observed against a starting value, within a percent
of each closed-form prediction — the residual is fp16 atlas-storage rounding compounding over 5
successive `mix()` writes, the same slack m10.5a's own convergence test budgets) — the two are not
"somewhat different," they are two different closed-form curves from the identical starting point
and the identical (lit → dark) transition.

**What it costs.** A fast-tracked probe's estimate is now a blend of only ~5 frames' worth of the
usual 64-rays-per-update sampling instead of dozens — a genuinely NOISIER estimate, the same
noise/latency trade §8 describes, just deliberately tipped toward latency for the handful of
updates right after a change. `kFastTrackHysteresis`/`kFastTrackUpdates` are deliberately
conservative round numbers (a bigger drop, or more updates, converges faster still) rather than
tuned to a razor's edge — there is no claim here that 0.5/5 is an optimum, only that it is a large,
well-understood improvement over doing nothing.

A shifted lattice (§2) resets `fast_track_remaining_` alongside `primed_` — a shifted lattice's
indices no longer name the same world-space probes, so a pending fast-track budget keyed to the OLD
indexing would mislabel whichever probe now happens to occupy that slot. A probe that is fast-
tracked AND newly primed in the same stretch spends its fast-track budget for nothing extra: priming
already snaps straight to the fresh estimate (hysteresis 0), so there is no stale history left for a
lowered hysteresis to shake off.

## 13. Limits, and what comes next

- **Round-robin, not per-frame growth.** `kMaxDdgiProbesPerUpdate = 512` (the m10.4c spike's
  measured full-frame budget — 8³ probes × 64 rays, ≈1.0–1.5× the m10.3 cluster cull) bounds a
  single update; a larger configured lattice cycles through `kMaxDdgiProbesPerUpdate` probes per
  frame instead of growing per-frame cost, refreshing the whole grid every
  `total_probes / kMaxDdgiProbesPerUpdate` frames.
- **A whole-lattice shift resets every probe's temporal history** (`DdgiStats::grid_shifted`) — the
  same "changed at all ⇒ start over" simplification `SdfClipmap` makes for its own compose state,
  applied here to "who is primed" (and, as of m10.5b, "who is fast-tracked") instead of "which
  voxels are stamped". A toroidal remap that PRESERVES history for probes landing at the same world
  position across a shift is the natural follow-up; v1 keeps the simpler, always-correct rule.
- **No shadow-cascade sampling** (§4) — self-shadowing via a second SDF trace instead.
- **No per-surface albedo, no multi-bounce** (§5) — a limitation the m10.5b thesis test's own
  numbers are honest about: the measured indirect signal moves because the SAME grey-world bounce
  now reaches from different, newly-unoccluded geometry, not because any surface's own colour is
  visible in it.
- **`kFastTrackHysteresis`/`kFastTrackUpdates` are fixed constants, not `LightingSettings` fields**
  (§12) — a profile or a specific game's pacing needs is the trigger for exposing them, not before.

## Verification pins — what the tests check

No golden images; every claim is a stated, justified margin.

`tests/render/ddgi_test.cpp` (m10.5a's own storage-side proofs, plus m10.5b's `invalidate()` proof):

1. **Physical sanity.** An open, sunlit floor's brightest texel reads clearly above the sky floor
   (measured ≈1.66, sky ≈0.02); a probe sealed inside a 6-slab box (generous corner overlap, no
   leaks) reads exactly 0 — the self-shadow trace (§4) working, not a scene with no sun in it.
2. **Directionality.** The texel decoding to straight-down outreads the straight-up texel by more
   than 5× (measured ≈83×) for a probe over a lit floor — a genuinely direction-dependent function,
   not one number. The same test cross-checks a REAL border/interior pair (down = (0,−1,0) encodes
   to the octahedral square's bottom edge midpoint, giving it an actual adjacent border texel) and
   finds them within measurement noise of each other — §6's border fold, checked against itself, not
   merely asserted correct in a comment.
3. **Temporal convergence.** §8's clean, noise-free instrument: with the sun ON for a prime update
   then OFF (sun AND sky radiance both exactly zero, so every subsequent ray contributes exactly
   zero and the blend is a bit-clean `stored_n = h·stored_{n−1}`), the stored value decays
   geometrically at exactly the configured hysteresis rate (measured: 0.6% off the analytic
   `initial·h^10` prediction) and settles near zero.
4. **Snap stability.** Sub-spacing camera motion re-primes zero probes and leaves the lattice
   origin bit-identical; a jump past the lattice's own extent shifts it and re-primes every probe —
   the same anti-shimmer property `sdf_clipmap_test.cpp` already proves for the SDF clipmap's own
   voxel grid, applied to the probe lattice.
5. **`invalidate()`'s fast-track (m10.5b), §12.** Two identical single-probe rigs, primed
   identically, then the light disappears — one rig is `invalidate()`'d at that exact moment, the
   other is not. After 5 updates (`kFastTrackUpdates`): the untouched rig retains ≈85.8% of its
   initial brightness (predicted 85.9%); the invalidated one retains ≈3.2% (predicted 3.1%) — both
   within a percent of their exact closed-form prediction, the residual being fp16 atlas-storage
   rounding compounding over 5 successive `mix()` writes. `DdgiStats::fast_tracked` is checked
   directly (exactly 1 on every one of the 5 updates for the invalidated rig, exactly 0 for the
   other) so the test does not rely on the brightness numbers alone to prove the mechanism engaged.

`tests/render/gi_thesis_test.cpp` (m10.5b's own end-to-end proofs, driven through `SceneRenderer`,
not the raw compute passes — the claim is about a rendered pixel):

6. **The thesis itself, and the retired ambient (m10.6).** A floor patch in a suspended slab's cast
   shadow (CSM-verified: shadows on vs. off at that exact pixel) is lit only indirectly. Since m10.6
   the traced field REPLACES the flat ambient rather than adding to it (pbr.md §6.1), so DDGI-on and
   DDGI-off are two whole renderers. With the slab present the patch sees mostly open sky, and the
   two AGREE to a single 16-bit LSB — DDGI's escaped-ray sky term *is* that ambient constant, so in
   the open-sky limit the field reduces to exactly what it replaced (a correctness property, and the
   flat baseline the break must move away from) — while both stay well below (< 50%) a sunlit
   control. The slab is then removed (`SdfClipmap::remove_instance` + `DdgiProbes::invalidate`) and,
   after `N = 6` (`kFastTrackUpdates` + 1 margin) fast-tracked updates, two things hold: the patch
   reads ≥3× brighter with DDGI either on or off (the now-unshadowed direct term — a CSM effect —
   dominates), AND the isolated indirect contribution (on − off) rises from ≈0 to ≈+0.0029 (~190
   16-bit LSB), the newly-unoccluded sunlit floor's bounce the flat constant cannot express. Fully
   deterministic (fixed-seed RNG, bit-identical across runs).
7. **The leak guard.** A probe 0.5 m inside a sealed (ceiling-shadowed) room reads dark (measured
   peak 0.0095, comparable to the sealed-box proof above); a probe 0.5 m past the room's only wall,
   in the open, reads clearly lit (measured peak 0.77). A floor fragment deep in the room but right
   against the wall's inner face sits 90%/10% between those two probes in the trilinear cage — a
   NAIVE (Chebyshev-free) blend would leak roughly 10% of the bright probe's own value onto it
   (≈0.077 by the measured peak); the actual rendered fragment's own indirect contribution measures
   ≈0.0018 — under 3% of that naive estimate, comfortably inside the test's 40% ceiling — because
   the bright probe's own visibility data shows its rays stopping at the wall a few tens of
   centimetres away, far short of the fragment's actual distance.
8. **The two-room walls-fall (m10.6) — GI as the entire signal.** The leak-guard scene, but the wall
   FALLS. A covered dark room (a ceiling blocks the vertical sun — the sealed-box mechanism) beside
   an open sunlit floor, divided by a wall; the dark room's floor gets NO direct light in either
   state, because the ceiling shadows it whether or not the side wall stands. So when the wall is
   removed (`remove_instance` + `invalidate`) the only thing that can brighten the dark floor is
   bounce. Measured: the DDGI-on floor rises 0.00068 → 0.0059 (≈8.6×, a 0.0052 resolved signal)
   while the DDGI-off control is BIT-IDENTICAL across the break (0.0159912 = 0.8·ambient — zero
   direct light throughout, confirming the ceiling seals the sun). Where the slab test (pin 6) has
   direct light dominating the change and GI as a small correction, here GI is the *whole* change —
   the constant-ambient renderer is blind to the fallen wall. And even lit by bounce the enclosed
   room stays DIMMER than the flat constant would have painted it (0.0059 < 0.016): occlusion
   honesty, and the dynamic no-free-energy witness — single-bounce GI can only ever compute *less*
   fill than the constant assumed, never invent more (pbr.md §6.1).
