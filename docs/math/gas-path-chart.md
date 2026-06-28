# The gas-path chart: a computed field as a 2-D profile (viewer D4)

ICEM's compressible nozzle (Brick 26, `icem` repo `docs/math/compressible.md`) computes a real 3-D field —
the Mach number, temperature and pressure of a gas accelerating through a de Laval nozzle. The viewer
already colours the nozzle surface by that field (the colormap, [colormap.md](colormap.md)) and cuts it
open (the cross-section, [clip-cap.md](clip-cap.md)). The **gas-path chart** adds the engineer's other
view of the same data: a 2-D line plot of the field *along the flow axis*, so you read the gas accelerate
through the throat and cross **M = 1** into the supersonic branch — the diagram every nozzle is designed
against. Code: `chart.hpp` (sampling + layout), `ui.hpp` (a new `line()` primitive), wired into the
windowed viewer as an overlay (`--chart`, the **H** key). Proof: `tests/rhi/chart_offscreen_test.cpp`.

## Sampling the field down the flow axis

The field arrives as a dense node lattice (the `.icef` 3-D texture, [colormap.md](colormap.md)): a value
and a validity flag per voxel on an $n_x\times n_y\times n_z$ grid. A gas-path profile is the
**cross-section average** at each axial station. The flow axis is taken to be the **longest grid axis** —
ICEM's revolved gas-path parts are long and slender, so the axis with the most samples is the streamwise
one (the nozzle here is $30\times30\times45$, so $z$). For each station $a$ along that axis,

$$ \bar f(a) = \frac{1}{|V_a|}\sum_{(u,v)\in V_a} f(u,v,a), \qquad
   V_a = \{\,\text{valid voxels in slice } a\,\}, $$

with empty slices marked `NaN` (skipped when drawing). `sample_axial_profile` is pure — no GPU — so the
test pins it directly against a known Mach distribution.

## A line primitive for the from-scratch UI

The E2 UI ([ui-text-layout.md](ui-text-layout.md)) draws only **axis-aligned** quads — fine for panels and
glyphs, useless for a sloping curve. So `ui.hpp` gains `line(x0,y0,x1,y1,thickness,c)`: a segment drawn as
a thin quad straddling the line. With segment direction $\mathbf d=(x_1-x_0,\,y_1-y_0)$ and length
$\ell=\lVert\mathbf d\rVert$, the unit **perpendicular** is $\hat{\mathbf n}=(-d_y,d_x)/\ell$, and the four
corners are $\mathbf p_{0,1}\pm \tfrac{t}{2}\hat{\mathbf n}$ — two triangles, emitted through the same
vertex path as `quad()` (texcoord $u=-1$ marks it untextured). Axes, the curve and the dashed sonic line
are all just `line()` calls.

## Plotting

The chart maps a station index $i\in[0,N)$ and a value $v$ into the inset plot rectangle
$[p_x,p_x{+}p_w]\times[p_y,p_y{+}p_h]$:

$$ x(i) = p_x + p_w\,\frac{i}{N-1}, \qquad
   y(v) = p_y + p_h\left(1 - \frac{v-v_\text{lo}}{v_\text{hi}-v_\text{lo}}\right), $$

so the inlet is on the left, the exit on the right, and — because screen $y$ points **down** — the value
axis is flipped to put high values at the top. The vertical range $[v_\text{lo},v_\text{hi}]$ is the
field's own min/max (carried in the `.icef`). The curve is the polyline through $(x(i),y(\bar f(i)))$ over
the finite stations; the axes, the top/bottom value labels and the `INLET`/`EXIT` ticks frame it. When the
field is `mach` and its range straddles 1, a dashed horizontal line at $y(1)$ marks the **sonic point** —
the throat, where the flow goes supersonic.

## Overlay integration

The chart is a screen-space **overlay**: each frame it is built into a `Ui`, uploaded, and recorded as a
final UI pass over the 3-D scene with `LoadOp::Load` (the scene and the colour legend underneath are
preserved, exactly as the legend itself overlays the mesh). It is toggled live with **H** and started by
`--chart`. The render test drives the identical `build_gas_path_chart` off-screen and asserts the panel
fill, the amber curve and the bright axis labels appear (and that the sonic line is present only when the
range crosses M = 1); `RIME_DUMP_CHART=<file.ppm>` writes the frame for eyeballing.

Together with the Mach colormap and the cross-section, this is the viewer's **turbojet/nozzle flow view**:
the computed compressible flow shown as a coloured supersonic core *and* read off as a gas-path curve —
the same numbers, the two ways an engineer looks at them.
