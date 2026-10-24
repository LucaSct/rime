# Shadow motion GPU regression leaf

Branch `test/shadow-motion-proof`, isolated worktree `/home/next/projects/rime-wt/shadow-motion-proof`, base `38f44c3`. Only `tests/render/shadow_motion_test.cpp` changed (240 lines); root owns CMake and integration.

Two public-API pixel tests:

1. A stationary downward spotlight illuminates a floor beneath three rigid body pieces. First frame must render the map; an identical warm frame must reuse it. Moving only the camera brings a deliberately earlier offscreen draw into view: the cull count must change, proving actual draw-order perturbation, while the local-shadow map must still reuse. Moving the three pieces then requires automatic redraw, the old world shadow patch relighting and the new patch darkening, without manual invalidation. Removing them must redraw and relight the new patch. A shadows-disabled frame restores the original pieces and provides independent lit radiance at both world anchors. The pieces prove shared rigid scene geometry, not skeletal animation or actual person assets.

2. A flipped, double-sided horizontal foliage card uses a real uploaded alpha texture (opaque left half, transparent right half) over a sunlit floor. Opaque leaf blocks sunlight, texture holes transmit it, repeat frame preserves the shadow, moving the card relights its old leaf patch and darkens a new patch while its new hole remains lit. Removing it and disabling shadows with the card restored are separate controls. Card/view geometry keeps receiver probes out of primary-view card coverage. This proves generic alpha-card depth handling, not actual tree asset presence.

Both isolate shadow radiance with temporal jitter/motion/TAA disabled; no claim about final temporal reconstruction acceptance. Readbacks use world-to-pixel reprojection and 3×3 patches well inside broad regions, ratio thresholds with margins, no golden images. The real existing masked-depth counterpart was `draw_path_test.cpp` (m16.4); there is no separate alpha_mask_test.cpp here.

Verification: owned file formatted with pinned clang-format; Clang23 syntax-only exit0 using actual release local-shadow compile flags; git diff --check clean. No build/configure/GPU runs/shader copies performed as instructed. Root must execute old/new Vulkan sensitivity and integrated suites. In particular the alpha test intentionally requires masking in SHADOW depth, so an opaque-only shadow pass must fail; this is not grounds to weaken the hole-control assertion.

Free space at handoff: 15.632 GiB.
