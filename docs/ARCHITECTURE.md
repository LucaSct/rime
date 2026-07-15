# Rime Architecture

This document explains **how Rime is structured and why**. It is written to be read by
someone learning how engines work, so it defines ideas as it goes. It describes the
*intended* architecture; much of it is not built yet. Sections are tagged:

- 🟢 **Built** — exists and works.
- 🟡 **Designed** — the shape is decided; code is partial or stubbed.
- ⚪ **Planned** — intended; not started.

As of 2026-07 the **bottom of the layer cake is built and CI-green on Windows, Linux, and
macOS**: `core` 🟢 and `platform` 🟢 (Milestones 0–2), `rhi` 🟡 (Milestone 3 + the M5.1–M5.3
renderer top-ups), `ecs` 🟢 (Milestone 4), `render` 🟢 (**Milestone 5 complete** — the M5.4 render
graph, M5.5 scene layer, and M5.6 forward-PBR pipeline, shown by `samples/06`/`07` and the M5.9
dogfood test), `stream` 🟡 (Track S0), and `app` 🟡 (the M5.7 fixed-tick loop). Of the feature
modules, `assets` 🟢 is **Milestone 6 complete** — the whole offline→runtime asset pipeline (glTF +
STL import; textured, normal-mapped, and skinned cooks; async loading on the job system; the GPU
asset bridge), shown end-to-end by `samples/08-gltf-zoo`; the new `capi` 🟢 C ABI (M6.9) exposes the
runtime loader to the Rust tools, and the `tools/` layer is real (the `rime` cooker + the pipeline
and FFI crates). `physics` 🟢 is the **Milestone 7 core** — an own rigid-body engine (bodies,
broadphase/narrowphase/solver, islands + sleeping, the parallel step, ECS sync, and scene queries),
its "done when" shown by `samples/09-physics-playground`. The rest of the feature modules are still
⚪. This document is
the blueprint we build toward; the per-section tags below say how far each part has actually come.

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
        │  capi  (C ABI) ── the stable FFI seam the tools call    │
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
Extra features landed to serve the ICEM viewer: depth attachment (ADR-0011), push constants
(ADR-0012), 3-D/volume textures (ADR-0013), and stencil (ADR-0014). *The M5.1–M5.3 renderer
top-ups then closed the pre-render-graph gaps:* a **descriptor model v2** with declared binding
layouts, uniform buffers, and per-frame transient sets (ADR-0020); **blending, multiple render
targets, and RGBA16Float HDR** (M5.1b); **compute pipelines + storage resources** (ADR-0021);
**mipmaps + anisotropy**, and **GPU timestamps + debug labels** (M5.3). *Remaining gaps (all
deliberately deferred):* no MSAA, one queue (no async compute), a fixed two frames in flight, and
single-threaded command recording — the render graph keeps the seams for these. → [ADR-0002](adr/0002-vulkan-first-rhi.md),
[ADR-0007](adr/0007-vulkan-backend-bootstrapping.md), [design/rhi.md](design/rhi.md)

### `ecs` 🟢 — *the world, as data*
An **Entity-Component-System**: entities are ids; components are plain data stored in
tight arrays; systems are functions that run over matching component sets, ideally in
parallel via the job system. This is how we get both performance *and* a model that's
easy to reason about and extend. *Built (M4, complete):* generational entities +
reflection-registered components, archetype/SoA chunk storage on `core`'s allocators
(ADR-0018), queries with `par_for_each` chunk parallelism, the access-set `Schedule`,
deferred structural changes (`CommandBuffer`), and the parallel transform hierarchy —
proven by 200k entities stepping at ≈10× on 16 cores (`samples/05-ecs-playground`).

