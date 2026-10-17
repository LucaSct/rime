# The Rime Engine — Vision

> *Rime (n.): the delicate, feathery frost that forms when freezing fog touches a
> surface. It builds crystal by crystal into something intricate and beautiful.
> That is how we build an engine.*

This document is the **north star**. Every milestone, every module, and every line
of code should be traceable back to something written here. When a decision is hard,
we re-read this file. When we disagree, this file breaks the tie. When it stops being
true, we change it on purpose (via an [ADR](docs/adr/)) — never by accident.

---

## 1. The dream in one sentence

**Rime is a free, open-source, multiplayer-first game engine for developing and
shipping large-scale games: Battlefield-class destruction and rendering,
Arma-like physical simulation, and a modular runtime shared by development and
shipped games. Its pipeline produces native artifacts for each supported target,
with dedicated hosting, browser play, and creator-enabled browser development.**

This is the product direction, not a claim that every target or scale is available
already. The source remains written to teach how a modern engine works.

The shipping targets include Windows and Linux PCs, PlayStation 4 and Xbox, with
other platforms added through the same target pipeline. Each target receives its
own executable and platform package. A console player build can be accompanied by
a compatible Linux/Windows dedicated-server build. Browser play initially uses the
native streaming architecture; a browser-native runtime is a separate target.

A creator can enable browser development so players can change their own game or
fork. Editing uses the same simulation and content pipeline as a shipped game;
changes become validated, versioned builds before entering shared sessions.
See [the multiplayer shipment plan](docs/design/multiplayer-shipping.md) and
[ADR-0085](docs/adr/0085-multiplayer-development-and-shipment.md).

## 2. Why this exists

There are three kinds of high-end engines today:

- **Closed AAA engines** (Frostbite, RE Engine, Snowdrop) — extraordinary technology,
  locked inside one company. You cannot read them, learn from them, or ship your own
  game on them.
- **Commercial licensed engines** (Unreal, Unity) — incredibly powerful, but
  proprietary, royalty/subscription bound, and largely opaque at the core.
- **Open engines** (O3DE, Godot, Bevy) — open and inspectable, but none yet combines
  *all three* of AAA-class real-time destruction, UE5-class lighting, and a fully
  modular, studio-grade toolchain.

Rime aims squarely at the empty spot: **open like O3DE, beautiful like Unreal,
destructible like Frostbite.** And — just as important — **legible**, so it doubles as
the best place in the world to *learn* how engines are built.

## 3. The fusion thesis — what we take from whom

We are not cloning any one engine. We are studying the best ideas in the industry and
re-implementing the *principles* (not the code) cleanly and openly. (See the full
[engine survey](docs/research/engine-survey.md) for sources and detail.)

| From | What we take | Why |
| --- | --- | --- |
| **Frostbite** | Part-based, networked, gameplay-coupled **destruction**: structures made of breakable parts, debris with real physics, "health-transition" hooks that spawn effects/sound/gameplay, and aggressive prioritization/culling so it scales to 64+ players. | Destruction is our headline feature and the hardest thing to do well. It must be a first-class engine system, not a bolt-on. |
| **Unreal Engine 5** | The **rendering and lighting** bar: virtualized geometry (Nanite-style), real-time global illumination + reflections (Lumen-style), virtual shadow maps, and a many-lights pipeline (MegaLights-style) — all sitting on a clean render-graph. | This is the visual ceiling players now expect. We design the renderer so these techniques have a home from day one. |
| **O3DE** | The **modular architecture**: an engine that is a small core plus composable modules ("Gems"), loaded at runtime, where whole subsystems can be swapped. A data-driven, ECS-centric world. | Modularity is what makes an engine survivable, extensible, and community-friendly. It is the difference between a tech demo and a platform. |
| **Jolt Physics (study)** | Multicore-friendly rigid-body simulation patterns; physics state that can be queried and streamed off the main thread. | Destruction *is* physics at scale. We need a parallel-first simulation model, whether we integrate Jolt or grow our own. |
| **Bevy / DOD community** | Ergonomic ECS, data-oriented design, and the proof that *readable* engine code can also be fast. | Our code must teach. Clean ECS is both performant and the easiest mental model for newcomers. |

## 4. Principles (in priority order)

