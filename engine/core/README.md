# engine/core — the foundation everything stands on

`rime::core` is the bottom of the layer cake (see [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md)):
the module every other module may depend on, and the one that **depends on nothing above it**. It
holds the pieces that have no better home and that everything needs — memory, math, containers,
jobs, diagnostics, reflection, and the module system.

**The one rule:** `core` depends on nothing but the C++ standard library and `fmt`. If something
here would need `platform` (an OS call) or any higher module, it belongs in *that* module instead.
Keeping `core` self-contained is what lets the whole engine — and its tests — rest on it.

Data-oriented by default: think in arrays of data and the transforms over them, not deep object
trees (VISION / ARCHITECTURE §1). The hot paths here — the allocators and the lock-free job system —
are written to be cache-friendly and lock-light.

## Status (built bottom-up, brick by brick — see [docs/ROADMAP.md](../../docs/ROADMAP.md))

| Brick | Provides | State |
| --- | --- | --- |
| M1.1 | diagnostics — `log` / `assert` / `RIME_PANIC` + a scoped timing/profile hook | landed |
| M1.2 | allocators — arena, stack, pool, and a tracking allocator | landed |
| M1.3 | math I — vectors & matrices (SIMD-friendly; derivation in `docs/math/`) | landed |
| M1.4 | math II — quaternions & transforms (`docs/math/`) | landed |
| M1.5 | containers — a generational **slot map** / handle table | landed |
| M1.6 | the **Chase-Lev work-stealing job system** (lock-free deque + workers) | landed |
| M1.7 | minimal **reflection** — describe + serialize a struct through bytes | landed |
| M1.8 | the runtime **module loader** (load/unload, resolve interfaces) | landed |

> All eight bricks are merged and CI-green on Windows, Linux, and macOS. Two guardrails watch the
> lock-free code: the deque's memory-ordering argument is written up in
> [docs/design/work-stealing-deque.md](../../docs/design/work-stealing-deque.md), and CI runs the
> `core` suite under **ThreadSanitizer** (Clang) and **ASan+UBSan** (Phase 0.3).
>
> One honest caveat: the **allocators are complete but not yet load-bearing** — most engine code
> still uses standard containers. The ECS (M4) is what puts arenas/pools on the hot path. The deque,
> by contrast, already carries real load under the job system.

## Layout

```
include/rime/core/   # public headers — dependents #include <rime/core/...>
  diagnostics/       #   log, assert, source-location, scoped profiling
  memory/            #   allocator interface + arena / stack / pool + tracking
  math/              #   scalar, vec, mat, quat, transform, simd
  containers/        #   handle + generational slot map (header-only)
  jobs/              #   chase_lev_deque.hpp + the job system
  reflect/           #   type_info + serialize
  modules/           #   module + the runtime module manager
  <subsystem>.hpp    #   one umbrella header per subsystem (math.hpp, jobs.hpp, …)
  version.hpp
src/                 # the .cpp implementations (containers is header-only)
```

`core` links only `fmt` (PUBLIC — the logging macros expand to `fmt::format` at the call site) and
the platform threads library (the job system spins up `std::thread` workers). Math conventions and
representations are fixed in [ADR-0004](../../docs/adr/0004-math-conventions.md) and
[ADR-0005](../../docs/adr/0005-rotation-representation.md); the systems have design notes under
[docs/design/](../../docs/design/) (job system, slot map, reflection, work-stealing deque).
