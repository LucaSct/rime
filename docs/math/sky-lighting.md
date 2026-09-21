# Sky lighting — nine numbers for the whole sky, and a table for the rays that miss

m17.7b's derivation: why a sky that only paints the background changes no lit pixel, why the
cosine kernel means **nine** coefficients are enough to light a scene with an entire sky, why the
projection integrates over a spherical Fibonacci set rather than over the table's own texels, and
why the sun's disc is deliberately absent from both. Code:
`engine/render/shaders/sky_common.glsl`, `sky_mapping.glsl`, `sky_skyview.comp`, `sky_sh.comp`,
`sky_sh_eval.glsl`, and `engine/render/src/lighting/sky.cpp`. References: Ramamoorthi & Hanrahan,
"An Efficient Representation for Irradiance Environment Maps", *SIGGRAPH 2001*; Hillaire, "A
Scalable and Production Ready Sky and Atmosphere Rendering Technique", *EGSR 2020*.
[ADR-0040](../adr/0040-sky-and-atmosphere.md) §2 and §6.

## 1. Two different things are called "the sky"

One is what you **see** when you look up: a background, drawn where nothing else was. The other is
what **lights** the scene — and on an overcast day that is the dominant source, with the sun a
minority contributor. m17.0 shipped the first and said so plainly. Its pass copies any pixel whose
depth is less than 1.0 straight through (`sky.frag`), so it changes no lit pixel *by construction*,
which is what made it safe to add mid-milestone.

What lit the scene until m17.7b was one constant — `SceneRenderer::ambient_`, `{0.02,0.02,0.02}`,
whose own comment called it "the crude GI stand-in until M10". M10 came and went. The constant
stayed, because nothing had ever produced a directional sky radiance to replace it with.

The three places that read it each want a slightly different thing, and the difference matters:

| consumer | what it needs |
|---|---|
| forward shading | **irradiance** — the cosine-weighted integral of the sky over the hemisphere around a surface normal |
| DDGI probe miss | **radiance** along one ray direction |
| SSR ray miss | **radiance** along one reflection direction |

Irradiance is an integral; radiance is a lookup. That is why m17.7b builds **two** things and not
one: a table for the lookups, and nine coefficients for the integral.

## 2. Why nine numbers are enough

A Lambertian surface with normal **n** reflects

$$ L_o(\mathbf{n}) \;=\; \frac{\rho}{\pi} \int_{\Omega} L_i(\boldsymbol{\omega})\,\max(0, \mathbf{n}\cdot\boldsymbol{\omega})\,d\boldsymbol{\omega} $$

The integral is the incoming radiance convolved with a clamped-cosine kernel. Ramamoorthi &
Hanrahan's observation is that this kernel is a **brutal low-pass filter**: expand it in spherical
harmonics and its coefficients are $\hat{A}_0=\pi$, $\hat{A}_1=2\pi/3$, $\hat{A}_2=\pi/4$, and then
$\hat{A}_3=0$, with everything above falling off like $l^{-2}$ and alternating to zero. Whatever
high-frequency structure the sky has — a hard cloud edge, a bright band at the horizon — the cosine
kernel annihilates it.

So you do not need the sky to compute irradiance. You need its first nine SH coefficients, and the
result is accurate to about 1% for *any* environment. That is the whole trick, and it is why a
scene can be lit by an entire sky for the cost of a quadratic polynomial per pixel.

The basis, through order 2, as polynomials in the direction's components:

$$
\begin{aligned}
Y_{0,0} &= 0.282095 \\
Y_{1,-1} &= 0.488603\,y \quad Y_{1,0} = 0.488603\,z \quad Y_{1,1} = 0.488603\,x \\
Y_{2,-2} &= 1.092548\,xy \quad Y_{2,-1} = 1.092548\,yz \quad Y_{2,1} = 1.092548\,xz \\
Y_{2,0} &= 0.315392\,(3z^2-1) \quad Y_{2,2} = 0.546274\,(x^2-y^2)
\end{aligned}
$$

Which axis one calls "up" is irrelevant here. The basis is orthonormal on the sphere whatever the
labelling; the only thing that must be true is that the projection and the evaluation use the
**same** polynomials in the **same** order. `sky_sh.comp` and `sky_sh_eval.glsl` write them out
identically for exactly that reason.

## 3. The projection, and why not over the table's texels

The coefficients are $L_i = \int_{\Omega} L(\boldsymbol{\omega})\,Y_i(\boldsymbol{\omega})\,
d\boldsymbol{\omega}$, which in practice is a sum over samples, each weighted by the solid angle it
represents.

The obvious implementation integrates over the sky-view table's texels. It is also the one with the
trap in it: under that table's square-root horizon warp (§5), a texel's solid angle is
$\frac{4\pi^2}{WH}\cos(l)\,|2v-1|$ — a Jacobian that is easy to get subtly wrong, and **a wrong
solid-angle weight does not look wrong**. It makes the scene quietly too bright, or too blue, with
nothing to compare against.

A spherical Fibonacci set has no such problem. Its points are placed at

$$ y_k = 1 - \frac{2(k+\tfrac12)}{N}, \qquad \varphi_k = k\,\pi(3-\sqrt5) $$

