# shadow_max_distance report

Commit: see `git log -1` on fix/runtime-shadow-distance (base 727c851). Owned files only:
settings.hpp, shadows.cpp, shadow_stability_test.cpp.

## Change
- `LightingSettings::shadow_max_distance = 0.0f` (world metres; teaching comment on why shadow range differs from terrain view range).
- `compute_cascades`: one `far_d = min(in.z_far, cap)` iff `isfinite(cap) && cap > in.z_near`, else `in.z_far`. Used for practical splits (and therefore the sphere fit). CascadeInputs, projection, shader ABI, resolution, bias, PCF, cascade count untouched.

## Tests added (existing ones unchanged, no tolerance loosened)
1. Unset/invalid caps (0, -5, NaN, inf, ==near, <near) and caps >= camera far: memcmp-identical fit to the default, counts 1/3/4.
2. Cap 128 with camera far 3072 and 4096 is memcmp-identical to an independent camera far 128 uncapped fit; non-vacuity: uncapped 3072 differs from it and from uncapped 4096.
3. Capped counts 1/3/4: independent split restatement with far=cap ends at 128 (Approx), first split = near; slice corners over 16 poses lie inside every cascade's NDC (+2e-3 guard) with z in [0,1].

## Verified
- clang++ (root release compile flags, paths redirected to this worktree) compiled both TUs without errors.
- Small external CPU proof: those two objects + doctest main + root librime_core.a, linked outside the build tree: 10/10 test cases, 10294/10294 assertions pass (includes all pre-existing stability tests).
- clang-format run on the three files.

## Not run
- No ctest / full build / GPU / lavapipe / sanitizers, no headless claims. No runtime check that any sample's behaviour changed (default is unchanged by construction). Root must set `shadow_max_distance` for the runtime city and check coverage.
- Does NOT claim to fix the report that floor shadows change while turning/walking; this is a texel-density/coverage fix only (cascade 0 at far 3072 ~ r551 m, ~0.54 m texel, ~2.5 m normalized bias).
