# ADR-0040: The sky — an analytic background now, a physical atmosphere later

- Status: Proposed
- Date: 2026-09-03

> **Written after the code, and that is worth admitting.** The m17.0 brick landed the analytic sky
> first and cited this ADR in five places as though it existed. It did not. This document records
> the decisions that brick actually made, plus the one it deferred — which is the only honest way
> to close a forward reference. Every other milestone here got its ADR before its bricks; M17 did
> not, and [ROADMAP](../ROADMAP.md) says so rather than quietly tidying it.

## Context

M17 is "The Visual Bar" — the UE5 column of [VISION](../VISION.md) §3. It is a milestone judged by
looking, and until m17.0 everything above the horizon was the forward pass's clear colour: black.
A landscape with a black sky cannot be judged against a visual bar, and neither can a screenshot of
one.

**Two different things get called "the sky", and conflating them is the trap.** One is what you
*see* when you look up — a background, drawn where nothing else was. The other is what *lights* the
scene: on a real overcast day the sky is the dominant light source, and the sun contributes a
minority of the illumination. Rime wants both. They differ by roughly two orders of magnitude in
implementation cost, and only one of them can change the value of an already-shipped GPU proof.

What the engine's lighting takes today is one constant. `SceneRenderer::ambient_` is read in
**exactly three places**, and every one of them is a place a real sky would belong:

| consumer | site | what it stands in for |
| --- | --- | --- |
| forward shading | `scene_renderer.cpp:493-495` (`FrameUniforms::ambient`) | all light not from a named light |
| DDGI probes | `scene_renderer.cpp:617-619` (`DdgiLightingInputs::sky_radiance`) | what a probe ray sees when it hits nothing |
| SSR | `scene_renderer.cpp:806-808` (`SsrInputs::ambient`) | what a reflection ray sees when it leaves the screen |

Its own comment still calls it "the crude GI stand-in until M10". M10 came and went; the constant
stayed, because nothing has ever produced a directional sky radiance to replace it with.

So a sky pass that only paints the background changes **no lit pixel**, and that is a property worth
having deliberately rather than by accident: it is what makes the pass safe to add mid-milestone,
and what lets its proof assert bit-identity on everything the scene drew.

## Decision

### 1. Ship the analytic sky now, and scope it to the BACKGROUND

m17.0 is a procedural daytime sky: a two-colour gradient in the view ray's up-component, a
forward-scatter glow lobe around the sun, a sun disc of authored angular radius, and one layer of
cloud — fBm over a value-noise basis, evaluated where the view ray pierces a flat slab at cloud
altitude and thresholded by a coverage parameter.

**It is not physical, and the code says so in the same breath everywhere it appears.** There is no
Rayleigh or Mie integral, no transmittance function, no multiple scattering, no aerial perspective,
and no contribution to ambient, DDGI or SSR. The colours are a fit chosen because they read
correctly, not a solution to anything.

Naming that plainly is the decision, not a disclaimer on it. A sky that looks physical and is not
is how a renderer acquires a permanent "why is the lighting slightly wrong" that nobody can find.

### 2. The seam is `sky_radiance()` and those three `ambient_` reads — and nothing else

The pass is shaped so the physical model is a **body replacement, not a refactor**:

- **What you see** is `sky_radiance(vec3 dir)` in `sky.frag`. It takes a world-space view ray and
  returns radiance. A LUT-based atmosphere has exactly that signature.
- **What lights the scene** is the three rows in the table above. The physical model makes them a
  sky-view lookup in a direction instead of a constant.
- **Everything structural stays**: the pass position (after forward, before SSR), the
  read-colour+depth / write-a-second-HDR-target shape, the `SkyParams` block, and the sun coupling.

Which is to say the expensive half — the LUT passes and their scheduling — is additive, and the
cheap half is a diff inside one function. That is the seam this brick was bought to leave.

### 3. The sky is a COMPONENT, not a renderer setting

`render::Sky` is an ECS component; the renderer takes the **first one found**, matching
`extract_scene`'s first-camera-wins rule for the same reason (a second one would silently do
nothing, and a rule that is stated is a rule that can be relied on).

A renderer setting would have been less code. It was rejected because the sky belongs to the
*scene*: a `.rscene` that names a coastline should carry the weather over it, the editor should
edit it in the Inspector like any other component, and the round-trip through save/load should
carry it for free. `set_sky()` survives as the **host-level default** for a world that authored
none — the same relationship `set_ambient` has to the lights.

### 4. The sun is coupled, not authored twice

