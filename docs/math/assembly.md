# Multi-part assemblies and the exploded view (viewer E1)

## What this is

ICEM emits a whole machine as **one named STL per component** — the ITER-class tokamak comes out as
`tokamak_vessel.stl`, `tokamak_blanket.stl`, `tokamak_tf_coils.stl`, `tokamak_plasma.stl`, … . The viewer
loads them as one **assembly** of coloured **parts**, each independently shown/hidden and tinted, and
adds an **exploded view** that slides the parts apart so the nested internals can be seen. See
`assembly.hpp`, `shaders/mesh.{vert,frag}`, and the `--assembly <dir>` mode in `main.cpp`.

## Reusing the lit mesh pass (no new pipeline)

A part is an ordinary `GpuMesh` drawn with the same `mesh.{vert,frag}` as a single part — assemblies cost
no new shader or pipeline. The per-part data rides in two push-constant slots that are **unused when no
field is bound** (an assembly is flat-tinted, never field-coloured):

- `field_scale.xyz` → the part's **tint** (the fragment albedo);
- `field_bias.xyz` → the part's **exploded-view offset** (a world translation, applied in the vertex
  shader before the model→clip transform, so the lit position and the silhouette move together).

A mode flag in `cam_pos.w` (`> 1.5` = "assembly") selects this interpretation; normal renders leave
`cam_pos.w = 1`, so the existing field/colormap path is byte-for-byte unchanged. The 128-byte push
constant (the portable budget, ADR-0012) does not grow.

## The exploded-view offset

Let the assembly have parts $p_1,\dots,p_N$ with centroids $\mathbf c_i$, an overall centroid
$\bar{\mathbf c}$ and enclosing radius $R$. Each part is given a target offset $\mathbf e_i$ that it
reaches at **explode factor** $t = 1$ (the runtime offset is $t\,\mathbf e_i$, eased $0\!\to\!1$ as the
view explodes). The offset has two terms, chosen so it separates *any* assembly:

$$ \mathbf e_i \;=\; \underbrace{(\mathbf c_i - \bar{\mathbf c})}_{\text{radial}} \;+\;
   \underbrace{\Big(r_i - \tfrac{N-1}{2}\Big)\,\frac{2R}{N-1}\;\hat{\mathbf z}}_{\text{axial fan}}. $$

- The **radial** term $(\mathbf c_i - \bar{\mathbf c})$ pushes each part straight out from the middle —
  it does the work for a *scattered* assembly (parts at distinct places).
- The **axial fan** is the key for a machine whose parts are **concentric shells**. A tokamak's nested
  tori share a centre, so their radial term is $\approx 0$ and the radial explode alone would leave them
  on top of each other. Ranking the parts by size ($r_i$ = a part's rank, largest first) and fanning
  them along $\hat{\mathbf z}$ guarantees they still separate: the biggest shell sinks to one end, the
  smallest rises to the other, the fan spanning about the assembly diameter $2R$ at $t=1$.

The camera frames the **exploded** enclosing radius
$\;R(t) = \max_i\big(\lVert \mathbf c_i + t\,\mathbf e_i - \bar{\mathbf c}\rVert + r_i\big)\;$ so the
spreading machine stays in view as it opens.

> **Honest limitation.** Parts whose *own* sizes vary by 10× or more (a 24 m cryostat beside a 1 m
> solenoid) cannot all fully clear one another at a spacing that also keeps the small parts visible —
> the fan reveals them peeking out rather than floating free. Per-part hand-tuned offsets (CAD-style)
> are a later polish; the uniform fan is the honest, automatic default.

## Colour

Parts are tinted from a fixed 12-entry palette by **load order** (so a given part keeps its colour run
to run). The first three palette entries are a clean red / blue / green — one dominant channel each — so
the render test can classify a part by its dominant colour channel, robust to the studio lighting that
multiplies the tint.

## Verification

`tests/rhi/assembly_offscreen_test` builds three **concentric** cubes (red ⊃ blue ⊃ green about the
origin — a stand-in for nested shells) and checks the three things the assembly view must do:

1. **colour** — assembled, the outer red shell is what shows (red dominates, little blue/green);
2. **visibility** — hide the outer part and the blue middle shell is revealed, the red gone;
3. **exploded view** — at $t=1$ the axial fan separates the concentric shells, so all three colours show
   at once, and the framing radius grows ($R(1) > R(0)$).

End to end, `icem_viewer --assembly tokamak/` loads ICEM's 10-part tokamak — vessel, blanket, first
wall, plasma, divertor, TF/PF coils, central solenoid, ports, cryostat — as coloured, number-key-
toggleable parts; pressing **E** fans them apart along the machine axis.
