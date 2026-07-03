# engine/app — the application framework & main loop (stub)

`rime::app` will tie a chosen set of modules into a runnable application: initialise subsystems, own
the **frame loop** (input → simulate → render → present), and shut down cleanly. It is the top of
the engine's own layer cake — the thing a game or sample launches. See the `app` module in
[../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md).

**Status: ⚪ a Milestone-0 stub.** Today this directory is just `rime_hello` — a ~20-line launcher
(`main.cpp`) that links `rime::core`, prints the engine name + version, and emits one log line to
exercise diagnostics end-to-end in a real binary. That is deliberately all M0 needed: proof the
toolchain builds, links a module, and runs.

The real framework arrives at **M5**, alongside the render graph — the loop needs something to
drive. Two shapes are already decided for it (see [../../docs/ROADMAP.md](../../docs/ROADMAP.md)):

- **A fixed simulation tick, separate from the render frame,** from day one — multiplayer (M11) is
  tick-based, and un-baking "`dt` everywhere" out of gameplay systems later is a rewrite.
- **Modular composition:** `app` wires modules through their interfaces; removing a feature module
  must still leave a buildable application (ARCHITECTURE guardrail #2).

## Layout

```
main.cpp           # the M0 `rime_hello` launcher (stub; grows into the real entry point)
CMakeLists.txt     # builds rime_hello, links rime::core + fmt
```

Until the framework lands, treat anything here as scaffolding — labelled a stub, per CLAUDE.md.
