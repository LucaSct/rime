# CSM stable-fit report (worktree csm-stable-fit, base 38f44c3)

## Verdict on root's baseline
Independently confirmed in kind, not in exact number. The old fit unprojects 8 corners via
inverse(proj*view) and takes a sphere radius from them, so the radius (hence texel size) carries
round-off that varies per pose; it also rebuilds look_at(eye,snapped) from translated floats and
snaps with floor(). My test harness (different camera path/point, 300 poses) fails the old object on
every phase and scale CHECK (old.exe: all light/copy/res combos red), pass on the new one. I did NOT
reproduce root's exact 0.425 texel / 3.25e-4 figures (no externalprobe run). Nothing in the spec contradicted.

## Change (shadows.cpp compute_cascades only)
- Radius analytic in camera space: hypot(d1*tan(fovy/2)*aspect, d1*tan(fovy/2), (d1-d0)/2) per slice;
  centre = inverse(camera_view) * (0,0,-(d0+d1)/2). Only the centre uses the world inverse.
- Fixed rotation-only light basis; view = that rotation + explicit translation
  (-cs.x, -cs.y, -(cs.z+2r)). No look_at from translated floats.
- Nearest-texel snap (round) in light XY with guard r = r0*N/(N-1) (half-texel shift is r0/(N-1)).
  N<=1 (incl. 0): no snap, no guard, no divide by zero.
- Splits, cascade count, z extents (near 0, far 3r), ABI unchanged. Header untouched.
- Note: floor->round changes snapped positions vs old; the unmodified existing nudge test
  (approx_eq 1e-3 on a 0.0002 nudge) could flip only if the centre sits within ~0.0002 of a texel
  rounding boundary; not run, see below.

## Tests (tests/render/shadow_stability_test.cpp, new; added to CMake list)
Fixed-world-point sub-texel phase (circular, minus an explicit float-storage budget) <= 0.002 texel
over 300 walking/turning poses; maps 1024/2048; copies 0, (100,-100), (-1000,1000); 3 suns (oblique,
near-vertical, raking). XY linear-part relative variation < 2e-6 at counts 1/3/4. Slice corners
(restated independently, camera->world) stay within guarded NDC and z in [0,1]. Ordering/finite for
counts 1/3/4 and res 0,1,2,4,1024. Three sensitivity controls (unsnapped slide, radius jitter 1e-3,
basis rotation 1e-3 rad) must make the oracle fail.

## Run / not run
- RUN: -Werror GCC compile of test + shadows.cpp; clang++ -fsyntax-only both TUs (clean);
  clang-format; scratch link (build.sh in csm-scratch/, against build/release libs, new shadows.o
  first): new.exe 7/7 cases, 8207 assertions pass. old.exe (release archive's old shadows) fails the
  stability cases.
- NOT RUN: real rime_render_tests/ctest, existing shadow_test.cpp (incl. nudge case), GPU, sanitizers,
  app/shader integration. Margin of the passing numbers not printed.
