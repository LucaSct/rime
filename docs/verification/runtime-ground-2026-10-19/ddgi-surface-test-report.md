# Production forward DDGI surface-query regression

Added only new `tests/render/ddgi_surface_query_test.cpp` (254 lines) and its CMake source-list entry, preserving existing shared-root edits. No commits, builds, GPU executions, full tests, subagents or consultations.

The fixture calls production `ForwardPbrPass::add_shadowed`, renders an actual horizontal mesh and uploads hand-packed real-format RGBA16F irradiance/RG32F visibility atlases using production packing constants. There is no copied DDGI sampler shader. The y=0 probe layer is fully blocked; the next layer in the geometric-normal direction has open visibility and irradiance RGB(1,.5,.25). Thus the normal-biased query receives known strong light; the old exact-plane query has only blocked support. The red control gap is 0.875, not a tiny contrast.

Two cases cover floor and downward-normal ceiling, exact 0 and ±.001 surface heights, yaw 0/±.312/0 at a fixed eye/pitch with off-axis projection pinning a fixed world point exactly at pixel (16,16), finite support and legacy mode, covered genuinely black probes, all-probes-rejected visibility, and points beyond the finite bias that must not acquire bright irradiance. Rejected probes use the documented finite fallback; covered black remains black. Direct lights, sky specular and temporal effects are absent from this diagnostic raw pass. Full-feature runtime acceptance remains separate.

Clang syntax using actual optimized test compilation flags passes; the existing doctest/GCC16 ciso646 compatibility suppression is retained. Pinned clang-format 20.1.8 and owned diff checks pass. Exact command/log: `ddgi-surface-clang-command.json` / `ddgi-surface-clang.log` in this report directory.

Root must build and run `rime_render_tests --test-case='ddgi surface query*'` and perform old-shader sensitivity. Expected total: 2 cases, 30 tiny renders. No GPU pass or falsification claim is made before root runs them. These tests pin the chosen stable-side surface-query policy; they do not establish that the reported actual city flicker was caused by Y-plane crossing, particularly when actual ground Y is exactly zero.
