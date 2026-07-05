# engine/app — the application framework & fixed-tick frame loop

`rime::app` ties the engine's modules into a runnable whole: it owns the subsystems and the **frame
loop** (input → simulate → render), and is the top of the engine's own layer cake — the thing a game
or sample launches. See the `app` module in [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md)
and the design decision in [../../docs/adr/0023-app-fixed-tick-loop.md](../../docs/adr/0023-app-fixed-tick-loop.md).

**Status: 🟡 real as of M5.7 — headless.** `Application` runs the whole loop; the
windowed/swapchain **present** path is a documented seam (it can't be tested on the headless
CI/dev box, so it lands and is eyeballed on a display with `07-first-light`, exactly as the
streaming track deferred its windowed client).

## The defining feature: a fixed simulation tick, decoupled from the render frame

The simulation advances in equal `fixed_dt` steps via a time accumulator (`FixedTimestep`); the
render frame runs once per loop iteration at whatever rate it manages; an interpolation `alpha`
bridges them. This is a **multiplayer (M11) seam kept from day one** — tick-based netcode needs a
deterministic step, and un-baking a variable `dt` out of gameplay systems later would be a rewrite.
The determinism it buys (world state is a pure function of the tick count, not of frame pacing) is
proven in `tests/app`.

```cpp
rime::app::Application app({.tick_hz = 60.0});           // headless, GPU-free by default
app.schedule().add("move", writes<Position>reads<Velocity>, integrate_system);  // sim systems
app.on_render([](rime::app::FrameContext& ctx) { /* declare passes into ctx.graph */ });
app.run();                                               // fixed ticks + one render per frame
```

`Application` owns the `JobSystem`, the ECS `World`, the sim `Schedule`, and the `FixedTimestep`,
and **optionally** an `rhi::Device` + `render::RenderGraph` (`AppConfig::gpu`) so pure-sim tools and
tests pay for no GPU. It depends on module *interfaces* only — a sim-only app still builds with a
feature module removed (guardrail #2). `rime_hello` stays as the trivial M0 launcher it always was.

### What is a seam, not yet built (ADR-0023)

- **Windowed present** — the loop renders into an offscreen target headlessly; presenting to a
  swapchain is wired when a display-bearing sample needs it.
- **Temporal interpolation** — `alpha` is plumbed to the render callback; the previous-tick history
  buffer that uses it (also an M11 enabler) is deferred until a smoothness workload asks for it. v0
  renders the latest tick.
- **ECS-native input** — v0 exposes the frame's input snapshot via `Application::frame_input()`;
  routing it through an ECS resource / per-tick timeline is a later refinement.

## Layout

```
include/rime/app/fixed_timestep.hpp   # the pure accumulator: variable dt -> whole ticks + alpha
include/rime/app/application.hpp       # Application, AppConfig, FrameContext
src/application.cpp                     # the frame loop
main.cpp                               # the M0 `rime_hello` launcher (kept)
CMakeLists.txt                         # builds rime_app (library) + rime_hello (exe)
```

Proofs: `tests/app/` — the `FixedTimestep` accumulator/clamp/alpha units, tick determinism
(different frame pacing → bit-identical world), a headless N-frame smoke run, input reaching a sim
system, and the GPU-owning render loop on lavapipe.