These are ordered. When two principles conflict, the higher one wins.

1. **Power and features come first.** Rime exists to make AAA-class games possible.
   Where a feature's quality or performance is at stake, we do not compromise it for
   convenience, portability, or simplicity.
2. **Cross-platform, but never at the engine's expense.** The engine and its editor
   currently build on **Windows, Linux, and macOS**. Native game shipment targets
   also include **PlayStation 4 and Xbox**, with explicit toolchain, runtime and
   package proofs before a target is called supported. *But* if a portability requirement would diminish the engine's power or
   performance, we narrow the platform set rather than weaken the engine. Portability
   is a goal, not a constraint that gets to win.
3. **The code teaches.** Source is written to be *read*. Generous, honest comments
   explain not just *what* but *why*. A motivated newcomer (including the project's own
   founders) should be able to open any file and learn how that piece of a real engine
   works. We optimize for the reader, then for the machine.
4. **Modular to the core.** A small kernel; everything else is a swappable module.
   Subsystems depend on *interfaces*, not on each other. You should be able to delete
   a feature and have the engine still build.
5. **Open and welcoming.** Apache-2.0 licensed. Decisions are public (ADRs). The
   project is built to become a community, not a walled garden. Games shipped on Rime
   stay 100% the creators' own, royalty-free, forever.
6. **Honest engineering.** We measure before we optimize, we write down our decisions,
   and we say plainly what works, what is a stub, and what is broken.

## 5. What success looks like

- **The headline demo:** a destructible urban block — buildings that fragment
  realistically under fire, debris that falls and hurts you, dust and sound emitted
  from the same destruction event, GI that updates as walls come down, all running at
  a playable frame rate. This single scene exercises destruction + lighting +
  modularity together. It is our "vertical slice" of the dream.
- **The platform proof:** a small team can develop and ship a Battlefield-style
  multiplayer combat sandbox without forking the engine. The exported game uses
  the same authoritative netcode and physics as development, includes an operable
  dedicated-server artifact, and has versioned native builds per supported target.
- **The browser proof:** players can play through a browser and, where the creator
  enables it, edit their own fork, preview it in isolation, and publish a validated
  version into their own multiplayer session.
- **The learning proof:** someone with no engine experience reads the repo and the
  docs and *understands* how the renderer, the ECS, and the destruction system fit
  together — and lands their first contribution.

## 6. Non-goals (for now — not forever)

To stay honest about scope, these are explicitly **out of the initial milestones**.
Each may return later via an ADR.

- Console backends are planned shipment work, not implemented support. Each console
  remains blocked until its toolchain, hardware, runtime and package gates pass.
- Mobile targets and a native WebAssembly runtime. Browser streaming and optional
  browser authoring are product requirements with their own delivery stages.
- A marketplace/asset store.
- Photorealistic-out-of-the-box content (we build the *engine*; art comes later).
- Backward-compatibility guarantees while we are pre-1.0. Things will change.

## 7. Decisions already made

These are settled (each has, or will have, an [ADR](docs/adr/)):

- **Name:** Rime. Code namespace `rime::`, repository `rime/`.
- **Core language:** **C++20/23** for the runtime, renderer, and all hot paths —
  matching the AAA reference engines and their middleware ecosystem.
- **Tooling language:** **Rust** for the editor, asset pipeline, and supporting
  services — memory-safe, modern tooling around the C++ core.
  → [ADR 0001](docs/adr/0001-cpp-core-rust-tooling.md)
- **Graphics:** **Vulkan-first**, behind a thin **Render Hardware Interface (RHI)** so
  D3D12 / Metal / console backends can be added later without a rewrite.
  → [ADR 0002](docs/adr/0002-vulkan-first-rhi.md)
- **License:** **Apache-2.0** (permissive + explicit patent grant).
  → [ADR 0003](docs/adr/0003-apache-2-license.md)

## 8. How we build it

Brick by brick. The big picture lives in [docs/ROADMAP.md](docs/ROADMAP.md) (created
when the milestone plan is approved). Each milestone is then broken into small,
reviewable "bricks," and each brick is planned again before it is built. We do not
skip the boring foundations — a great engine is mostly great foundations.

> The frost does not form all at once. Crystal by crystal.
