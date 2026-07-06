# ADR-0023: The application framework — a fixed simulation tick decoupled from the render frame (M5.7)

- Status: Accepted
- Date: 2026-07-05

## Context

`engine/app` has been a Milestone-0 stub since the start: `rime_hello`, a ~20-line launcher that
proves the toolchain links a module and runs. M5 is where it becomes real — the render graph
(M5.4), the scene layer (M5.5), and the PBR pipeline (M5.6, [ADR-0022](0022-forward-pbr.md)) now
give the loop something to drive. `app` is the top of the engine's layer cake: the thing a game or
sample launches, which wires modules through their interfaces and owns the frame loop.

One shape was fixed for it back in the 2026-07-02 plan and must land from day one: **a fixed
simulation tick, separate from the render frame.** Multiplayer (M11) is tick-based — lockstep and
client prediction both assume the simulation advances in equal, deterministic steps — and un-baking
"`dt` everywhere" out of gameplay systems *after* they exist is a rewrite. The cheap seam now
(a constant tick `dt`, a deterministic step) is the expensive rewrite avoided later. This is the
same forward-seam discipline the ECS took with per-component change versions and reflection.

This dev/CI environment is **headless** (lavapipe, no display), so — as the streaming track did with
its windowed client — the *testable* spine is headless, and the windowed/swapchain present path is a
documented seam built when a display-bearing sample (07-first-light on the Mac) needs it.

## Decision

**`rime::app::Application` owns the subsystems and runs a frame loop whose simulation advances in
fixed-size ticks via a time accumulator, decoupled from a variable-rate render frame; the render
frame is a per-frame callback, and GPU ownership is optional so the loop is provable GPU-free.**

### 1. The fixed-timestep accumulator

