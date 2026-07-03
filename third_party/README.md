# third_party/ — vendored dependencies

External libraries Rime depends on live here. We keep this list **small and
deliberate** — every dependency is code we ship, debug, and are bound by the license of.

## Policy

- **License compatibility first.** Only permissive licenses compatible with Apache-2.0
  (MIT, BSD, Apache-2.0, Zlib, etc.). **No copyleft** (GPL/LGPL/MPL) that would
  compromise our permissive promise. When in doubt, ask in an issue.
- **Record every dependency** in [`../NOTICE`](../NOTICE) with its copyright and license.
- **Prefer few, well-chosen deps.** A game engine is partly *about* controlling its own
  destiny; we don't pull in a library for something we should understand and own.
- **Pin versions** and document why each dependency is here.

## What we actually depend on (through M3)

Rime pulls its C++ build dependencies through **Conan** (see [`../conanfile.py`](../conanfile.py)),
not by vendoring source into this directory — so `third_party/` currently holds no code. Each
dependency is pinned and recorded in [`../NOTICE`](../NOTICE); all are permissive (Apache-2.0 /
MIT / BSD), per the policy above.

| Need | Choice | Via | Notes |
| --- | --- | --- | --- |
| Vulkan loading | **volk** + Vulkan-Headers | Conan | the RHI Vulkan backend; no loader linked at build time (ADR-0007) |
| GPU memory | **VulkanMemoryAllocator (VMA)** | Conan | battle-tested allocator (ADR-0007) |
| Shader compilation | **glslang** | Conan | offline GLSL→SPIR-V at build time (ADR-0008); no runtime compiler shipped |
| Formatting | **fmt** | Conan | logging + string formatting in `core` |
| C++ unit tests | **doctest** | Conan | header-only; one small test exe per module |

Decisions already made that *removed* a would-be dependency (VISION principle #1 — own our
destiny):

- **Windowing/input:** written **natively** (Win32 / Cocoa / Xlib + Wayland), **no SDL/GLFW** —
  [ADR-0006](../docs/adr/0006-native-windowing.md).
- **Math:** our own SIMD-friendly types in `engine/core`, **not glm**.
- **UI:** a from-scratch immediate-mode UI, **not Dear ImGui** — [ADR-0015](../docs/adr/0015-imgui-free-ui.md).

## Likely future dependencies (candidates, not yet added)

| Need | Likely choice | Notes |
| --- | --- | --- |
| Physics | **Jolt Physics** | multicore, AAA-proven (M7; see [engine survey](../docs/research/engine-survey.md#physics)) |

Each real addition gets discussed (and big ones get an ADR).
