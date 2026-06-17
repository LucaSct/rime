# Rime Roadmap

The big-picture map from "empty repo" to the **vision demo**: a destructible urban
block with dynamic lighting, networked, at a playable frame rate (see [VISION.md](../VISION.md)).

**How we use this roadmap.** These are *milestones* — the map, not the turn-by-turn
directions. Each milestone is decomposed into small **bricks**, and every brick is
planned again before it's built. A milestone is **"done" only when its proof runs** (a
`samples/` demo and/or CI gate) — never when it merely compiles. We re-plan at each
milestone boundary; time estimates come at brick-decomposition, not here.

> **Status (2026-06-17):** **Milestones 0 (build bootstrap) and 1 (core foundation) —
> COMPLETE.** The repo is public at https://github.com/LucaSct/rime with **CI green on
> Windows, Linux, and macOS**. `scripts/build` builds and tests the C++ engine and the Rust
> tooling with warnings-as-errors; format / lint / license-header gates are enforced in CI.
>
> **M1 (core foundation) is done — all four "done when" proofs pass:** the unit-test battery
> is green; `samples/jobs_core_saturation` saturates the cores through the work-stealing job
> system; reflection describes and round-trips a struct through bytes; and a module loads at
> runtime. Bricks M1.1–M1.8 landed as focused commits (see `git log`): diagnostics
> (log/assert/timing) · allocators (arena/stack/pool, tracked) · math I (vectors & matrices)
> · math II (quaternions & transforms) · containers (generational slot map) · the lock-free
> work-stealing deque (M1.6a) and the job system + core-saturation sample (M1.6b) · minimal
> reflection · the runtime module loader. Math bricks ship derivation notes (`docs/math/`),
> systems bricks ship design notes (`docs/design/`), and decisions live in ADRs 0004–0005.
>
> **Next: Milestone 2 (Platform & window)** — `engine/platform`: window, input, filesystem,
> timers, threads, behind a seam with no OS `#ifdef`s leaking upward. **Decomposed into
> bricks M2.1–M2.5** (see the M2 detail below); first up is **M2.1 — the platform module &
> seam**.

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

*Bricks (planned 2026-06-17, bottom-up):* **M1.1** diagnostics (log/assert/timing) ·
**M1.2** allocators (arena/stack/pool, tracked) · **M1.3** math I — vectors & matrices
(+derivation) · **M1.4** math II — quaternions & transforms (+derivation) · **M1.5**
containers — slot map / handle table · **M1.6** work-stealing job system (+ a sample that
saturates all cores) · **M1.7** minimal reflection (describe + serialize a struct) ·
**M1.8** module loader. Proofs map to M1's "done when": test battery (all) · cores
saturated (M1.6) · struct serialized (M1.7) · module loaded at runtime (M1.8). After M1.1,
the memory / math / reflection lines can proceed in parallel.

**M2 — Platform & window.** `engine/platform` — window, input, filesystem, timers,
threads for Win32/Linux/macOS. No OS `#ifdef`s leak upward. Sample `00-hello-window`.

*Bricks (planned 2026-06-17, bottom-up):* **M2.1** platform module & seam — the
`engine/platform` target, public interface headers, and OS-backend selection in CMake
(backends compiled per-OS under `src/<platform>/`, never `#ifdef`-ed into public headers),
plus `init`/`shutdown`, a monotonic clock, and thread-naming; *proof:* builds and links on
all three OSes (CI). · **M2.2** window — open/close/resize and the event pump behind a
`Window` interface, implemented **natively** (no GLFW/SDL — ADR-0006). Decomposed per-OS:
**M2.2a** `Window`/event seam + native-handle struct + null backend + **Cocoa**; **M2.2b**
**Win32**; **M2.2c** **X11/Xlib**; **M2.2d** **Wayland** + runtime backend selection. · **M2.3**
input — keyboard & mouse as events + polled state through the platform interface. · **M2.4**
filesystem & time — file/path utilities (read/write, exists, base dirs) and a
high-resolution frame timer (complementing `core` profiling). · **M2.5** sample
`00-hello-window` — **the proof:** a window opens, handles keyboard/mouse, and closes
cleanly, CI-built on all three OSes. M2's "done when" maps to M2.5. After M2.1, the
window/input and filesystem/time lines can proceed in parallel.

> *Decided for M2.2 (ADR-0006):* the windowing/input backend is **native** — Win32 / Cocoa /
> Xlib + Wayland — for full control of DPI, raw input, cursor capture, event timing, and
> fullscreen, and to ship zero windowing dependencies (VISION #1). The `Window`/event seam keeps
> a backend swappable (a GLFW backend could still be added later for bring-up). "Native" is
> scoped to windowing/input; the clock and filesystem use `std::chrono`/`std::filesystem`, with
> native shims only where std has no answer (thread naming, exe path, user dirs).

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
