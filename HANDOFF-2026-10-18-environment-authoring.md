# Handoff — environment authoring, 2026-10-18

Branch: `feat/environment-authoring`, based on `perf/plain-contact-bounds`
(`f9371de`). Implementation commit: `a1f26763ac980753e0798f887f5ef14befe0b5e8`. A following documentation commit
retains this evidence/handoff. Nothing merged; parent gates stay open.

## Owner's latest direction

Continue water, smoke, fire, ice and the editor pipeline. For ice the owner
explicitly chose **water freezing/melting from temperature first**, ahead of
cosmetic solid ice or breakable sheets. Preserve this priority. The ordered
bricks are in `docs/design/environment-effects-milestones.md`, with proposed
ADR-0090. Product shipment order remains ADR-0085, not the old ADR-0074 list.

## Built

* Removable `rime::vfx_scene` above ECS/VFX; reflected `SmokeEmitter` and
  `FireEmitter`, both disabled by default, registered in the engine profile.
* Existing inspector commands/undo and `.rscene` save/load carry descriptions.
  A bounded consumer owns 16 sources / 256 particles, process-local sorted
  admission, fractional debt, capacity/refusal counters, strict ticks and
  disposable presentation. Fire remains cosmetic, without fuel/heat/damage.
* Editor host Play/Step advances, Pause reconciles removals, Stop clears. Source
  edits derive missing world transforms before consumption, including newly
  spawned entities. Default viewport now calls its supplied registrar once.
* Copied bounded transient light input to SceneRenderer; no PointLight entities
  baked into saves. Empty smoke adds no smoke pass. Smoke RGB remains approximate.
* Rust `editor --smoke --effect-sources` verifies live defaults/edit/undo/redo;
  combine `--frames 3` for twelve acknowledged Steps and Stop. Save and reopen
  are proven in separate host processes. No special per-component Rust UI.

## Evidence / limitations

See `docs/verification/environment-authoring-2026-10-18/README.md` and raw logs.
Full C++: 89/90, same inherited three block locality/collapse claims. No test
threshold retunes. New adapter: six cases/442 assertions, ASan/UBSan; four changed
C++ TUs Clang-check. Rust GUI suite 120/120; strict Clippy and installed SDK pass.
Live preview: 12 ticks/12 lit ticks, eight spawned/eight cleared, zero drops.
No new native performance measurement, clicked environmental GUI proof, exported
multiplayer effects proof or TSan run. The parent hardware gate still misses
sim.block p99 and the comparison was refused; do not mark M18 done.

All emitted state is cosmetic; process-local entity handles are not canonical
network effect IDs. Old smoke ages normally after source removal, freezes during
Pause and clears on Stop. Paused fire description edits clear light until the
next accepted tick. Rate edits discard whole/fractional debt; whole debt is counted.
No soft intersections, smoke motion vectors, flames, temperature or ice yet.

## Next concrete bricks

1. Research/review and implement a bounded CPU energy/phase oracle for water/ice:
   stored energy, temperature and liquid fraction, documented material parameters,
   latent heat, heat/cool reversibility, phase bounds, zero-input invariance,
   refusal atomicity and a latent-heat-disabled sensitivity control. Keep it
   independent of transport/rendering first. This is the owner's ice priority.
2. Water body displacement/momentum feedback, with mass and coupling witnesses;
   the shallow-water grid has no vertical momentum, so do not invent full momentum
   conservation from a recorded reaction.
3. Reflected basin/probe/temperature/heat-source descriptions, visible dynamic
   surface and phase/thickness, editor undo/save/reload/Play. Couple liquid/solid
   mass and transported energy before claiming flowing water freezes correctly.
4. Smoke lighting/depth/temporal quality, destruction join/relevance churn, fire
   authority/lifecycle; later heat/spread/damage and breakable ice separate proofs.
5. Clicked creator workflow, existing Blender-authored M16 material proof,
   validated export, server + two exported clients, late join/compatibility tests.

Do not kill other workloads or measure alongside builds/tests. Maintain 15 GB
free reserve; this run retained local scratch under
`/home/next/reports/rime-environment-authoring-2026-10-18/`. Existing untracked
notes belong to other sessions. No paid consult or subagent ran.
