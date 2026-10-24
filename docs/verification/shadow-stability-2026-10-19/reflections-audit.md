# Reflection / temporal stability audit

Read-only source audit; no builds, pixel tests, edits, subagents or consultations. Findings distinguish source facts from unmeasured image hypotheses.

## Verified wiring and discrepancies

1. **Projection/sign wiring is internally consistent.** `engine/render/src/scene_renderer.cpp:523` builds the unjittered camera; `:538` jitters rasterization, `:546` keeps the previous unjittered matrix, and `:1131` supplies the rasterized projection to SSR reconstruction. `engine/render/shaders/velocity.vert:75` computes current/previous unjittered clips but rasterizes with the jittered matrix; `velocity.frag:25` subtracts current minus previous. `taa_resolve.frag:129` correctly looks backward by half that NDC delta. TAA runs after SSR, before tonemap (`scene_renderer.cpp:1460`), with separate read/write histories (`:1483`). No obvious sign/matrix mismatch explaining ordinary walking was found. `scene_renderer.hpp:296` says “previous - current”; that comment contradicts the actual correct shader sign.

2. **Editor/runtime lighting differs.** Generic `engine/app/editor_host_app.cpp:537` enables clustering only. `engine/render/include/rime/render/lighting/settings.hpp:53`, `:88`, `:112`, `:122`, `:171` leave directional shadows, spot shadows, SDF, DDGI and SSR disabled. Runtime `samples/99-the-block/main.cpp:1748`, `:1757`, `:1767`, `:1768`, `:1777` enables them. Jitter/motion/TAA default on in both (`scene_renderer.hpp:640`, `:651`, `:663`). Thus editor and runtime are not equivalent reflection/shadow observations; this is a verified configuration discrepancy, not proof of the walking flicker's cause.

3. **Masked-geometry derivative contract violation.** `pbr_forward_shadowed.frag:453` discards by sampled alpha before `:462` calls `filter_specular_alpha`. `brdf.glsl:87` explicitly requires uniform control flow for its `dFdx/dFdy` (`:97`). Move derivative-dependent sampling/filter work before divergent discard when addressing masked materials. Source ordering is verified; GPU symptoms were not measured, and opaque city walls do not take this discard path.

4. **History gap handling is incomplete.** With no active camera, `scene_renderer.cpp:497` returns early and invalidates TAA only if a first-person overlay was previously active (`:498`). An ordinary world-only renderer retains history across that gap. TAA itself validates bounds and clamps current color neighborhoods (`taa_resolve.frag:133`), without previous-depth/object-ID rejection (`:136`). These are source-confirmed history/disocclusion limitations; they can cause transient stale history, but do not establish a cause during uninterrupted walking.

## Reflection risks, not proven defects

SSR uses discrete fixed-distance samples and a binary thickness hit (`ssr_resolve.frag:356`, `:375`), then immediately switches hit radiance versus fallback; border fading (`:379`) does not smooth interior hit/miss instability. Thin geometry and grazing rays may therefore shimmer while walking. Confirmation requires a controlled sequence, not merely changing thickness.

SAA uses perturbed-normal variance (`brdf.glsl:93`). Its widened roughness is shared correctly by direct lights, filtered sky and G-buffer (`pbr_forward_shadowed.frag:462`, `:467`, `:619`, `:686`); SSR consumes it (`ssr_resolve.frag:255`, `:308`). Sky roughness levels interpolate continuously (`sky_specular_eval.glsl:46`), so no obvious roughness-level popping bug was found. The analytic solar disc is background-only (`sky.frag:54`); geometry receives direct solar specular through `shade_light` (`pbr_forward_shadowed.frag:619`). Adding the disc to the environment blindly risks double counting.

## Meaningful pixel controls

- Walk a deterministic camera through subpixel translations/yaw over stationary shadow receivers and glossy/normal-mapped materials. Compare each pose to a supersampled reference; track world-anchored shadow-edge position, highlight energy and temporal error, rather than requiring screen pixels to stay constant while the camera moves.
- Sweep a thin reflected occluder through the SSR hit/miss threshold with analytical planar hit coordinates. Require smooth, correct coverage and preserved reflection energy; include a deliberately wrong projection as sensitivity control.
- Test alpha-mask quad boundaries with high-frequency normals across multiple camera subpixel offsets; require finite stable widened roughness and consistent forward/SSR results.
- Render, remove the camera for one frame, then restore/change it: first resumed output should match fresh-history behavior. Compare editor/runtime using the same explicit lighting configuration.

Keep shadow, SSR, jitter and normal-detail features enabled in acceptance sequences; no blanket blur or feature-disable workaround is proposed.
