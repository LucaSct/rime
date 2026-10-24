# Authored city environment runtime

Commit: `187e32b`, branch `feat/city-authored-environment`, based exactly on `bf9fa1d`.
Worktree: `/home/next/projects/rime-wt/city-authored-environment`.

Implemented sample-local `AuthoredEnvironment` consuming existing `vfx_scene::Effects` and `fluids_scene::ThermalPreview`. `Demo::step_sim` supplies accepted fixed ticks; repeated render/publication only reconciles descriptions, never advances particles, phase energy, flame/detail time. Invalid double-precision dt is refused before narrowing to float.

Publication merges at most 256 legacy smoke plus 256 authored particles, preserving old `--effects-preview` ownership/toggle when authored VFX is absent. Flames stay bounded at 48, lights at 16, phase rows at 16. The same copied snapshot reaches main and optical scope, including a narrow approved hook for scopes created later in that render. Existing scripted ECS fire lights remain untouched. Removal clears flame/light immediately while smoke retires naturally; reset restarts both consumer clocks.

Thermal state comes from the existing validated preview; invalid descriptions clear rows and refused steps retain committed appearance. `PhaseDetail` allocates only once validated opted-in surface rows exist. The consumer is declared before both renderers; detail textures survive borrowed renderer rows and are retained through reset/removal until safe owner teardown. Null-device CPU assembly supports proofs without allocating GPU maps.

One shutdown diagnostic reports enabled/admitted sources, actual smoke/flames/lights, phase rows/cells, atlas readiness, accepted FX/thermal ticks and invalid/refused counters. Invalid enabled sources remain visible in diagnostics and are never silently clamped.

Nine targeted tests cover inert/disabled behavior, authored emission without scripted preview, repeated/invalid ticks, bounded legacy composition, removal/reset, thermal opt-in/refusal, renderer pass baseline and legacy toggles, main/scope publication, and lazy phase map selection with actual pixel sensitivity and generation clearing.

Measured: new implementation and tests plus modified full `main.cpp` pass GCC and Clang syntax checks using release compile-db flags; owned C++ formatting and `git diff --check` pass. One initial namespace mismatch was caught by full-main syntax and corrected before commit. Adversarial self-review completed; no independent review ran.

Deferred to root: compiled tests, GPU execution, real CLI/copied-city/export validation. No build tree, scene edits, engine edits, windows, push or merge.

Limitation agreed with root: consumes existing `WorldTransform`; current city loader copies flat local poses and does not compose parented-source hierarchies. No broad world/physics pose mutation introduced. Thermal/fire remain cosmetic, without replicated heat, gameplay forces or damage. Component registration/load ownership belongs to the parallel loader brick.
