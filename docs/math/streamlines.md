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

## Viscous flow: the boundary layer and the speed volume (D2·V)

ICEM's brick25 adds **viscosity** — a steady laminar Navier–Stokes solve — and with it the **no-slip
boundary layer**: the flow is fast in the core and falls to *zero* at the walls, a velocity gradient the
inviscid brick24 field (a uniform plug) does not have. The streamlines already reveal it: seeds across
the inlet trace lines whose colour is the local speed, so the core lines run hot-red while the near-wall
lines stay cool-blue — the parabolic profile, drawn.

To see the same speed as a **volume** (not just along lines), the viewer derives a scalar **speed field**
$\;s(\mathbf x) = \lVert \mathbf u(\mathbf x)\rVert\;$ from the vec3 velocity (`speed_field` in
`field.hpp`). Because it is an ordinary `ScalarField`, every scalar tool then works on flow speed: the
surface **colormap + legend**, the **isotach** isosurface, the **slice** on the cut plane, and the
**DVR**. The derivation reuses the velocity field's grid, world→uvw affine and one-voxel boundary
dilation — the magnitude of the already-dilated vector is the dilated speed, and the validity channel
carries straight over — so boundary sampling stays clean. In the DVR, opacity rises with speed, so the
fast core composites into a bright cloud while the slow near-wall fluid stays nearly transparent: the
boundary layer rendered volumetrically. CLI: `--speed [name]` makes the colormap / `--iso` / `--dvr` use
$\lVert\mathbf u\rVert$ of the named (or first) velocity field.

## Verification

`tests/rhi/streamlines_offscreen_test` builds a velocity field that flows along +z and speeds up
downstream: the integrator produces a non-empty line set, the lines render, and the fast (downstream)
end is hot-red — the speed colouring works. End to end, ICEM's brick24 converging-duct flow
(`icem cfd`) loaded with `icem_viewer flow.stl --field flow.icef --flow velocity` shows streamlines
threading the contraction, blue at the wide slow inlet → red at the narrow fast outlet (the computed
~4× acceleration of the 2:1 area contraction, made visible).

`tests/rhi/viscous_offscreen_test` (D2·V) builds a **Poiseuille channel** — $v_z(x)=v_{\max}(1-x^2)$,
zero at the no-slip walls $x=\pm1$ — and checks the boundary layer is visible: `speed_field` reproduces
the parabola ($v_{\max}$ on the centreline, 0 at the walls); the streamlines render *both* a hot-red core
and cool-blue near-wall lines; and the DVR of the derived speed composites a red-dominated (fast-core)
cloud. End to end, ICEM's brick25 viscous channel (`icem viscous`) loaded with
`icem_viewer viscous.stl --field viscous.icef --flow velocity` shows the boundary layer as streamlines,
and `--dvr --speed` shows it as a volume.

## Scope / next

Static streamlines coloured by speed, plus the speed scalar (colormap / isotach / slice / DVR). This
closes the rime side of phase D. **Animated particles** along the lines, **arrow glyphs**, and the 1-D
gas-path station overlay are deferred niceties; the underlying flow fidelity grows with the rest of the
CFD ladder in ICEM (D3 compressibility → D4 the turbojet flow view on brick10/11 geometry).
