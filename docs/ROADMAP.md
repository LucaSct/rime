# Rime Roadmap

The big-picture map from "empty repo" to the **vision demo**: a destructible urban
block with dynamic lighting, networked, at a playable frame rate (see [VISION.md](../VISION.md)).

**How we use this roadmap.** These are *milestones* — the map, not the turn-by-turn
directions. Each milestone is decomposed into small **bricks**, and every brick is
planned again before it's built. A milestone is **"done" only when its proof runs** (a
`samples/` demo and/or CI gate) — never when it merely compiles. We re-plan at each
milestone boundary; time estimates come at brick-decomposition, not here.

> **Status (2026-06-17):** Repo scaffolded. **Milestone 0 in progress.**
> **Brick M0.1 (C++ build foundations) — done:** `conan install` → `cmake --preset dev`
> → `ninja` builds `rime::core` (static lib) and `rime_hello`, which runs and prints the
> engine version. fmt is resolved via Conan; the build is warnings-as-errors clean; and
> `compile_commands.json` is exported for clangd.
> **Brick M0.2 (C++ test harness) — done:** doctest wired via Conan (a test-only
> dependency) behind `RIME_BUILD_TESTS`, with a `tests/` tree (one target per module) and
> a `rime_add_test` helper. First test exercises `rime::core`; `ctest --preset dev` is
> green.
> **Brick M0.3 (Rust workspace) — done:** Cargo workspace under `tools/` (`resolver = "2"`,
> shared `[workspace.package]`), pinned via `rust-toolchain.toml` (stable + rustfmt/clippy),
> with the `rime-cli` crate (binary `rime`) as a stub. `cargo fmt --check`, `cargo clippy
> -D warnings`, `cargo build`, and `cargo test` all pass.
> **Brick M0.4 (dev scripts) — done:** `scripts/setup` + `scripts/build` (POSIX `.sh` +
> Windows `.ps1`) give one-command setup and build of both halves. `build.sh --clean`
> rebuilds C++ (conan→cmake→ctest) and Rust (cargo build+test) from scratch on macOS;
> the `.ps1` mirrors are exercised by CI on Windows. **Remaining M0 brick:** M0.5 CI
> matrix (Win/Linux/macOS) with format/lint/license gates.

## Ordering principles (why this sequence)

1. **Bottom-up the layer cake** ([ARCHITECTURE.md](ARCHITECTURE.md)): destruction needs
   physics; physics needs core/jobs/math; rendering needs the RHI. Earn each layer
   before standing on it.
2. **Seams before features.** The render graph exists *before* Lumen-class GI; the
   part/physics model *before* spectacular destruction. The hard-to-retrofit seams
   (RHI, ECS, destruction event model) go in early — that's the whole bet.
