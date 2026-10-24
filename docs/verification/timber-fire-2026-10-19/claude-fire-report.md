# Fire dynamics polish report

Commit: see `git log -1` on branch feat/fire-dynamics-polish (base 9110168). Owned files only: engine/vfx_scene/src/effects.cpp, tests/vfx_scene/effects_test.cpp.

## Change
Still 3 FlamePresentation rows per source (max 48), no header/ABI/ECS change, smoke and light adapter untouched.
Each tongue now sums two sinusoids at different INTEGER harmonics of the 8 s loop (surge: 4/9, 7/13, 6/11; sway: 3/8, 5/10, 4/9), seeded per source by two hashes of entity identity. Integer harmonics make value and slope join at the wrap. Tongue 0 is the anchored, bright root (+-10% size, small lift/lean, opacity 0.85-1.0x of base); side tongues are faster, +-26% size, quadratic lift (dart up, linger low), and opacity/heat flare and retire. Colour stays within {2.8, <=2.2, <=1.4}; opacity clamped [0,1]. Teaching comments added.

## Measured
- `clang-format` (pinned) run on both files.
- `clang++`/release-flag `-fsyntax-only` of both TUs using compile_commands from /home/next/projects/rime/build/release with paths redirected to this worktree: exit 0 for both.
- A Python emulation of the formulas (4 entity indices, 160 ticks of 0.05 s) showed the new test's thresholds hold with margin: worst per-tick step 0.5-0.84 of the limits; size span root 18% / side 41%; opacity span root 0.097 / side 0.245.

## Not measured / limitations
- The new test ("flame tongues surge independently, smoothly and loop seamlessly") and the existing suite were NOT compiled or run: no new build tree (disk 18 GiB free, reserve rule). Root must run the vfx_scene tests.
- Visual look not captured; amplitudes are my judgment of "more dynamic, same warm style" and may need tuning.
- Test thresholds were validated against an emulation, not the real binary. The emulation assumes size scale 1 (thresholds are relative to size).
- Existing assertions retained unchanged; the x-differs-after-tick-2 check relies on tongue 0 sway moving between 0.25 s and 0.5 s (it does in emulation).
- Nothing contradicted the spec.
