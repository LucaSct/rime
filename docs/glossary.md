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
- **SDK — Software Development Kit.** Rime's installed, out-of-tree form: `cmake --install`
  exports the engine libraries + headers so a *separate* project can `find_package(rime CONFIG)`
  and link `rime::core`, `rime::assets`, … (M6.8). The C ABI ships in it as `rime::capi`.

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
- **Change detection.** Tracking *what changed since when* so a consumer processes only the
  deltas instead of re-scanning the world. Rime stamps every chunk's component *columns* with a
  monotonic world *version* (bumped once per tick); a query's `for_each_changed(since, …)` skips
  any chunk whose queried columns were all last written at or before `since`. The grain is the
  chunk column — *conservative*: it never misses a change but may include unchanged neighbours
  sharing a changed chunk. Powers GPU re-upload, editor live-sync (M9), and networked-destruction
  deltas (M11). See [adr/0018 §4](adr/0018-ecs-storage-model.md) and
  [design/simulation-tick.md](design/simulation-tick.md).
- **Fixed timestep / simulation tick.** Advancing the simulation in equal-sized steps
  (Rime's default: 60 per second) that are *decoupled* from the render frame rate — a time
  accumulator runs whole ticks and carries the remainder. It makes the sim deterministic
  (its state is a pure function of the tick count, not of frame pacing), which is what
  networked play (M11) needs; the render frame interpolates between ticks. See
  [adr/0023](adr/0023-app-fixed-tick-loop.md) (M5.7).
- **Spiral of death.** The failure a fixed timestep must guard against: if one frame takes
  longer to simulate than a tick, the next frame owes more ticks, which take longer still.
  Rime clamps ticks-per-frame and drops the backlog — the sim slows rather than freezing.

## Rendering & lighting

- **Render graph / frame graph.** A description of one frame as a graph of *passes* and
  the *resources* (textures/buffers) they read and write. The engine uses it to order
  passes, reuse transient memory, and insert GPU synchronization automatically. Rime's is
  *frame-declared* — rebuilt each frame from setup+execute lambdas (ADR-0019, M5.4).
- **Transient resource.** A texture/buffer that lives only within a frame: a render-graph
  pass *describes* it (extent, format), and the graph backs it with physical GPU memory
  drawn from a cross-frame cache and recycled once no live pass needs it. You never
  allocate or free it yourself. *Imported* resources (a swapchain image, a history buffer)
  are the opposite — externally owned, wrapped so passes can name them.
- **Pass.** One step of rendering (e.g. depth pre-pass, lighting pass, post-process).
- **Depth pre-pass.** A first pass that writes *only* depth, so the expensive shading pass
  then shades each visible pixel exactly once — its `Equal` depth test rejects everything
  hidden — instead of shading overdrawn fragments. Optional per frame; a win only when
  overdraw and shading cost are high enough to measure (M5.6).
- **Pipeline (PSO).** A bundle of GPU state (shaders + fixed-function config) compiled
  ahead of time. "PSO" = Pipeline State Object.
- **Shader.** A small program that runs on the GPU (vertex, fragment/pixel, compute…).
- **PBR — Physically Based Rendering.** Shading that models real light/material physics
  so surfaces look correct under any lighting. Rime's is a forward Cook-Torrance path
  (ADR-0022); the full derivation is [math/pbr.md](math/pbr.md).
- **BRDF — Bidirectional Reflectance Distribution Function.** The function at the heart of
  PBR: for an incoming and an outgoing direction, how much light the surface bounces
  between them. Rime uses the Cook-Torrance microfacet BRDF.
- **Cook-Torrance / microfacet model.** Treats a rough surface as countless microscopic
  mirrors; the specular highlight is the statistics of how many face the halfway direction
  (the GGX *distribution* D), how many are shadowed/masked by neighbours (the Smith
  *geometry* G), and how reflective each is at this angle (the *Fresnel* F).
- **Metallic-roughness.** The material parameterization Rime and glTF share: a base color,
  `metallic` 0..1 (dielectric vs. metal), and `roughness` 0..1 (mirror vs. matte). Two
  sliders span most real opaque surfaces.
- **HDR — High Dynamic Range.** Carrying light values above 1.0 (a highlight can be many
  times a dim wall) in a float target (RGBA16Float) instead of clamping, so the tonemap
  can map that range down later without having thrown the bright detail away (M5.1b/M5.6).
- **Tonemapping.** Compressing unbounded HDR radiance into the [0,1] a display shows via a
  curve — Rime ships an ACES filmic fit: a gentle toe keeps shadow contrast, a long
  shoulder rolls highlights off instead of clipping them to flat white.
- **Linear vs. sRGB.** Lighting must be computed in *linear* light (2× the value = 2× the
  photons); displays and most color textures use the perceptual *sRGB* curve. Rime samples
  sRGB textures (the hardware decodes to linear), shades linear, and sRGB-encodes once at
  the very end ([math/pbr.md](math/pbr.md) §display).
- **Colour space (of a texture).** Whether a texture's bytes are perceptual *sRGB* colour
  (baseColor, emissive) or *linear* data (normal / metallic-roughness / occlusion maps — the
  number *is* the quantity). Cooked textures tag which (`RGBA8Srgb` / `RGBA8Unorm`), because it
  changes two things: the GPU decodes sRGB on sampling but not linear, and **mips must be filtered
  in linear light** either way.
- **Gamma-correct mip generation.** Averaging sRGB-encoded bytes directly is a category error —
  it averages perception, not light — and makes every minified colour surface too dark (the classic
  "dark mipmaps"). The fix: linearise, average, re-encode. Rime's cooker does this offline (M6.3);
  a black/white checker's coarse mip is sRGB ~188, not the too-dark 128 the naive average gives.
- **Normal mapping.** Faking fine surface detail (bumps, pores) by storing a per-texel *normal* in a
  texture and shading with it, instead of adding triangles. The stored normals live in tangent space,
  so the same map reuses across a surface facing any direction (M6.4).
- **Tangent space / TBN.** The per-point orthonormal frame a normal map is written in: **T**angent
  (the direction texture `u` increases), **B**itangent (`v` increases), and the surface **N**ormal.
  A flat texel is `(0,0,1)` = the byte triple `(128,128,255)`. Rime ships one tangent per vertex (a
  4×f32: `xyz` + a handedness sign in `w`, so the bitangent is `w·(N×T)`); the full derivation is
  [math/tangent-space.md](math/tangent-space.md).
- **MikkTSpace.** The de-facto *standard* algorithm for generating per-vertex tangents (Morten
  Mikkelsen's). Because a normal map is baked against a specific tangent basis, both baker and renderer
  must agree on it or seams glow and lighting slides; glTF mandates MikkTSpace for meshes that omit
  tangents, so Rime's cooker generates with it.
- **AO — Ambient occlusion.** A texture (or computed term) darkening creases and contact points that
  self-shadow from *ambient/indirect* light. It multiplies the indirect term only — never direct
  lights, which have their own real shadows — or contact edges look wrong (M6.4).
- **Emissive.** A material's own emitted light (glowing screens, lava), added after the BRDF so it
  shows even with no light hitting the surface. Linear RGB; glTF default is black (no emission).
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
- **Mipmap / mip chain.** A precomputed chain of half-size copies of a texture (level 0 full
  size, each level halved down to 1×1); the GPU samples the level matching a surface's on-screen
  size, so a texture seen small doesn't shimmer/alias. Rime makes chains two ways: for runtime
  textures, successive GPU *blits* inside `write_texture` (M5.3); for **cooked** textures, the
  offline pipeline generates the chain gamma-correctly and the engine uploads it verbatim via
  `write_texture_mips` (M6.3) — no on-device regeneration, so the correct (linear-filtered) mips
  are the ones sampled.
- **Anisotropic filtering.** Sharper texture sampling on surfaces viewed at a grazing angle
  (a floor receding to the horizon), where an isotropic mip lookup would over-blur along
  the stretched direction. The sampler takes a `max_anisotropy` (M5.3).
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
- **ECS↔physics sync.** The bridge (`PhysicsSync`) that keeps game data (entities with
  RigidBody/Collider/WorldTransform components) and the simulation (a world of rigid bodies) in
  step each tick: *bind* new intent entities to freshly-created bodies, `step()`, then *write back*
  each **awake** body's pose into its WorldTransform (stamping it for [change
  detection](#the-world)). A sleeping body writes nothing, so a settled world costs a change-tracking
  consumer nothing. Canonical tick order: [design/simulation-tick.md](design/simulation-tick.md).
- **Collision detection / query.** Finding what touches/overlaps/hits what (and
  raycasts/sweeps used by gameplay).
- **Broadphase / narrowphase.** The two stages of collision detection. *Broadphase* quickly finds
  pairs that *might* touch — Rime walks a **dynamic AABB tree** (a bounding-volume hierarchy) instead
  of testing all n² pairs. *Narrowphase* then does the exact geometry test on each candidate:
  **GJK + EPA** for general convex shapes, analytic fast paths for spheres/capsules, producing a
  contact *manifold*. See [design/physics.md](design/physics.md), [math/gjk-epa.md](math/gjk-epa.md).
- **Contact manifold.** The narrowphase's output for a touching pair: a shared contact normal plus up
  to four contact points (four is enough to keep a resting box from tipping). The solver's input.
- **Convex hull.** The smallest convex shape containing a set of points — no dents, so any two points
  inside connect without leaving it. The collision shape of destruction: fracture tools cut walls into
  convex pieces because convex-vs-convex collision is fast and robust (one GJK support function covers
  them all). In Rime, hull geometry is registered once with the physics world and referenced by a small
  id ([ADR-0027](adr/0027-convex-hull-shapes.md)); *building* a hull from points (**quickhull**) is a
  cook-time job, not a runtime one.
- **Compound shape.** One rigid body whose collision shape is *several* convex children (primitives
  and/or convex hulls), each at a fixed local pose — how an intact destructible is one body whose
  parts detach when it breaks. Registered once with the physics world like a hull and referenced by a
  small id; its mass and inertia are composed from the children by the **parallel-axis theorem**, and
  a body pair involving a compound can carry several contact manifolds — one per touching child pair
  ([ADR-0028](adr/0028-compound-shapes.md), [math/compound-mass-properties.md](math/compound-mass-properties.md)).
- **Parallel-axis theorem.** Moving an inertia measurement away from a body's centre of mass adds
  `m·d²`-style terms: a mass carried at an offset is harder to spin about the distant axis. The tool
  that lets a compound's inertia be *composed* from its children's own inertias exactly, with no new
  integration.
- **Sutherland–Hodgman clipping.** Cutting a polygon against a half-space by walking its edges: keep
  inside vertices, emit a new vertex where an edge crosses the boundary. Run against several planes in
  a row it trims a polygon to a region — how the narrowphase turns "these two faces touch" into the
  actual overlap patch (the contact points).
- **Inertia tensor / principal axes.** The 3×3 matrix version of "how hard is this body to spin" — it
  can differ per axis (a plank spins easily along its length). Every solid has three perpendicular
  **principal axes** in which that matrix is diagonal (three numbers, the *principal moments*); Rime
  diagonalizes a hull's tensor once at registration (a **Jacobi** eigendecomposition) and stores the
  diagonal plus the axes' rotation, so the solver keeps its cheap diagonal math for every shape. See
  [math/polyhedral-mass-properties.md](math/polyhedral-mass-properties.md).
- **Solver (sequential impulse).** The stage that turns manifolds into motion by applying *impulses*
  at the contacts — stopping penetration, applying friction — sweeping over them repeatedly
  (projected Gauss-Seidel) until velocities agree. A separate **NGS** pass then removes leftover
  overlap without injecting energy (deliberately not *Baumgarte*).
  See [math/sequential-impulse.md](math/sequential-impulse.md).
- **Warm starting.** Seeding this tick's solver with the impulses it converged to last tick (matched
  by a stable per-contact *feature id*), so a resting stack starts near its answer instead of from
  zero — far fewer iterations to stay stable.
- **Island.** A connected group of bodies that touch, directly or through a chain, solved as one
  independent unit. Islands share no dynamic body, so they solve in **parallel with a bit-identical
  result**, and one that comes to rest can *sleep* as a whole.
- **Sleeping.** Deactivating a body (or whole island) that has stopped moving so it costs nothing to
  step, until something disturbs it. A settled world does no work — and, paired with
  [change detection](#the-world), dirties nothing for its consumers.
- **Static / kinematic / dynamic.** A body's motion class. *Static* never moves (the level). *Dynamic*
  is fully simulated (gravity, forces, contacts). *Kinematic* is moved directly by the game (a moving
  platform, an animated door): it pushes dynamic bodies but is not pushed back.
- **Raycast / scene query.** Asking the world a geometric question without stepping it: a *raycast*
  finds the nearest body a ray hits (hitscan weapons, line-of-sight, mouse picking); an *overlap*
  finds bodies inside a volume (an explosion radius). Both ride the broadphase tree, so they cost
  O(log n), not a scan of every body.
- **CCD — continuous collision detection.** Catching collisions a fast, thin body would *tunnel*
  through in one step (a bullet through a wall) by sweeping its motion rather than testing only its
  end pose. Planned for the physics core (speculative contacts); not yet built.
- **Fracture.** Splitting a mesh into pieces, often precomputed, used for destruction.
- **Part-based destruction.** Modeling a destructible as an assembly of breakable parts
  with connectivity, rather than one monolithic object (Frostbite's approach).
- **Fracture pattern.** The cooked recipe for one destructible: its convex **parts**, the
  **bond** graph between them, and the **anchors** — authored/cooked once, instanced many
  times. In Rime an intact instance is one static *compound shape* built from this pattern.
- **Part.** One convex piece of a destructible (a fracture cell). Intact, it is a child of
  the instance's compound body, not a simulated entity; when it detaches it becomes a real
  physics body (a hull, or a compound if several parts detach together). See
  [ADR-0029](adr/0029-destruction-model.md).
- **Bond / anchor.** A *bond* is the glue between two touching parts, with a strength (from
  the cook, roughly the shared area); an *anchor* is a part pinned to the world (a wall's
  base). Damage removes bonds; a **connectivity solve** (union-find over the live bonds)
  finds parts no longer connected to any anchor — those detach and fall.
- **Body swap (fracture transition).** How a Rime destructible actually breaks (ADR-0029 §2):
  registered compound shapes are immutable, so a damaged wall can't lose a child in place —
  instead its body is *replaced*: destroy it, re-register the anchored remainder as a fresh
  compound, and spawn each unsupported island as a new dynamic body. Placed by one COM recipe
  so nothing visibly moves on the swap tick.
- **Debris freeze / lifecycle.** Reclaiming a settled debris body so the physics stores stay bounded
  under continuous refracture (M8.5): once it comes to rest and lingers, its body is destroyed and any
  runtime compound it owns is unregistered, while its roster record is kept (so debris ids never
  shift). A live-body cap freezes the least-interesting settled debris early — deterministically, with
  camera distance deliberately excluded.
- **Render leaf.** A per-part render entity — one renderable per destructible part — that follows the
  part each frame: its standing placement while intact, or its debris body's pose once it detaches
  (M8.6, ADR-0029 §5).
- **Health transition.** A destructible part moving between health states; transitions
  can trigger effects, debris, sound, or gameplay (Frostbite term).
- **Destruction event.** What a break reports each tick as *data* (never a mid-solve callback),
  published in a canonical, replay-stable order (M8.4, ADR-0029 §7). Four kinds: **PartDamaged**
  (a part took damage and stands), **PartDied** (its health hit zero — it leaves as debris),
  **IslandDetached** (a still-standing group lost support and broke free as one debris body), and
  **DebrisSettled** (a debris body came to rest — off a physics *sleep* event). Each carries a
  world-space AABB (the M10-C2 hook). One stream fans out to VFX, audio, and gameplay.
- **EventChannel.** A generic double-buffered event queue (`core::EventChannel<T>`): a system
  *pushes* typed events, *publishes* once at a tick boundary, and consumers read the published batch
  as a stable span until the next publish. The M7.9 "events are data read after the step" pattern,
  generalized for every producer above physics (destruction first).
- **Debris.** A physics body spawned when part(s) detach from a destructible on fracture —
  real dynamic geometry that falls, collides, and eventually *settles* (a sleep event) so
  it can be budgeted or frozen.
- **Determinism / replication.** *Determinism*: the same inputs always produce the same
  result (vital for networked physics). *Replication*: syncing state across the network.

## Assets & the pipeline

- **Asset.** A piece of content the engine consumes — a mesh, texture, material, sound,
  animation clip, destructible. Source assets (glTF, PNG, STL…) are what tools edit;
  the engine only ever loads *cooked* assets.
- **Asset id (`AssetId`).** An asset's stable identity — its content hash (below). The
  runtime registry keys on it (so identical content loads once), and the manifest maps each
  source path to the id of its cooked file.
- **Import → cook.** *Import* parses a source format; *cooking* transforms it into the
  engine's own binary layout (mips generated, tangents computed, data validated) so the
  runtime does zero parsing work. Rime cooks offline in Rust (`rime-cli cook`); the C++
  engine never contains a glTF/PNG parser (ADR-0024).
- **Cooked container (`RMA1`).** Rime's on-disk asset format: a small versioned header
  (magic, kind, schema hash, payload size) read field-by-field with every length checked —
  the same trust-nothing discipline as the streaming protocol.
- **Content hash.** A fingerprint (FNV-1a 64 here) computed from an asset's cooked bytes,
  used as its identity: identical content ⇒ identical id, edits ⇒ a new id. Powers the
  cook cache and duplicate detection, and survives renames.
- **Manifest.** The plain-text index a cook emits: one line per asset mapping source path →
  kind, asset id, cooked file. Regenerable at any time (derived data — it can't lie);
  read by the runtime registry and, later, the editor's asset browser.
- **Schema (type) hash.** A hash of a reflected type's field names/types/order, embedded
  in cooked data; a mismatch at load means "the code moved on — re-cook," caught cleanly
  instead of misreading bytes.
- **Skeleton / joint (bone).** A *skeleton* is a hierarchy of *joints* (bones) a character
  mesh deforms with. Each joint has a parent (or is a root) and a rest placement; Rime stores
  joints *parent-before-child* (topological order) so animating them is one forward pass.
- **Skinning / skin weights.** Binding mesh vertices to joints so they follow the skeleton.
  Each vertex names up to four joints and a *weight* per joint (weights sum to 1); Rime's
  default is **linear blend skinning (LBS)** — a vertex's position is the weighted average of
  its position as moved by each joint. Cheap and standard; it pinches at extreme bends (the
  known LBS artifact later dual-quaternion skinning can fix).
- **Bind pose / inverse-bind matrix.** The *bind pose* is the skeleton's rest pose, the one
  the mesh was authored against. A joint's *inverse-bind matrix* takes a vertex from model
  space into that joint's rest-local frame, so that re-placing the joint (animated) moves the
  vertex correctly. Derived in [math/skinning.md](math/skinning.md).
- **Skinning palette.** The per-frame array of one matrix per joint (`world · inverse-bind`)
  the skinning shader multiplies vertices by. Rime's CPU sampler produces it at M6.7; GPU
  palette skinning (AN1) consumes the same array.
- **Animation clip / keyframe.** A *clip* is a timed set of per-joint tracks (translation /
  rotation / scale); a *keyframe* is one sampled value at one time. Sampling reads the
  bracketing keys and blends them — **STEP** holds the previous key, **LINEAR** interpolates
  (nlerp for rotations, along the shortest arc). Looping vs clamping is chosen at play time.

## Streaming & codecs

- **Codec.** Coder-decoder: shrinks a frame for the wire and reconstructs it. Rime's
  streaming track (Track S) uses four, chosen by measurement: **Raw** (baseline), **LZ4**
  (lossless/local), **JPEG** (the S0 intra wire codec), and **AV1** (the S1 inter-frame
  wire codec). See [ADR-0017](adr/0017-streaming-codec.md), [ADR-0030](adr/0030-streaming-v1.md).
- **Intra-frame vs inter-frame.** An *intra* codec (JPEG) compresses each frame alone. An
  *inter-frame* codec (AV1) also encodes what *changed* since earlier frames, so a
  mostly-static view costs a fraction of the bandwidth — the big S1 win, at the price of
  per-stream *state*.
- **Keyframe (I-frame) / delta frame / GOP.** A *keyframe* decodes on its own; a *delta
  frame* (P-frame) references earlier frames, so it is small but useless without its chain.
  A *GOP* (group of pictures) is a keyframe and the deltas that follow it. A joining or
  recovering client can only start at a keyframe — hence the protocol's `KeyframeRequest`.
  (Distinct from an *animation* keyframe, above.)
- **B-frame.** A delta frame that references *future* frames too. Better compression, but
  the encoder must wait for that future — built-in latency — so interactive streaming
  forbids them (Rime configures the encoder *low-delay*, P-frames only).
- **AV1.** A modern, **royalty-free** video codec (Alliance for Open Media). Rime encodes
  with **SVT-AV1** (scalable multi-core) and decodes with **dav1d** (small, fast) — both
  permissive, unlike patent-pool H.264/HEVC. The `VideoEncoder`/`VideoDecoder` seam lets
  hardware encoders (VideoToolbox/VAAPI/NVENC) replace SVT-AV1 per platform later.
- **YUV / Y′CbCr, 4:2:0 (chroma subsampling).** A colour space splitting *luma* (brightness,
  Y) from *chroma* (colour, Cb/Cr). Because the eye resolves brightness far more finely than
  colour, **4:2:0** stores one chroma pair per 2×2 luma block — half the samples for a loss
  rarely seen. Video codecs work in YUV, so Rime converts RGBA↔I420 (planar 4:2:0) around
  them.
- **PSNR (peak signal-to-noise ratio).** A dB measure of reconstruction quality after a
  lossy codec; higher is better (∞ = identical). ~45–50 dB reads as visually clean. The
  codec proofs assert a PSNR floor rather than bit-exactness (lossy codecs are not
  deterministic).
- **Sequence header / parameter set.** The global stream parameters (resolution, profile)
  a stateful decoder needs *before* the first frame — AV1's analog of H.264's SPS/PPS.
  Rime ships it out-of-band in the protocol's `StreamConfig` message.

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
- **Scene / `.rscene`.** A saved world — which entities exist, their components, and how
  they are placed and parented. `.rscene` is the v1 **human-diffable text** form: reflection-
  serialized, versioned per-component by `type_hash`, entity references written as scene-local
  ids. Loadable with or without an editor (`rime_hello --scene <file>`). See
  [design/scene-format.md](design/scene-format.md).
- **Scene-local id.** The ordinal (`0..N-1`) a `.rscene` gives each saved entity so a
  reference between entities (a `Parent`) is stored position-independently and remapped to a
  fresh runtime handle on load — never a volatile raw entity handle.
- **Gizmo.** The on-screen handles that move/rotate/scale a selected object by dragging —
  translate arrows, rotate rings, scale cube-ends. In Rime the **engine renders** them as an
  always-on-top overlay while the **editor does the drag math** (screen-ray to axis/plane), so
  a drag is just an undoable `SetComponent` (M9.6). Derivation:
  [math/gizmos.md](math/gizmos.md).
- **Picking / ID buffer.** Answering "which object is under this pixel?" by re-rendering the
  scene writing each object's **id** (not its shading) into an integer target, then reading back
  the one texel the cursor is over — the nearest surface wins by the depth test. How a viewport
  click selects an entity (M9.6); the physics raycast stays the gameplay path.
- **Screen-constant size.** Drawing an overlay (a gizmo, a handle) scaled by its distance from
  the camera so it covers a fixed fraction of the viewport at any depth — you never have to
  approach an object to grab its gizmo. See [math/gizmos.md](math/gizmos.md).
