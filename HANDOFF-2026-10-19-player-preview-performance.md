# Walk Preview and renderer work — 2026-10-19

Read global rules `/home/next/.claude-switch/profiles/default/CLAUDE.md`, root
`CLAUDE.md`, ADR-0100 and `docs/verification/player-preview-2026-10-19/README.md`.
The user chose player control and performance, Esc release, Stop restoration,
and walking/look/jump/collision first, with weapons in the separate game window.

## Ready to use

```sh
./scripts/city-v2.sh
```

Check **Walk**, press **▶**, then click the viewport. WASD moves, mouse looks and
Space jumps. Esc, Shift+Esc, focus loss, Pause and Ctrl/Alt/Command release input.
Resume retains the player pose and requires another click. Stop restores the
original scene/camera. Stop before editing or saving. Inspect retains its fly
camera. The stream remains 960×540. Verification windows were closed afterward.

The source showcase and live `build/release/city-v2.rscene` retain 662 entities.
Only the two water/ice SlabRole values changed from Kerb (7) to the explicit cosmetic
Phase sheet (13). Existing camera and other live edits were preserved; do not reset
the live copy. Explicit colliders, thermally styled walls and timber remain solid.
Other older maps need one enabled PlayerSpawn and supported collision to use Walk.

Updated standalone Linux development export:

```sh
/home/next/rime-exports/city-player-v2-2026-10-19/the_block
```

This is the weapon game. Manifest SHA-256:
`3dc2ce954edc20322b204970047782f1599a62e1edb4323b14c6c53998d3b00e`.
With repository/home hidden, a real Vulkan window loaded three weapons, authored
spawn/effects and four frames / 24 ticks. Content/integrity refusals pass. Three
older city exports remain intact. Clean-machine release qualification stays open.

## Delivery and checks

Branch `feat/editor-player-preview`, parent `ef11a1e` on
`feat/environment-editor-v2` (draft PR #344, following #343). Feature integration
commit `622798c` follows the protocol/controller/cache/helper commits. Nothing
merged; no browser deployment. The publication URL is appended after pushing.

- Full Release build passes. CTest 92/94: inherited three collapse claims and the
  user's edited ±1 GW courtyard fail. Canonical copied courtyard passes 2160 ticks.
- GUI 171/171; default Rust workspace 398, protocol 31. Strict default/GUI Clippy,
  rustfmt, pinned clang-format, license and own whitespace checks pass.
- Renderer 230/230, with 1,707,944 assertions on both RTX 3060 and required lavapipe.
  Controller 17/177; editorhost 40/730; app 58/509.
- Changed-TU Clang syntax and ASan/UBSan/LSan cover controller 17/177,
  editorhost 40/730, stream 8/105 and cache 3/29. Dependency archives are not fully
  instrumented. No threading primitive changed; no TSan run in this pass.
- Real host: 637 static bodies and five source hulls; support/walk/jump, ignored
  uncaptured/stale input, release, Pause/Step/Resume, Save/spawn refusal, exact
  Stop snapshot restoration and unchanged source. Missing-spawn refusal restores
  the baseline and Inspect works afterward. The positive flow also passes with
  changed app/codec/translator/controller TUs under ASan/UBSan; GPU leak checks
  are off there, while pure CPU tests enable LSan.
- Actual X11 window screenshots show capture, movement/look, Esc, focus loss,
  Shift+Esc and Stop. Hint backing is readable. Windows/Wayland were not tested.

## Performance evidence and limits

Pass-local texture/sampler reuse compares full generational handles and always
binds the first value, including null. No sorting, draw/range/UBO/shader or GPU
count change. Fixed-state reference controls and integrated pixel tests pass.
The actual showcase camera submitted 36 textured draws per frame across three
frames: 540 attempts → 36 binding calls, 504 avoided. This is a small-view count,
not native FPS improvement or descriptor-set savings. Diagnostic binaries were
removed; product code has no instrumentation.

Two fresh 600-frame hardware receipts (960×540 and standard 1920×1080) were rejected:
clocks moved 53.9%/93.5%, the historical baseline fingerprint differs after the
driver update, and budgets remain unpaid. Full evidence:
`docs/perf/material-bindings-2026-10-19/`. No baseline, GPU setting or budget changed.
Existing plain-contact-bounds is already integrated; its frozen replay re-audit
passes. Speculative lazy padding was rejected before editing. No merge while the
inherited collapse/performance/shipment gates remain open.

## Collaboration and workspace

Claude Sonnet 5.5 on the personal subscription wrote bounded protocol and input
helper leaves in isolated worktrees. Root corrected the real-module import,
modifier/modified-Esc handling and X11 confinement, then integrated and verified.
Native parallel work supplied the controller, renderer cache/count proof and
widget checks. Independent review caught an Inspect release regression, fixed
before final checks. No premium API/gateway consult ran. Personal usage at finish:
14% five-hour, 10% weekly. The earlier code-account probe returned organization HTTP 403.
No Claude job remains active. All four own worktrees were removed; branches kept.
Reports/specs: `/home/next/reports/rime-player-performance-2026-10-19/`.

User courtyard blob remains `794da29c6968416bdebd1ee34a6f6f95e190ed8c`, including
its existing EOF blank line. Old notes and other sessions' worktrees remain.
Own sanitizer objects/binaries, diagnostic executable and temporary Claude
harness cache were cleaned after logs/fingerprints. Reuse `build/release` and
preserve 15 GiB free space.

Next creator brick: authored heater interaction and exit objective, then presets.
Walk is query-only: no weapons, pushing, swimming, skating, terrain-heightfield
walking or gameplay freezing. Unsupported collision refuses admission. Existing
shallow large-box GJK overlap limitations remain. The generic void tick hook can
log an internal controller fault without a separate published fault state;
Esc/Stop remain available. General action authoring, animation clips and adaptive
stream resolution are later bricks.
