# Walking shadow stability — 2026-10-19

Implementation: `9507cb2` and its feature commits, based on `38f44c3`
(draft PR #345). See [ADR-0101](../../adr/0101-walking-shadow-stability.md).
No merge, browser deployment, authored map reset or resolution increase.

## Measured defects and corrections

- The same 600 walking/turning poses at city coordinates changed the original
  fixed-world shadow grid phase by **0.425367355 texel**, with relative XY scale
  variation **0.000325352**. The camera-local analytic sphere and fixed light
  basis reduce those to **0.000122070 texel** and **zero**. The original object is
  explicitly compiled from `38f44c3` and placed before the current render archive;
  the baseline proof does not depend on a stale archive. Logs and provenance are
  retained. This is a world-grid oracle, not a demand that walking screen pixels
  stay unchanged.
- One production sampler now serves mesh and terrain receivers. Full-contrast
  GPU probes cross XY/depth boundaries and overlapping cascade fade bands.
  Original hard switching measures a maximum adjacent visibility step of **1**.
  The initial raw-coarse blend also steps by **1** at overlapping bands; composing
  against the already-faded coarse result fixes it. Current maximum steps are
  **0.0874023 / 0.0112305 / 0.115723**, within the unchanged **0.1 / 0.03 / 0.15**
  bounds. Interior pixels retain nine PCF taps; transition pixels can sample
  additional cascades. Map resolution, bias and PCF radius are unchanged.
- Local spotlight cache identity now includes depth-affecting world draw data.
  Pixel checks exercise moving/removing rigid pieces and masked double-sided
  foliage; old and new footprints refresh. Unchanged and camera-only/reordered
  draws preserve cache hits. Unknown deformation bounds invalidate conservatively.
  In-place GPU texture writes retaining their handle still require explicit
  invalidation.
- Derivative-dependent material sampling/normal filtering precedes masked
  discard. Missing-camera frames clear world temporal history. Editor Preview
  now enables the authored directional/local shadows, matching those runtime
  gates while retaining its 960×540 stream.

## Integrated checks

- Full Release build passes. Complete renderer: **250/250** on RTX 3060 and
  required lavapipe, zero skipped. Hardware records **1,718,787 assertions**;
  lavapipe's conditional pixel assertions produce its own count in the log.
- Full lavapipe CTest: **92/94**. The failures remain the inherited three collapse
  claims in `the_block_headless` and the user's edited ±1 GW courtyard. A separate
  canonical courtyard copy passes **2160 ticks**. The user's file retains git blob
  `794da29c6968416bdebd1ee34a6f6f95e190ed8c`.
- Changed translation units pass Clang syntax checks. Precise CPU ASan/UBSan/LSan:
  **16 cases / 8389 assertions**. Vulkan ASan/UBSan: **6 cases / 2208 assertions**,
  with driver leak checking disabled. Dependency archives are not fully
  instrumented; no threading primitive changed and no TSan run was made here.
  An earlier overly broad CPU filter accidentally included GPU cases: all
  assertions passed, but LSan reported 2435 bytes in 17 external driver/dbus
  allocations. Its log is retained; the precise CPU run has leak checking on and
  passes. The supplied verification script uses the corrected filter.
- Pinned clang-format over CI's engine/tests file set and the license check pass.
  Rust sources are unchanged; the prior 171 GUI cases were not rerun for this
  renderer-only change.
- The actual shipped editor and Vulkan host run through Xvfb: Walk capture,
  movement/look, Esc, focus loss, Shift+Esc and Stop, with the source document
  unchanged. Screenshots were visually inspected. This proves the real window
  flow, not a temporal image-quality oracle or Windows/Wayland behavior.
- A fresh [hardware performance attempt](../../perf/shadow-stability-2026-10-19/README.md)
  is retained as rejected evidence. It establishes no comparable FPS baseline.

The complete CTest output was retained before export cooking. The renderer's
lavapipe log was captured separately because cooking replaces CTest's temporary
LastTest log. Reproduction helpers require the Release build/compile database,
the named git revisions, C++/GLSL tools, Vulkan and, for GUI/export, Xvfb/xdotool,
bubblewrap and Pillow. Set `RIME_REPO_ROOT` when running copied helpers elsewhere.

## Standalone build

`/home/next/rime-exports/city-stable-lighting-2026-10-19/the_block`

Manifest SHA-256:
`ba7f0766d2d80e900fcdcf47bf01bd3305dd164aeaca9fd96102ecdcd8557426`.
With repository and home hidden, the copied binary opens a Vulkan window,
loads three weapons and the 662-entity authored city, and runs four frames /
24 ticks with the authored effects. Integrity, destination refusal, missing
content, payload tampering and escaping cooked paths are checked. Old exports
remain intact. This is the Linux workstation development profile, not release
qualification for a clean machine.

## Independent work and remaining scope

Claude Sonnet 5.5 implemented the isolated stable cascade-fit leaf. Native parallel
work supplied the caster tracker and motion/foliage regressions. Root integrated
them, corrected a spotlight reference that accidentally removed light shading,
implemented the shared transitions/history/editor gates, and verified the result.

Astra independently audited the shadow/reflection paths, then reviewed the diff.
Its initial numerical concern required measurement; the fixed-world probe
confirmed it. Astra also found the overlapping-band defect in the intermediate
sampler. Root added a failing-before GPU case and corrected composition. The
consultation reports preserve the distinction between confirmed defects and
unmeasured risks; they are not test results.

Fable did **not** complete a consultation. Live gateway availability/pricing was
checked: Fable and Astra were listed at $10/M input, $50/M output, but the account
had no plan, allowances or balance; the bounded Fable request returned HTTP 402.
The authenticated Claude subscription route then rejected Fable with
`model_requires_usage_credits` and zero usage/cost. Sonnet and Astra ran through
subscription CLIs. Paid API spend for this task is **$0**; the approved $7 ceiling
was not consumed. No credentials or account identifiers are included here.

Sun specular, reflection roughness, reconstruction, jitter and velocity routing
were audited. The SSR tracing algorithm is unchanged; binary hit/miss popping
and previous-depth disocclusion rejection remain limitations. Tests cover the
common world-mesh path, not the existence of every tree/person asset. This work
does not add point-light cube shadows, terrain casting, a full-body first-person
actor or volumetric particle shadows. Inherited collapse/performance and shipment
gates stay open. Reopen the updated editor with `./scripts/city-v2.sh`.
