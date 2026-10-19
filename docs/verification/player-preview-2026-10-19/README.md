# Walk Preview verification — 2026-10-19

Implementation: `622798c` and its stacked feature commits, based on `ef11a1e`
(draft PR #344). User chose player controls and performance; Esc releases input,
Stop restores edit state, and the first controller supports walk/look/jump/static
collision while weapons remain in Run game. No browser deployment or merge.

## What ran

- Release full build passed. Full CTest: **92/94**, with the inherited three
  collapse claims in `the_block_headless` and the user's edited ±1 GW courtyard
  failing. The canonical courtyard copy separately passes its full 2160 ticks.
  The user file was preserved byte-for-byte (git blob
  `794da29c6968416bdebd1ee34a6f6f95e190ed8c`).
- Controller: **17 cases / 177 assertions**. Real floor dimensions, floor removal,
  walls including thermally styled walls, cosmetic phase sheets, shape/scale/
  rotation admission, refusal, lifecycle and cleanup are covered. The adapter
  remains query-only; existing large-box shallow-overlap limitations apply.
- Editorhost: **40 cases / 730 assertions**, including new strict wire codecs,
  state combinations, truncation/UTF-8 bounds and the real classifier. App:
  **58 cases / 509 assertions**, including recapture pointer baseline preservation.
- Complete renderer on both RTX 3060 and required lavapipe: **230 cases /
  1,707,944 assertions** each. Existing pixel regressions and new cache state
  controls pass. See the separate [performance evidence](../../perf/material-bindings-2026-10-19/README.md)
  for actual-view call counts and rejected clock qualification; no FPS claim.
- Rust default workspace: **398 passed**, four previously ignored tests remain
  ignored. GUI configuration: **171 passed**, including actual widget intent/ack,
  key/look routing, Esc, save refusal and modified-Esc/shortcut release checks.
  Strict workspace and GUI Clippy (`--all-targets -- -D warnings`), rustfmt,
  pinned clang-format and license checks pass.
- Changed-TU Clang syntax plus ASan/UBSan/LSan: controller 17/177, editorhost 40/730,
  stream input 8/105; renderer cache 3/29 separately retained in performance
  evidence. Dependency archives are not fully instrumented. No threading
  primitive changed; no TSan run was made in this pass.

## Real host and real window

`prove-player-host.py` launches the actual Vulkan city host and exchanges the
production wire. It admits 637 static bodies and five source hulls; checks actual
ground support, walking/jumping, uncaptured/stale-session rejection, release,
Pause/single Step/Resume without recapture, server Save/spawn refusal, **exact
serialized baseline restoration on Stop**, and the unchanged source document.
JSON and host log are retained. A missing-spawn Begin refusal also restores the
edited baseline exactly and permits a subsequent Inspect session. The same complete flow passes with changed app,
translator, codec and controller translation units under ASan/UBSan; GPU leak
detection is disabled there, while the CPU tests above enable it.

`prove-player-gui.py` drives the shipped editor through Xvfb/X11 using xdotool.
Screenshots were visually inspected: Walk selection and admission, captured
movement/look, Esc, focus loss/refocus, Shift+Esc and Stop are visible. The hint
has a dark backing to remain readable against the sky. This window exercise is
not an automated image oracle; exact movement/restoration comes from the host
proof and widget assertions. Windows/Wayland cursor behavior was not exercised.

## Export and collaboration

The updated standalone development bundle is
`/home/next/rime-exports/city-player-v2-2026-10-19/the_block`, manifest SHA-256
`3dc2ce954edc20322b204970047782f1599a62e1edb4323b14c6c53998d3b00e`.
With repository/home hidden, it opens a real Vulkan window, loads three weapons,
executes four frames / 24 ticks, and consumes the authored effects/spawn.
Existing destinations, missing explicit/packaged content, changed payload and
escaping cooked paths refuse. Old exports remain intact. This is a Linux
workstation development profile, not clean-machine release qualification.

Claude Sonnet 5.5 on the personal subscription supplied the bounded protocol leaf
and input helper in isolated worktrees. Root corrected the helper's real-module
import, modifier/Esc behavior and X11 cursor confinement, integrated all routing,
and verified the actual engine/GUI. Native parallel work supplied the collision
adapter/tests, renderer cache/count proof and widget checks. Independent review
caught an Inspect key-release regression; root fixed it before final checks.
No premium API/gateway consultation ran. Final personal usage read: 14% five-hour,
10% weekly. Reports describe each contribution and original verification limits;
the integrated results above supersede those limits. Own worktrees were removed.

Reopen with `./scripts/city-v2.sh`, check **Walk**, press **▶**, then click the
viewport. Next creator brick: one authored heater interaction and exit objective,
then reusable presets. General terrain walking, dynamic pushing, swimming,
weapon possession, gameplay freezing, collapse/performance qualification and
shipment remain open. The controller's void tick hook can log an internal fault
without publishing a distinct runtime fault status; Stop/Esc remain available.
