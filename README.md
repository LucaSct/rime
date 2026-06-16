<div align="center">

# ❄ Rime Engine

**An open-source, AAA-grade game engine — the destruction of Frostbite, the lighting of Unreal 5, the modularity of O3DE.**

*Open like O3DE. Beautiful like Unreal. Destructible like Frostbite.*

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Status](https://img.shields.io/badge/status-pre--alpha-orange.svg)](#status)
[![Engine](https://img.shields.io/badge/runtime-C%2B%2B20-00599C.svg)](docs/ARCHITECTURE.md)
[![Tools](https://img.shields.io/badge/tooling-Rust-DEA584.svg)](tools/README.md)
[![Graphics](https://img.shields.io/badge/graphics-Vulkan-AC162C.svg)](docs/adr/0002-vulkan-first-rhi.md)

</div>

---

## What is Rime?

Rime is a game engine for building **large-scale, high-fidelity games** — think
real-time destructible worlds at the scale and look of *Battlefield 6* — and it is
**free and open source** so anyone can use it, ship commercial games on it
royalty-free, *and read the source to learn how a modern engine works*.

We study the best ideas across the industry and re-implement the **principles** cleanly
and openly:

- 💥 **Destruction like Frostbite** — buildings made of breakable parts, debris with
  real physics, destruction events that drive gameplay, sound, and VFX from one source,
  scaled to many players over the network.
- 💡 **Lighting like Unreal Engine 5** — a render graph designed for virtualized
  geometry, real-time global illumination & reflections, virtual shadow maps, and
  many-lights rendering.
- 🧩 **Modularity like O3DE** — a small core plus composable, swappable modules; a
  data-oriented, ECS-centric world; delete a feature and the engine still builds.

The full picture is in **[VISION.md](VISION.md)**.

## Why another engine?

Closed AAA engines (Frostbite) can't be read or used by you. Licensed engines (Unreal,
Unity) are powerful but proprietary. Open engines (O3DE, Godot, Bevy) are inspectable
but none yet combines AAA destruction *and* UE5-class lighting *and* a studio-grade
modular toolchain. Rime aims at that empty spot — and at being the most *legible*
high-end engine out there. See the [engine survey](docs/research/engine-survey.md).

## Tech at a glance

| Area | Choice | Rationale |
| --- | --- | --- |
| Runtime / renderer | **C++20/23** | AAA performance ceiling + the middleware ecosystem ([ADR-0001](docs/adr/0001-cpp-core-rust-tooling.md)) |
| Editor / asset pipeline / tools | **Rust** | Memory-safe, modern tooling around the core ([ADR-0001](docs/adr/0001-cpp-core-rust-tooling.md)) |
| Graphics | **Vulkan-first**, behind a thin **RHI** | One great backend now; D3D12/Metal/console later ([ADR-0002](docs/adr/0002-vulkan-first-rhi.md)) |
| Platforms | Windows · Linux · macOS | Power beats portability when they conflict |
| License | **Apache-2.0** | Permissive + patent grant; ship commercial games freely ([ADR-0003](docs/adr/0003-apache-2-license.md)) |

## Status

**Pre-alpha — foundations in progress.** There is no playable engine yet. We are
building it in the open, brick by brick. Watch [docs/ROADMAP.md](docs/ROADMAP.md) for
the milestone map. Expect rapid change and clearly-labeled stubs.

## Repository layout

```
rime/
├── VISION.md        # The north star
├── docs/            # Architecture, roadmap, research, decisions (ADRs), glossary
├── engine/          # The C++ engine — one module per subdirectory
├── tools/           # Rust tooling: editor, asset pipeline, CLIs
├── samples/         # Example projects that exercise the engine
├── third_party/     # Vendored dependencies
└── scripts/         # Setup / build / lint helpers
```

Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) to understand how the pieces
fit, and [docs/glossary.md](docs/glossary.md) if any term is unfamiliar.

## Getting involved

Rime is built to become a community. Whether you want to ship games on it, push the
graphics frontier, or *learn how engines work by reading and contributing*, you're
welcome here.

- Read [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md).
- Browse the [architecture](docs/ARCHITECTURE.md) and [roadmap](docs/ROADMAP.md).
- Foundations are the best first contributions — they're where the learning is densest.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Games you build with
Rime are entirely yours; no royalties, ever.

<div align="center"><sub>The frost does not form all at once. Crystal by crystal. ❄</sub></div>
