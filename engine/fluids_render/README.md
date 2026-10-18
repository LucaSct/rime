# Opt-in thermal phase presentation

`rime::fluids_render` sits above `fluids_scene` and `render`, keeping both lower
modules independent. Worldkit registers `ThermalSurface { enabled=false }`.
Adding/enabling it on a validated thermal cell opts that entity's rendered mesh
into a bounded fixed diagnostic palette: pale rough ice, darker smooth water.

`phase_surfaces(world, preview)` reads committed cell fractions and matching
live generation-safe entity bindings. It returns at most 16 factor rows, with
no heap storage or ECS writes. It has no cache to leak across revisions. Empty,
invalid and reset graphs produce no rows. Failed numerical ticks retain the last
accepted appearance; the editor's separate diagnostics show the refusal. Surface
binding changes while paused do not reset the numerical graph.

The renderer copies `SurfaceAppearance` rows, matching full entity handles after
normal material/submesh resolution. It modifies private frame copies of RGB and
roughness only; alpha, maps and other authored factors remain. The whole entity
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
