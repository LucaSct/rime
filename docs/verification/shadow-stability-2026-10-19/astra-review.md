One concrete shader issue remains; I found no additional confirmed tracker, ownership, cache, or projection regression in the permitted reads.

- **P2 — [sun_shadow.glsl:54–58](/home/next/projects/rime/engine/render/shaders/sun_shadow.glsl:54): overlapping cascade transition bands can still produce a discontinuity.** The fine cascade blends toward the coarse cascade’s **raw PCF value**. Immediately outside fine coverage, that coarse cascade becomes the selected cascade and applies its own boundary fade. Those results differ whenever the coarse cascade is also inside its transition band.

  Concrete example: coarse visibility is `0`, its boundary weight is `0.5`, and its fallback is lit (`1`). Approaching the fine boundary, fine weight tends to zero, so the returned visibility tends to `0`. Crossing the boundary selects coarse and returns `0.5`. Separate slice fits do not guarantee that transition bands cannot overlap.

  **Fix:** compose each cascade against the already-composed result of subsequent cascades, including their boundary weights. A coarse-to-fine accumulation starting from visibility `1` provides this continuity; preserve an interior fast path if needed.

  This is a conditional mathematical defect, **not evidence that it occurs along the measured city walk**. The reported x/z probe bounds do not establish coverage of overlapping transition bands.

Still needs measurement: extend the production-function GPU probe to cross a fine boundary while coarse is also fading, and to cross coarse coverage while fine remains in its blend band. Use depths that distinguish coarse visibility from its fallback. Check whether those configurations occur with supported camera/light settings, then measure residual walking shadow/reflection flicker in the actual scene. If vertex-override streams are present, measure the cost of their deliberate every-frame global local-shadow invalidation.

The reported phase and scale measurements substantiate the fixed-basis CSM improvement. I performed only the requested reads—no edits, builds, tests, or subagents.