### `render` 🟡 — *where the picture comes from*
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
*Built (M5.4–M5.9):* the **render graph v0** — frame-declared raster/compute passes, virtual
resources with a cross-frame transient cache, declared access driving order, culling, and
**graph-owned barriers** through the RHI's `texture_barrier` seam, with per-pass GPU timestamps
+ debug labels (ADR-0019, [design/render-graph.md](design/render-graph.md)); the **scene layer**
(M5.5) — the graduated orbit camera, procedural meshes, mesh/material registries, and
reflection-registered ECS render components; and the **forward-PBR pass library** (M5.6, ADR-0022,
[math/pbr.md](math/pbr.md)) — depth pre-pass → Cook-Torrance HDR → tonemap, plus a `SceneRenderer`
that extracts a World into that frame. Proven on screen by `samples/06-render-graph` and
`samples/07-first-light` (M5's "done when": a lit PBR scene, headless or streamed). **M5.9 closes
the milestone** with a dogfood-acceptance test (ADR-0016 rule 4) that re-expresses the ICEM
viewer's cross-section frame — clip-planed lit mesh → stencil cut-mark → solid cap → alpha-tested
UI overlay — as four graph passes sharing one colour + one D32FloatS8 depth+stencil target,
proving the resource model really covers depth+stencil attachments and Load/keep-across-passes
(`tests/render/viewer_frame_graph_test.cpp`, offscreen, GPU-free in CI).

### `physics` 🟢 — *simulation, multicore-first*
Rime's **own** rigid-body core — no Jolt ([ADR-0026](adr/0026-physics-core.md)): a SoA body pool, a
dual-AABB-tree broadphase, a GJK/EPA narrowphase, a sequential-impulse solver with an NGS position
pass, and **island-parallel stepping that is bit-identical across thread counts**. It steps on
`core::JobSystem` inside the app's fixed tick, syncs to the ECS with change detection (awake bodies
only), and answers raycast/overlap queries — all behind the `PhysicsWorld` seam, all GPU-free.
Built as the *universal simulation substrate* the destruction system (M8) sits on. M7's "done when"
is proven by `samples/09-physics-playground`; contact events, convex/mesh shapes, and CCD are the
planned fast-follows. → see [design/physics.md](design/physics.md),
[survey](research/engine-survey.md#physics).

### `destruction` 🟡 — *our headline system (Frostbite-inspired)*
*Milestone 8 underway ([ADR-0029](adr/0029-destruction-model.md)): a cooked fracture pattern stands as
one static compound body (M8.2), damage → connectivity → the fracture body-swap detaches real debris
(M8.3), and a canonical event stream fans out to VFX / audio / gameplay (M8.4). Lifetime/budgets
(M8.5) and the headless sample proof (M8.6) remain.* A first-class engine system, not a plugin. Core
ideas, mapped from how Battlefield 6's Frostbite does it:
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

### `audio` 🟡 — *the sound seam*
*The interface exists (M8.4): `AudioBackend::play(sound, position, gain)` and a `NullAudioBackend`
that logs calls, so the destruction fan-out has a stable boundary to call and audio is testable
headless. The real mixing/spatialization backend (track au1) swaps in behind the interface.*

### `vfx` 🟡 — *the effects seam (dust stub today)*
*A deletable CPU dust field (M8.4): destruction events bloom capped, deterministic billboard puffs. A
stub — the real GPU-driven FX system (track fx1) replaces it; its actual draw pass lands with the M8.6
sample.*

### `animation` ⚪, `net` ⚪
Skeletal animation & blending; networking/replication. Each behind its own interface.

### `assets` 🟢 — *cooked-asset loading (files are the boundary)*
The runtime side of the asset pipeline: open cooked binary files, validate them completely, and hand
back typed, registry-owned assets. All importing/cooking is offline in Rust (`tools/asset-pipeline`);
the engine ships **no** source-format parser and loads only cooked files ([ADR-0024](adr/0024-asset-model.md)).
*Built (M6, complete):* the **RMA1 container reader** — a versioned header + kind-specific payload,
little-endian field-by-field, **bounds-checked before every allocation** (cooked bytes are trusted no
more than network bytes); the **AssetRegistry** — handle-based ownership with **content-hash
de-duplication**; and the plain-text **cook manifest** reader (M6.1). Meshes, textures (with their
offline mip chains), PBR materials, and skeletons + animation clips all have readers (M6.2–M6.4, M6.7);
cooked meshes carry an attribute-flags vertex layout so tangents and skinning extend the format without
a container break, plus a reflection **schema hash** so a file cooked against an old layout is rejected
rather than misread. The **AssetServer** runs IO/parse as jobs with placeholder assets and a
frame-point drain (M6.5); the **GpuAssetBridge** uploads a cooked texture's mip chain and swaps a
material's placeholder for the real handle (ADR-0025). Depends only on `core` + `platform` — the
renderer consumes assets, never the reverse. Hot reload is a documented seam, not yet a feature. →
[ADR-0024](adr/0024-asset-model.md), [design/assets.md](design/assets.md).

### `stream` 🟡 — *graphics streaming (Track S)*
Capture a rendered frame, encode it, and ship it to a thin remote client that presents it and sends
input back — **one stack** for dev streaming (now), the editor viewport (M9), and remote play (later).
*Built (S0.2):* the **frame tap** (`FrameStreamer` — RHI readback, double-buffered, measured).
*Built (S0.3):* the **codec** (`FrameEncoder`/`FrameDecoder`) — JPEG (libjpeg-turbo) on the wire, LZ4
for lossless, chosen by measurement ([ADR-0017](adr/0017-streaming-codec.md)); both libraries stay
hidden behind the public header. *Built (S0.4):* the **protocol** (`ProtocolConnection` +
`FrameMessage`/`InputEvent`) — a versioned, length-prefixed message stream over the platform TCP
sockets, the same wire the M9 editor viewport will ride. A *removable* feature module that depends
only on the RHI interface + the platform transport, so it captures from any backend. →
[ADR-0016](adr/0016-editor-is-a-client-of-the-engine.md),
[design/graphics-streaming.md](design/graphics-streaming.md).

### `app` 🟡 — *the application framework & main loop*
Ties a module set together into a runnable application: owns the `JobSystem`, ECS `World` + sim
`Schedule`, and (optionally) an `rhi::Device` + render graph, and runs the frame loop. *Built
(M5.7, headless):* `Application` with a **fixed simulation tick decoupled from the render frame**
— the sim advances in equal `fixed_dt` steps via a time accumulator, the render callback runs once
per frame with an interpolation alpha, and the whole thing is provable GPU-free (ADR-0023). The
determinism this buys is the multiplayer (M11) seam. The windowed/swapchain *present* path is a
documented seam (filled on a display); `rime_hello` stays the trivial M0 launcher. →
[ADR-0023](adr/0023-app-fixed-tick-loop.md)

### `capi` 🟢 — *the C ABI (the FFI seam)*
A thin C-linkage shared library (`librime_capi`) that re-exports a hand-picked slice of the engine
across a stable, opaque-handle boundary — the one place Rust tools (and any other language) reach the
engine at runtime, per ADR-0001's "files and a C ABI, never internals." *Built (M6.9):* an opaque
handle API over the asset loader that the `tools/rime-ffi` crate drives in its own tests, plus a
reserved protocol message-type space for the M9 editor channel. This is Rime's **first shared
library**, so it set the project-wide policy of position-independent code + hidden default visibility
(only the annotated `RIME_CAPI` symbols are exported). Installs as `rime::capi`. →
[design/ffi.md](design/ffi.md), [ADR-0001](adr/0001-cpp-core-rust-tooling.md).

## 4. Threading model ⚪

Rime is **job-system-centric**. The frame is not a single thread doing everything in
order; it is a graph of jobs scheduled across all cores by `core`'s work-stealing
scheduler. Render-graph passes, ECS system updates, physics islands, and asset
streaming are all jobs. Consequences for everyone writing engine code:
- No hidden global mutable state; no assumed execution order.
- Data is owned clearly so it can be touched in parallel safely.
- Synchronization is explicit (and rare on hot paths).

## 5. The Rust tooling layer (`tools/`) 🟡

The **editor**, **asset pipeline** (importers, bakers, cookers), and **CLIs** are
written in Rust for memory safety and modern tooling. They communicate with the C++
engine across *stable* boundaries (a C ABI, files/formats, or a command protocol) —
never by reaching into engine internals. Rationale and the FFI strategy:
→ [ADR-0001](adr/0001-cpp-core-rust-tooling.md).

*Built (M6):* the **`asset-pipeline`** crate (glTF + STL import → RMA1 cook, content-hash cache) and
the **`rime`** CLI (`rime cook`/`inspect`) — the offline half of the asset boundary the engine's
`assets` module reads; and the **`rime-ffi`** crate, whose tests drive the engine through the `capi`
C ABI. The **editor** is still ⚪ (M9). See [tools/README.md](../tools/README.md) and the per-crate
READMEs.

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