The disc follows the world's **first `DirectionalLight`**, so the sky and the lighting cannot
disagree about where the sun is. There is one sign rule and it is the whole subtlety:
`DirectionalLight::direction` is the direction light **travels**; the shader wants the direction it
comes **from**. Two authored sun directions that drift apart is a bug class this closes by
construction, and the sign is pinned by `tests/render/sky_test.cpp`, which requires flipping the
light to take the disc *out* of frame.

### 5. Off allocates nothing

With no `Sky` component and no `set_sky()`, the pass declares nothing, allocates no target, and the
tonemap reads the raw forward HDR — the frame is byte-identical to the pre-sky renderer. That is
[ADR-0032](0032-lighting-v2.md) §11's rule for every switchable pass, and it is what keeps the
existing M5/M10 proofs' margins valid.

It sits **before** SSR rather than after, so a reflection ray that leaves the screen finds sky
rather than clear colour.

### 6. The end state, when it is taken on: Hillaire 2020

The intended replacement is the precomputed-LUT atmosphere of Sébastien Hillaire's *A Scalable and
Production Ready Sky and Atmosphere Rendering Technique* (EGSR 2020) — the model behind UE5's
SkyAtmosphere. Four small LUTs, each bought for a specific reason:

- a **transmittance** LUT (parameterised by altitude and sun zenith angle) — what reddens the sun as
  it sets, and the term every other LUT is built on;
- a **multiple-scattering** LUT — the cheap approximation that keeps the sky from going black at
  twilight without a full multi-bounce integral;
- a **sky-view** LUT, evaluated per view — this is the one that replaces the three `ambient_` reads,
  and it is what makes the sky a light source;
- an **aerial-perspective froxel volume** — distance haze applied to the scene itself, which is what
  makes a landscape read as kilometres deep rather than as a matte painting.

**It is not scheduled here.** It is a milestone-sized brick, it changes lighting (so every existing
GPU proof's margins move with it), and it should not land before there is authored content worth
judging it against. This ADR fixes the *shape* so that taking it on is a decision about time rather
than about architecture.

## Consequences

**What we get.** A horizon that is not black, in both hosts, from a component a scene owns; a sun
that cannot disagree with the light; and a pass whose "off" is provably free. The analytic model
costs one fullscreen triangle over background pixels only.

**What we accept, named rather than left to be discovered:**

- **The sky does not light the scene.** A bright sky over a scene lit by a dim constant ambient will
  look wrong in a way no current test can see, because every lighting proof asserts against
  `ambient_` and the sky pass cannot touch it. This is the single largest known gap and it is
  decision 6's whole subject.
- **Clouds are a 2-D slab, not a volume.** They read correctly for a sky you look *at*. Fly a camera
  up through cloud altitude and the model has nothing to say. Real volumetric clouds ray-march a 3-D
  density field and are a different and much more expensive animal.
- **A scene can author its sky but not its sun's colour.** `Sky` carries zenith, horizon, intensity,
  disc radius and the cloud parameters; `sun_radiance`, `use_scene_sun` and the below-horizon
  `ground` darkening stay host-level in `SkyParams`. That asymmetry is deliberate for now (the sun's
  *colour* is a lighting fact, and in the physical model it comes from the transmittance LUT rather
  than from an author) but it means a `.rscene` alone cannot fully specify its own look.
- **It is a daytime fit.** There is no night, no twilight and no moon. The gradient has no
  parameterisation that produces them, and adding one is not worth it when decision 6 gives them for
  free.
- **The cloud cost is unmeasured.** Five octaves plus a second offset sample is ten value-noise
  evaluations per background pixel, and no perf run has covered it. Under M17's frame-rate clause
  that is a number somebody has to produce before the sky is defended as cheap.

## Alternatives considered

**Do the Hillaire atmosphere now, and skip the analytic stage.** Rejected on sequencing, not on
merit — it is where this is going. It is four LUT passes, a multiple-scattering solve and a froxel
volume, it perturbs every lighting proof in the tree, and it would have landed with no authored
content to judge it against. The analytic pass costs a few hundred lines and buys the seam.

**An authored HDRI cubemap skybox.** Rejected. It cannot follow the sun, which makes a dynamic time
of day impossible, and it introduces a new asset class for something the analytic model produces for
free. It would also have been the *third* place a scene's lighting is authored.

**A renderer setting instead of a component.** Rejected — see decision 3. A `.rscene` that cannot
carry its own weather pushes the weather into host code, which is exactly the fork
[ADR-0038](0038-platform-proof-m15.md)'s platform proof exists to prevent.

**Volumetric ray-marched clouds.** Deferred, not rejected. The 2-D slab is the right cost for a
background; a volume becomes worth it when a camera can reach cloud altitude, which nothing in the
roadmap currently does.
