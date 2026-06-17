# Module system — design note (M1.8)

Companion to `engine/core/include/rime/core/modules/`. This is the seam that makes Rime
*modular* — the O3DE-"Gem" idea, and architectural guardrail #2 in CLAUDE.md: **the engine must
still build and run if a feature module is removed.** It is the last brick of Milestone 1.

## What "a module" is, and why

A **module** is a self-contained unit of engine functionality — rendering, physics, destruction,
audio — that the engine drives entirely through one abstract interface, `IModule`. The core never
`#include`s or names a module's concrete type. That decoupling buys three things:

1. **Removability / optional features.** A module you don't register simply isn't there; nothing
   else needs to change (guardrail #2).
2. **Extensibility.** New features arrive as new modules implementing the interface — the
   plug-in/Gem model — without editing the core.
3. **Explicit lifecycle and ordering.** Subsystems come up and go down in a defined,
   dependency-respecting order instead of via fragile global-constructor races.

## The interface and lifecycle

`IModule` is deliberately tiny: a `name()`, an optional `version()`, a list of dependency names,
and two lifecycle hooks:

- `on_load(ModuleManager&)` — called once, **after all dependencies are loaded**. Acquire
  resources / register systems here. The manager is handed in so the module can `get()` the
  interfaces of the dependencies it declared.
- `on_unload(ModuleManager&)` — called once, **after every module that depends on this one has
  already been unloaded**, and before its own dependencies are unloaded. Release what `on_load`
  acquired.

Construction (via the factory) must be cheap and side-effect-free; all real work lives in
`on_load`. That separation is what lets the manager construct a module, read its dependency list,
and *then* decide when to initialize it.

## The manager: runtime loading with dependency resolution

`ModuleManager` holds a `name -> factory` **registry**. Compiled-in modules register a factory
(`register_module<T>("name")`); a future dynamic loader would register one after opening a shared
library. Either way, the manager **loads a module by name at runtime**:

```
load("Renderer"):
  load_recursive("Renderer", visiting = {}):
    already loaded? -> done
    registered?     -> no: error (return false)
    on the visiting stack? -> yes: cycle error (return false)
    push "Renderer" onto visiting
    construct it; for each dependency: load_recursive(dep, visiting)   # deps first
    pop "Renderer"
    on_load(); record in load order
```

- **Dependency order** comes from the depth-first walk: a module's dependencies are fully loaded
  (and `on_load`'d) before its own `on_load` runs. The test verifies the trace `load A` then
  `load B (A ready)`.
- **Cycle detection** uses the `visiting` set — the modules currently on the recursion stack. Re-
  entering one is a cycle, reported and rejected rather than overflowing the stack.
- **Missing dependencies / unknown names** fail the load cleanly (nothing partially loaded).
- **Idempotent**: loading an already-loaded module is a no-op success.

**Unloading** runs the reverse: `unload(name)` first unloads everything that depends on `name`
(found by scanning declared dependencies), then unloads `name`; `unload_all` walks the recorded
load order backwards so dependents always precede their dependencies. The destructor calls
`unload_all`, so a manager cleans up its modules deterministically.

The engine only ever holds `IModule*` (via `get`) and the `ModuleManager` — never a concrete
module type. Removing a module is "don't register it"; everything still compiles.

## "Loads at runtime" — scope, and the dynamic-library extension

This brick loads **compiled-in** modules at runtime: instantiated and lifecycled by name, by the
manager, with the engine ignorant of their concrete types — which is the modularity that matters
for the foundation, and satisfies M1's proof. Loading a module from an actual **shared library**
(`dlopen`/`LoadLibrary`, building modules as `.so`/`.dll`/`.dylib`) is the natural next step, but
it is inherently platform-specific and belongs **behind the platform layer (M2)**. The
`register_module(name, factory)` entry point is exactly the hook a dynamic loader will call after
opening a library and finding its export — so adding it later changes *how a factory gets
registered*, not the loading/dependency machinery here.

## Deliberate limitations (labeled, per CLAUDE.md)

- **In-process only (no `dlopen` yet)** — see above; the dynamic-library loader is an M2+ addition
  behind the same registry seam.
- **No hot-reload.** Modules load and unload; reloading a changed module at runtime (editor
  iteration) is a later feature.
- **Dependencies are by name only** — no version constraints (`>= 2.1`) yet, though `version()`
  exists to grow into them.
- **Single-threaded manager.** Module load/unload happen at well-defined points (startup/shutdown),
  not concurrently; the manager is not internally synchronized.

## Where this goes

From M2 onward, subsystems can be modules: the platform layer, the RHI backend, rendering,
physics, and — the headline — destruction. The dependency resolver here is what will bring them up
in the right order (e.g. renderer after RHI after platform). Standing this seam up in M1, before
there are many modules, is the "seams before features" bet from the roadmap. *Inspired by: O3DE
Gems and the module/subsystem managers in O3DE and other component engines.*
