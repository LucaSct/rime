# ADR-0002: Vulkan-first behind a thin RHI

- Status: Accepted
- Date: 2026-06-16

## Context

We need world-class rendering (the UE5-class targets in
[engine-survey.md](../research/engine-survey.md)) and we want to run on Windows, Linux,
and macOS — without letting portability dilute the engine (VISION principle #2).

The graphics-API options: pick one modern API; abstract several from day one; or pick
one now but leave a seam for more.

- **Vulkan** is modern, explicit, high-performance, and runs natively on Windows/Linux
  and on macOS via **MoltenVK** (Vulkan-on-Metal). It is the best single bet for a
  cross-platform, AAA-capable backend today.
- A full multi-backend abstraction (Vulkan + D3D12 + Metal), like Unreal/O3DE ship, is
  the eventual industry-standard shape — but it is roughly 3× the renderer work before
  anything renders.

## Decision

**Build one excellent Vulkan backend now, behind a thin Render Hardware Interface
(RHI).** The renderer and all engine code target `rime::rhi` interfaces. **The Vulkan
backend is the only code permitted to include Vulkan headers.** D3D12, Metal, and
console backends can be added later as additional RHI implementations without changing
the renderer.

## Consequences

**Good**
- Best power-to-effort ratio now: we reach advanced rendering fastest by perfecting one
  modern backend, while macOS comes "for free" enough via MoltenVK.
- The seam means future backends (native D3D12/Metal, consoles) are *additive*, not a
  rewrite. We get most of multi-backend's future-proofing at a fraction of today's cost.

**Costs we accept**
- The RHI abstraction has a small design/maintenance cost and must be kept honest —
  it's easy to accidentally leak Vulkan concepts through it. Reviews guard this.
- macOS via MoltenVK is a translation layer: not every Vulkan feature/extension maps
  perfectly, and it can trail native Metal. Acceptable now; revisit with a native Metal
  RHI backend if macOS becomes a first-class shipping target.

## Alternatives considered

- **Full RHI (Vulkan+D3D12+Metal) on day one.** Maximum native reach, but triples the
  work before first pixels and slows everything that depends on the renderer. We chose
  to design the seam now and fill it later.
- **Vulkan-only, no abstraction.** Lowest overhead and most focus, but adding any other
  backend later becomes a costly refactor through the whole renderer. The thin RHI costs
  little and buys the future; not worth giving up.
