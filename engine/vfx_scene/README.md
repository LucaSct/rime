# Authored smoke and fire presentation

`rime::vfx_scene` connects reflected ECS descriptions to the core-only particle
and fire adapters. It depends on ECS/VFX, with no renderer, editor or physics
dependency. Games can own the same `Effects` consumer as the editor host.

The engine component profile registers `SmokeEmitter` and `FireEmitter`. Both
default to disabled. In the editor, use **+ spawn**, select the entity, then
**+ add component** and choose either type. Set the local transform, half extents
in metres, particles per second, intensity and enabled. Press Play, Pause or Step
to preview; Stop discards particles, lights and emission debt. The ordinary
inspector commands provide undo/redo, and engine-owned `.rscene` saving preserves
the descriptions. There is no effect asset format or separate cook required for
these procedural sources. This does not prove the outstanding Blender material
import/edit/export workflow.

Call `reconcile` after deriving world transforms to retire removed, disabled or
invalid sources even while paused. A changed fire pose/intensity clears its old
light until the next accepted tick. Existing smoke remains frozen while paused
and retires normally when ticking resumes. A positive-rate moving fire retains
fractional emission debt. Rate edits replace the emitter and discard old debt.

One consumer admits 16 sources, sorted by process-local entity index, generation
and source kind; an entity with both components consumes two slots. It retains
256 particles, forwards at most eight particles per source/update and stores at
most 16 deferred particles per source. Source-cap drops count visits during
reconciliation, including paused frames. Invalid inputs, emission budget drops,
deferred/discarded whole particles and particle-pool drops are observable through
`Effects::stats()` and `field().stats()`. Valid rates are `(0,1024]`/s, intensity
`[0,4]`, nonnegative finite extents and finite transformed bounds. Tick dt must
be `(0,0.25]` seconds with strictly increasing caller ticks.

The editor converts the bounded pool to sorted alpha smoke and submits fire
lights through `SceneRenderer::set_transient_point_lights`. These copied light
rows never become authored `PointLight` components. Authored scene lights retain
priority under the existing forward-light cap; the clustered path uses its
existing shared limits. The setter counts invalid/copy-cap drops, not later
combined-light budget rejection. Empty input clears transient lights. No enabled
sources means no particles, no light rows and no additional smoke pass.

V2 adds three tapered HDR flame sprites per accepted fire source, at most 48.
They use a fixed array independent of the smoke-pool capacity. Tongue size and
sway use accepted simulation time and a stable source phase; repeated rendering,
Pause and rejected ticks do not animate them. Removal, invalid edits, disable,
extinguish and Stop clear flames alongside the light. Residual smoke keeps its
normal lifetime. No flame state is written into ECS or a saved scene.

Smoke now has smooth procedural density, soft birth/retirement, cooler ambient
colour and bounded warm illumination from the same local fire lights. This is an
approximate incident-light calculation, not self-shadowing or a density solver.
Flames composite into HDR before smoke, so a plume can attenuate them. Both keep
depth read-only and preserve it for subsequent passes. Off/empty lists declare
no pass; the renderer independently caps and counts copied/submitted sprites.

These are cosmetic billboards and a warm fire light. There is no temperature,
fuel, spread, damage, soft scene intersections or smoke motion vectors. Source
selection uses local handles, not replicated source
identity. New enabled scenes still need hardware capacity and multiplayer
lifecycle evidence before release. The authoring tests exercise the C++ host
edit/inverse, save/load and Play-session seams; they do not claim a clicked GUI
proof or a qualified hardware performance run. The Rust smoke client also checks
the real engine's schema, disabled defaults and edit/inverse/redo bytes:

```sh
tools/target/debug/editor --smoke --effect-sources --engine build/release/bin/rime-engine
```

Use a fresh scene for this source-default check. `--save <file.rscene>` additionally
asks the engine to save those final descriptions; reopen it with ordinary
`--smoke --scene <file.rscene>` to exercise the fresh-process load path.