The loop (Glenn Fiedler's "Fix Your Timestep", the standard):

```
accumulator += frame_dt                     // real elapsed time this iteration
ticks = 0
while accumulator >= fixed_dt and ticks < max_ticks_per_frame:
    run one simulation tick                 // advances the world by EXACTLY fixed_dt
    accumulator -= fixed_dt
    ticks += 1
alpha = accumulator / fixed_dt              // in [0, 1): where we are between ticks
```

A **simulation tick** runs the sim `Schedule` and then `propagate_transforms` — one deterministic
step of the world. The crucial invariant: **a tick always advances the world by the constant
`fixed_dt`, never by the frame `dt`.** Systems integrate against `fixed_dt` (exposed as a
resource/argument), so the world's state after $k$ ticks is a pure function of $k$ — independent of
how real time was sliced into frames. That purity *is* the determinism the M11 netcode will require,
and the M5.7 proof asserts it directly (two different frame-pacing patterns driven to the same tick
count produce bit-identical worlds). `fixed_dt` defaults to 1/60 s.

`FixedTimestep` is a small pure struct (accumulate → count ticks → alpha), unit-tested in isolation,
so the accumulator logic is verified without a World, a device, or a clock.

### 2. The spiral-of-death clamp

`max_ticks_per_frame` (default 8) caps ticks per iteration. Without it, a frame that takes longer
than `fixed_dt` to simulate falls behind, so the next frame owes *more* ticks, which take longer
still — an unrecoverable spiral. When the clamp trips, the backlog beyond the cap is **dropped**
(the accumulator is pulled back below `fixed_dt`): simulation time slows relative to wall-clock
rather than freezing. It is a safety valve on the *pacing*, deliberately outside the determinism
guarantee (which holds only while no frame exceeds `max_ticks_per_frame · fixed_dt` — the honest
boundary, stated so netcode knows it).

### 3. Render is a per-frame callback; the interpolation alpha is exposed

After the tick loop, the loop renders **once**, invoking a user-supplied callback with the World and
the `alpha`. Rendering at frame rate while simulating at tick rate is the entire point of the
decoupling: a 144 Hz display stays smooth over a 60 Hz sim.

**v0 renders the latest tick's state**; the `alpha` is computed and handed to the callback but the
default draw ignores it. True temporal interpolation — rendering a blend of the previous and current
tick states by `alpha`, which removes the residual judder of a fast display over a slow sim — needs
the sim to retain the *previous* tick's transforms, a per-component history buffer. That buffer is a
documented seam (it is also an M11 enabler: the same previous/current snapshots feed client-side
interpolation), not built until a visible-smoothness workload asks for it. Exposing `alpha` now means
turning interpolation on later changes no call signatures.

### 4. What Application owns; GPU is optional

`Application` owns the `JobSystem`, the `ecs::World`, the sim `Schedule`, the `FixedTimestep`, and
(when windowed) platform init + the `Window`. It **optionally** owns an `rhi::Device` and a
`render::RenderGraph`, created only when the app is configured to render (`AppConfig::gpu`). The
default is GPU-free: the tick-determinism and headless-smoke proofs, and any pure-sim tool, pay for
no device. A rendering app (07-first-light) flips one flag; its callback then builds passes into the
Application-owned graph, which the loop executes and hands to a present (windowed) or capture
(headless/streamed) sink.

Ownership goes through module *interfaces* only — remove `engine/render` and a sim-only Application
still builds (guardrail #2). The loop names no queue and assumes no single thread beyond the
platform/JobSystem main-thread contract those modules already document.

### 5. Input: sampled at the frame edge, consumed by the frame's ticks

Windowed, the loop pumps the OS event queue once per iteration (`platform::pump_events` +
`poll_event`) into an input snapshot the sim systems read. Headless, events can be **injected**
(scripted) into the same snapshot — which is how "an input event is visible to a sim system" is
proven without a window. v0 exposes the frame's input to every tick that frame runs; per-tick input
timelines (a netcode nicety) are a later refinement on the same seam.

## Consequences

**Good**

- The M11 seam is in from the first frame: a deterministic fixed tick, a stated determinism
  boundary, previous-state interpolation and per-tick input as marked seams — none of which is a
  rewrite to enable later.
- The framework is provable *here*: tick determinism, a headless N-frame smoke run, and
  input-to-system are all GPU-free and land in CI on lavapipe; a small GPU-backed headless run
  exercises the optional device-owning path too.
- `Application` collapses the hand-rolled `init → loop → shutdown` every sample currently repeats
  into one reusable, correct object; `rime_hello` stays as the trivial launcher it always was.
- The decoupling delivers its point (smooth render over a fixed sim) and keeps the door open to
  interpolation, variable-rate rendering, and pipelined frames without API breaks.

**Costs we accept**

- **No temporal interpolation in v0** — a very fast display over a slow sim shows slight judder. The
  `alpha` is plumbed; the history buffer that uses it is deferred to a workload that needs it.
- **The windowed present path is a seam, not shipped** — it cannot be tested on this headless box, so
  it lands and is eyeballed on the Mac with 07-first-light (the streaming track's exact precedent).
- **Frame-granularity input** — all of a frame's ticks see the same input snapshot. Fine for local
  play; per-tick input is a later netcode refinement.
- **Determinism is conditional** on no frame exceeding the clamp and on the systems themselves being
  deterministic (the ECS scheduler already guarantees run order; floating-point associativity across
  a fixed thread count holds within a build). Stated, not hidden.

## Alternatives considered

- **Variable timestep (`dt` everywhere).** Simplest, and wrong for this engine: gameplay integrated
  against a wall-clock `dt` is non-deterministic and un-replicable, and retrofitting a fixed tick
  onto systems written against `dt` is the rewrite this ADR exists to avoid. Rejected on the M11
  requirement.
- **Fixed tick with no interpolation seam.** Simpler loop, but renders locked to tick state — judder
  on high-refresh displays and no path to fix it without changing the render callback's contract.
  Exposing `alpha` now costs nothing and keeps the seam.
- **Application owns a mandatory device.** Cleaner story ("an app renders"), but forces a GPU on
  pure-sim tools and tests and couples the loop to the RHI. Optional ownership keeps the loop
  provable GPU-free and honors the removability guardrail.
- **A full windowed present loop now.** Untestable on this box; building display code no one here can
  run invites rot. Deferred to the Mac-run sample, exactly as the S0.5 windowed client was.

---

*This ADR is brick **M5.7**. Code: `engine/app/include/rime/app/{fixed_timestep,application}.hpp`
+ `src/`. Proofs: `tests/app/` — `FixedTimestep` accumulator/clamp/alpha units; tick determinism
(different frame pacing → identical world); a headless N-frame smoke run; input injected into a sim
system; a GPU-backed headless render run on lavapipe. Builds on
[ADR-0018](0018-ecs-storage-model.md) (World/Schedule), [ADR-0016](0016-editor-is-a-client-of-the-engine.md)
(the M11 seams), [ADR-0019](0019-render-graph.md)/[ADR-0022](0022-forward-pbr.md) (the render it
drives). Next: **M5.8** — `06-render-graph` and `07-first-light` put this loop on screen. See
[ROADMAP.md](../ROADMAP.md) → M5.*
