# Glossary

Rime is meant to be *learned from*. This glossary explains the engine terms used across
the docs and code in plain language. If you hit a term that isn't here, add it — that's
a perfect first contribution.

Entries are grouped roughly by area and kept short on purpose.

## Architecture & core

- **Engine vs. game.** The *engine* is the reusable machinery (rendering, physics,
  audio…). A *game* is built on top of it. Rime is the engine.
- **Module / Gem.** A self-contained chunk of engine functionality behind an interface,
  loadable at runtime. "Gem" is O3DE's name for the idea; Rime just says "module."
- **Core / kernel.** The lowest layer everything depends on: memory, math, containers,
  the job system, logging, reflection, and the module loader.
- **Handle (generational).** A small, copyable id that refers to an element in a
  container instead of a raw pointer — an `index` plus a `generation` stamp. Survives the
  container relocating its storage, and detects *use-after-free*: reusing a slot bumps its
  generation, so a stale handle (old generation) is rejected. See
  [design/slot-map.md](design/slot-map.md).
- **Slot map.** A container giving O(1) insert/erase/lookup via generational handles while
  keeping values in a *packed* (gap-free) array for cache-friendly iteration. The backbone
  of handle-based, data-oriented storage (entities, assets, GPU objects).
- **Dense vs. sparse array.** *Dense* = packed with no gaps (great to iterate). *Sparse* =
  indexed by id with holes (great for O(1) lookup). The slot map combines both: a sparse
  slot table redirects handles to values living in a dense array.
- **Swap-and-pop.** O(1) removal from an unordered array: move the last element into the
  hole left by the removed one, then shrink by one. Keeps the array dense; order is not
  preserved.
- **RHI — Render Hardware Interface.** The abstraction that hides the specific graphics
  API. Engine code talks to the RHI; a *backend* (e.g. Vulkan) implements it. Lets us
  add D3D12/Metal later without rewriting the renderer.
- **Backend.** A concrete implementation of an interface (e.g. the *Vulkan backend* of
  the RHI; a *Win32 backend* of the platform layer).
- **FFI — Foreign Function Interface.** How code in one language (Rust tools) calls code
  in another (C++ engine). The boundary we keep stable and explicit.
- **ABI — Application Binary Interface.** The binary contract (layout, calling
  convention) two compiled modules use to interoperate. A *C ABI* is the stable lowest
  common denominator across languages.

## The world

- **ECS — Entity-Component-System.** A data-oriented way to build a game world.
  *Entities* are just ids. *Components* are plain data attached to entities (Position,
  Velocity, Mesh…). *Systems* are functions that run over all entities with a given set
  of components. Fast (cache-friendly) and easy to extend.
- **Data-oriented design (DOD).** Designing around how data is laid out in memory and
  processed in bulk, rather than around object hierarchies. The key to engine
  performance and parallelism.
- **Scene / world.** The collection of entities/components representing what currently
  exists in the game.

## Rendering & lighting

- **Render graph / frame graph.** A description of one frame as a graph of *passes* and
  the *resources* (textures/buffers) they read and write. The engine uses it to order
  passes, reuse transient memory, and insert GPU synchronization automatically.
- **Pass.** One step of rendering (e.g. depth pre-pass, lighting pass, post-process).
- **Pipeline (PSO).** A bundle of GPU state (shaders + fixed-function config) compiled
  ahead of time. "PSO" = Pipeline State Object.
- **Shader.** A small program that runs on the GPU (vertex, fragment/pixel, compute…).
- **PBR — Physically Based Rendering.** Shading that models real light/material physics
  so surfaces look correct under any lighting.
- **GI — Global Illumination.** Indirect light: light that bounces off surfaces before
  reaching the eye. "Real-time GI" (Lumen-style) computes this live, without baking.
- **Baking.** Precomputing lighting into textures offline. Fast at runtime but static.
  Rime targets *dynamic* lighting to support destruction changing the scene.
- **Virtualized geometry (Nanite-style).** Rendering enormous geometric detail by
  streaming and culling at very fine granularity, so triangle count stops being the
  budget you fight.
- **Shadow map / Virtual Shadow Map (VSM).** A shadow map is a depth render from a
  light's view used to test what's in shadow. *Virtual* shadow maps provide very high,
  consistent resolution efficiently.
- **Many-lights (MegaLights-style).** Techniques to render very large numbers of
  dynamic, shadow-casting lights affordably.
- **Barrier / synchronization.** Explicit instructions that make the GPU wait until a
  resource is safe to use. Modern APIs (Vulkan) make these the programmer's job; the
  render graph automates them.

## Physics & destruction

- **Rigid body.** A solid object that doesn't deform, simulated with position, velocity,
  mass, and collisions. Debris chunks are rigid bodies.
- **Collision detection / query.** Finding what touches/overlaps/hits what (and
  raycasts/sweeps used by gameplay).
- **Fracture.** Splitting a mesh into pieces, often precomputed, used for destruction.
- **Part-based destruction.** Modeling a destructible as an assembly of breakable parts
  with connectivity, rather than one monolithic object (Frostbite's approach).
- **Health transition.** A destructible part moving between health states; transitions
  can trigger effects, debris, sound, or gameplay (Frostbite term).
- **Determinism / replication.** *Determinism*: the same inputs always produce the same
  result (vital for networked physics). *Replication*: syncing state across the network.

## Performance & threading

- **Job system / task scheduler.** Splits work into many small *jobs* spread across CPU
  cores, often *work-stealing* (idle cores grab jobs from busy ones). The backbone of
  multicore engine performance.
- **Work-stealing deque (Chase-Lev).** The per-worker queue behind work stealing: its
  owner pushes/pops one end (LIFO, uncontended), while idle threads *steal* from the other
  end (FIFO). Lock-free. See [design/work-stealing-deque.md](design/work-stealing-deque.md).
- **Lock-free / atomic / memory ordering.** *Lock-free*: threads coordinate via atomic
  operations instead of mutexes, so no thread waits on another holding a lock. *Memory
  ordering* (relaxed / acquire / release / seq-cst) controls how one thread's memory writes
  become visible to others — the correctness knobs of lock-free code.
- **ABA problem.** A lock-free hazard: a value reads as `A`, changes to `B`, then back to
  `A`, fooling a compare-and-swap into thinking nothing changed. Avoided here by using
  ever-increasing indices that are never reused.
- **Cache-friendly.** Laid out so the CPU's caches are used well (usually contiguous
  arrays processed in order). Often a bigger win than algorithmic cleverness.
- **SIMD.** "Single Instruction, Multiple Data" — CPU instructions that process several
  values at once (e.g. 4 floats). Used heavily in math.
- **Hot path.** Code that runs every frame / very frequently. Where performance rules
  override convenience.

## Project & process

- **ADR — Architecture Decision Record.** A short document capturing one significant
  decision and its trade-offs. See [adr/](adr/).
- **Milestone / brick.** A milestone is a big chunk of the [roadmap](ROADMAP.md); a
  *brick* is a small, individually-planned, reviewable piece of a milestone.
- **Stub.** A placeholder implementation that compiles but isn't real yet. Always
  labeled as such.
