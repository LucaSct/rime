# Smoke / flame presentation V2 brick

Commit: `d6b057f` on `feat/vfx-shading-v2`, based exactly on `b5bf53b`.
Worktree: `/home/next/projects/rime-wt/vfx-shading-v2`.

Implemented bounded `FxBlendMode::Flame` with distinct `fx-flame` pipeline/pass and instance-buffer name. It shares the unchanged 32-byte particle / 96-byte push ABI; mode 2 travels in `camera_right.w`. Camera-facing flame spans halfwidth `0.55*size`, halfheight `1.5*size`. Smooth procedural turbulence bends a tapered tongue; additive HDR core varies from cream near the root to orange at tips/edges. Caller RGB multiplies that heat tint. No shader clock or per-frame random seed exists.

Smoke now combines three smooth deterministic noise scales for nonradial density and a softened boundary, retaining straight alpha, caller lighting, stable back-to-front sorting and black occlusion at intensity zero. Original additive dust/muzzle falloff and RGB equation remain unchanged.

Flame inherits smoke's invalid-particle, basis, finite-intensity, cap counters and ring admission, without unnecessary alpha sorting. Depth remains tested without writes. At root's review request, shared depth attachment now uses Store: read-only alone does not stop DontCare from discarding depth needed by later translucent passes.

Five structural GPU tests were added: flame off/empty/null/depth/intensity/ring controls; tall tapered shape versus an additive-disc sensitivity control plus no-write layer accumulation; invalid/cap admission and pass identity; deterministic nonradial smoke and black occlusion; flame followed by smoke preserving occluder depth. Existing assertions were retained.

Measured verification: both shaders compile with glslangValidator and pass spirv-val; owned C++ and test TU pass GCC and Clang syntax checks against release compile-db flags; clang-format and git diff --check pass. Adversarial self-review completed. No separate agent review ran.

Deferred to root integration: actual GPU execution, visual inspection, full builds/tests. No build tree created, no scenes changed, no push/merge/windows modified. Limitation: no soft scene-depth sampling or geometry-intersection fading; billboards still have depth-tested intersection boundaries. Spec otherwise matched.
