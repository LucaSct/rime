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

## Likely early dependencies (candidates, not yet added)

| Need | Likely choice | Notes |
| --- | --- | --- |
| Vulkan loading | Vulkan SDK / volk | the RHI Vulkan backend |
| GPU memory | VulkanMemoryAllocator (VMA) | battle-tested allocator |
| Windowing/input | SDL3 or GLFW | platform layer (TBD via ADR) |
| Physics | **Jolt Physics** | multicore, AAA-proven (see [engine survey](../docs/research/engine-survey.md#physics)) |
| Shader compilation | glslang / shaderc / DXC | GLSL/HLSL → SPIR-V |
| Math (maybe) | glm | or our own in `engine/core` |
| Tests | GoogleTest / Catch2 | C++ unit tests |

Each real addition gets discussed (and big ones get an ADR). Nothing is vendored yet.
