# ADR-0008: Offline (build-time) shader compilation

- Status: Accepted
- Date: 2026-06-18

## Context

The Vulkan backend (M3) consumes **SPIR-V**, but shaders are authored in **GLSL**. Something has to
translate GLSL → SPIR-V, and the question is *when and where*:

- **Offline / build-time:** compile during the CMake build with `glslangValidator`, and make the
  result available to the engine (as files or embedded bytes). The shipped engine contains no shader
  compiler.
- **Runtime:** link a shader compiler (e.g. **libshaderc**) into the engine and compile GLSL while the
  program runs. Enables hot-reload, at the cost of a heavy runtime dependency and slower startup.

Rime is a lean, teaching-first codebase (VISION #1/#3) with a deliberately small dependency set
(`third_party/README.md`). It also *already has the pattern*: `engine/platform` runs `wayland-scanner`
at build time via a CMake `add_custom_command` to generate code from the xdg-shell XML. Shader
compilation is the same shape — a build-time codegen step.

## Decision

**Compile GLSL → SPIR-V offline, at build time, and embed the result.** A CMake helper
`rime_add_shaders(target …)` (in the top-level `CMakeLists.txt`) runs `glslangValidator` (provided by
the Conan **`glslang`** `tool_requires`, [ADR-0007](0007-vulkan-backend-bootstrapping.md)) on each
GLSL source to produce a `.spv`, then runs a tiny `cmake -P` script (`cmake/embed_spirv.cmake`) that
emits a C header defining the SPIR-V as a `static const uint32_t` array. The consuming target includes
that header and hands the bytes to `rhi::Device::create_shader`. **The engine ships no runtime shader
compiler.** Runtime/hot-reload compilation is deferred to the editor and asset pipeline (M6/M9), where
iteration speed matters and the tooling lives in Rust anyway.

## Consequences

**Good**
- **Zero runtime shader-compiler dependency** and fast startup — the engine just uploads ready SPIR-V.
- **Hermetic tests/samples:** the SPIR-V is embedded, so there is no runtime file lookup or asset-path
  problem for the M3 proof — it runs identically on a dev box and on a CI runner.
- **One known-good pattern:** it reuses the exact `add_custom_command` codegen approach already proven
  by the Wayland backend, so the build stays consistent and readable.
- Compile errors in a shader fail the **build**, early and loudly, not the program at runtime.

**Costs we accept**
- **No hot-reload yet:** changing a shader means rebuilding. Fine pre-editor; the editor (M9) will add
  runtime compilation for iteration without changing this default for shipped builds.
- Embedding SPIR-V grows the binary slightly — negligible at this scale, and revisitable (we can load
  `.spv` from disk via `platform::filesystem` instead) without changing the offline-compile decision.
- `glslangValidator` must be available at build time; it comes from Conan, with a `find_program`
  fallback to a system install (the same robustness `rime_add_shaders` already applies).

## Alternatives considered

- **Runtime compilation via libshaderc.** Real hot-reload, but a large runtime dependency and slower
  startup for a capability the *engine* runtime doesn't need (the editor does). Deferred to tooling.
- **Ship `.spv` files loaded at runtime.** Avoids embedding, but reintroduces asset-path/management
  concerns for tests and samples now; embedding is more hermetic at this stage. We can switch later if the
  asset pipeline makes file-based shaders the natural choice.
- **Check compiled SPIR-V into the repo.** Removes the build-time tool, but the artifacts drift from
  their source and bloat history; letting the build compile them keeps source the single source of truth.