3. **Every milestone ends in a runnable proof** — a `samples/` demo and/or a CI gate.
4. **Power > portability at the edges.** All three OSes are CI-gated from M0; if a
   portability cost ever threatens engine quality, we narrow platforms (VISION #2).
5. **Math is derived, not hand-waved.** Math-heavy milestones (M1, M5, M7, M10) ship a
   short derivation note alongside the code.

## Cross-cutting tracks (continuous, not milestones)

- **CI/CD:** build + test on Windows/Linux/macOS from M0; format/lint/license-header
  gates; warnings-as-errors.
- **Testing & profiling:** unit tests per module; a profiling/timing hook in `core`
  early, so "measure before optimize" is real.
- **Docs:** keep ARCHITECTURE, glossary, and ADRs current as we build.
- **Audio & animation:** feature tracks that slot in — audio *stub* at M8 (destruction
  event fan-out), real audio ~M8–M9; skeletal animation ~M6–M7.

---

## Milestones

| # | Milestone | Done when (the proof) |
| --- | --- | --- |
| **M0** | Build bootstrap & skeleton | CI green on Win/Linux/macOS; `hello` runs; a trivial test passes |
| **M1** | Core foundation | test battery green; a sample saturates all cores via the job system; reflection describes & serializes a struct; a module loads at runtime |
| **M2** | Platform & window | a window opens and handles keyboard/mouse on all three OSes |
| **M3** | RHI + Vulkan backend | a textured quad renders through the RHI (Win/Linux + macOS/MoltenVK) |
| **M4** | ECS / the world | 100k+ entities update in parallel; transforms compose correctly |
| **M5** | Render graph + PBR | a lit PBR scene draws via the render graph; adding a pass is easy |
| **M6** | Asset pipeline + runtime assets | import → cook → load → render a real glTF model with textures |
| **M7** | Physics (rigid bodies) | objects fall/collide/stack; raycasts hit; runs parallel to the frame |
| **M8** | **Destruction v1** | a wall fractures on impact, debris falls/settles, one event drives a VFX+sound stub |
| **M9** | Editor v1 (Rust) | build a small scene in the editor, tweak components, hit Play |
| **M10** | Advanced lighting | dynamic GI updates as the scene changes — *including when walls fall* |
| **M11** | Networking + networked destruction | two clients see synchronized destruction at meaningful scale |
| **M12** | **"The Block" (vision demo)** | a destructible urban block (M8+M10+M11) runs at a playable frame rate and *feels* right |

### Detail

**M0 — Build bootstrap & skeleton.** One command builds the C++ engine and the Rust
tools on all three OSes. CMake presets + a trivial `engine/core` lib and a `hello` exe;
C++ test harness; Cargo workspace under `tools/`; `scripts/setup`+`scripts/build`; a CI
matrix with format/lint/license gates. *Inspired by: modern C++/Rust project hygiene.*

**M1 — Core foundation.** The bedrock: allocators (arena/pool/stack, tracked); SIMD math
(+ derivation notes); cache-friendly containers (slot map, handle table); a
**work-stealing job system**; logging/asserts + profiling hooks; minimal **reflection**;
the **module loader**. *Inspired by: O3DE modules; Bevy/DOD.*

**M2 — Platform & window.** `engine/platform` — window, input, filesystem, timers,
threads for Win32/Linux/macOS. No OS `#ifdef`s leak upward. Sample `00-hello-window`.

**M3 — RHI + Vulkan backend (first pixels).** `engine/rhi` interfaces (device, swapchain,
command buffers, pipelines, descriptors, sync) + the **Vulkan backend** (only place that
includes Vulkan headers); GLSL/HLSL→SPIR-V; VMA. Samples `01-hello-triangle` → textured
quad. *(ADR-0002.)*

**M4 — ECS / the world.** `engine/ecs` — entities, components, archetype storage,
parallel systems on the job system, queries, a transform hierarchy. Sample
`02-ecs-playground`.

**M5 — Render graph + PBR (first light).** `engine/render` — **render graph** (passes,
transient resources, auto-barriers), mesh/material/camera, **PBR** (+ derivation), depth
pre-pass, one dynamic light. Samples `03-render-graph`, `04-first-light`. The home for
M10. *Inspired by: UE5 render-graph discipline.*

**M6 — Asset pipeline + runtime assets.** `tools/asset-pipeline` (Rust) imports glTF +
textures → cooked formats; `engine/assets` loads/streams at runtime; `tools/rime-cli`
cooks; the stable C-ABI/file **FFI boundary** stands up. *(ADR-0001.)* Skeletal-animation
import begins.

**M7 — Physics (rigid bodies, multicore).** `engine/physics` behind an interface —
bodies, collision, queries — stepped on the job system. Evaluate **integrating Jolt** vs.
own core (its own ADR). *Inspired by: Jolt.*

**M8 — Destruction v1 (the headline begins).** `engine/destruction` — part-based
destructibles + connectivity, precomputed fracture, debris as real physics bodies,
**health-transition hooks**, and a **one-event → physics/VFX/audio fan-out**. Sample
`10-destructible-wall`. *Inspired by: Frostbite (Battlefield 6) — see
[engine-survey.md](research/engine-survey.md).*

**M9 — Editor v1 (Rust).** `tools/editor` — scene/world editing, reflection-driven
inspectors, embedded live viewport, play-in-editor. *Inspired by: Unity/UE iteration.*

**M10 — Advanced lighting (the Unreal-class push).** Each its own sub-effort + ADR:
dynamic GI + reflections (Lumen-style), virtual shadow maps, many-lights
(MegaLights-style), virtualized geometry (Nanite-style). *Inspired by: UE5.*

**M11 — Networking & networked destruction.** `engine/net` — client-server, replication,
and **prioritization + culling** of part-destruction/debris; determinism where required.
*Inspired by: Frostbite's networked destruction at 64 players.*

**M12 — The vision demo: "The Block."** Sample `99-the-block` — destruction + dynamic
lighting + scale, together, at a playable frame rate. The thesis, demonstrated.

---

## Rough shape (not commitments)

Foundations **M0–M5** are the long, unglamorous climb everything depends on — they pay
back forever. **M6–M9** make it usable and show the first destruction. **M10–M12** are
the "wow." We re-plan at each boundary.

> The frost does not form all at once. Crystal by crystal. ❄
