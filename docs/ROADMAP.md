# Rime Roadmap

The big-picture map from "empty repo" to the **vision demo**: a destructible urban
block with dynamic lighting, networked, at a playable frame rate (see [VISION.md](../VISION.md)).

**How we use this roadmap.** These are *milestones* — the map, not the turn-by-turn
directions. Each milestone is decomposed into small **bricks**, and every brick is
planned again before it's built. A milestone is **"done" only when its proof runs** (a
`samples/` demo and/or CI gate) — never when it merely compiles. We re-plan at each
milestone boundary; time estimates come at brick-decomposition, not here.

> **Update (2026-07-17) — Milestone 8 complete; Track S1 kicks off as the M9 runway.** M8 (Destruction
> v1) landed on `main` (through `samples/10-destructible-wall`, PR #62). M9 (Editor v1) is next, but its
> **hard entry gate is Track S1 streaming** — the editor is a *client of the engine* over the streaming
> protocol ([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)), and s1.4 is the viewport's local
> wire. S1 had been skipped on the way here (after M6 the path ran S0.7 → M7 → M8), so the decision this
> session (Luca): **build the full S1 track (s1.0–s1.4) before M9**, rather than shortcut the editor onto
> the S0 TCP loopback. **S1.0 landed [ADR-0030](adr/0030-streaming-v1.md)** — the streaming-v1 decisions:
> the inter-frame **wire codec is AV1** (SVT-AV1 encode + dav1d decode), chosen on the same ship-safe
> licensing test that ruled out GPL x264 in ADR-0017 — AV1 is royalty-free, H.264 rides the MPEG-LA patent
> pool; `Codec::Av1 = 3` is appended, **JPEG stays the intra fallback, LZ4 the lossless/local editor
> path**; **async readback** kills the measured S0 capture stall and needs one new RHI primitive (a
> non-blocking submit + completion token — the Vulkan backend already fences frames-in-flight; S0 exposed
> only `submit_blocking`/`wait_idle`); a **seven-stage latency ledger** (no NTP — echoed-timestamp one-way
> estimates) makes glass-to-glass honest; and the protocol grows (codec negotiation, a parameter-set
> message, a keyframe-request seam; `kProtocolVersion` bumps). **S1 is decomposed into bricks s1.0–s1.4:**
> **s1.0** ADR-0030 (this) · **s1.1** async readback (the RHI completion primitive + an N-deep readback
> ring, latest-wins drop) · **s1.2** the AV1 software codec (SVT-AV1 + dav1d behind
> `VideoEncoder`/`VideoDecoder` seams; hardware encoders slot in later; the confirming `codec_bench`
> numbers land here) · **s1.3** input v2 + the latency ledger · **s1.4** the **local fast path**
> (UDS/named-pipe transport + LZ4-lossless default) — **the M9 viewport's wire, the one hard M9 blocker**.
> Proofs stay GPU-free/structural on lavapipe; the video codec serves the WAN path, off the editor's
> LZ4-lossless critical path. **Next:** s1.1 — async readback.
>
> **Update (2026-07-04) — Milestone 4 merged to `main`; M5 begins.** The M4 stack landed via PRs
> #11, #15, #13, #14 (#12 was closed by a base-branch race when #11 merged; #15 supersedes it),
> CI-green on all three OSes + both sanitizer jobs. One first-contact find on the way in, caught
> by the Clang TSan job — the first compiler to build `rime_ecs` under Clang: Clang < 19 defaults
> sized-deallocation off, hiding the sized+aligned `operator delete`, so superblocks now free
> through the unsized aligned form. **M5 — the render graph + PBR — is decomposed into bricks
> M5.0–M5.9** (see the M5 detail below). Scope decisions recorded: **no shadows** in M5 (M10 owns
> them), **no IBL**/cube textures, dogfood acceptance is an offscreen **test** expressing the
> viewer's frame as a graph (ADR-0016 rule 4 — no viewer port), and first light gets watched
> **live over Track S0 streaming** (`07-first-light --serve`) since the dev server is headless.
> **M5.0 landed:** [ADR-0019](adr/0019-render-graph.md) settles the graph architecture —
> **frame-declared passes** (rebuilt every frame, UE-RDG-style), **virtual resources** with a
> desc-keyed transient cache, **declared access driving order, culling, *and* barriers** (emitted
> as explicit transitions through a new RHI seam, cashing in the deferral `command_buffer.hpp`
> has documented since M3), raster/compute/copy pass kinds, **serial single-queue v0 with the
> parallel + async-compute seams kept open**, and per-pass GPU timestamps from day one.
>
> **Update (2026-07-03) — Track S0 landed; M4 (ECS) kicks off.** The **S0 dev-stream** track is in:
> blocking TCP sockets (S0.1), the `engine/stream` frame tap (S0.2), the JPEG/LZ4 codec
> (S0.3, [ADR-0017](adr/0017-streaming-codec.md)), the versioned protocol (S0.4), an engine-side
> loopback proof (S0.6), and the headless `samples/04-remote-view` server+client (S0.5) — the full
> render→capture→encode→transport→present→input loop, verified GPU-free on lavapipe (S0.5's *windowed*
> client is the one piece deferred to a machine with a display). **M4 — ECS / the world — now begins**,
> decomposed into bricks **M4.0–M4.6** (see the M4 detail below). **M4.0 landed:**
> [ADR-0018](adr/0018-ecs-storage-model.md) settles the storage model — **archetype/SoA chunked
> tables**, generational-`Handle` entities, chunks drawn from `core`'s (now load-bearing) allocators,
> and change detection designed in from day one. **M4.1 – M4.3 have since landed** — the `engine/ecs`
> module: the generational entity directory + reflection-aware component registry (M4.1), the
> allocator-backed chunk storage primitives `ChunkPool` / `ChunkLayout` / `Chunk` (M4.2a), the
> `World` archetype integration — `spawn`, add/remove component = **archetype move**, `get`/`has`,
> directory `location` wired (M4.2b), **`Query<Ts...>`** — column-wise iteration over the entities that
> have a given component set (M4.3), and **`Query::par_for_each`** — that iteration run across all cores
> with **one chunk per job** (chunks are separate pooled buffers ⇒ no false sharing), the engine's
> first real multicore load on the M1.6 deque; the Phase 0 **TSan** CI job now nets `rime_ecs_tests`
> too (M4.4a), and the **`System` + `Schedule`** scheduler that batches systems into parallel **phases**
> from their declared read/write **access sets** — independent systems run side by side, conflicting
> ones fall into ordered phases (M4.4b), and a **`CommandBuffer`** that records structural edits
> (spawn/despawn/add/remove) from inside a system — thread-safe under `par_for_each` — for the schedule
> to apply at each phase boundary (M4.4c). **M4.4 is complete**: all ASan+UBSan-clean, and the
> data-parallel, concurrent-systems, and concurrent-recording paths are all TSan-clean. **M4.5 has
> since landed** — the **transform hierarchy**: `LocalTransform` / `WorldTransform` / `Parent` +
> `propagate_transforms` composing `world = parent.world * local` depth-by-depth, each level updated in
> parallel (flat scenes take a fully-parallel fast path); derivation in
> [docs/math/transform-hierarchy.md](math/transform-hierarchy.md). Change detection's dirty-subtree
> optimization is **deferred** (measure before optimize; recompute-all is correct + parallel).
> **M4.6 has landed, and with it MILESTONE 4 IS COMPLETE**: the proof sample
> `samples/05-ecs-playground` runs both of M4's "done when" clauses green — **200k entities stepped in
> parallel** through the ECS (a `Query::par_for_each` integrate system on a `Schedule`), timed against a
> serial baseline at **≈10× on 16 cores (Release)** and verified bit-for-bit identical, and a **transform
> hierarchy** (a tank: hull → turret → barrel → muzzle) composing `world = parent·local` correctly and
> following its root when moved. `engine/ecs` is in the default build; the sample self-checks (non-zero
> exit on failure). **Next:** M5 — the render graph + PBR (first light).
>
> **Update (2026-07-03) — Phase 0: land + harden.** `feat/icem-viewer` (all of M3 plus the ICEM
> viewer through ladder **F**) merged to `main` via **PR #2** and is now **CI-green on Windows,
> Linux, and macOS** (Linux on lavapipe, `RIME_REQUIRE_VULKAN=1`) — that 57-commit branch had never
> run CI before. Landing it needed the X11 leaky-macro fix (the engine had not compiled on Linux
> since M3.1). CI now also builds `feat/**` pushes, and two Linux **sanitizer** jobs guard the
> lock-free code (ASan+UBSan over all suites; Clang **ThreadSanitizer** over the deque + job system).
> Direction set this session: **the editor is a client of the engine**
> ([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)) and a graphics-**streaming** track
> (Track S — S0 dev-stream now, shippable remote play later; see Cross-cutting tracks).
>
> **Status (2026-06-21):** **Milestones 0 (build bootstrap) and 1 (core foundation) —
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
> **Milestone 2 (Platform & window) — COMPLETE.** `engine/platform` provides window, input,
> filesystem, timers, and threads behind a seam with no OS `#ifdef`s leaking upward. All bricks
> landed (see `git log`): **M2.1** module & seam · **M2.2a–d** a native window + event pump on
> **Cocoa, Win32, X11, and Wayland** (Linux selects Wayland or X11 at runtime) · **M2.3** polled
> keyboard/mouse input · **M2.4** filesystem + frame timer · **M2.5** the `00-hello-window` proof.
> CI builds and link-checks all four backends on Windows/Linux/macOS; the runnable proof opens a
> window and handles input on Cocoa/Win32/X11 (a Wayland surface is created and event-wired but maps
> on screen once the M3 renderer attaches a buffer).
>
> **Milestone 3 (RHI + Vulkan backend) — COMPLETE.** The graphics seam `engine/rhi` and its Vulkan
> backend are up: a `Device` (volk + VMA, Vulkan 1.3 dynamic rendering + synchronization2), offline
> GLSL→SPIR-V shaders, and a triangle rendered **off-screen** with a pixel-readback proof
> (**M3.1–M3.3**); **swapchain presentation** — the same triangle in a real window via frames-in-flight,
> with surfaces built from `platform::NativeWindow` across all four window systems (**M3.4**, ADR-0009);
> and **index buffers + texture upload + samplers + a combined-image-sampler descriptor model** that
> draw M3's "done when" — a **textured quad**, pixel-verified off-screen (`tests/rhi/textured_quad_test`,
> four R/G/B/Y quadrants) and presented in a window (**M3.5**, ADR-0010). Verified locally on
> macOS/MoltenVK (Vulkan 1.3.334); the off-screen proofs keep M3 runnable **GPU-free in CI** on lavapipe,
> mirroring M2's headless split. Decisions in ADRs 0007 (Vulkan bootstrapping), 0008 (offline shaders),
> 0009 (swapchain/presentation), and 0010 (textures & descriptors). **Next:** M4 — ECS / the world.

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
- **Simulation effects (Tracks FX & FL):** dedicated modules for the hardest simulation domains,
  building on the M7 physics core's seams and the render graph. **Track FX** (`engine/vfx`) — a GPU
  particle substrate with fire and dust/smoke as effect families (spawned from the M8 destruction event
  fan-out; fire drives lights, smoke reads the M10 lighting data); it replaces M8.4's dust stub and
  hard-gates M12's block. **Track FL** (`engine/fluids`) — CPU heightfield water with two-way buoyancy
  coupling into physics; decided at M12.0. Both are cross-cutting (interleave under mainline-first), not
  milestones; most of both is provable GPU-free/structural on lavapipe. *Inspired by: Frostbite/Niagara
  effects; shallow-water + SPH literature.*