so $y$ is spaced **uniformly**, and by Archimedes' hat-box theorem uniform $y$ on a sphere is
uniform **area**. Every sample therefore carries exactly the same solid angle, $4\pi/N$ — a
constant, not a derivation. The engine already uses this set for DDGI's probe rays
(`ddgi_trace.comp`), for the same reason of even coverage.

`sky_sh.comp` takes $N = 4096$: generous for nine smooth coefficients, costing one dispatch on the
frames where the sky actually changed, and quiet enough that moving clouds do not make the ambient
term shimmer.

## 4. Where the π goes, and the check that pins it

The stored coefficients are pre-convolved with the cosine lobe **and** divided by the $\pi$ that
turns irradiance into a Lambertian surface's outgoing radiance, so the per-pixel path is nine
multiply-adds and nothing else. The three scale factors collapse to

$$ \hat{A}_0/\pi = 1, \qquad \hat{A}_1/\pi = \tfrac23, \qquad \hat{A}_2/\pi = \tfrac14 $$

This choice is what makes the replacement **unit-for-unit** rather than a re-tuning, and there is a
check you can do in your head. Take a uniform sky of radiance $L$. Only the $l=0$ term survives:

$$ c_0 = L\,Y_{0,0}\,4\pi = L \cdot 0.282095 \cdot 4\pi $$

and the evaluation multiplies by $Y_{0,0}$ again:

$$ E(\mathbf{n}) = c_0 Y_{0,0} = L \cdot 0.282095^2 \cdot 4\pi = L \cdot 1.0 = L $$

A uniform sky of radiance $L$ produces exactly $L$ — which is precisely what the flat constant
meant. That normalization identity still pins the SH projection itself, but m17.7d's runtime body
is not uniform: it is physical single scattering. Its structural tests therefore assert solar
scale, atmospheric colour, source occlusion and the sky-off gate instead of pretending an authored
gradient can still manufacture the old uniform fixture.

## 5. The table, and its horizon warp

SSR and DDGI need radiance along a *direction*, per ray. Evaluating the sky body there would mean
a spherical scattering integral plus the cloud layer per ray, so the sky is baked once into a small
table and sampled.

The parameterisation is Hillaire 2020's sky-view LUT mapping: azimuth linear, elevation stored
through a **signed square root**,

$$ v = \tfrac12 + \tfrac12\,\mathrm{sign}(l)\sqrt{\frac{|l|}{\pi/2}} $$

which concentrates texels near the horizon. That is deliberate and it is where a sky's angular
detail actually is — the gradient changes fastest there, and it is where the eye looks. A
linear-in-elevation map spends half its rows on the near-uniform dome overhead and blurs the
horizon, which is exactly backwards. `sky_mapping.glsl` holds the forward map and its exact inverse
($l = \mathrm{sign}(t)\,t^2\,\pi/2$); the proof asserts the round trip rather than trusting the
algebra.

## 6. The sun is not in either of them

`sky_radiance()` includes the sun's **disc**, because that is part of the picture you see. Neither
the table nor the coefficients include it, and this is a correctness matter rather than a taste
one.

The sun already reaches every shaded pixel as the world's first `DirectionalLight` — and by
[ADR-0040](../adr/0040-sky-and-atmosphere.md) §4 that is the *same* light the sky couples its disc
to, so the two cannot even disagree about where it is. A sky-derived ambient that also carried the
disc would therefore add the sun to the frame a **second** time, with the error growing with the
sun's brightness.

The forward-scatter **glow** around the sun stays in both, and the distinction is physical: the
glow is light the atmosphere scattered *out* of the beam, which no directional light accounts for.

There is a second, independent reason the disc is excluded from the table: at 192×108 a texel spans
roughly 1.8°, and the sun subtends about 0.7°. The disc is smaller than the texel meant to hold it.
Hillaire keeps it analytic in the final pass for exactly this reason. The visible consequence is
that a mirror reflects the sun's glow but not its disc; the seam for adding it back analytically in
SSR is left open, not taken.

## 7. What this is not, yet

m17.7d replaces `sky_lighting_radiance()` with a 24-segment spherical single-scattering integral:
Rayleigh and Henyey–Greenstein Mie phase terms sample the precomputed transmittance LUT, and the
small multiple-scattering table contributes a bounded ambient approximation. The full-resolution
background remains the authored m17.0 gradient/glow/cloud picture in this brick, so its art controls
do not contaminate the sky-view LUT or SH. The multiple-scattering producer is not Hillaire's full
closure, and there is still no froxel aerial perspective; those limits are intentional and visible
in the shader comments rather than being presented as a completed four-LUT model.

Two approximations are worth naming as limitations rather than discovering later:

- **The observer is fixed near ground.** The first physical body has no planet-centre camera
  coordinate, so sky-view and every DDGI probe use the same 2 m observer. That keeps the cache
  medium-only today; camera altitude must become a bake dependency before flight can be correct.
- **Clouds are in the integral.** That is what makes an overcast sky actually dim the scene, but it
  also means the coefficients change whenever the wind moves the clouds, so the dirty check has to
  treat wind as a parameter and not as animation it can ignore.
