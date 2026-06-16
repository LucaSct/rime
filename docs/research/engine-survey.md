# Engine Survey — learning from the state of the art

Before writing our own engine, we studied the leading ones. This document records
**what each does well, where it falls short, the key technology, and what Rime should
borrow or avoid.** We re-implement *principles and ideas*, never code. Sources are
linked at the bottom and inline.

> This is a living document. Add to it as we learn more; cite your sources.

---

## Frostbite (EA / DICE) — *the destruction benchmark*

**What it's great at.** Large-scale, reactive, destructible worlds. Frostbite began as
a destruction- and multiplayer-focused engine and grew into a full AAA suite (rendering,
animation, audio, physics at scale). It treats the world as "an ensemble of interactive
parts rather than static backdrops," which is why Battlefield feels so reactive.

**Destruction, concretely (Battlefield 6).** This is the system Rime most wants to
learn from:
- **Part-based destruction + debris spawning**, deliberately made *extensible* so
  studios can customize behavior per asset type.
- A **health-transition system** — flexible health states whose transitions run
  "custom-built processes to alter a state, modify gameplay, or spawn new assets."
- **Physically real debris:** fragments fall naturally and can damage players beneath;
  destruction couples to soldier/ragdoll/vehicle impulses, forming an "ecosystem."
- **Networked at scale:** explicit **prioritization and culling** of part-destruction
  and debris to keep 64-player matches synchronized within bandwidth limits.
- **One event → many systems:** "seam and surface emitters" listen to debris activation
  and explosion shockwaves and pick VFX by material; the *same parameters* drive audio
  — unified feedback from a single source.
- History: voxel-based (FB1) → destruction masking via signed **volume distance fields**
  (FB2) → procedural fracturing based on impact/material (FB3) → extensible part-based
  (FB modern).

**Weaknesses / cautions.** Entirely closed; famously hard to onboard to (steep internal
learning curve); not general-purpose outside EA. Nothing to copy in terms of openness —
the opposite of our goals.

**→ Rime borrows:** the entire *philosophy* of destruction as a first-class, networked,
gameplay-coupled, event-driven system (see `engine/destruction` in
[ARCHITECTURE.md](../ARCHITECTURE.md)). We make it open and legible.

## Unreal Engine 5 (Epic) — *the rendering/lighting benchmark*

**What it's great at.** The highest-fidelity real-time graphics in a generally
available engine, plus a deep, structured toolchain (Content Browser, Blueprints,
MetaHuman, etc.).

**Key tech.**
- **Nanite** — virtualized micropolygon geometry; render film-class detail (billions of
  source triangles) by streaming/culling at fine granularity. UE 5.7 extends Nanite to
  foliage.
- **Lumen** — real-time global illumination *and* reflections, no lightmap baking.
- **Virtual Shadow Maps (VSM)** — consistent high-resolution shadows designed to work
  with Nanite, Lumen, and World Partition.
- **MegaLights** (5.5 → beta in 5.7) — thousands of dynamic, shadow-casting lights in
  real time (demoed at 1000+ on PS5), choosing ray tracing or VSM for shadows.

**Weaknesses / cautions.** Proprietary; royalty/licensing terms; enormous and complex;
heavy at runtime; the source is available to licensees but is famously hard to learn
from. Defaults can be performance traps.

**→ Rime borrows:** the *targets* (virtualized geometry, dynamic GI + reflections, VSM,
many-lights) and the discipline of building a **render graph** that makes them possible.
We do not attempt to match UE5 feature-for-feature on day one — we build the seams.

## O3DE — Open 3D Engine (Linux Foundation) — *the modularity benchmark*

**What it's great at.** A genuinely **modular, open-source** AAA-oriented engine.
- **Gems**: modular, packaged features/assets. Include only the Gems you need; the
  community can publish Gems that even replace entire subsystems.
- **AZ framework**: a layered core (AzCore, AzFramework, AzGameFramework,
  AzToolsFramework, AZQtComponents) you extend.
- **Module system**: a Gem's compiled library is a *module*; an `Application` uses a
  `ModuleManager` to load the dynamic library and call defined entry points (helped by
  macros like `AZ_DECLARE_MODULE_CLASS`).
- Apache-2.0 / MIT licensed; data-driven; cross-platform.

**Weaknesses / cautions.** Smaller community and ecosystem than Unreal/Unity; steeper
ramp; rendering (Atom) is capable but not at UE5's lighting frontier; documentation/
polish still maturing.

**→ Rime borrows:** the *small-core-plus-loadable-modules* architecture and the
"swap an entire subsystem" ambition — implemented as our own minimal module system in
`engine/core` (see [ARCHITECTURE.md](../ARCHITECTURE.md)). We also adopt its **license
family (Apache-2.0)**, [ADR-0003](../adr/0003-apache-2-license.md).

## Unity — *the accessibility benchmark*

**Great at:** approachability, fast iteration, drag-and-drop asset workflow, huge asset
store, mobile, massive community. **Weak at:** top-end AAA graphics vs. Unreal; core is
proprietary; licensing/business-model turbulence has hurt trust.
**→ Rime borrows:** the lesson that **iteration speed and a friendly asset pipeline**
matter enormously — a guiding constraint for the Rust tools layer. (Not a tech source.)

