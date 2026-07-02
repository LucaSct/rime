# Engine cut-away — assembly + Mach flow together (viewer Bview)

## What this is

ICEM's showcase is a **geared turbofan**, computed end-to-end. Its `engine` command emits the machine
the same way every ICEM assembly comes out — **one named STL per component** (`engine_fan.stl`,
`engine_hpc.stl`, `engine_combustor.stl`, `engine_hpt.stl`, `engine_core_nozzle.stl`, … ) — *plus* two
computed **compressible-throughflow** fields, `engine_core.icef` and `engine_bypass.icef`, each carrying
a vec3 `velocity` and a scalar `mach` on the same node grid (ICEM brick 28/29).

The **engine cut-away** view fuses two views the viewer already had so the engine reads as engineers
draw it — a sectioned machine with the gas flowing through it:

* the **multi-part assembly** ([assembly.md](assembly.md)) — every part tinted, toggle-able and
  explodable — but now with a **cut-away** clip plane through the axis, so the gas path is laid open; and
* **streamlines** ([streamlines.md](streamlines.md)) traced through *both* ducts, coloured not by raw
  speed but by the computed **Mach number**, on one Mach scale shared by the core and the bypass.

Both are drawn into **one rendering scope**: they share the camera and the depth buffer, so the metal the
cut leaves occludes the flow behind it while the opened half reveals it — a true cut-away. No new
pipeline, shader or RHI surface is added. See `engine.hpp`, `streamlines.hpp`, and the `--engine <dir>`
mode in `main.cpp`.

## Colouring streamlines by Mach, not speed

A streamline is a curve everywhere tangent to the velocity, integrated by arc-length RK4 of the unit
flow direction (the ODE and integrator are derived in [streamlines.md](streamlines.md)):

$$\frac{d\mathbf{x}}{ds} = \frac{\mathbf{u}(\mathbf{x})}{\lvert \mathbf{u}(\mathbf{x})\rvert}.$$

The plain flow view colours each vertex by the normalized speed $\lvert\mathbf u\rvert/\lvert\mathbf
u\rvert_{\max}$. For a gas path that is the wrong quantity: what an engineer reads off a compressible
duct is the **Mach number**

$$M = \frac{\lvert\mathbf u\rvert}{a}, \qquad a = \sqrt{\gamma R T},$$

and because the speed of sound $a$ falls through the hot combustor and rises again through the turbine,
$M$ is **not** proportional to $\lvert\mathbf u\rvert$ along the path. ICEM has already solved the
quasi-1-D compressible field and written the scalar $M(\mathbf x)$ into the `.icef` next to the velocity,
so the viewer **samples that computed Mach** (trilinearly, on the same grid as the velocity it is
integrating) and uses it as the colour coordinate:

$$t(\mathbf x) = \operatorname{clamp}\!\left(\frac{M(\mathbf x)}{M_{\mathrm{ref}}},\, 0,\, 1\right),
\qquad M_{\mathrm{ref}} = \max\!\big(M^{\text{core}}_{\max},\, M^{\text{bypass}}_{\max}\big).$$

The reference $M_{\mathrm{ref}}$ is the larger of the two ducts' peak Mach, **shared** across both, so a
single legend bar reads both streams and the cold, fast bypass is directly comparable with the hot core.
$t$ then drives the same 5-stop transfer function ([colormap.md](colormap.md)) the field colormap and the
legend use — blue (low $M$) → cyan → green → yellow → red (high $M$) — so the lines and the bar agree by
construction. Implementation: `build_streamlines(vf, &mach, M_ref)` in `streamlines.hpp` carries the Mach
value on the line vertex's `w` channel exactly where the speed used to ride, so `streamline.{vert,frag}`
are unchanged.

## The cut-away plane

The lit mesh shader already supports a cross-section: a fragment at world point $\mathbf p$ is discarded
where it lies in the cut half-space

$$\mathbf n\cdot\mathbf p > w,$$

with the plane $(\mathbf n, w)$ delivered in the push constant ($\mathbf n=\mathbf 0,\ w=+\infty$ disables
it). The cross-section derivation is in [cross-section.md](cross-section.md); the cut-away simply switches
that plane **on for every assembly part at once**.

