# Rime Architecture

This document explains **how Rime is structured and why**. It is written to be read by
someone learning how engines work, so it defines ideas as it goes. It describes the
*intended* architecture; much of it is not built yet. Sections are tagged:

- 🟢 **Built** — exists and works.
- 🟡 **Designed** — the shape is decided; code is partial or stubbed.
- ⚪ **Planned** — intended; not started.

As of 2026-07 the **bottom of the layer cake is built and CI-green on Windows, Linux, and
macOS**: `core` 🟢 and `platform` 🟢 (Milestones 0–2) and `rhi` 🟡 (Milestone 3 — the
graphics seam and its Vulkan backend). Everything above — `ecs`, `render`, and the feature
modules — is still ⚪. This document is the blueprint we build toward; the per-section tags
below say how far each part has actually come.

> New to the vocabulary? Keep [glossary.md](glossary.md) open in a tab.

---

## 1. Guiding ideas

Three ideas shape every decision (they come straight from [VISION.md](../VISION.md)):

1. **Small core, many modules** (from O3DE). The engine is a tiny kernel that knows how
   to load modules and route messages. *Everything else* — rendering, physics, audio,
   destruction — is a module behind an interface. You can remove one and still build.
2. **Data-oriented design** (from the ECS/Bevy world). We think in terms of *data laid
   out for the cache* and *systems that transform that data*, not deep trees of objects
   calling virtual methods. This is what makes engines fast and parallel.
3. **Seams where the future goes** (from hard-won industry experience). The two biggest
   bets — the **RHI** (so we can add graphics backends) and the **destruction/lighting
   first-class design** — are isolated behind interfaces from day one, because
   retrofitting them later is what kills engines.

## 2. The layer cake

Dependencies point **downward only**. A layer may use the layers below it, never above.
This is the single most important structural rule in the codebase.

```
        ┌───────────────────────────────────────────────────────┐
        │  Games & Samples            (built ON Rime)             │
        ├───────────────────────────────────────────────────────┤
        │  Tools (Rust): Editor · Asset Pipeline · CLIs           │
        ├───────────────────────────────────────────────────────┤
        │  Gameplay / Scripting layer                             │
        ├───────────────────────────────────────────────────────┤
        │  Feature modules:                                       │
        │    render · physics · destruction · audio · animation  │
        │    · scene/world · assets(runtime) · net                │
        ├───────────────────────────────────────────────────────┤
        │  ecs   (the data-oriented world: entities, components)  │
        ├───────────────────────────────────────────────────────┤
        │  rhi   (Render Hardware Interface) → [Vulkan backend]   │
        ├───────────────────────────────────────────────────────┤
        │  platform (window, input, files, threads, time)         │
        ├───────────────────────────────────────────────────────┤
        │  core  (memory, math, containers, jobs, log, reflect,   │
        │         the module/plugin system)                       │
        └───────────────────────────────────────────────────────┘
```

## 3. Module map (`engine/`)

Each becomes a subdirectory under `engine/` with its own README and build file. Listed
bottom-up. (Names are stable targets; not all directories exist yet.)

### `core` 🟢 — *the foundation everything stands on*
- **Memory:** custom allocators (linear/arena, pool, stack), tracked allocations. We
  control memory because AAA performance demands it.
- **Math:** vectors, matrices, quaternions, transforms — SIMD-friendly.
- **Containers:** cache-friendly arrays, slot maps, handle tables.
- **Jobs:** a work-stealing **job system** — the heart of multicore. Almost all engine
  work is expressed as jobs so it scales across cores.
- **Log / assert / profiling hooks.**
- **Reflection:** lightweight type info so data can be serialized, inspected in the
  editor, and scripted. (Engines live or die by their reflection/serialization story.)
- **Module system:** load/unload modules at runtime, resolve their interfaces. This is
  the O3DE-style "Gem" mechanism, in our own minimal form.

*Built (M1):* all of the above — allocators (arena/stack/pool, tracked), SIMD-friendly math
(with derivation notes), a generational slot map, the **Chase-Lev work-stealing job system**,
diagnostics (log/assert/timing), minimal reflection, and the runtime module loader. One
caveat: the allocators are complete but not yet *load-bearing* — the ECS puts them to work
at M4.

### `platform` 🟢 — *the thin OS abstraction*
Windowing, input, filesystem, high-resolution timers, threads/atomics — one interface,
three implementations (Win32, Linux, macOS). Keeps OS `#ifdef`s out of the rest of the
engine. *Built (M2):* a native window + event pump on **Cocoa, Win32, X11, and Wayland**
(Linux selects Wayland or X11 at runtime), polled keyboard/mouse input, filesystem, and a
frame timer — all behind the seam, no OS `#ifdef` leaking upward ([ADR-0006](adr/0006-native-windowing.md)).

### `rhi` 🟡 — *Render Hardware Interface (the graphics seam)*
A modern, explicit graphics abstraction (devices, queues, command buffers, pipelines,
descriptor/binding model, synchronization). **The Vulkan backend is the only code that
includes Vulkan headers** — enforced by the build (its deps are linked PRIVATE), not just
by review. Everything above talks to `rhi` interfaces. This is the seam that lets D3D12 /
Metal / console backends arrive later without touching the renderer. *Built (M3, complete
and CI-green):* device bring-up (volk + VMA, Vulkan 1.3 dynamic rendering + synchronization2),
offline GLSL→SPIR-V shaders, graphics pipelines, buffers/textures/samplers, swapchain +
presentation across all four window systems, and a combined-image-sampler descriptor model —
proven by a windowed textured quad *and* GPU-free off-screen pixel-readback tests (ADRs 0007–0010).
Extra features landed to serve the ICEM viewer and will be adopted by the M5 render graph:
depth attachment (ADR-0011), push constants (ADR-0012), 3-D/volume textures (ADR-0013), and
stencil (ADR-0014). *Known gaps before renderer-ready (M5):* no compute dispatch, no
multiple-render-targets or blending, no MSAA/mipmaps, a single-set/single-binding descriptor
model, one queue, a fixed two frames in flight, and single-threaded command recording. → [ADR-0002](adr/0002-vulkan-first-rhi.md),
[ADR-0007](adr/0007-vulkan-backend-bootstrapping.md), [design/rhi.md](design/rhi.md)

