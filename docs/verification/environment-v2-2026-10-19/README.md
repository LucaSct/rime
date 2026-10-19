# Editor navigation and authored environment V2

Measured locally on 2026-10-19, extending `b5bf53b` (draft PR #343).
Implementation through `36bdf07`; the final gameplay diagnostic is included in
the exported executable. This directory retains results, replay scripts and
screenshots rather than generated binaries or sanitizer objects.

## Behavior observed

The real Rust GUI and city host passed Preview navigation, release without
snap-back, Pause navigation, Stop restoration and movement immediately after
Stop. [Saved coordinates](navigation-proof.json) show that release preserved
the moved position exactly, and Stop restored the original position exactly.
The saved file contained no renderer handles. [GUI image](navigation-after.png).

The courtyard GUI selected another ThermalCell through the typed reference
picker, saved it, undid and redid it: `144 → 146 → 144 → 146`.
[Receipt](picker-proof.json), [picker image](picker-options.png).

The full city and isolated export consumed the saved PlayerSpawn, fire, smoke,
water and ice descriptions. With `/home` and the checkout hidden, an actual
Vulkan window loaded all three weapons and ran 4 frames / 24 ticks. The
authoritative and predicted capsule centres were both `(-10, 0.902, 2)` after
startup settling, with zero yaw/pitch. The final receipt reported two admitted
effect sources, three flames, one light, two phase surfaces/cells, one atlas,
and zero thermal refusals. [Game output](final-isolated-game.log),
[image](final-packaged-game.png).

The canonical courtyard ran on the RTX 3060 for 2160 ticks with no thermal
refusals: cooling reached 260.236 K, melting 279.491 K, control 273.150 K.
[Selftest](canonical-yard-selftest.log), [hardware run](yard-v2-rtx-complete.log),
[ice, water, smoke and flame image](yard-v2-rtx-complete.png).

The native `./scripts/city-v2.sh` launcher opened the new editable copy on the
RTX 3060: 630 entities, 569 slab previews, no skipped components or unknown
patterns, and the existing 960×540 stream. [Startup](native-v2-editor.log).
Subsequent launches preserve `build/release/city-v2.rscene`; the user's city
and edited courtyard files were not overwritten.

## Checks and remaining failures

| Check | Result and evidence |
| --- | --- |
| Release build | Pass; [build](build-release-final.log), [final sample relink](build-final-ready.log) |
| Rust workspace | Pass; four pre-existing ignored tests; [output](cargo-workspace.log) |
| GUI editor | 151/151; [output](editor-gui-final.log) |
| Clippy | Default workspace and GUI all-targets with `-D warnings` pass; [default](clippy-default.log), [GUI](clippy-gui.log) |
| Format/license | Pinned clang-format over CI's engine/tests file set and owned sample files; cargo fmt and license check pass; [Rust](rust-format.log), [license](license.log) |
| Phase rendering | 11 cases / 613435 assertions, including GPU readback; [output](phase-tests.log) |
| VFX scene | 9 cases / 29479 assertions; [output](vfx-scene-tests.log) |
| Renderer | 19 cases / 122 assertions in final sanitizer run; [output](clang-sanitizers.log) |
| Runtime environment | 9 cases / 896 assertions with Vulkan required; [output](runtime-environment-tests.log) |
| Camera and spawn | 9 cases / 372 assertions; [output](spawn-camera-tests.log) |
| Clang | 15 changed translation units syntax checked; final main.cpp checked again after diagnostic change; [output](clang-sanitizers.log), [final check](final-main-syntax.log) |
| ASan/UBSan | 52 cases across VFX, phase maps, camera/spawn, runtime and renderer; [output](clang-sanitizers.log) |
| Bundle checks | 32 Python tests, 11 cook fixtures, integrity and refusal checks; [tests](bundle-tests.log), [export](export-v2.log), [isolation](isolated-export-proof.log) |
| Full CTest | **92/94** in 282.68 s; [output](ctest-final.log) |

Sanitizers instrumented the changed implementation/test translation units,
using existing uninstrumented dependency archives. This is not a complete
sanitized engine build. Leak checks were enabled for CPU tests and disabled
for the Vulkan driver tests. Lavapipe was required for GPU checks.

The CTest failures are the inherited three collapse assertions in
`the_block_headless`, and `thermal_courtyard_selftest` against the user's
pre-existing local ±1 GW experiment. The latter exceeds the admitted thermal
step range; a copy of the canonical scene passes the full 2160-tick selftest.
No user edits or assertions were changed to obtain a pass.

The hardware performance attempt also **failed**: unstable GPU clocks, frame
and simulation budgets exceeded, and a baseline fingerprint mismatch. Its
[rejected receipt](../../perf/environment-v2-2026-10-19/README.md) is retained;
there is no new accepted baseline or permission to merge.

## Export and replay

Local bundle: `/home/next/rime-exports/city-v2-2026-10-19/the_block`.
Manifest SHA-256:
`b396c7cdabebd1f908de61d4ac6726e06f1c2ed35b89c0f41027e1adc74331b3`.
The older `city-demo-2026-10-19` bundle is untouched. Isolation hides repository
and home paths; it still uses this machine's system libraries and drivers, so
it does not establish compatibility on a second machine.

From the repository root, with a Release build, GUI binary, Xvfb, xdotool,
Pillow and bubblewrap available:

```sh
xvfb-run -a -s '-screen 0 1280x800x24' \
  python3 docs/verification/environment-v2-2026-10-19/prove-navigation.py
xvfb-run -a -s '-screen 0 1280x800x24' \
  python3 docs/verification/environment-v2-2026-10-19/prove-picker.py
python3 docs/verification/environment-v2-2026-10-19/prove-export.py
python3 docs/verification/environment-v2-2026-10-19/verify-cpp.py
```

The navigation replay expects the prior editable `build/release/city-editor.rscene`;
both GUI scripts operate on copies and use fixed window coordinates. Export
replay accepts `RIME_V2_BUNDLE`, and all scripts accept `RIME_REPO_ROOT`.
Replay overwrites evidence files beside the scripts. The export refusal checks
temporarily append to README.txt and restore it in `finally`; they leave the
verified payload unchanged. A fresh export has a new manifest digest, which
must be supplied by updating the replay's pinned digest after review.

## Parallel implementation and review

Claude Sonnet 5.5 used the working `personal` subscription in two isolated
worktrees: the multiscale water/frost atlas and authored player spawn resolver.
[Phase report](phase-detail-report.md), [spawn report](authored-spawn-report.md).
These are original delegation receipts; their limitations describe the code
at delegation time, before integration. Root built and verified the integrated
code and replaced Claude's permissive quaternion normalization with finite,
near-unit input and composed-pose validation, plus adversarial tests.
Native parallel agents delivered camera reconciliation, typed reference UI,
flame/smoke shaders and the authored runtime bridge; root integrated the seams,
flame/smoke presentation, showcase, docs and verification. Their reports are
retained here. No premium gateway consultation or API spend was used.

## Scope limits

The phase atlas contains 266 immutable 128² mipmapped maps, 23,243,080 texture
bytes (22.17 MiB, excluding driver overhead). Phase is quantized to eighths
and moving detail has 32 frames; this is surface presentation, not flowing
water physics. Fire/smoke and thermal state in the city remain client-local
cosmetics without damage, collision or replicated combustion. Environment
transforms retain the city's existing flat transform contract. Spawn markers
support parent composition, but do not check geometry overlap or teams.

The camera protocol still lacks per-edit revisions/session identities. A
snapshot matching an old pending local pose can be conservatively ignored
until acknowledgement; identical-pose ownership changes remain ambiguous.
General possession, event wiring, prefab libraries and adaptive editor stream
resolution are future bricks. See ADR-0098/0099 and the creator guide.
