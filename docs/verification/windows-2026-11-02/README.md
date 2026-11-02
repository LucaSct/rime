# Windows session, 2026-11-02 — a real GPU box, 401 commits on

Branch `fix/windows-editor-transport-and-pipeline-click-test`, parent `a70e9db4` (#351).
Two commits: `4d0915c5` (the editor's Windows transport + a whole-pipeline click test) and
`9d585c0b` (right-click menus, clipboard, spawn archetypes).

This is a findings checkpoint written so the work can be resumed on Linux. **It is not a green
report**: one test fails, and several findings are diagnosed but deliberately not fixed.

## The machine

| | |
|---|---|
| OS / shell | Windows 11 Pro 26200. **Windows PowerShell 5.1 only** — `pwsh` is not installed |
| Compiler | MSVC 14.44.35207 (VS 17.14), so `compiler.version=194` |
| Tools | Portable CMake 3.31.6 / Ninja 1.12.1 / Python 3.12.8 in `C:\Users\gaming\toolchain` (`env.ps1`) |
| GPU | NVIDIA RTX 3060, driver 591.86, Vulkan 1.4.325 |
| Second ICD | AMD Radeon iGPU, Vulkan 1.4.315. The device picker correctly prefers the 3060 (type score 1000 vs 500) — worth knowing that "first device" and "best device" now differ here |
| Not present | Vulkan validation layers (no LunarG SDK). Every GPU run logs `validation requested but layer/debug-utils unavailable — disabling`, so **nothing on this box was validated** |

## Suite state, end of session

| suite | result |
|---|---|
| `ctest --test-dir build/dev` (`RIME_REQUIRE_VULKAN=1`) | **98 / 99**, 477 s — [ctest-dev-full.log](ctest-dev-full.log). The one failure is #1 below |
| `cargo test --workspace --exclude rime-auth` | **272 passed, 0 failed, 0 ignored** |
| `cargo test -p editor --features gui` | **173 passed**, 1 ignored (the real-engine pipeline test, run separately and passing) |
| `cargo clippy -p editor --features gui --all-targets` / `cargo fmt --check` | clean |

The three `hello_game` Python tests pass here only because of a local workaround — see #3; on a
clean Windows box the count would be 95/99.

## What is green

* **C++ builds clean in both presets** from a fully clean tree: 892 ninja targets, 73 executables,
  **0 warnings and 0 errors** under `/W4 /WX`. The Windows build path fixed in #167 has survived 401
  commits, including the `CreateProcessW` / `SIO_UDP_CONNRESET` work that landed days ago.
* **Rust workspace: 272 passed, 0 failed, 0 ignored.** Including the four `rime-ffi` live tests,
  which ran against the real `rime_capi.dll` rather than compiling themselves out.
* **`editor --smoke` runs on Windows for the first time** — it was a `#[cfg(not(unix))]` stub that
  printed "Unix-only for now" and exited 2:
  `33 schema types, 4 entities, 2 assets browsed; typed field edit 0 -> 1 applied and confirmed via
  snapshot; pick answered honestly (no viewport => miss); clean shutdown`
* **`editor --features gui` builds and runs on Windows for the first time**, with the streamed
  viewport live on the 3060. ADR-0031's "Mac-eyeballed" is no longer "Mac-only".
* **Editor tests**: 173 with `--features gui`, 15 without (the light build CI uses), plus the new
  real-engine pipeline test. `clippy --all-targets` and `fmt --check` clean.
* **`the_block --environment-smoke`** exits 0 on the GPU: 8 water surfaces, **711/1099 water
  reflections**, 226366 cloud pixels, 26 trees / 3466 grass / 7 puddles / 1 pond, 12244 px of firing
  feedback, terrain-collision gap max 0.000 m.

## What is red or open

### 1. `hello_game_net_proof` FAILS — and it is a Windows timer defect, not flake

`[FAIL] applied 85/240`, then `52/240` on a re-run, against a `measured_applied >= 120` bar
(`samples/hello-game/net_proof.cpp:621`). Everything else in the proof passes: child spawn,
`client_connected`, 240 valid tick lines, and a replay digest equal to the server's.

Root cause, measured three times on an idle box: **`std::this_thread::sleep_for(1ms)` actually costs
~16 ms here** (15.94 / 16.03 / 16.12 ms) — one full Windows timer tick, the default ~15.6 ms
resolution. The loop at `net_proof.cpp:568-580` sends at most **one** command per iteration against
an accumulating 16.667 ms deadline:

```cpp
if (now() >= next && welcomed) { ...send...; next += 16667us; }  // one send per iteration
std::this_thread::sleep_for(1ms);                                 // ~16 ms here
```

So the poll is coarser than the send period and every slip becomes debt the loop can never repay. On
Linux the 1 ms poll is ~16× finer than the period, every deadline is hit, ~240 land, and the bar has
enormous margin.

**Recommended fix — `while` instead of `if`**, so a late wake drains its backlog. That removes the
timing assumption rather than tuning the threshold, and is correct on both platforms.

### 2. The job system parks for ~16 ms — a Release-surviving Windows defect

This is the most valuable finding, and it **shares a root cause with #1**.
`engine/core/src/jobs/job_system.cpp:325-336` backs off with 64 × `yield()`, then
`sleep_for(100µs)`. Measured here:

| | measured |
|---|---|
| `sleep_for(100µs)` | **15.87 ms** (159× the request) |
| `64 × yield()` | **0.027 ms** |

A worker burns its entire spin budget in 27 microseconds and then parks for ~16 ms — idle ~99.8% of
any wait window. **The engine never calls `timeBeginPeriod`** (zero hits repo-wide).

Against the committed Linux baseline on the *same* GPU, preset, resolution and frame count
(`docs/perf/2026-10-11-99-the-block-nvidia-geforce-rtx-3060-bdd3be0.json`):

| zone p50 / p99 ms | Linux (committed) | Windows (measured) |
|---|---|---|
| `sim.client` | 5.044 / 8.224 | 3.703 / 8.120 ← *faster* |
| `sim.server` | 5.409 / 7.772 | 3.671 / 7.846 ← *faster* |
| **`sim.join`** | **0.054 / 1.296** | **0.978 / 6.457** ← 18× / 5.0× worse |
| `sim.block` | 5.682 / 8.533 | 6.271 / **14.558** |

Both halves are *faster* on Windows, yet the tick is 1.71× slower, and at the tail
`sim.client` p99 + `sim.join` p99 = 14.58 ≈ `sim.block` p99 14.558 — the two halves are **fully
serialized**. The p2-perf / ADR-0077 optimization that makes `sim.block` a `max()` rather than a
`sum()` is largely defeated on Windows. `job_system.cpp:331` already calls condition-variable
parking "the better long-term answer"; this is the number that justifies it.

Two secondary sites, same cause, **mechanism-only, not run**: `engine/app/src/launch.cpp:687`
(Dedicated paces with `sleep_for`, so a dedicated server would tick at roughly half rate) and
`engine/app/editor_host_app.cpp:1402` (33 ms `sleep_until` overshoots to ~46.9 ms, so the editor
preview runs ~21 fps instead of 30).

### 3. Three `hello_game` tests depend on a bare `python3` on PATH

`hello_game_listen_ready`, `hello_game_listen_bind_conflict`, `hello_game_play_connect` invoke the
command `python3` (`samples/hello-game/CMakeLists.txt:66,79,99`), resolved at run time. This box's
portable Python ships only `python.exe`, so `python3` fell through to the **Microsoft Store alias
stub**, which prints "Python was not found" and **exits 49** — so it reads as a test failure, not a
missing tool. CI only works because `actions/setup-python` provides a real `python3`.

`samples/99-the-block/CMakeLists.txt:305` already does it correctly, via
`find_package(Python3 COMPONENTS Interpreter)` + `${Python3_EXECUTABLE}`. **Recommended fix: make
hello-game match.** Worked around locally with a `python3.exe` copy in the toolchain; all three then
pass, so this is the only thing standing between a clean Windows dev box and a green run.

### 4. The ball goes through the box in `hello-game`

`integrate_player` (`samples/hello-game/game.cpp:130-149`) is pure arithmetic — normalize,
`position += dir * speed * dt`, clamp to the arena, pin Y. **There is no collision query anywhere in
the player's movement path.** The player is kinematic, so `PhysicsSync::push_in` drives the solver
and the crate *is* pushed, but a solver cannot push back on a kinematic body. The crate moves; the
player is never blocked, so it walks through it.

Not a physics-core bug — sphere/box narrowphase and determinism all pass. It is a missing
depenetration step in the sample's character controller. Visible because the player is drawn as a
sphere (`game.cpp:453`, "because `make_capsule` does not exist") under a third-person chase camera.

### 5. Debug vs Release, measured — and the catch-up clamp

Same scene, same 600 frames, `the_block --play` at 1280x720:

| build | frames | ticks | ticks/frame | wall | fps |
|---|---|---|---|---|---|
| `build/dev` (Debug) | 600 | 4792 | **7.99** | 272.4 s | **2.2** |
| `build/release` | 600 | 581 | **0.97** | 10.3 s | **58.3** |

**26×.** The clamp (`max_ticks_per_frame = 8`,
`engine/app/include/rime/app/fixed_timestep.hpp:23`) is **correct and has no death spiral** —
backlog is discarded via `fmod` (`:74`) exactly per ADR-0023 §2. It was simply *saturated on every
frame* in Debug. A user-reported 25-minute session read `4096 frames, 32760 ticks`, and
32760 = 4095 × 8 exactly.

Two things worth acting on anyway:

* **`step.clamped` is computed and never read anywhere** in the engine or the samples. Nothing logs
  or counts it. Had it been on the exit line this diagnosis would have been a one-liner. At 8 ticks
  the loop pays ~6× the frame time for ~5% more sim rate versus 1 tick, so a sustained-saturation
  back-off is worth considering too.
* **Input is sampled once per frame** (`application.cpp:406-408`) and replayed across all 8 ticks —
  one stale sample driving 133 ms of simulated time in a burst. That explains the *feel* better than
  the frame rate alone.

Quick triage, no profiler needed: `--play --frames N` prints `N frames, M ticks`.
**`M/N == 8.0` means the clamp is saturated; `M/N ≈ 1` is healthy.**

### 6. `/Ob0` defeats the one solver optimization the project landed

`core::rotate` is `RIME_FORCE_INLINE` (`engine/core/include/rime/core/math/quat.hpp:100`), and that
header records the **−19.6% on `physics.solve`** that justified it. Compiled with this exact MSVC
and read back: under `/Od /Ob0 /RTC1` it **emits a real `call`**; under `/O2 /Ob2` it does not. So
the dev build runs the un-optimized version of the hot path.

Paired `--perf` runs with bit-identical sims (`debris.peak_live` 381, `parts.alive_end` 1630 in
both) give `physics.client.solve` p99 **48× worse** in Debug against only **5.6×** for the
narrowphase — precisely the distortion `docs/ROADMAP.md:2722-2725` warns about, now quantified.
Corollary: **do not profile the Debug build**, it will rank the solver far above the narrowphase.
Two intuitions corrected: `_ITERATOR_DEBUG_LEVEL` is *not* a factor here (release CRT made no
difference), and adding `/Ob1` to the dev preset recovers only ~10%, so it is not worth a band-aid.

### 7. Settled debris already retire — the demo just raises the caps

`engine/destruction/include/rime/destruction/world.hpp` already does what "release the bricks on the
ground" asks: `freeze_delay_ticks = 120` (settled ~2 s → **physics body destroyed**, record kept),
`max_live_debris = 128` (past it, settled debris are frozen), `max_visual_debris = 512` (past it,
the **oldest frozen debris are retired**).

`the_block` raises both to **512 / 1024** (`samples/99-the-block/main.cpp:623-624`), 4× and 2× the
defaults, and the profile measured `debris.peak_live` **381** — under the cap, so it never bit and
~381 bodies stayed live. Lowering them is the lever.

**Caveat before anyone tries it:** that is a non-bit-identical change to a chaotic fracture cascade,
and per the root `CLAUDE.md` the 29 `--headless` claims fail innocently in *both* directions for
such a change. It needs the fixed-workload bench, not the sample's claims.

### 8. `build/release` has no cooked content until the cook fixtures run

The asset cooking is a **ctest fixture** (`block_demo_cook`), so `scripts/build.ps1 -NoTests` builds
a release tree whose `the_block --play` dies instantly on
`missing cook 'wall_bg' under build/release/samples/99-the-block/cooked`. Fix:
`ctest --test-dir build/release -R '^block_demo_cook'` (10/10 pass, ~12 s), or pass
`--cooked build/dev/samples/99-the-block/cooked`.

### 9. Not verified here: Linux and macOS

Both commits are Rust-only and touch `tools/editor`. The transport change is either Windows-gated or
a type alias that resolves to today's `std` path on Unix, and nothing in the context-menu work is
platform-specific — but **only CI on those two runners proves it**. `uds_windows` enters as a
`[target.'cfg(windows)'.dependencies]` entry and was *already* in `tools/Cargo.lock` (zbus depends
on it), so no new crate joins the graph on any platform.

## Suggested order when resuming

1. **The job-system backoff (#2).** Real engine work, survives the Release fix, and shares a root
   cause with the one failing test. Measured payoff: `sim.block` p99 14.56 → ~8.5 ms, recovering the
   ADR-0077 overlap. Park on a condition variable, and/or `timeBeginPeriod(1)` while a loop is live.
2. **`net_proof`'s `if` → `while` (#1).** Small, removes a timing assumption, turns the suite green.
3. **hello-game's `python3` (#3).** One-line-ish, and it is what a fresh Windows dev box trips over.
4. **Open a PR for this branch** so the three-OS matrix settles #9.
5. `step.clamped` observability (#5) and the ball/box depenetration (#4) whenever they come up.