### `ecs` ⚪ — *the world, as data*
An **Entity-Component-System**: entities are ids; components are plain data stored in
tight arrays; systems are functions that run over matching component sets, ideally in
parallel via the job system. This is how we get both performance *and* a model that's
easy to reason about and extend.

### `render` ⚪ — *where the picture comes from*
A high-level renderer built on a **render graph** (a.k.a. frame graph): each frame is
described as a graph of passes and the resources they read/write; the graph schedules
them, manages transient memory, and inserts barriers. This structure is precisely what
makes UE5-class techniques tractable:
- **Virtualized geometry** (Nanite-style) — render massive detail by streaming/culling
  at fine granularity.
- **Global illumination & reflections** (Lumen-style) — dynamic, no baking.
- **Virtual shadow maps** — high-res, consistent shadows.
- **Many lights** (MegaLights-style) — large counts of shadow-casting lights.
We don't build all of these at once; we build the render graph *so that they fit*.

### `physics` ⚪ — *simulation, multicore-first*
Rigid bodies, collision, queries — designed around parallel simulation and the ability
to query/stream physics state off the main thread. We will evaluate integrating **Jolt
Physics** (proven, multicore, open) vs. growing our own; either way the destruction
system sits on top. → see [survey](research/engine-survey.md#physics).

### `destruction` ⚪ — *our headline system (Frostbite-inspired)*
A first-class engine system, not a plugin. Core ideas, mapped from how Battlefield 6's
Frostbite does it:
- **Part-based destructibles:** structures are assemblies of breakable *parts* with
  connectivity, not monolithic meshes.
- **Health-transition hooks:** parts move through health states; transitions can spawn
  assets, debris, VFX, audio, or gameplay effects from one event source.
- **Real debris:** fragments become physics bodies, fall, settle, and can damage
  players — not instantly-vanishing decoration.
- **One event, many listeners:** a single destruction event drives physics, VFX
  ("surface/seam emitters"), and audio coherently.
- **Network-aware from the start:** prioritization & culling of part-destruction and
  debris so it scales to 64+ players. Determinism/replication is a design constraint,
  not an afterthought.

### `audio` ⚪, `animation` ⚪, `assets` (runtime) ⚪, `net` ⚪
Audio mixing/spatialization; skeletal animation & blending; runtime asset
loading/streaming; networking/replication. Each behind its own interface.

### `stream` 🟡 — *graphics streaming (Track S)*
Capture a rendered frame, encode it, and ship it to a thin remote client that presents it and sends
input back — **one stack** for dev streaming (now), the editor viewport (M9), and remote play (later).
*Built (S0.2):* the **frame tap** (`FrameStreamer` — RHI readback, double-buffered, measured).
*Built (S0.3):* the **codec** (`FrameEncoder`/`FrameDecoder`) — JPEG (libjpeg-turbo) on the wire, LZ4
for lossless, chosen by measurement ([ADR-0017](adr/0017-streaming-codec.md)); both libraries stay
hidden behind the public header. A *removable* feature module that depends only on the RHI interface +
the platform transport, so it captures from any backend. →
[ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md),
[design/graphics-streaming.md](design/graphics-streaming.md).

### `app` ⚪ (stub) — *the application framework & main loop*
Ties a module set together into a runnable application: initializes subsystems, owns the
frame loop (input → simulate → render → present), and shuts down cleanly. *Today* it is only
the Milestone-0 `rime_hello` launcher (it links `core` and prints the version); the real
frame-loop framework is built at M5, once the render graph gives it something to drive.

## 4. Threading model ⚪

Rime is **job-system-centric**. The frame is not a single thread doing everything in
order; it is a graph of jobs scheduled across all cores by `core`'s work-stealing
scheduler. Render-graph passes, ECS system updates, physics islands, and asset
streaming are all jobs. Consequences for everyone writing engine code:
- No hidden global mutable state; no assumed execution order.
- Data is owned clearly so it can be touched in parallel safely.
- Synchronization is explicit (and rare on hot paths).

## 5. The Rust tooling layer (`tools/`) ⚪

The **editor**, **asset pipeline** (importers, bakers, cookers), and **CLIs** are
written in Rust for memory safety and modern tooling. They communicate with the C++
engine across *stable* boundaries (a C ABI, files/formats, or a command protocol) —
never by reaching into engine internals. Rationale and the FFI strategy:
→ [ADR-0001](adr/0001-cpp-core-rust-tooling.md).

## 6. Build system ⚪

- **Engine:** CMake (with presets) builds the C++ modules. Each `engine/<module>` is a
  CMake target; the top-level `CMakeLists.txt` wires them together. Backends (e.g.
  Vulkan) are selectable options.
- **Tools:** Cargo builds the Rust workspace under `tools/`.
- **Glue:** `scripts/` provides cross-platform setup (toolchain + Vulkan SDK discovery)
  and convenience wrappers.

## 7. How the dream maps to the build order

We deliberately build foundations before features. The render graph exists *before* the
fancy lighting; the part/physics model exists *before* spectacular destruction; the
module system and ECS exist *before* almost anything. The detailed sequence lives in
[ROADMAP.md](ROADMAP.md). The point of this document is that **every one of those later
features already has a home and a seam waiting for it.**
