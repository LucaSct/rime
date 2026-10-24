# Handoff — city editor run and export, 2026-10-19

Branch `feat/city-editor-play-launch`, implementation `aa2720e`, parent `9702423`
(#342). Nothing merged. Day-end working demo checkpoint.

## Open and play

`./scripts/city-editor.sh` opens the persistent saved city at
`build/release/city-editor.rscene` (625 entities / 569 destructibles).
Press **Run game**: successful save first, separate full player/weapon window.
Preview triangle remains scene-system simulation. Camera: hold RMB continuously
with WASD, Q/E vertical, Shift faster, wheel dolly. The editor's scene camera is
not the sample's runtime player spawn policy. Sources are selected by Outliner
Effects/Thermal filters; Display List/Tiles uses vector type icons, not thumbnails.

Final bundle: `/home/next/rime-exports/city-demo-2026-10-19/the_block`.
Copied executable defaults to Play; keep sibling content/notices/manifest.
WASD/mouse, left fire, Q/RMB aim, R reload, 1/2/3 weapons, F charge, Escape quit.
Export another saved map: `./scripts/export-city.sh NEW-DIRECTORY [--scene FILE]`.

For thermal water/ice + smoke/fire: `./scripts/thermal-courtyard.sh --editor`.
Blue frost veins are static; liquid ripples are cosmetic normals, paused with the
accepted preview clock. No physical waves, water transport, refraction or ice
collision added. General workflow, Unity and historical public Frostbite examples:
`docs/creator-workflow.md`. No browser UI or deployment changed.

## Important corrections

Palette, city host and courtyard preparer now mark derived draw data with the
existing DerivedComponents tag. Without it, real GUI saves baked renderer indices
into scene files. Actual viewport save/run/reopen is now proven; prior headless
save evidence did not cover this. Do not bake MeshRef/MaterialRef into new maps.
Authored-material reads are copied before structural edits invalidate pointers.

The editor serializes saves, drains stale replies before new actions, locks edits
and document switching while saving, and launches only after fresh exact-path
success. Save As only adopts a successful path. No protocol request-ID change.
Game launch is direct argv; the game stays independent when the editor closes.
Runtime/cook ownership remains in the game; no duplicate weapon runtime in host.

## Gates and evidence

Full Release build; CTest 93/94 (same three inherited block-collapse claims).
GUI 134 tests; workspace tests (four ignored); default+GUI Clippy, format/license;
bundle tests 32. Actual Clang checks on six TUs; changed-TU ASan/UBSan CPU load and
palette regression 7 cases/302 assertions with LSan; instrumented-host GUI GPU
save/run/reopen with driver leak detection off. Final exported binary has an actual
windowed Vulkan run with checkout/home hidden, all weapons loaded, 4 frames/24
ticks, exit 0, and positive/negative bundle integrity/content checks.

Evidence: `docs/verification/city-editor-2026-10-19/`; scratch matching
`/home/next/reports/`. Full engine libraries were not rebuilt with sanitizer flags.
No new hardware perf qualification; production export and clean second-machine
compatibility remain open. Generic smoke's brightest-RGB picker is invalid for
this sky-filled scene; initial failure retained, fixture unchanged. Actual GUI and
explicit isolated runtime provide final proof. Astra/Fable consultations cost
$0.07937 total, matching key usage delta; independent advice/disagreements recorded.

## Next bricks

Stop here for the demo. Next: in-viewport player possession/action focus using the
existing GameDefinition/InputMap foundations; authored spawn points; sensor → action
wiring and event debugging; presets/prefabs and an entity reference picker; adaptive
viewport resolution/aspect; skeletal clip playback and animation authoring. Treat
smoke transport, fire/thermal coupling, moving water/buoyancy and ice collision as
separate bounded milestones with accepted-state and performance gates.

Do not merge while inherited locality/performance or stack CI gates remain open.
Do not clean up unrelated pre-existing untracked `.brick-*`, `.report-*` or handoffs.

The final native editor is left open for Luca on RTX 3060, PID 2749436, current
editor binary verified through /proc. Log: scratch/native-editor-ready.log.
Other pre-existing editor windows belong to the user; do not kill them as cleanup.