- **Graphics streaming (Track S):** the engine renders → captures → encodes → transports → a thin
  client presents and sends input back. **S0** (LAN/loopback dev-stream — TCP, JPEG/LZ4, a thin
  Rime-built client) lands right after Phase 0, before M4; **S1+** (hardware codecs, QUIC/WebRTC)
  post-M5. It is a **shippable engine feature** (`engine/stream` over `engine/net`), not dev-only
  tooling, and the *same* versioned protocol carries the M9 editor viewport
  ([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)). Ship-safe codecs only under
  Apache-2.0 — never GPL x264 in the engine.

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

*Bricks (planned 2026-06-18, bottom-up):* **M3.1** the `engine/rhi` seam (agnostic interface +
opaque handles) and Vulkan device bring-up — instance + validation, physical-device selection
(requires Vulkan 1.3 + dynamic rendering + synchronization2), logical device + queue, VMA; *proof:*
builds & link-checks the backend on all three OSes (CI) and a headless device-creation test on
**lavapipe**. · **M3.2** offline GLSL→SPIR-V (`rime_add_shaders`, mirroring `wayland-scanner`) + the
RHI `Shader`. · **M3.3** graphics pipeline (dynamic rendering, no render-pass objects), the command
encoder, vertex buffers/textures, and an **off-screen triangle verified by pixel readback** — the
GPU-free "first pixels" proof (`tests/rhi`), runnable as `samples/01-hello-triangle`. · **M3.4**
`VkSurfaceKHR` from `platform::NativeWindow` (all four window systems) + swapchain + frames-in-flight
+ present — the triangle in a real window (Win/Linux + macOS/MoltenVK), and the M2 Wayland surface
finally maps. · **M3.5** index buffers, texture upload + sampler + descriptor model → the **textured
quad** (M3's "done when"). M3.1–M3.3 land the first triangle off-screen; M3.4–M3.5 put it on screen
and texture it.

> *Decided for M3 (ADRs 0007–0008):* the Vulkan backend uses the **volk** meta-loader (no loader
> linked at build time), **VMA** for memory, and a **Vulkan 1.3** baseline — **dynamic rendering +
> synchronization2**, so there are no `VkRenderPass`/`VkFramebuffer` objects and the RHI maps cleanly
> onto the M5 render graph. Shaders are compiled **offline** (GLSL→SPIR-V at build time) and embedded;
> the engine ships no runtime shader compiler. Build dependencies come from **Conan**; the runtime
> loader + ICD (a GPU driver, **MoltenVK** on macOS, **lavapipe** on GPU-less CI) are the
> environment's, so the off-screen render proof runs green on all three OSes without a GPU.

**M4 — ECS / the world.** `engine/ecs` — entities, components, archetype storage,
parallel systems on the job system, queries, a transform hierarchy. Sample
`05-ecs-playground`.

*Bricks (planned 2026-07-03, bottom-up):* **M4.0** the storage-model decision —
**archetype/SoA chunked tables** over sparse sets, entity IDs on the generational
`core::Handle`, chunks drawn from `core`'s allocators (the allocator module finally becomes
load-bearing), and **change detection** via per-component version stamps designed in from day
one ([ADR-0018](adr/0018-ecs-storage-model.md)); *proof:* the ADR — no code, the decision the
rest of M4 cites. · **M4.1** the `engine/ecs` seam + **entity directory** (generational spawn /
despawn / liveness / recycling) + **component registration through reflection** (registered once
⇒ serializable now, editor-inspectable at M9; extends `RIME_REFLECT_*`). · **M4.2**
**archetype / chunk storage**, in two steps: **M4.2a** the storage primitives — an allocator-backed
`ChunkPool` (16 KiB blocks from `core`'s pool allocator, finally load-bearing), the per-signature SoA
`ChunkLayout`, and the `Chunk` row store with swap-remove · **M4.2b** the World integration — an
archetype keyed by `ComponentSignature`, spawn-with-components, and add/remove component = archetype
move (the entity relocates; its directory location is wired). · **M4.3** **queries + chunk-wise
iteration** — find the archetypes matching a signature and scan their columns. · **M4.4** the
**parallel system scheduler** on the `JobSystem`, in three steps: **M4.4a** `Query::par_for_each` — the
query body run across all cores with **one chunk per task** (chunks are separate pooled buffers ⇒ no
false sharing), the first real multicore load on the Chase-Lev deque, with Phase 0's TSan job
extended over `rime_ecs_tests` as the net · **M4.4b** the **`System` + `Schedule`** scheduler — declared
read/write **access sets** batched into parallel **phases** (ASAP leveling of the conflict order:
independent systems run together, conflicting ones keep declared order), phases run concurrently on the
job system · **M4.4c** **deferred structural changes** — a command buffer applied at phase boundaries,
lifting the no-structural-change-inside-a-system rule. · **M4.5** the
**transform hierarchy** — `LocalTransform`/`WorldTransform`/`Parent` + `propagate_transforms`,
`core::Transform` composition (`world = parent.world * local`) processed depth-by-depth with each level
updated in parallel (flat scenes take a fully-parallel fast path); the change-detection dirty-subtree
optimization is deferred (measure first). · **M4.6 (done)** the proof sample
`samples/05-ecs-playground` — **200k entities updating in parallel** (≈10× on 16 cores, Release, verified
bit-for-bit vs serial) and **transforms composing correctly** (M4's "done when"), self-checking;
`engine/ecs` builds by default. M4.0–M4.3 build the world's data model bottom-up; M4.4 runs systems over
it in parallel; M4.5–M4.6 land the two proofs. A `docs/design/ecs.md` note accompanies the storage
bricks and a `docs/math/` derivation the transform hierarchy. **Milestone 4 complete.**

**M5 — Render graph + PBR (first light).** `engine/render` — **render graph** (passes,
transient resources, auto-barriers), mesh/material/camera, **PBR** (+ derivation), depth
pre-pass, one dynamic light. Samples `06-render-graph`, `07-first-light` (renumbered — 03/04
were taken by the viewer and remote-view). The home for
M10. *Inspired by: UE5 render-graph discipline.* — *Note: several RHI features were pulled
ahead of M5 to unblock the ICEM viewer — the **depth attachment** + depth test (ADR-0011),
**push constants** (ADR-0012), and **3-D/volume textures** (ADR-0013, for field colormaps).
The render graph adopts and extends them (multiple targets, stencil, MSAA, streamed volumes).*

> **Status (2026-07-06): MILESTONE 5 COMPLETE — M5.0–M5.9 built and green on lavapipe.** The RHI
> top-ups (M5.1–M5.3), the **render graph** (M5.4), the **scene layer** (M5.5), the **forward-PBR
> pipeline** (M5.6, [math/pbr.md](math/pbr.md)), the **fixed-tick application loop** (M5.7,
> ADR-0023), the two proof samples (M5.8) — `07-first-light` draws M5's "done when": a lit PBR scene
> through the graph, headless-self-checked and streamable over Track S0 — and the **dogfood
> acceptance test** (M5.9) are all in. M5.9 re-expresses the ICEM viewer's cross-section frame
> (clip-planed lit mesh → stencil cut-mark → solid cap → alpha-tested UI overlay) as four
> render-graph passes **sharing one colour + one D32FloatS8 depth+stencil target**, proving the
> graph's resource model covers depth+stencil attachments and Load/keep-across-passes semantics
> (`tests/render/viewer_frame_graph_test.cpp`, offscreen, GPU-free in CI, ADR-0016 rule 4).
> **Next:** M6 — asset pipeline + runtime assets (import → cook → load → render a real glTF model).

*Bricks (planned 2026-07-04, bottom-up):* **M5.0** the architecture decision — a **frame-declared
render graph** with virtual resources, declared access driving order *and* barriers, graph-owned
transitions through a new RHI barrier API, serial single-queue v0 with the parallel seams kept
([ADR-0019](adr/0019-render-graph.md)); *proof:* the ADR — no code, the decision the rest of M5
cites. · **M5.1** RHI top-up I — **descriptor model v2**: declared binding layouts, **uniform
buffers**, per-frame descriptor pools (ADR-0020); then **blending**, **multiple render targets**,
and **RGBA16Float** (the HDR scene format); *proofs:* UBO-driven draw, blended quads, MRT —
pixel-verified, GPU-free on lavapipe. · **M5.2** RHI top-up II — **compute pipelines + dispatch +
storage buffers/images** (ADR-0021); *proofs:* compute pattern → exact readback; compute-written
image sampled by a draw. · **M5.3** RHI top-up III — **mipmaps** (blit-generated chains) +
**anisotropic sampling**; **GPU timestamps** + debug names/labels (RenderDoc legibility);
*proofs:* minification pixel test; monotonic timestamps. · **M5.4** the **render graph v0** —
`RGTexture`/`RGBuffer`, `add_pass` (raster/compute/copy), `import`, compile (resource versioning →
edges → topological order → cull), the transient cache, graph-emitted barriers, per-pass timing;
*proofs:* multi-pass pixel tests (incl. compute-in-graph, pass culling, transient reuse); compile
overhead measured and recorded. · **M5.5** the **scene layer** — `OrbitCamera` graduates from the
viewer (ADR-0016 rule 3), procedural mesh primitives + mesh/material registries, ECS render
components registered through reflection (`Camera`, `MeshRef`, `MaterialRef`, lights). · **M5.6**
the **PBR forward pipeline** — depth pre-pass → Cook-Torrance forward shading into HDR → tonemap,
as reusable graph passes, + the `docs/math/pbr.md` derivation (ADR-0022); *proofs:* structural
radiometric asserts on a metallic×roughness sphere grid. · **M5.7** **`engine/app` becomes real**
— the fixed-tick frame loop (simulation ticks decoupled from render frames — the M11 seam;
ADR-0023) with a headless mode; *proofs:* tick determinism; a headless CI run. · **M5.8** the
**proof samples** — `06-render-graph` ("adding a pass is easy," demonstrated in ~10 lines) and
`07-first-light` (M5's "done when": the lit PBR scene through the full stack; `--headless`
self-check in CI; `--serve` streams it over Track S0 to the thin client for the first live look).
· **M5.9 (done)** **dogfood acceptance** — the ICEM viewer's frame (mesh + stencil cap + UI overlay)
expressed as a render graph in an offscreen test (ADR-0016 rule 4). M5.1–M5.3 make the RHI
renderer-ready; M5.4 is the seam itself; M5.5–M5.7 build the scene and the loop; M5.8–M5.9 land
the proofs. **Milestone 5 complete.**

**M6 — Asset pipeline + runtime assets.** `tools/asset-pipeline` (Rust) imports glTF +
textures → cooked formats; `engine/assets` loads/streams at runtime; `tools/rime-cli`
cooks; the stable C-ABI/file **FFI boundary** stands up. *(ADR-0001.)* Skeletal-animation
import begins.

> **Status (2026-07-11): MILESTONE 6 COMPLETE — the whole offline→runtime asset pipeline is built
> and green on lavapipe.** [ADR-0024](adr/0024-asset-model.md) settled the model — **Rust cooks, C++
> loads, files are the boundary** (ADR-0001 made concrete) — and M6.1–M6.10 made it real: the `RMA1`
> container + `engine/assets` reader/registry/manifest (M6.1); the `tools/asset-pipeline` crate + the
> `rime` CLI cooking **glTF meshes** (M6.2), **textures** with gamma-correct offline mip chains
> (M6.3), **PBR materials** with MikkTSpace tangents + normal/MR/AO/emissive maps (M6.4), and
> **skeletons + animation clips** with a CPU sampler (M6.7); **async loading** on the job system with
> placeholder assets + the **GPU asset bridge** (M6.5, [ADR-0025](adr/0025-gpu-asset-bridge.md)); the
> **STL dogfood** in the ICEM viewer (M6.6); the **SDK** (`find_package(rime CONFIG)`, an out-of-tree
> consumer built in CI, M6.8); and the **C ABI** `librime_capi` + the `rime-ffi` crate (M6.9).
> `samples/08-gltf-zoo` (M6.10) runs the milestone's "done when" end-to-end: **import → cook → load →
> render** three real glTF models (base-color-textured cube, normal-mapped metallic-roughness sphere,
> and a CPU-posed skinned rig) through the render graph — self-checked headless in CI and streamable
> live over Track S0. **M7 — physics (own rigid-body core) — the milestone's "done when" is met**:
`samples/09-physics-playground` self-checks (objects fall/collide/stack, a raycast+impulse topples a
tower, everything sleeps, two runs hash identically) headless in CI. **Next:** M8 — destruction v1
(building on the physics core's seams), or the M7 fast-follows (contact events, shapes II, CCD).

*Bricks (planned 2026-07-06, bottom-up):* **M6.0 (done)** the asset-model decision
(ADR-0024); *proof:* the ADR. · **M6.1** `engine/assets` is born — the RMA1 reader
(trust-nothing decode, negative-battery tested), `AssetId`, the handle-based registry,
synchronous mesh loads. · **M6.2** the Rust `tools/asset-pipeline` crate + **glTF mesh
import→cook** and `rime-cli cook`/`inspect`; the cross-language golden fixture (Rust cooks
what C++ reads in CI). · **M6.3** **textures** — PNG/JPEG decode, offline linear-space mip
chains, sRGB-vs-linear by semantic; RHI top-up: uploading pre-generated mip data. ·
**M6.4** **materials + the PBR texture upgrade** — cooked metallic-roughness materials;
normal mapping (MikkTSpace tangents at import), MR/occlusion/emissive textures in the
forward-PBR shaders (+`docs/math/tangent-space.md`). · **M6.5** **async loading** on the
job system — IO/parse jobs, frame-point GPU-upload drain, placeholder assets, TSan-netted.
· **M6.6** dogfood — **STL import→cook** and the ICEM viewer loading cooked meshes
(ADR-0016 rules 3+4; the content-hash cook cache earning its keep on real multi-MB parts).
· **M6.7** **skeletal-animation import begins** — glTF skins/skeletons/clips cooked + a
CPU clip sampler with paper-checked poses (GPU skinning follows at M7). · **M6.8** the
**SDK story** — CMake install/export, `find_package(rime CONFIG)`, an out-of-tree consumer
app built in CI (arms the ICEM-migration trigger, ADR-0016 rule 5). · **M6.9** the
**C-ABI FFI boundary stands up** — a tiny `rime_capi` shared library + a Rust FFI crate
whose tests drive the engine's own loader; protocol message-type space reserved for the M9
editor channel. · **M6.10 (done)** the proof — `samples/08-gltf-zoo`: cook → load → render three
**hand-authored** glTF models (base-color-textured, normal-mapped + metallic-roughness, and a
CPU-posed skinned rig — first-party, so no third-party asset licenses), `--headless` self-check in
CI, `--serve` streamed live; docs true-up. M6.1–M6.2 land the boundary, M6.3–M6.5 make assets
real, M6.6–M6.7 widen the funnel, M6.8–M6.9 open the SDK/FFI doors, M6.10 closes the milestone.
**Milestone 6 complete.**

**M7 — Physics (rigid bodies, multicore).** `engine/physics` behind an interface —
bodies, collision, queries — stepped on the job system. **Decision (M7.0,
[ADR-0026](adr/0026-physics-core.md)): Rime builds its OWN rigid-body core — no Jolt** (VISION #1
power-first, #3 code-as-textbook; the core is shaped for destruction rather than adapted to it). The
core is the *universal simulation substrate* — destruction (M8), lighting invalidation (M10), and the
effects/fluids tracks all build on its seams. *Inspired by: Jolt (studied, not integrated); Bullet;
the sequential-impulse literature.*

*Bricks (planned 2026-07-12, own-core, bottom-up):* **M7.0 (done)** the physics-core decision
(ADR-0026: own core; the algorithm suite; substrate seams; same-binary determinism; the deferred
register); *proof:* the ADR. · **M7.1** `engine/physics` born — the seam headers, the SoA `BodyPool`
(generational ids), ECS `RigidBody`/`Collider` components + reflection, semi-implicit-Euler +
quaternion integration (no collision yet). · **M7.2** **broadphase** — dual dynamic AABB trees, fat
AABBs, parallel queries, the canonical pair list. · **M7.3** **narrowphase I** — GJK + EPA +
reference-face-clipping manifolds (feature ids, warm-start cache), sphere/capsule fast paths. ·
**M7.4** **the solver** — sequential-impulse PGS, warm starting, friction pyramid, restitution, the
NGS position pass (+`docs/math/sequential-impulse.md`). · **M7.5** **islands + sleeping + the parallel
step** on `core::JobSystem` — bit-identical world hash across thread counts, TSan-netted. · **M7.6**
**fixed-tick + ECS sync + change-detection stamps** (lands ADR-0018 §4 as public `engine/ecs` surface;
awake-only write-back). · **M7.7** **queries** — ray/overlap/shape-cast via the BVH, batched
parallel-safe variants, filters. · **M7.8** **contact/trigger/sleep events** — point + normal +
impulse, canonical per-tick order, double-buffered (the M8-damage input). · **M7.9** **shapes II** —
runtime convex hull (quickhull), polyhedral mass properties, static triangle mesh + midphase, compound
(static + dynamic). · **M7.10** **CCD** (speculative contacts) + debris-scale tuning + `WorldStats` +
the stress harness. · **M7.11** the proof — `samples/09-physics-playground` (`--headless` self-check +
`--serve`) + docs true-up. Track brick **an1** (skeletal-animation runtime: palettes on jobs + GPU
skinning) interleaves after M7.4 under the mainline-first rule.

*Status (2026-07-14):* **M7.0–M7.6 shipped**, then **M7.7 (scene queries — raycast + overlap — plus
external impulses)** and the **proof sample `samples/09-physics-playground`** land the milestone's
"done when" (objects fall/collide/stack, raycasts hit, the sim runs on the job system inside the
fixed tick; the sample self-checks headless in CI). **Reordered from the original plan to reach the
acceptance proof sooner:** the remaining planned bricks were **deferred as fast-follows** into M8's
runway, since M8 destruction is their first real consumer. Two of them have since landed:
**M7.9 — contact & sleep events** (`engine/physics/events.hpp`): began/persisted/ended contact
events carrying point + normal + impulse (the M8-damage input) plus `Slept`/`Woke` sleep events,
buffered and double-buffered in canonical per-tick order, with the event stream proven bit-identical
across worker counts (the **trigger/sensor** third of that brick is held back — no sensor-body concept
in M7's scope, no consumer yet — with its design pinned in `docs/design/physics.md`); and **M7.10 —
CCD (speculative contacts)**: a per-body opt-in flag, a velocity-swept broadphase bound, GJK-distance
speculative contacts (a negative-penetration gap), and a solver gap-bias, so a fast body is arrested
at a thin wall instead of tunnelling — no time-of-impact rewind, determinism preserved, and the stop
surfaces as an M7.9 contact event (the projectile-damage path). A third has now landed:
**M7.11 — shapes II, the convex hull** ([ADR-0027](adr/0027-convex-hull-shapes.md), the
shape-storage decision this brick wanted first): a world-owned hull store (`register_hull` →
`HullId`; `ShapeDesc` stays a flat POD), authored-and-validated geometry (no runtime quickhull —
that is the M8.1 cook), exact polyhedral mass properties diagonalized to principal axes
(`docs/math/polyhedral-mass-properties.md`), the hull support function through the unchanged
GJK/EPA path, reference-face clipping generalized from boxes to arbitrary hull faces (stable
feature ids, warm-start-cache compatible), hull raycast/overlap/CCD, and the determinism hash
proven across worker counts with hulls in the scene. Then **M7.12 — compound shapes**
([ADR-0028](adr/0028-compound-shapes.md), built on M7.11's store): one rigid body made of many
convex children at local poses, parallel-axis mass composition, and a narrowphase-expansion
multi-region contact model (per-child manifolds and per-region events — the M8 "which part was
hit" signal); the standing dumbbell is its witness. And **M7.13 — the measure-first capstone**:
`WorldStats` (a deterministic per-tick snapshot of body/collision/island counts via `stats()`,
counts not clocks so the tick stays reproducible) plus `samples/09-physics-playground --stress`, a
debris-scale load that reports peak solver load and throughput and self-checks that a 1000+-body
pile settles and hashes identically twice — the instrument that turns the remaining optimizations
from guesses into measurements. Still outstanding as fast-follows: the **static triangle mesh +
midphase** (the last shape), and the **debris-scale performance pass** the harness now measures
(its first target: the every-tick narrowphase). They remain tracked here; nothing about them is
cancelled.

**M7 non-goals (deferred, recorded in [ADR-0026](adr/0026-physics-core.md)):** joints/motors/
character controller (m12.0) · soft bodies/cloth/fluids (own modules — water is Track FL) · TGS solver
mode · implicit gyroscopic integration · dynamic mesh-vs-mesh (convex decomposition at M8.1) · scaled
colliders (v1 ignores scale) · cross-platform lockstep determinism.

**M8 — Destruction v1 (the headline begins).** `engine/destruction` — part-based
destructibles + connectivity, precomputed fracture, debris as real physics bodies,
**health-transition hooks**, and a **one-event → physics/VFX/audio fan-out**. Sample
`10-destructible-wall`. *Inspired by: Frostbite (Battlefield 6) — see
[engine-survey.md](research/engine-survey.md).*

*Bricks (planned 2026-07-15, bottom-up — refreshed against the shipped M7 substrate;
[ADR-0029](adr/0029-destruction-model.md) is the model):* **M8.0** the destruction-model ADR
(this) · **M8.1** the **fracture cook** (Rust) — seeded Voronoi → convex part hulls + bond/anchor
graph + render meshes, a new `Destructible` asset (quickhull lands here) · **M8.2** `engine/destruction`
runtime — load a pattern, register it once, stand an instance as one static compound (costs ≈ static
baseline) · **M8.3** **damage → connectivity → detach** (the hard core: contact-impulse + explicit
damage, union-find support solve, the fracture body-swap; determinism across worker counts) · **M8.4**
**health-transition fan-out** — a generic `EventChannel<T>` + a VFX dust stub + the `engine/audio` null
seam · **M8.5** **lifetime** — debris budgets over `WorldStats` + the physics `unregister_hull/compound`
· **M8.6** the proof — `samples/10-destructible-wall` (a wall fractures on impact, debris settles, one
event fires) headless-self-checking in CI. Two small **physics** seam additions are M8-owned:
`RayHit::child` (M8.3) and hull/compound `unregister` (M8.5). Proofs stay structural/headless on
lavapipe; the damage→fracture path is deterministic (the M11 replay contract). *Status: **M8
COMPLETE** — M8.0–M8.6 all landed: the model, the cook, the runtime, the fracture body-swap, the
event fan-out (`core::EventChannel` + the `engine/audio` seam + the `engine/vfx` dust stub), the
debris lifecycle/budgets over the physics `unregister_hull`/`unregister_compound`, and the
`samples/10-destructible-wall` proof — a wall fractures on impact, an island detaches, debris settles,
and one event stream fans out to dust/audio/gameplay, PBR-lit with per-part render leaves and
deterministic across physics worker counts.*

**M9 — Editor v1 (Rust).** `tools/editor` — a **client of a live engine process**
([ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md)): the Rust shell owns docking,
reflection-driven inspectors, and the asset browser; the **engine renders the viewport** and
delivers it over the (by then S1-hardened) streaming protocol; edits flow back as
reflection-described component data. The viewport toolkit graduates from the ICEM viewer and the
protocol is already proven by Track S, so M9 becomes assembly, not invention. Play-in-editor.
*Inspired by: Frostbite's FrostEd (editor-as-client) + Unity/UE iteration.*

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

---

## Appendix: the ICEM Viewer (Frostlens) — a flagship application

Rime's first non-trivial **application**: a from-scratch 3-D viewer for the computed engineering
parts, simulation fields and flow produced by **ICEM** (a separate, deterministic
computational-engineering project). It is built *on* Rime — `samples/03-icem-viewer` links only
`engine/{core,platform,rhi}` — so it **dogfoods the engine** and pulls exactly the features Rime's own
roadmap wants next. Several RHI bricks were landed early to serve it and are adopted+extended by the M5
render graph: the **depth attachment** (ADR-0011), **push constants** (ADR-0012), **3-D/volume textures**
(ADR-0013) and **stencil** state (ADR-0014). The two repos share only *files* (STL/OBJ meshes + a native
`.icef` field binary), never code. Full plan lives outside the repo; the brick ladder, with proofs:

- **A — foundations.** A1 depth attachment (RHI) · A2 orbit camera · A3 the `.icef` field bridge (ICEM
  side). **DONE.**
- **B — surfaces + cross-section.** B1 load an STL → lit, depth-correct, orbitable part · B2 movable clip
  plane + **stencil solid cap** to look *inside* a part. **DONE.**
- **C — visualize the existing simulations.** C1 colour the part (and the cut face) by an `.icef` scalar
  field + legend · C2 GPU raymarched **isosurface + DVR** · C3 vector-field **warp** (animated
  displacement / modal mode). **DONE.**
- **D — real 3-D CFD → flow view.** ICEM grew a genuine 3-D CFD ladder (D1 inviscid potential flow, D2
  viscous Navier–Stokes); the viewer renders the computed velocity as **RK4 streamlines** coloured by
  speed (`docs/math/streamlines.md`), and — **D2·V** — derives the scalar **speed** $\lVert\mathbf u\rVert$
  so the colormap / isotach / slice / **DVR** show the viscous **boundary layer** as a volume
  (`tests/rhi/viscous_offscreen_test`). **DONE.** **D3 — compressibility:** ICEM gained a third CFD model
  (brick26, `core/sim/compressible.hpp`) — quasi-1-D isentropic **de Laval nozzle** flow recovered from the
  area–Mach relation, where ρ/T/p ride the flow, gated against `thermo::gasdyn`; the viewer colours the
  nozzle by the computed **Mach** field (0.15 subsonic inlet → green sonic throat → 2.47 supersonic exit),
  cross-sectioned. **D4 — turbojet/nozzle flow view:** a from-scratch **gas-path chart** — `--chart` / **H**
  overlays a 2-D line plot of the field along the flow axis (Mach vs station, with the dashed M = 1 sonic
  line), built on a new `ui.hpp` `line()` primitive (`docs/math/gas-path-chart.md`,
  `tests/rhi/chart_offscreen_test`). **DONE — milestone D complete.**
- **E — assemblies, from-scratch UI, provenance.** E1 multi-part **assemblies** — the ITER-class tokamak
  loads as 10 colour-tinted, number-key-toggleable parts with an axial **exploded view**
  (`tests/rhi/assembly_offscreen_test`, `docs/math/assembly.md`) — **DONE**. E2 a **from-scratch
  immediate-mode UI** on the RHI — a built-in bitmap font + panel/label/button/checkbox/slider, no Dear
  ImGui, drawn as one alpha-tested overlay pass; the assembly control panel toggles parts and scrubs the
  explode slider ([ADR-0015](adr/0015-imgui-free-ui.md), `docs/math/ui-text-layout.md`,
  `tests/rhi/ui_offscreen_test`) — **DONE**. E3 a **provenance panel** — `--provenance` reads ICEM's
  `.icejson` "why" Ledger (the DAG of which law / material / rule / safety-factor produced every value)
  and lays it out on the E2 UI as an origin-tinted, scrollable list; clicking a value expands its
  **derivation** (the values it was computed from). Shares a file format with ICEM, never code
  (`docs/math/provenance-panel.md`, `tests/rhi/provenance_offscreen_test`) — **DONE**. E4 **export polish**
  — `--turntable N` renders a full 360° orbit to a numbered PPM sequence (frame $i$ at azimuth
  $\varphi_0+2\pi i/N$) with per-frame render-throughput stats, the windowed **P** key screenshots the live
  view, and all three share one off-screen `render_view` path so the export is pixel-identical to the
  interactive frame (`turntable.hpp`, `docs/math/orbit-camera.md` §export, `tests/rhi/turntable_test`) —
  **DONE**. **Milestone E complete.**
- **F — the engine cut-away (Bview).** ICEM's showcase is a computed **geared turbofan**: `icem engine`
  emits 15 `engine_*.stl` parts plus `engine_core.icef` / `engine_bypass.icef`, each carrying a `velocity`
  and a `mach` field. This view **fuses D and E** into how engineers actually draw an engine — the
  multi-part assembly, now opened by a **meridional cut-away** clip plane through the flow axis, drawn in
  **one shared-camera, shared-depth pass** with **streamlines** traced through both ducts and coloured by
  the computed **Mach** (not raw speed), on one Mach scale shared by core and bypass. Sharing the depth
  buffer makes the remaining metal occlude the flow behind it while the opened half reveals it — a true
  cut-away. `--engine <dir>` (oriented y-up, the engine lying horizontal); the panel gains a CUT-AWAY
  toggle + Mach readout, and **C** toggles the section. No new pipeline/shader/RHI: the assembly's mesh
  clip is switched on and the Mach rides the streamline `w` channel the speed used to. A latent streamline
  bug surfaced here — a line reaching an open OUTFLOW face ran straight to `max_steps` because the sampler
  clamps at the domain edge; it is now stopped at the domain bound (`engine.hpp`,
  `docs/math/engine-view.md`, `tests/rhi/engine_offscreen_test`). **DONE.**

Each viewer brick follows Rime's conventions — a `docs/math/` derivation for the math-heavy ones, an ADR
for engine decisions, an off-screen pixel-readback proof in `tests/rhi/` that stays GPU-free in CI, and
an auto-committed+pushed focused change.
