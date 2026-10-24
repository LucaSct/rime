# Timber basins and dynamic fire — 2026-10-19

Follow-up to the editor/environment V2 work on draft PR #344.
Implementation commits: `f0d8ad6` (fire) and `18a1fbe` (timber).

Both city sheets now sit inside low wooden basins: two courses of boards, a
narrow cap and proud corner posts. Thirty-two ordinary static prop entities
were appended to the source showcase and the user's saved editable city. The
existing entity text, transforms, camera and authored effect values were
preserved byte-for-byte before the appended section. The tracked courtyard
experiment remains untouched. [Authoring receipt](timber-authoring.json).

The appended public SlabRole kind 12 reconstructs a scaled cube with the
existing warm rough wood material. It adds no texture, material or cooked
destruction dependency; old role values retain their meanings. The editor names
these entities “Timber board”. Boards are static visual props, with the same
scope as the existing city furniture.

Fire retains three rows per source and the 48-row cap. A steady bright root is
surrounded by quicker side tongues with independent surges, lean, lift,
opacity and warm radiance. Seeded integer harmonics join smoothly at the
eight-second loop boundary; accepted simulation time is the only clock.
Smoke/light adapters and particle interfaces are unchanged.

## Observed results

The real GUI Preview loaded 662 entities, all roles/patterns resolved, and showed
both framed surfaces with moving effects. [GUI image](framed-editor-playing.png),
[output](framed-editor.log), [receipt](framed-editor-proof.log).
The actual weapon game ran 12 frames / 88 ticks with all three weapons and
the existing authored spawn: two source FX, three flames, one light, two phase
surfaces/cells and no thermal refusals. [Image](framed-game.png),
[output](framed-game.log).

| Check | Result |
| --- | --- |
| Release targets | Pass; [timber build](build-timber.log), [final fire build](build-fire.log) |
| Blockkit palette, save/reopen | 7 cases / 321 assertions; [output](blockkit-tests.log) |
| Fire/VFX state | 10 cases / 34496 assertions; [output](fire-tests.log) |
| Authored runtime Vulkan checks | 9 cases / 896 assertions; [output](runtime-tests.log) |
| GUI | 151/151; [output](gui-tests.log) |
| Strict GUI clippy | Pass; [output](gui-clippy.log) |
| Clang syntax / ASan+UBSan+LSan | Four changed TUs; 17 cases / 34817 assertions; [output](clang-sanitizers.log) |
| Formatting and licenses | Pinned clang-format over all 694 CI engine/test files, cargo fmt and license check pass |
| New standalone export | 11 cook fixtures and isolated window/integrity/refusal checks pass; [export](export.log), [proof](export-proof.log) |

Sanitizer instrumentation covers changed implementation/test objects, using
existing uninstrumented dependency archives. It is not a full sanitized build.
The first GUI run, concurrent with Vulkan/game checks, hit the existing
500 ms child-process deadlines in two Run-game tests. The suite passed 151/151
when rerun without that concurrent work; no tests or deadlines were changed.
The [initial failure](gui-tests-concurrent-initial.log) is retained.

The previous full CTest result remains 92/94 (inherited collapse assertions and
the user's edited ±1 GW courtyard; canonical courtyard copy passes). This
presentation follow-up used the targeted suites above instead of repeating the
unchanged full suite. A fresh hardware performance attempt also failed;
its [rejected receipt](../../perf/timber-fire-2026-10-19/README.md) closes no gate.

## Use and replay

```sh
./scripts/city-v2.sh
/home/next/rime-exports/city-v2-timber-2026-10-19/the_block
```

The launcher keeps `build/release/city-v2.rscene`, including the user's saved
camera. The standalone uses the committed showcase and leaves both earlier
exports intact. Manifest SHA-256:
`7f4cca9c8c2abe7e1c490142cb24de16171d422e04e71512c510dfca70c9d0eb`.
Isolation hides home and checkout while retaining this host's system libraries;
it is not a second-machine compatibility proof.

From the repo root, after building Release/GUI targets:

```sh
xvfb-run -a -s '-screen 0 1280x800x24' \
  python3 docs/verification/timber-fire-2026-10-19/prove-framed-editor.py
python3 docs/verification/timber-fire-2026-10-19/prove-framed-export.py
python3 docs/verification/timber-fire-2026-10-19/verify-focused-cpp.py
```

Scripts accept `RIME_REPO_ROOT`. The export proof also accepts `RIME_V2_BUNDLE`
and pins the reviewed digest. Replay writes evidence beside the script, uses
a scene copy, and temporarily changes/restores the bundle README for an
integrity refusal check. It does not rewrite the live editor map.

Claude Sonnet 5.5 implemented the fire leaf in an isolated worktree while root
implemented timber geometry/palette and integrated/reviewed the result.
[Original delegate report](claude-fire-report.md); its unrun-test caveat was
resolved by the integrated tests and visual checks above. No paid gateway
consultation was used. Nothing merged.
