# engine/ — the C++ engine

This is the heart of Rime: the runtime, in C++20. It is organized as a set of
**modules**, each in its own subdirectory with its own `CMakeLists.txt` and README.

**The one rule:** dependencies point *downward* through the layers below. A module may
use modules in lower layers, never higher ones. This is what keeps the engine modular
and buildable-in-pieces. The full picture is in
[../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).

## Planned modules (bottom-up)

| Module | Layer | Responsibility |
| --- | --- | --- |
| `core` | foundation | memory/allocators, math, containers, **job system**, logging, reflection, the **module loader** |
| `platform` | foundation | OS abstraction: window, input, filesystem, threads, timers (Win/Linux/macOS) |
| `rhi` | graphics seam | Render Hardware Interface + the **Vulkan backend** (only place that includes Vulkan headers) |
| `ecs` | world | entities, components, systems — the data-oriented world |
| `render` | feature | render graph + high-level renderer (home of UE5-class lighting techniques) |
| `physics` | feature | rigid bodies, collision, queries (multicore-first; may integrate Jolt) |
| `destruction` | feature | **part-based, networked destruction** — our headline system |
| `audio` | feature | mixing & spatialization |
| `animation` | feature | skeletal animation & blending |
| `assets` | feature | runtime asset loading & streaming |
| `net` | feature | networking & replication |
| `app` | top | application framework & the main loop |

> Directories appear here as each module is implemented; we don't create empty stubs.
> Today this is the map we build toward — see [../docs/ROADMAP.md](../docs/ROADMAP.md).

## Conventions

- Namespace: `rime::<module>` (e.g. `rime::rhi`, `rime::render`).
- Every source file carries the SPDX header (see [../CLAUDE.md](../CLAUDE.md)).
- Public headers live in `engine/<module>/include/rime/<module>/`; private code in
  `engine/<module>/src/`. (Layout finalized in Milestone 0.)
- The code teaches: comment the *why*, name the techniques you use.
