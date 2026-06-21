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
  render graph automates them. Vulkan's modern form is *synchronization2*
  (`VkImageMemoryBarrier2`), which states the source/destination stage+access in one place.
- **SPIR-V.** The binary intermediate language GPUs consume. Rime authors shaders in GLSL
  and compiles them to SPIR-V *at build time* (offline; see
  [adr/0008](adr/0008-offline-shader-compilation.md)), then hands the bytes to the RHI.
- **Command buffer.** A recorded list of GPU commands (begin a render, bind, draw, copy)
  that is built on the CPU and then *submitted* to a queue for the GPU to execute.
- **Swapchain.** The set of images the windowing system shows on screen, cycled
  (*presented*) one per frame. Off-screen rendering needs no swapchain — which is why
  Rime's first-pixels proof can run headlessly in CI (it renders to an image and reads it
  back). Presentation, and thus the swapchain, lands in M3.4 (ADR-0009).
- **Surface (`VkSurfaceKHR`).** The Vulkan handle that ties a swapchain to a specific OS
  window. Rime builds it from `platform::NativeWindow` (the type-erased native handles) —
  the one place the Vulkan backend touches an OS windowing type.
- **Present mode.** How finished frames reach the display. *FIFO* queues them and shows
  one per refresh (vsync, tear-free, always available — Rime's default); *mailbox* keeps
  only the newest (low latency, may drop frames). Off-screen rendering has no present mode.
- **Frames in flight.** Letting the CPU record the next frame while the GPU still works on
  the previous one, instead of stalling. Rime keeps 2, each with its own synchronization
  (an image-available semaphore + an in-flight fence; a per-image render-finished
  semaphore gates the present). The M3.3 off-screen proof, by contrast, submits one frame
  and blocks — the simplest correct model, replaced by this once presentation paces frames.
- **Dynamic rendering.** The Vulkan 1.3 way to render without pre-declared `VkRenderPass`/
  `VkFramebuffer` objects: you simply begin/end rendering against an attachment. Less
  boilerplate, and a clean fit for a render graph. Rime's RHI uses it (ADR-0007).
- **Descriptor / descriptor set.** How a shader is told *which* resources (textures,
  buffers, samplers) to use — a binding table the pipeline reads from. Rime's M3.5 model is
  deliberately minimal: a pipeline that opts in (`sampled_texture`) gets one set holding a
  single *combined image-sampler*, bound per draw; richer per-material sets arrive with the
  render graph (ADR-0010).
- **Combined image-sampler.** One descriptor that bundles a texture (its *image view*) with a
  *sampler* — the least machinery to give a shader something to sample. Vulkan also allows
  separate image/sampler descriptors and bindless arrays; Rime starts combined and grows into
  those at the render graph.
- **Sampler.** The GPU object that says *how* a texture is read: *filtering* (Nearest =
  blocky/exact texels, Linear = smooth interpolation) and *addressing* (what a UV outside
  [0,1] does — repeat, clamp…). Decoupled from the image, so one texture can be read several
  ways.
- **Texel.** A single element of a texture ("texture pixel"); a 2×2 texture has four texels.
- **UV / texture coordinate.** The 2-D coordinate (conventionally `u`,`v` in [0,1]) saying
  where on a texture a vertex samples; the rasterizer interpolates it across a triangle so each
  pixel reads the right texel.
- **Index buffer.** A list of indices into the vertex buffer defining which vertices form each
  triangle, so shared corners are stored once (a quad: 4 vertices + 6 indices, not 6 vertices).
  Used by an *indexed* draw.
- **Staging buffer.** A temporary CPU-visible buffer used to get data into a fast *device-local*
  resource the CPU can't write directly: fill the staging buffer, then copy it across on the
  GPU. Rime uploads textures this way (`write_texture`).
- **Validation layers.** Optional Vulkan layers that check every API call for misuse and
  report errors. Rime enables them in debug builds (off when optimized), so mistakes are
  caught loudly and early.
- **VMA (Vulkan Memory Allocator).** A widely-used library that sub-allocates GPU memory
  from a few large device allocations, so the engine never calls `vkAllocateMemory`
  directly. Rime asks for memory by *access pattern* (GpuOnly / CpuToGpu / GpuToCpu).
- **volk.** A Vulkan "meta-loader": it loads the Vulkan entry points at runtime (and
  per-device), so the engine links no loader at build time. See ADR-0007.
- **Loader / ICD.** The Vulkan *loader* (`libvulkan`) is the library apps call; an *ICD*
  (Installable Client Driver) is an actual implementation it dispatches to — a GPU driver,
  **MoltenVK** (Vulkan-on-Metal, for macOS), or **lavapipe** (Mesa's CPU/software Vulkan,
  used to run Rime's render proof on GPU-less CI machines).

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
- **Fork/join.** A parallel pattern: *fork* work into many jobs, then *join* (wait) until
  they all finish. In Rime a job group shares an atomic counter; `parallel_for` forks one
  job per chunk and joins before returning. See [design/job-system.md](design/job-system.md).
- **Cache-friendly.** Laid out so the CPU's caches are used well (usually contiguous
  arrays processed in order). Often a bigger win than algorithmic cleverness.
- **SIMD.** "Single Instruction, Multiple Data" — CPU instructions that process several
  values at once (e.g. 4 floats). Used heavily in math.
- **Hot path.** Code that runs every frame / very frequently. Where performance rules
  override convenience.

## Platform & OS

- **Monotonic clock.** A clock that only ever moves forward at a steady rate and never jumps
  (unlike wall-clock time, which NTP/DST can shift). The right source for measuring durations and
  frame times — only *differences* are meaningful. Rime's `platform::Clock` wraps
  `std::chrono::steady_clock`.
- **XDG Base Directory.** The Linux/freedesktop convention for where per-user files live: data in
  `$XDG_DATA_HOME` (default `~/.local/share`), config in `$XDG_CONFIG_HOME` (`~/.config`), cache in
  `$XDG_CACHE_HOME` (`~/.cache`). The platform layer follows it on Linux, and the equivalent
  `~/Library` and `%APPDATA%`/`%LOCALAPPDATA%` conventions on macOS and Windows.
- **Windowing system / backend.** The OS service that owns on-screen windows and input. Rime has a
  native backend per system — **Win32** (Windows), **Cocoa** (macOS), and on Linux both **X11** (the
  long-standing X Window System) and **Wayland** (its modern replacement) — all behind one `Window`
  interface; Linux picks Wayland or X11 at runtime.
- **Compositor (Wayland).** The Wayland display server: it owns the screen and composites client
  surfaces. Wayland is a *protocol*, so a client binds the interfaces it needs (compositor, shell,
  seat) from a registry and communicates through asynchronous messages rather than direct calls.
- **xdg-shell.** The Wayland protocol extension that gives a bare surface a desktop-window role —
  title, move/resize, minimize/close — via `xdg_surface` + `xdg_toplevel`. Its client code is
  generated from an XML description by `wayland-scanner` at build time.
- **DPI / content scale.** The ratio between physical pixels and layout points on HiDPI/Retina
  displays. A window reports its *framebuffer* size (real pixels, what the swapchain uses), its
  *logical* size (points/DIPs), and their ratio (`content_scale()`).

## Project & process

- **ADR — Architecture Decision Record.** A short document capturing one significant
  decision and its trade-offs. See [adr/](adr/).
- **Milestone / brick.** A milestone is a big chunk of the [roadmap](ROADMAP.md); a
  *brick* is a small, individually-planned, reviewable piece of a milestone.
- **Stub.** A placeholder implementation that compiles but isn't real yet. Always
  labeled as such.
- **Reflection.** The ability to inspect a type's structure (its fields, their names,
  types, and offsets) at runtime. Lets generic code — serializers, editor inspectors —
  work on any registered struct without per-type boilerplate. See
  [design/reflection.md](design/reflection.md).
- **Serialization.** Turning in-memory data into a flat byte stream (and back), e.g. to
  save a scene or cook an asset. Rime's is reflection-driven: register a struct and it
  serializes for free.
