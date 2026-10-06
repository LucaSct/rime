# samples/ — example projects

Small, focused projects built **on** Rime. Samples have two jobs:

1. **Prove the engine works** end-to-end (they're the most honest integration test).
2. **Teach** — each sample shows how to use one slice of the engine, with commentary.

A milestone in [ROADMAP.md](../docs/ROADMAP.md) is **"done" only when its proof runs** — never when
it merely compiles — and for most milestones that proof is a sample here. Those are marked
**"done when"** below. They are self-checking and headless-capable, so CI gates on them: a sample
that can only be judged by eye is not a proof.

## What's here (build order)

| Sample | Proves | What it does |
| --- | --- | --- |
| `00-hello-window` | M2 **"done when"** | Opens a native window, runs a real frame loop, reacts to keyboard/mouse through the polled input layer, live FPS in the title bar. No rendering — this is the window/input/timing seam alone, one source file, no OS `#ifdef`s, on all three OSes. |
| `01-hello-triangle` | M3.4 | The classic first triangle through the RHI, two ways: *windowed* (swapchain + frames-in-flight — the headed proof that pixels reach a screen) and *off-screen* (render to an image, read it back). |
| `02-textured-quad` | M3 **"done when"** | An indexed quad sampling a 2×2 texture — index buffer, texture upload, sampler, descriptors. Windowed, or `--offscreen` to PPM where there is no display. |
| `03-icem-viewer` | — (Frostlens) | The **ICEM 3-D viewer** (see the ROADMAP appendix): a from-scratch engineering viewer built on `core`+`platform`+`rhi` only. Cross-sections, scalar/vector fields, DVR, streamlines, assemblies, an ImGui-free UI, a provenance panel, turntable export. Rime's first non-trivial *application*, and its dogfood. |
| `04-remote-view` | Track S (S0.5) | The remote-view endpoint. `remote_view server` renders off-screen and streams it (tap → codec → protocol) while applying input a client sends back; `remote_view client --headless` connects, scripts input, receives and decodes frames. The same wire the M9 editor viewport rides. |
| `05-ecs-playground` | M4 **"done when"** | 200k entities stepped **in parallel** through the ECS (`Query::par_for_each` under a `System` `Schedule`), timed against a serial baseline (≈10× on 16 cores, Release) and verified bit-for-bit — plus a transform hierarchy (hull → turret → barrel → muzzle) composing `world = parent·local`. |
| `06-render-graph` | M5.8 | A procedural PBR scene — five metallic spheres of rising roughness on a checkered floor under one point light — drawn through the pass library on a `RenderGraph`, off-screen. The runnable companion to ADR-0019 and ADR-0022. |
| `07-first-light` | M5 **"done when"** | A lit PBR scene through the render graph, driven by the application framework: a 6×2 metallic×roughness sphere grid on a mipmapped-checker floor under an orbiting point light, with an orbit camera. |
| `08-gltf-zoo` | M6 **"done when"** | Import → cook → load → render real glTF models with textures, through the whole offline→runtime asset pipeline: three models cooked to `RMA1` by the `rime` CLI, loaded through `engine/assets`, uploaded via the GPU asset bridge. |
| `09-physics-playground` | M7 **"done when"** | Objects fall, collide and stack; raycasts hit; the simulation runs on the job system inside the app's fixed tick. A floor, a four-box tower, a 3-2-1 pyramid, a scatter of spheres. Self-checking headless. |
| `10-destructible-wall` | M8 **"done when"** | A cooked destructible wall takes a hit, sheds part of itself as tumbling debris, and drives **one** destruction event stream out to three systems that have never heard of each other — a VFX dust puff, the null audio backend, and gameplay. |
| `11-lit-rooms` | M10 **"done when"** | The whole advanced-lighting stack on one scene — CSM, local shadows, clustered forward, SDF clipmap, DDGI, SSR — and **opening a wall visibly changes the light in the room behind it**. The first place every M10 technique runs together in one frame. |
| `12-networked-destruction` | M11 **"done when"** | A dedicated headless server plus two clients that receive **different bytes** — each has its own relevancy set — and must still agree **bit for bit** on which parts died and what debris exists. Scripted-loss deterministic by default; `--transport=udp` runs the same code over real sockets. |
| `13-networked-player` | M12 **"done when"** | A server and two clients running a **predicted, reconciled** player under scripted loss: own-input response within one tick against a prediction-off control in the same match, remote motion continuous, both clients converging bit-exactly. GPU-free. |
| `14-virtual-geometry` | m18.3d / [ADR-0043](../docs/adr/0043-virtualized-geometry-m18.md) gate 4 | **Benchmark:** the virtual-geometry complexity sweep — candidates, selected triangles, CPU submission and GPU time at a *fixed* projected size, over a generated quadtree whose depth is the only variable. Includes the row where the cut overflows the command buffer and the builder degrades to the coarse cut. Needs a GPU (`--sweep`, or `scripts/perf.sh --sample virtual-geometry`). |
| `15-terrain` | m19.8e / [ADR-0073](../docs/adr/0073-m19.8e-terrain-budgets.md) | **Benchmark:** the terrain travel budget — a straight fly-through of a generated, `rime terrain-world`-cooked 4 km world at walk (5 m/s), vehicle (30) and aircraft (150) speeds, paced to 60 Hz with the byte budget, upload cap and culling on: terrain GPU ms, selection/residency CPU ms, upload and resident bytes, fallback draws, cap waits, and the **travel envelope** (the fastest speed whose fallback stays under 1 % of drawn leaves; `--envelope` adds 300–2400 m/s). `samples/15-terrain/make_world.sh` once, then `terrain_flythrough --perf` or `scripts/perf.sh --sample terrain`. Needs a GPU. |
| `hello-game` | m15.7 on-ramp / M20 prerequisite | **The smallest complete game here:** walk a character around an arena, touch five trigger volumes, push a crate, win. Read this one first — every other sample demonstrates a subsystem; this one shows the shape of a *game*. Its rules live in one class with three methods and know nothing about devices, windows or frames, which is the seam [ADR-0046](../docs/adr/0046-exported-games-and-the-blender-boundary.md) §1 will cut M20's `GameDefinition` along. GPU-free self-check; `--windowed` to play. |
| `99-the-block` | M13 **"done when"** (partly) | **The vision demo:** a destructible urban block — destruction, dynamic GI, many lights and networking together. Its 27 structural claims are green; the frame-rate clause is **not** met and is carried by M18 ([ADR-0041](../docs/adr/0041-the-visual-bar-m17.md)). Listed honestly rather than as done or as absent. |
| `target-range` | M15 **"done when"** | **The platform proof:** a game that is *not the block* — five crates authored as a `.rscene`, shot with real raycasts, counted — running with **no `engine/` or `tools/` change**. Its own reflected component (`targetrange::Target`) round-trips through the engine's serializer, and it brings its own `target-range-host` so the editor understands it. The milestone's diff touches only `samples/` and `docs/`. |
| `codec_bench` | Track S (S0.3) | **Benchmark:** encode representative frames with each streaming codec (raw / LZ4 / JPEG) and print ratio, throughput, wire bandwidth and JPEG PSNR — the measurement behind [ADR-0017](../docs/adr/0017-streaming-codec.md). GPU-free. |
| `jobs_core_saturation` | M1.6 | **Benchmark:** saturate every core through the work-stealing job system — a CPU-heavy function over millions of items, serial then `parallel_for`, results compared and the speedup reported. On an N-core machine it should approach N. |

M9 (Editor v1) is the exception: its proof is the Rust editor in [`tools/`](../tools), not a sample.

`99-the-block` is the "vertical slice" of the dream described in [../VISION.md](../VISION.md) — when
it runs and looks *and feels* right, the core thesis is proven. It runs; ADR-0041 tracks what it does
not yet do at frame rate.

**Where to start reading:** `hello-game` for the shape of a game, `00-hello-window` for the platform
seam, `06-render-graph` for how a frame is declared, `09-physics-playground` for the fixed tick.

## Running them

Samples build with the engine (`scripts/build.sh`) and land in `build/<preset>/bin/`. The ones CI
gates on take a `--headless` or `--offscreen` flag and exit non-zero on failure; several read cooked
assets and are registered in `ctest` behind a cook fixture, so `ctest --preset dev` runs them the way
CI does. Each sample directory has its own README with the details and the reasoning.