The turbofan's flow axis is $z$ (ICEM authors the axial direction as $+z$; the `velocity`'s axial
component is $u_z$). A **meridional** section — one that opens the gas path along the whole length —
therefore needs a plane that *contains* the $z$ axis, i.e. a **radial** normal. We take

$$\mathbf n = \hat{\mathbf e}_y, \qquad w = c_y,$$

where $c_y$ is the assembly's bounding-box centre, so the half $y>c_y$ is removed and the camera looks
straight into the core and bypass annuli. (Cutting along $z$ — the flow axis — would instead lop off the
front or back of the engine, which is exactly what we do *not* want.)

Crucially, the mesh shader applies this clip **first and unconditionally**, before the branch that picks
the per-part assembly tint, so sectioning and tinting compose on the same draw — no second pipeline.

## One pass, one depth buffer

The assembly parts and both streamline sets are recorded into a single `begin/end_rendering` scope that
clears one shared colour + depth target (`record_engine`). Because the streamlines are depth-tested
against the metal:

* in the **removed** half the casing fragments are discarded, so the streamlines there draw against the
  background and the cut face — the flow is *revealed*;
* in the **kept** half the solid wins the depth test, so it *occludes* the flow behind it.

That depth interaction is what makes the image read as a cut-away rather than as lines floating over a
model. The streamlines themselves are never clipped (the section is a property of the solid, not the
flow), so each line stays whole and you can follow it from inlet to nozzle.

## What is reused (and what is not)

| Piece | Source | Reused as-is? |
|---|---|---|
| Part load, palette, explode, framing | `assembly.hpp` (E1) | yes |
| Lit mesh + its clip half-space | `shaders/mesh.{vert,frag}` | yes (clip switched on) |
| RK4 streamline integrator + line pipeline | `streamlines.hpp` (D) | yes (Mach on the `w` channel) |
| `.icef` reader (vector + scalar) | `field.hpp` | yes |
| Colormap + legend bar | `shaders/{streamline,legend}.frag` | yes |
| From-scratch UI panel | `ui.hpp` (E2) | yes (+ CUT-AWAY checkbox, Mach readout) |

New code is the **composition** only: `EngineScene` (assembly + the two traced, Mach-coloured line sets +
the shared $M_{\mathrm{ref}}$), the per-part cut-away push, and the one-scope `record_engine`. No new
shader, pipeline or RHI entry point.

## Honest limitations

* **No cap on the cut face.** The section shows the parts as the open shells the clip leaves; the
  two-sided lighting in `mesh.frag` lights those interior walls so they read as solid, but there is no
  filled cap quad (the stencil cap in [clip-cap.md](clip-cap.md) is single-mesh; capping 15 parts is left
  for later). Thin-walled engine parts read well without it.
* **Throughflow, not blade-resolved.** The streamlines are ICEM's quasi-1-D compressible field on the
  real annuli, so they show the gas accelerate/heat/expand along the path — they are not a blade-row RANS
  solution (stated in ICEM's `docs/math/engine-flow.md`).
* **Seeding is a uniform inlet grid** filtered to the fluid, so a very thin annulus gets fewer seeds; the
  seed count is a tunable (`load_engine(dir, n_seed)`).

## Verification

`tests/rhi/engine_offscreen_test.cpp` builds a synthetic stand-in — a casing cube plus a velocity field
flowing along $+z$ with a Mach scalar ramping $0.2\to0.9$ — and checks the three new behaviours:

1. **Mach colouring (data):** the built line vertices' colour coordinate spans a cool inlet
   ($w<0.4$) and a hot outlet ($w>0.8$) — the lines read Mach, not raw speed.
2. **Combined render:** the cut-away casing (red) and the revealed cool inlet streamlines (blue, a colour
   the casing never makes) both land on screen.
3. **Cut-away:** switching the clip plane on draws strictly less of the red casing than the whole engine.

Off-screen + readback, so it runs GPU-free on lavapipe in CI.