## CryEngine — *the environmental-rendering veteran*

**Great at:** beautiful, realistic environments and rendering. **Weak at:** steep
learning curve, weaker docs, smaller community, demanding to work with.
**→ Rime borrows:** the bar for environment/vegetation/atmosphere rendering as
inspiration; the cautionary lesson that **documentation and approachability** are
features, not afterthoughts.

## Godot — *the open-source success story*

**Great at:** free/open (MIT), lightweight, fast iteration, elegant **scene/node**
model, vibrant and growing community. **Weak at:** high-end 3D/AAA graphics; smaller
asset ecosystem than Unity/Unreal.
**→ Rime borrows:** proof that an **open** engine can build a thriving community, and
that a clean, comprehensible content model is worth a lot. Our scope (AAA fidelity)
is higher, so our core is heavier.

## Bevy — *the data-oriented / readable-code reference*

**Great at:** an ergonomic, modern **ECS** and data-oriented design in a memory-safe
language (Rust); proof that engine code can be both *readable* and fast; friendly to
newcomers. **Weak at:** young; not yet AAA-class in rendering/tooling; smaller feature
surface.
**→ Rime borrows:** the ECS ergonomics and the conviction that **legible engine code**
is achievable — central to our "the code teaches" principle.

## Physics middleware {#physics}

The destruction system rides on physics, so the physics choice matters.

| Engine | Notes | Fit for Rime |
| --- | --- | --- |
| **Jolt Physics** | Modern, lightweight, **multicore-friendly** rigid-body + collision in C++; designed for concurrent access to physics data, background loading/streaming, and parallel queries; shipped in *Horizon Forbidden West* and *Death Stranding 2*; open source. | **Strong candidate.** Open, fast, parallel-first, AAA-proven — matches our job-system world and destruction-at-scale needs. |
| **PhysX (NVIDIA)** | Scalable, mature, rigid bodies/cloth/fluids. But being removed from modern Unreal, and integration/licensing friction. | Reference only. |
| **Chaos (Epic)** | State-of-the-art real-time **destruction & fracture**, cinematic quality. But **tightly coupled to Unreal** — can't be used elsewhere. | Great inspiration for destruction *quality*; not usable as a dependency. |

**→ Rime plan:** evaluate **integrating Jolt** vs. growing our own rigid-body core
behind a `physics` interface; either way the `destruction` module sits on top. Decision
will get its own ADR when we reach that milestone.

---

## Synthesis — Rime's position

| Capability | Frostbite | UE5 | O3DE | Unity | Godot | **Rime (goal)** |
| --- | :---: | :---: | :---: | :---: | :---: | :---: |
| AAA destruction | ★★★ | ★★ | ★ | ★ | ½ | **★★★** |
| UE5-class lighting | ★★ | ★★★ | ★½ | ★½ | ★ | **★★★ (target)** |
| Modular / swappable core | ★½ | ★½ | ★★★ | ★ | ★★ | **★★★** |
| Open source | ✗ | ½ (source-available) | ✓ | ✗ | ✓ | **✓ (Apache-2.0)** |
| Readable / teaches | ✗ | ✗ | ½ | ✗ | ★★ | **★★★ (core principle)** |

No existing engine fills all five rows. **That intersection is Rime.**

---

## Sources

- EA — *How Battlefield 6 reimagines/redefined destruction*: https://www.ea.com/news/how-battlefield-6-redefined-destruction
- *How Battlefield 6's Frostbite Engine Pushes Physics to Its Limits* (Niche Gamer): https://nichegamer.com/how-battlefield-6s-frostbite-engine-pushes-physics-to-its-limits/
- *Destruction Masking in Frostbite 2 using Volume Distance Fields* (SIGGRAPH 2010): https://www.slideshare.net/slideshow/siggraph10-arrdestruction-maskinginfrostbite2/4883521
- Unreal Engine 5.7 — MegaLights / Nanite foliage (WCCFtech): https://wccftech.com/unreal-engine-5-7-out-now-with-nanite-foliage-and-megalights-powered-stunning-dynamic-shadow-casting-lights/
- Unreal Engine docs — MegaLights: https://dev.epicgames.com/documentation/unreal-engine/megalights-in-unreal-engine
- Unreal Engine docs — Virtual Shadow Maps: https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine
- O3DE docs — Gem Module System: https://docs.o3de.org/docs/user-guide/programming/gems/overview/
- O3DE docs — Key Concepts: https://www.docs.o3de.org/docs/welcome-guide/key-concepts/
- Jolt Physics (GitHub): https://github.com/jrouwe/JoltPhysics
- *Jolt vs PhysX* discussion: https://github.com/jrouwe/JoltPhysics/discussions/327
- Chaos Physics Overview (Unreal docs): https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-physics-overview
- Engine comparison guides (2025/2026): https://generalistprogrammer.com/tutorials/game-engine-comparison-complete-developer-guide-2025 · https://hyper3d.ai/blog/best-game-engines
- *Comparative Analysis of Unity, Unreal, Godot, and CryEngine* (ResearchGate): https://www.researchgate.net/publication/396627898
