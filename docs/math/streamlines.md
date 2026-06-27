# Streamlines (viewer flow view, D·V)

## What this is

Given a computed 3-D **velocity** field (ICEM's potential-flow `.icef`, brick24), the viewer traces
**streamlines** — curves everywhere tangent to the flow — and draws them coloured by speed. This is the
flow visualization the CFD brick unblocks (and the streamlines deferred from C3, which needed a real
3-D vector field). Integration is on the CPU (trilinear sampling of the loaded volume); the lines are
drawn as a GPU `LineList`. See `streamlines.hpp`, `shaders/streamline.{vert,frag}`.

## The streamline ODE

A streamline $\mathbf x(s)$ is tangent to the velocity everywhere. Parameterised by **arc length** $s$,

$$ \frac{d\mathbf x}{ds} = \hat{\mathbf u}(\mathbf x) = \frac{\mathbf u(\mathbf x)}{\lVert \mathbf u(\mathbf x)\rVert}. $$

Using the *unit* direction (rather than $\mathbf u$ itself, i.e. $d\mathbf x/dt = \mathbf u$) makes the
points come out **evenly spaced** regardless of how fast the flow is locally — nicer lines through a
contraction where the speed varies a lot.

## RK4 integration

Each streamline is advanced with the classic 4th-order Runge–Kutta step (step $\Delta s = 0.6\,h$, a
fraction of the field cell size $h$):

$$ \mathbf k_1 = \hat{\mathbf u}(\mathbf x),\;
   \mathbf k_2 = \hat{\mathbf u}(\mathbf x + \tfrac{\Delta s}{2}\mathbf k_1),\;
   \mathbf k_3 = \hat{\mathbf u}(\mathbf x + \tfrac{\Delta s}{2}\mathbf k_2),\;
   \mathbf k_4 = \hat{\mathbf u}(\mathbf x + \Delta s\,\mathbf k_3), $$
$$ \mathbf x \leftarrow \mathbf x + \frac{\Delta s}{6}\big(\mathbf k_1 + 2\mathbf k_2 + 2\mathbf k_3 + \mathbf k_4\big). $$

$\mathbf u(\mathbf x)$ is read by **trilinear** interpolation of the field volume (the same world→uvw
affine as the colormap). A line terminates when it leaves the fluid (validity $<0.5$), the velocity
vanishes, or a step cap is hit. **Seeds** are a grid spread across the inlet face (the low-uvw·z plane),
keeping only seeds that land in the fluid; each is integrated downstream.

## Drawing

Each integrated step emits a `LineList` segment — two vertices, packed as `vec4` (xyz = world position,
w = the **normalized speed** $\lVert\mathbf u\rVert / \lVert\mathbf u\rVert_{\max}$ at that point). The
vertex shader transforms by the camera matrix and forwards the speed; the fragment shader maps it
through the shared colormap, so a streamline reads cool where the flow is slow and hot where it
accelerates. Drawn depth-tested so nearer lines occlude.

## Verification

`tests/rhi/streamlines_offscreen_test` builds a velocity field that flows along +z and speeds up
downstream: the integrator produces a non-empty line set, the lines render, and the fast (downstream)
end is hot-red — the speed colouring works. End to end, ICEM's brick24 converging-duct flow
(`icem cfd`) loaded with `icem_viewer flow.stl --field flow.icef --flow velocity` shows streamlines
threading the contraction, blue at the wide slow inlet → red at the narrow fast outlet (the computed
~4× acceleration of the 2:1 area contraction, made visible).

## Scope / next

Static streamlines coloured by speed. **Animated particles** along them, **arrow glyphs**, and the
1-D gas-path station overlay are the remaining D4 niceties; the underlying flow fidelity grows with the
CFD ladder (D2 viscous Navier–Stokes, D3 compressibility) in ICEM.
