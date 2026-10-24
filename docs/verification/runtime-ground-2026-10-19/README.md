# Run-game floor lighting follow-up — 2026-10-19

**Work stopped at the owner's request. The later clarification—tree silhouettes
on house walls and beneath trees—remains unresolved.** The results below establish
the separate floor GI-band correction. A current tree-wall observation recording
is retained locally; the exact next experiment and camera targets are in the
[handoff](../../../HANDOFF-2026-10-19-runtime-ground-lighting.md).

Feature commits: `84f3126` (Claude's bounded shadow range) and `b340d44`
(receiving-side diffuse query, runtime profile and GPU regressions), based on
`727c851` / draft PR #346. See [ADR-0102](../../adr/0102-runtime-floor-lighting.md).

## Record the reported path

The previous editor exercise and stable cascade-grid tests did not establish
that the owner's actual Run-game symptom was fixed. This pass records the real
`the_block --play --scene …` window on RTX 3060, at 1280×720, through a private
Xvfb server and ffmpeg X11 capture. The initial recording starts at the saved
basin spawn. The retained city recordings change only the spawn X coordinate to
22 in a **separate diagnostic copy**, then look down, yaw back/forth and briefly
walk forward/back using xdotool. No live map or user process is changed.

- [Before: city ground turning/walking](ground-city-before.mp4)
- [After: the same input sequence](ground-city-after.mp4)
- Raw [before](floor-band-before.png) and [after](floor-band-after.png) frozen
  captures isolate the diffuse-query change before the shadow-range cap.

The recordings are unretouched rendered pixels, H.264 at 30 capture frames/s,
without audio. Capture cadence is not a performance measurement. Inputs are
wall-clock driven rather than a deterministic simulation tape; do not subtract
same-numbered video frames as an image oracle. The scripts stop only their own
game/server processes. Esc did not promptly terminate the private capture game,
so the harness terminates its own PID after a bounded wait. The original basin
capture therefore ended with a harness timeout after its video was saved; the
city harness handles that shutdown explicitly. The original basin recording and
diagnostic scene copies remain in `/home/next/reports/rime-runtime-ground-2026-10-19/`.

## Isolate the floor band

A frozen diagnostic uses the real city after the same 30-tick replication warmup
as Run game, including all 6796 world draws, stream terrain and the runtime
lighting stack. Eye `(22,1.5,2)`, pitch `-0.624`, yaw `0, ±0.312`; the scene and
probe data settle between captures. A sharp band remains with SSR off and
disappears with DDGI off. These are controls, not production feature removals.

For a matched yaw-zero frame, average encoded RGB luma across columns 320–959
and inspect adjacent rows 360–389, spanning the original row-374 band:

| Configuration | Original maximum step | Receiving-side query |
| --- | ---: | ---: |
| Full runtime lighting | 0.0412104951 | 0.0006447549 |
| SSR disabled control | 0.0468021961 | 0.0002737721 |

These are gamma-encoded image values, not physical radiance. Raw screenshots
and `surface-band-metrics.json` retain the evidence; neighboring texture gradients
remain. Frames extracted from both videos during motion were visually inspected for the original band; see the [motion contact sheet](video-motion-contact.png).

Do not overstate this result: the original *settled basin-spawn* world-anchored
yaw samples were already stable (maximum encoded-luma delta 0.00427 across 3825
floor samples). Early diagnostic captures before the replication warmup had only
108 draws and were superseded by the complete-city controls. Neither is presented
as reproduction of every motion artifact. The actual visibility atlas was not
read back; the exact probe acceptance/normalization explanation is inferred from
source and the controlled forward-GI intervention.

## Regression and integration

The new fixture calls the production `ForwardPbrPass::add_shadowed`, renders a
real plane and supplies real-format, hand-filled DDGI atlases. It covers floor
and ceiling normals, exact/near plane positions, pinned-world yaw, authored and
finite support modes, fully occluded probes, measured black and positions beyond
the offset. Open red irradiance is 1 versus fallback 0.125, a large control gap.

`prove-old-diffuse.py` compiles the original `727c851` fragment shader, overrides
that embedded forward shader in a newly compiled pass object, and links the
current tests. The original query fails **51 assertions** in the receiving-side
case; the black/occlusion case still passes. The fixed production query passes
both cases / **241 assertions**. No test threshold was loosened.

- Full Release build passes. Full RTX 3060 renderer: **255 cases / 1,720,843
  assertions**; required lavapipe: **255 / 1,720,843**, zero skipped; existing closed-room/GI leak tests remain green.
- Changed-TU Clang syntax passes for shadow fit, pass integration, both new/changed
  tests and the runtime sample. CPU ASan/UBSan/LSan: **10 cases / 10294 assertions**.
  Vulkan ASan/UBSan: **2 cases / 241 assertions**, driver leak checks disabled.
  Dependency archives are not fully instrumented. No threading change or new
  TSan run. Rust sources/GUI logic are unchanged.
- Pinned formatting over CI's engine/tests file set plus the changed sample and
  the license checks pass. Full lavapipe CTest is **92/94**: inherited collapse claims and the preserved
  edited courtyard fail; the canonical courtyard copy passes 2160 ticks. Performance
  and standalone export receipts are recorded below.

The shadow cap keeps default/invalid settings bit-identical, compares 3072/4096
camera ranges capped at 128 against the independent uncapped 128-range fit, and
checks capped-frustum corner coverage. Runtime uses 160 while raster terrain
still sees 3072. This separately improves sun-shadow density; it is not evidence
that the original GI band was caused by cascades.

The [hardware performance attempt](../../perf/runtime-ground-2026-10-19/README.md)
was rejected for unstable clocks, baseline-driver mismatch and exceeded budgets.
No comparable FPS or performance-gate claim follows.

## Standalone build

`/home/next/rime-exports/city-ground-lighting-2026-10-19/the_block`

Manifest SHA-256:
`a029f50332e276315da914caef4f9bb91d6cdd78a7fd1a3c63ef7e46227f979e`.
This exports the actual current saved `build/release/city-v2.rscene`, not the
diagnostic X=22 spawn copy. With repository/home hidden, the copied binary opens
a Vulkan window, loads all three weapons and runs four frames / 24 ticks with
authored effects. Integrity and destination/content/tamper/path refusals pass.
Source/live city and the user's edited courtyard remain unchanged; older bundles
are preserved. This is a Linux workstation development profile, not clean-machine
shipment qualification.

## Review and practical limits

Claude Sonnet 5.5 supplied the isolated range leaf; root integrated it and owns
the GI fix, real recordings and verification. Native parallel work supplied the
production DDGI fixture and independent runtime-path analysis. Astra reviewed
the correction and distinguished measured band removal from an unproven exact
visibility-atlas mechanism. Its report is retained. No Fable consultation is
claimed; the previous unfunded routes were not repeatedly charged/probed here.
No paid API spend.

The offset is not clearance-aware and can cross a sufficiently narrow gap.
The surface normal is unperturbed interpolated mesh data, not necessarily a
triangle face normal. Minimum-offset behavior at tiny spacing, authored-grid
boundaries, binary SSR hit/miss, camera-relative GI support and the sample's
approximate default-building SDF proxies remain limitations. Editor Preview
still lacks runtime DDGI/SSR; this pass does not silently enable those there.
No global claim that all shadows/reflections are now artifact-free.
