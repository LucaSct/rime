# Opt-in thermal phase presentation

`rime::fluids_render` sits above `fluids_scene` and `render`, keeping both lower
modules independent. Worldkit registers `ThermalSurface { enabled=false }`.
Adding/enabling it on a validated thermal cell opts that entity's rendered mesh
into a bounded fixed diagnostic palette: blue polished ice, darker smooth water.

`phase_surfaces(world, preview)` reads committed cell fractions and matching
live generation-safe entity bindings. It returns at most 16 factor rows, with
no heap storage or ECS writes. It has no cache to leak across revisions. Empty,
invalid and reset graphs produce no rows. Failed numerical ticks retain the last
accepted appearance; the editor's separate diagnostics show the refusal. Surface
binding changes while paused do not reset the numerical graph.

The renderer copies `SurfaceAppearance` rows, matching full entity handles after
normal material/submesh resolution. It modifies private frame copies of RGB and
roughness; alpha and other authored factors remain. The factor-only API inherits
authored maps; the optional detail owner below explicitly overrides two map slots. The whole entity
is affected, including each resolved submesh. More than 16 rows, duplicate/null
keys or invalid RGB/roughness refuse the whole batch and clear old rows. Empty
input restores authored appearance; stale generations never target a new entity.
There are no writes to shared material registries, scene files or undo state.

This is an explicit presentation blend, not a mixture BRDF, volume fraction,
water optical model or geometric phase front. No new shaders, transport, ice
geometry/collision or automatic combustion heating. The [courtyard demo](../../samples/thermal-courtyard/README.md)
uses the same adapter and renderer seam as the editor host. See
[ADR-0095](../../docs/adr/0095-transient-phase-surfaces-and-courtyard.md) and the
[structural pixel proof](../../docs/verification/thermal-courtyard-2026-10-18/README.md).

## Cosmetic surface detail (ADR-0096)

`PhaseDetail(device)` optionally adds immutable borrowed normal/base maps to these
same rows. Declare it before the renderer (destroy after it); the device outlives
both. Destruction waits idle. V2 has one static frost normal, eight liquid levels
with 32 animation frames each, and nine tint maps, all 128² with eight mip levels.
The 266 maps occupy exactly 23,243,080 texture bytes (22.17 MiB), excluding driver
allocation overhead. This replaces the original 64-frame water-only normal bank.
Initialization uses blocking uploads once per host; partial creation publishes no
maps and falls back to factor-only rendering. It allocates even if the host scene
has no enabled thermal surfaces. No per-frame map writes/allocations or new shader
ABI. Existing PBR normal/colour sampling handles every resolved submesh.

`surfaces(world, preview)` uses the committed presentation-duration accumulator;
Pause and refusal cannot advance it, and graph rebuild/Stop restarts its epoch.
Frost and water slopes are baked together per liquid level. Water has directional
swell, chop and finer ripples; periodic cellular frost has long veins, branches
and fine detail over polished blue variation. Liquid fraction rounds to the
nearest eighth for map selection. Fractions below 1/16 use the same static frost
map as fully frozen cells, removing the former last-fraction normal switch.
Both normal and tint quantization are explicit visual approximations; numerical
fraction, factor blending and energy remain continuous and unchanged.
The geometry stays fixed: waves alter shading, never collision/silhouette/flow.
See [creator controls/workflow](../../docs/creator-workflow.md).
